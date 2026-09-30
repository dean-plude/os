/*
 * um_thread.c — threads, synchronization objects and waits for programs
 *
 * Objects: events (manual/auto reset), mutants (recursive, abandoned when
 * the owner ends), semaphores and threads (signaled once ended).  Object
 * state changes happen under g_um_oblock, so a check and the acquisition
 * that follows it are atomic whatever the other CPUs do.  Waiting threads yield until an
 * object is signaled, the timeout passes or the thread is being ended.
 */

#include "um_internal.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../ke/smp.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../arch/x86_64/cpu.h"
#include "../net/net_internal.h"      /* net_lock: the net thread stirs the entropy pool */
#include "../net/tls.h"

#define ST_SUCCESS                0x00000000u
#define ST_ABANDONED              0x00000080u
#define ST_TIMEOUT                0x00000102u
#define ST_INVALID_HANDLE         0xC0000008u
#define ST_INVALID_PARAMETER      0xC000000Du
#define ST_NO_MEMORY              0xC0000017u
#define ST_ACCESS_VIOLATION       0xC0000005u
#define ST_OBJECT_TYPE_MISMATCH   0xC0000024u
#define ST_MUTANT_NOT_OWNED       0xC0000046u
#define ST_SEMAPHORE_LIMIT        0xC0000047u
#define ST_INVALID_INFO_CLASS     0xC0000003u
#define ST_INFO_LENGTH_MISMATCH   0xC0000004u
#define ST_THREAD_IS_TERMINATING  0xC000004Bu
#define ST_TOO_MANY_HANDLES       0xC000011Fu
#define ST_STILL_ACTIVE           0x00000103u
#define ST_CANCELLED              0xC0000120u

#define MAXIMUM_WAIT_OBJECTS      64

/* -----------------------------------------------------------------------
 * Objects
 * ----------------------------------------------------------------------- */
KSpinLock g_um_oblock = KSPINLOCK_INIT;

UmObject *um_ob_ref(UmObject *o)
{
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    return o;
}

/* -----------------------------------------------------------------------
 * The object namespace: named events, mutexes and semaphores that other
 * processes can open ("Local\\x", "Global\\x" and "x" are one name).
 * An entry lives as long as its object; lookups and the last release of a
 * named object serialize on g_ns_lock so a dying object is never found.
 * ----------------------------------------------------------------------- */
#define NS_MAX      256
#define NS_NAME_MAX 128
static struct { char name[NS_NAME_MAX]; UmObject *o; } g_ns[NS_MAX];
static KSpinLock g_ns_lock = KSPINLOCK_INIT;

static void ns_remove_locked(UmObject *o)
{
    for (int i = 0; i < NS_MAX; i++) if (g_ns[i].o == o) { g_ns[i].o = NULL; g_ns[i].name[0] = 0; }
}

/* The name in OBJECT_ATTRIBUTES @oa_ptr (UTF-16 kept as UTF-8 bytes; empty
 * if unnamed), without the session prefixes.  False if unreadable. */
static bool ns_name(UINT64 oa_ptr, char *out)
{
    out[0] = 0;
    if (!oa_ptr) return true;
    UINT64 oa[6];
    if (!NT_SUCCESS(CopyFromUser(oa, (const void *)(uintptr_t)oa_ptr, sizeof(oa)))) return false;
    if (!oa[2]) return true;
    UINT64 us[2];
    if (!NT_SUCCESS(CopyFromUser(us, (const void *)(uintptr_t)oa[2], sizeof(us)))) return false;
    UINT32 n = (UINT32)(us[0] & 0xFFFF) / 2;
    UINT16 w[NS_NAME_MAX];
    if (n >= NS_NAME_MAX) n = NS_NAME_MAX - 1;
    if (n && !NT_SUCCESS(CopyFromUser(w, (const void *)(uintptr_t)us[1], 2 * n))) return false;
    int k = 0;
    for (UINT32 i = 0; i < n && k < NS_NAME_MAX - 3; i++) {
        UINT32 c = w[i];
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800) { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
        else { out[k++] = (char)(0xE0 | c >> 12); out[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[k] = 0;
    static const char *const prefixes[] = { "\\BaseNamedObjects\\", "\\Sessions\\1\\BaseNamedObjects\\", "Local\\", "Global\\", "Session\\1\\" };
    for (int again = 1; again; ) {
        again = 0;
        for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
            int pl = (int)strlen(prefixes[i]);
            if (!strncmp(out, prefixes[i], pl)) { memmove(out, out + pl, strlen(out + pl) + 1); again = 1; }
        }
    }
    return true;
}

/* A referenced object named @name, or NULL */
static UmObject *ns_lookup(const char *name)
{
    UmObject *o = NULL;
    IrqState s = spin_lock_irqsave(&g_ns_lock);
    for (int i = 0; i < NS_MAX; i++)
        if (g_ns[i].o && !strcmp(g_ns[i].name, name)) { o = um_ob_ref(g_ns[i].o); break; }
    spin_unlock_irqrestore(&g_ns_lock, s);
    return o;
}

static bool ns_add(const char *name, UmObject *o)
{
    bool ok = false;
    IrqState s = spin_lock_irqsave(&g_ns_lock);
    for (int i = 0; i < NS_MAX; i++)
        if (!g_ns[i].o) {
            strncpy(g_ns[i].name, name, NS_NAME_MAX - 1);
            g_ns[i].name[NS_NAME_MAX - 1] = 0;
            g_ns[i].o = o;
            o->named = true;
            ok = true;
            break;
        }
    spin_unlock_irqrestore(&g_ns_lock, s);
    return ok;
}

void um_ob_unref(UmObject *o)
{
    if (!o) return;
    if (o->named) {
        IrqState s = spin_lock_irqsave(&g_ns_lock);
        int left = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
        if (!left) ns_remove_locked(o);
        spin_unlock_irqrestore(&g_ns_lock, s);
        if (left) return;
    } else if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    /* The last reference may go in a service that runs without the big
     * kernel lock; the destructors (a process's, a socket's...) want it */
    bkl_acquire();
    if (o->destroy) o->destroy(o);
    kfree(o);                                       /* UmThread: ob is its first member */
    bkl_release();
}

static UmObject *ob_new(UmObType type)
{
    UmObject *o = kzalloc(sizeof(*o));
    if (o) { o->type = type; o->refs = 1; }
    return o;
}

/* Under g_um_oblock: can @o be acquired by @me right now? */
static bool ob_ready(UmObject *o, UmThread *me)
{
    switch (o->type) {
    case UO_EVENT:     return o->signaled;
    case UO_SEMAPHORE: return o->count > 0;
    case UO_MUTANT:    return !o->owner || o->owner == me;
    case UO_THREAD:    return o->signaled;
    default:           return o->signaled;
    }
}

/* Under g_um_oblock: take @o (it is ready).  True if it was abandoned. */
static bool ob_acquire(UmObject *o, UmThread *me)
{
    switch (o->type) {
    case UO_EVENT:     if (!o->manual) o->signaled = false; break;
    case UO_SEMAPHORE: o->count--; break;
    case UO_MUTANT: {
        o->owner = me;
        o->recursion++;
        bool ab = o->abandoned;
        o->abandoned = false;
        return ab;
    }
    default: break;
    }
    return false;
}

static UINT64 deadline_ticks(INT64 timeout_100ns)
{
    if (timeout_100ns < 0) return UINT64_MAX;
    return sched_ticks() + ((UINT64)timeout_100ns + 99999) / 100000;
}

/* Threads waiting on objects (g_um_oblock) */
static UmThread *g_waiters;

void um_ob_wake(UmObject *o)
{
    for (UmThread *w = g_waiters; w; w = w->wait_next) {
        if (w->wake) continue;
        for (int i = 0; i < w->wait_n; i++) {
            if (w->wait_objs[i] != o) continue;
            w->wake = 1;
            if (w->kt) sched_unblock(w->kt);
            break;
        }
    }
}

static void waiter_unlink(UmThread *me)
{
    for (UmThread **pp = &g_waiters; *pp; pp = &(*pp)->wait_next)
        if (*pp == me) { *pp = me->wait_next; break; }
    me->wait_next = NULL;
    me->wait_objs = NULL;
    me->wait_n = 0;
}

/* Wait on @n objects: any one (index returned) or all of them.  The waiter
 * sleeps on the object list until a signaler wakes it (um_ob_wake), the
 * timeout passes, or 100 ms go by (then it looks again anyway). */
static UINT32 wait_objects(UmObject **o, int n, bool all, INT64 timeout_100ns)
{
    UmThread *me = UmCurrentThread();
    UINT64 until = deadline_ticks(timeout_100ns);
    for (;;) {
        IrqState s = ob_lock();
        if (all) {
            bool ready = true;
            for (int i = 0; i < n && ready; i++) ready = ob_ready(o[i], me);
            if (ready) {
                bool ab = false;
                for (int i = 0; i < n; i++) ab |= ob_acquire(o[i], me);
                ob_unlock(s);
                return ab ? ST_ABANDONED : ST_SUCCESS;
            }
        } else {
            for (int i = 0; i < n; i++) {
                if (ob_ready(o[i], me)) {
                    bool ab = ob_acquire(o[i], me);
                    ob_unlock(s);
                    return (ab ? ST_ABANDONED : ST_SUCCESS) + (UINT32)i;
                }
            }
        }
        if (um_stopping()) { ob_unlock(s); return ST_THREAD_IS_TERMINATING; }
        if (timeout_100ns == 0 || sched_ticks() >= until) { ob_unlock(s); return ST_TIMEOUT; }
        me->wait_objs = o;
        me->wait_n = n;
        me->wake = 0;
        me->wait_next = g_waiters;
        g_waiters = me;
        ob_unlock(s);
        UINT64 nap = sched_ticks() + 10;
        sched_sleep_until(&me->wake, until < nap ? until : nap);
        s = ob_lock();
        waiter_unlink(me);
        ob_unlock(s);
    }
}

UINT32 um_wait_one(UmObject *o, INT64 timeout_100ns)
{
    return wait_objects(&o, 1, false, timeout_100ns);
}

void um_abandon_mutants(UmProcess *p, UmThread *t)
{
    um_lock(&p->lock);
    for (int i = 0; i < UM_MAX_HANDLES; i++) {
        UmHandle *h = &p->handles[i];
        if (h->kind != H_OBJECT || h->obj->type != UO_MUTANT || h->obj->owner != t) continue;
        IrqState s = ob_lock();
        h->obj->owner = NULL;
        h->obj->recursion = 0;
        h->obj->abandoned = true;
        um_ob_wake(h->obj);
        ob_unlock(s);
    }
    um_unlock(&p->lock);
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static bool put_handle(UINT64 ptr, UINT64 h)
{
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, &h, 8));
}

static bool put_u32(UINT64 ptr, UINT32 v)
{
    return !ptr || NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, &v, 4));
}

/* A LARGE_INTEGER timeout: NULL = forever, negative = relative,
 * positive = absolute system time.  Returns 100 ns units or -1. */
static bool get_timeout(UINT64 ptr, INT64 *out)
{
    if (!ptr) { *out = -1; return true; }
    INT64 v;
    if (!NT_SUCCESS(CopyFromUser(&v, (const void *)(uintptr_t)ptr, 8))) return false;
    if (v <= 0) *out = -v;
    else { UINT64 now = um_now_100ns(); *out = (UINT64)v > now ? (INT64)((UINT64)v - now) : 0; }
    return true;
}

/* New object + handle stored at @handle_ptr */
static UINT64 new_handle(UmProcess *p, UmObject *o, UINT64 handle_ptr)
{
    UINT64 h = um_handle_new_object(p, o);
    um_ob_unref(o);                                  /* the handle holds it now */
    if (!h) return ST_TOO_MANY_HANDLES;
    if (!put_handle(handle_ptr, h)) {
        um_close_handle(h);
        return ST_ACCESS_VIOLATION;
    }
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Events, mutants, semaphores
 * ----------------------------------------------------------------------- */
/* NtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, EVENT_TYPE, BOOLEAN InitialState) */
#define ST_OBJECT_NAME_EXISTS    0x40000000u
#define ST_OBJECT_TYPE_MISMATCH  0xC0000024u
#define ST_OBJECT_NAME_NOT_FOUND 0xC0000034u

/* Create-or-open for a named object: 0 = create a new one (named @name
 * if non-empty); otherwise the status to return (a handle to the existing
 * one was made, or an error). */
static UINT64 open_existing(const char *name, UmObType type, UINT64 handle_ptr)
{
    if (!name[0]) return 0;
    UmObject *o = ns_lookup(name);
    if (!o) return 0;
    if (o->type != type) { um_ob_unref(o); return ST_OBJECT_TYPE_MISMATCH; }
    UINT64 r = new_handle(UmCurrent(), o, handle_ptr);
    return r ? r : ST_OBJECT_NAME_EXISTS;
}

static UINT64 finish_create(UmObject *o, const char *name, UINT64 handle_ptr)
{
    if (name[0]) ns_add(name, o);
    return new_handle(UmCurrent(), o, handle_ptr);
}

/* NtOpenEvent / NtOpenMutant / NtOpenSemaphore(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES) */
static UINT64 open_named(UmObType type, UINT64 handle_ptr, UINT64 oa)
{
    char name[NS_NAME_MAX];
    if (!ns_name(oa, name)) return ST_ACCESS_VIOLATION;
    if (!name[0]) return ST_INVALID_PARAMETER;
    UmObject *o = ns_lookup(name);
    if (!o) return ST_OBJECT_NAME_NOT_FOUND;
    if (o->type != type) { um_ob_unref(o); return ST_OBJECT_TYPE_MISMATCH; }
    return new_handle(UmCurrent(), o, handle_ptr);
}
static UINT64 sys_open_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)     { (void)a2; (void)a4; return open_named(UO_EVENT, a1, a3); }
static UINT64 sys_open_mutant(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)    { (void)a2; (void)a4; return open_named(UO_MUTANT, a1, a3); }
static UINT64 sys_open_semaphore(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a2; (void)a4; return open_named(UO_SEMAPHORE, a1, a3); }

static UINT64 sys_create_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2;
    char name[NS_NAME_MAX];
    if (!ns_name(a3, name)) return ST_ACCESS_VIOLATION;
    UINT64 r = open_existing(name, UO_EVENT, a1);
    if (r) return r;
    UmObject *o = ob_new(UO_EVENT);
    if (!o) return ST_NO_MEMORY;
    o->manual = a4 == 0;                             /* NotificationEvent */
    o->signaled = um_stack_arg(5) & 0xFF;
    return finish_create(o, name, a1);
}

static UINT64 event_op(UINT64 h, UINT64 prev_ptr, int op)
{
    UmObject *o = um_handle_object(UmCurrent(), h, UO_EVENT);
    if (!o) return ST_INVALID_HANDLE;
    IrqState s = ob_lock();
    UINT32 prev = o->signaled;
    o->signaled = op == 1;
    if (op == 1) um_ob_wake(o);
    ob_unlock(s);
    um_ob_unref(o);
    return put_u32(prev_ptr, prev) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

static UINT64 sys_set_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)   { (void)a3; (void)a4; return event_op(a1, a2, 1); }
static UINT64 sys_reset_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a3; (void)a4; return event_op(a1, a2, 0); }
static UINT64 sys_clear_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a2; (void)a3; (void)a4; return event_op(a1, 0, 0); }

/* NtCreateMutant(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN InitialOwner) */
static UINT64 sys_create_mutant(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2;
    char name[NS_NAME_MAX];
    if (!ns_name(a3, name)) return ST_ACCESS_VIOLATION;
    UINT64 r = open_existing(name, UO_MUTANT, a1);
    if (r) return r;
    UmObject *o = ob_new(UO_MUTANT);
    if (!o) return ST_NO_MEMORY;
    if (a4 & 0xFF) { o->owner = UmCurrentThread(); o->recursion = 1; }
    return finish_create(o, name, a1);
}

/* NtReleaseMutant(HANDLE, PLONG PreviousCount) */
static UINT64 sys_release_mutant(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmObject *o = um_handle_object(UmCurrent(), a1, UO_MUTANT);
    if (!o) return ST_INVALID_HANDLE;
    UINT32 st = ST_SUCCESS, prev = 0;
    IrqState s = ob_lock();
    if (o->owner != UmCurrentThread()) st = ST_MUTANT_NOT_OWNED;
    else {
        prev = o->recursion;
        if (--o->recursion == 0) { o->owner = NULL; um_ob_wake(o); }
    }
    ob_unlock(s);
    um_ob_unref(o);
    if (st) return st;
    return put_u32(a2, (UINT32)(1 - (INT32)prev)) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* NtCreateSemaphore(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, LONG Initial, LONG Maximum) */
static UINT64 sys_create_semaphore(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2;
    INT32 init = (INT32)a4, max = (INT32)um_stack_arg(5);
    if (max <= 0 || init < 0 || init > max) return ST_INVALID_PARAMETER;
    char name[NS_NAME_MAX];
    if (!ns_name(a3, name)) return ST_ACCESS_VIOLATION;
    UINT64 r = open_existing(name, UO_SEMAPHORE, a1);
    if (r) return r;
    UmObject *o = ob_new(UO_SEMAPHORE);
    if (!o) return ST_NO_MEMORY;
    o->count = init;
    o->max = max;
    return finish_create(o, name, a1);
}

/* NtReleaseSemaphore(HANDLE, LONG ReleaseCount, PLONG PreviousCount) */
static UINT64 sys_release_semaphore(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    INT32 n = (INT32)a2;
    if (n <= 0) return ST_INVALID_PARAMETER;
    UmObject *o = um_handle_object(UmCurrent(), a1, UO_SEMAPHORE);
    if (!o) return ST_INVALID_HANDLE;
    UINT32 st = ST_SUCCESS;
    INT32 prev;
    IrqState s = ob_lock();
    prev = o->count;
    if (o->count > o->max - n) st = ST_SEMAPHORE_LIMIT;
    else { o->count += n; um_ob_wake(o); }
    ob_unlock(s);
    um_ob_unref(o);
    if (st) return st;
    return put_u32(a3, (UINT32)prev) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* -----------------------------------------------------------------------
 * Waits
 * ----------------------------------------------------------------------- */
/* NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER Timeout) */
static UINT64 sys_wait_single(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a4;
    INT64 t;
    if (!get_timeout(a3, &t)) return ST_ACCESS_VIOLATION;
    UmObject *o = um_handle_object(UmCurrent(), a1, 0);
    if (!o) return ST_INVALID_HANDLE;
    UINT32 st = um_wait_one(o, t);
    um_ob_unref(o);
    return st;
}

/* NtWaitForMultipleObjects(ULONG Count, PHANDLE Handles, WAIT_TYPE (0 all, 1 any),
 *                          BOOLEAN Alertable, PLARGE_INTEGER Timeout) */
static UINT64 sys_wait_multiple(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UINT32 n = (UINT32)a1;
    if (!n || n > MAXIMUM_WAIT_OBJECTS || a3 > 1) return ST_INVALID_PARAMETER;
    UINT64 hv[MAXIMUM_WAIT_OBJECTS];
    if (!NT_SUCCESS(CopyFromUser(hv, (const void *)(uintptr_t)a2, 8ULL * n))) return ST_ACCESS_VIOLATION;
    INT64 t;
    if (!get_timeout(um_stack_arg(5), &t)) return ST_ACCESS_VIOLATION;
    UmProcess *p = UmCurrent();
    UmObject *o[MAXIMUM_WAIT_OBJECTS];
    UINT32 st = ST_SUCCESS;
    UINT32 got = 0;
    for (; got < n; got++) {
        o[got] = um_handle_object(p, hv[got], 0);
        if (!o[got]) { st = ST_INVALID_HANDLE; break; }
    }
    if (!st) st = wait_objects(o, (int)n, a3 == 0, t);
    for (UINT32 i = 0; i < got; i++) um_ob_unref(o[i]);
    return st;
}

/* -----------------------------------------------------------------------
 * Threads
 * ----------------------------------------------------------------------- */
/* NtCreateThreadEx(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE Process,
 *   PVOID StartRoutine, PVOID Argument, ULONG CreateFlags, SIZE_T ZeroBits,
 *   SIZE_T StackSize, SIZE_T MaximumStackSize, PPS_ATTRIBUTE_LIST) */
static UINT64 sys_create_thread_ex(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3;
    UmProcess *p = UmCurrent();
    if (a4 != UINT64_C(0xFFFFFFFFFFFFFFFF)) return ST_INVALID_HANDLE;   /* this process only */
    UINT64 start = um_stack_arg(5), arg = um_stack_arg(6);
    UINT32 flags = (UINT32)um_stack_arg(7);
    UINT64 stack = um_stack_arg(10) ? um_stack_arg(10) : um_stack_arg(9);
    if (!start) return ST_INVALID_PARAMETER;
    UINT32 st;
    UmThread *t = um_create_thread(p, start, arg, stack ? stack : UM_THREAD_STACK, true, &st);
    if (!t) return st;
    UINT64 h = um_handle_new_object(p, &t->ob);
    if (!h || !put_handle(a1, h)) {
        if (h) um_close_handle(h);
        t->terminate = true;                         /* never starts */
        t->term_status = ST_CANCELLED;
        __atomic_store_n(&t->suspend, 0, __ATOMIC_RELEASE);
        return h ? ST_ACCESS_VIOLATION : ST_TOO_MANY_HANDLES;
    }
    if (!(flags & 1)) __atomic_store_n(&t->suspend, 0, __ATOMIC_RELEASE);   /* not CREATE_SUSPENDED */
    return ST_SUCCESS;
}

/* NtTerminateThread(HANDLE (0 or -2: the caller), NTSTATUS) */
static UINT64 sys_terminate_thread(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmThread *me = UmCurrentThread();
    if (!a1 || a1 == UINT64_C(0xFFFFFFFFFFFFFFFE)) um_exit_thread((UINT32)a2);
    UmObject *o = um_handle_object(me->proc, a1, UO_THREAD);
    if (!o) return ST_INVALID_HANDLE;
    UmThread *t = (UmThread *)o;
    if (t == me) { um_ob_unref(o); um_exit_thread((UINT32)a2); }
    if (!t->exited && !t->terminate) {
        t->term_status = (UINT32)a2;
        t->terminate = true;
    }
    um_ob_unref(o);
    return ST_SUCCESS;
}

static UINT64 suspend_op(UINT64 h, UINT64 prev_ptr, int delta)
{
    UmObject *o = um_handle_object(UmCurrent(), h, UO_THREAD);
    if (!o) return ST_INVALID_HANDLE;
    UmThread *t = (UmThread *)o;
    IrqState s = ob_lock();
    INT32 prev = t->suspend;
    if (delta > 0 && prev < 127) t->suspend = prev + 1;
    if (delta < 0 && prev > 0) t->suspend = prev - 1;
    ob_unlock(s);
    um_ob_unref(o);
    return put_u32(prev_ptr, (UINT32)prev) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

static UINT64 sys_resume_thread(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)  { (void)a3; (void)a4; return suspend_op(a1, a2, -1); }
static UINT64 sys_suspend_thread(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a3; (void)a4; return suspend_op(a1, a2, +1); }

/* NtQueryInformationThread(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG ReturnLength) */
static UINT64 sys_query_info_thread(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (a2 != 0) return ST_INVALID_INFO_CLASS;                 /* ThreadBasicInformation */
    if (a4 < 48) return ST_INFO_LENGTH_MISMATCH;
    UmObject *o = um_handle_object(p, a1, UO_THREAD);
    if (!o) return ST_INVALID_HANDLE;
    UmThread *t = (UmThread *)o;
    UINT64 b[6] = { 0 };
    b[0] = t->exited ? t->exit_code : ST_STILL_ACTIVE;         /* ExitStatus */
    b[1] = t->exited ? 0 : t->teb;                             /* TebBaseAddress */
    b[2] = p->pid;                                             /* ClientId */
    b[3] = t->tid;
    b[4] = 1;                                                  /* AffinityMask */
    b[5] = 8 | (UINT64)8 << 32;                                /* Priority, BasePriority */
    um_ob_unref(o);
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, sizeof(b)))) return ST_ACCESS_VIOLATION;
    return put_u32(um_stack_arg(5), 48) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* NtSetInformationThread: priorities, names, hiding from debuggers — accepted, ignored */
static UINT64 sys_set_info_thread(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmObject *o = um_handle_object(UmCurrent(), a1, UO_THREAD);
    if (!o) return ST_INVALID_HANDLE;
    um_ob_unref(o);
    return ST_SUCCESS;
}

/* NtQueryInformationProcess(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG) */
static UINT64 sys_query_info_process(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (a1 != UINT64_C(0xFFFFFFFFFFFFFFFF)) return ST_INVALID_HANDLE;
    if (a2 != 0) return ST_INVALID_INFO_CLASS;                 /* ProcessBasicInformation */
    if (a4 < 48) return ST_INFO_LENGTH_MISMATCH;
    UINT64 b[6] = { ST_STILL_ACTIVE, p->lay.peb, 1, 8, p->pid, 0 };
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, sizeof(b)))) return ST_ACCESS_VIOLATION;
    return put_u32(um_stack_arg(5), 48) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* A process named by a handle (-1: the caller); referenced by @ob if not
 * the caller */
static UmProcess *proc_of(UmProcess *self, UINT64 h, UmObject **ob)
{
    *ob = NULL;
    if (h == UINT64_C(0xFFFFFFFFFFFFFFFF)) return self;
    UmObject *o = um_handle_object(self, h, UO_PROCESS);
    if (!o) return NULL;
    if (!o->proc || o->proc->reclaimed) { um_ob_unref(o); return NULL; }
    *ob = o;
    return o->proc;
}

/* NtDuplicateObject(HANDLE SourceProcess, HANDLE Source, HANDLE TargetProcess,
 *                   PHANDLE Target, ACCESS_MASK, ULONG Attributes, ULONG Options)
 * Either process may be another one (a process handle): a parent giving
 * a child a handle, or taking one of the child's. */
static UINT64 sys_duplicate_object(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT32 attrs = (UINT32)um_stack_arg(6), options = (UINT32)um_stack_arg(7);
    UmObject *sob = NULL, *tob = NULL;
    UmProcess *sp = proc_of(p, a1, &sob);
    if (!sp) return ST_INVALID_HANDLE;
    UmProcess *tp = a3 ? proc_of(p, a3, &tob) : NULL;
    if (a3 && !tp) { if (sob) um_ob_unref(sob); return ST_INVALID_HANDLE; }
    UINT64 nh = 0;
    UINT32 st = ST_SUCCESS;
    if (a2 == UINT64_C(0xFFFFFFFFFFFFFFFE) && sp == p) {     /* GetCurrentThread() */
        nh = tp ? um_handle_new_object(tp, &UmCurrentThread()->ob) : 0;
        if (tp && !nh) st = ST_TOO_MANY_HANDLES;
    } else if (a2 == UINT64_C(0xFFFFFFFFFFFFFFFF) && sp == p && tp && tp != p) {
        st = ST_INVALID_PARAMETER;                           /* (the caller's own process) */
    } else {
        DesktopLock();
        um_lock(&sp->lock);
        UmHandle src;
        bool ok = a2 >= 4 && !(a2 & 3) && a2 / 4 - 1 < UM_MAX_HANDLES && sp->handles[a2 / 4 - 1].kind != H_FREE;
        if (ok) {
            src = sp->handles[a2 / 4 - 1];
            if (src.kind == H_FILE || src.kind == H_DIR) RamfsRef(src.node);
            if (src.kind == H_OBJECT) um_ob_ref(src.obj);
        }
        um_unlock(&sp->lock);
        if (!ok) st = ST_INVALID_HANDLE;
        else if (tp) {
            if (!(options & 4)) src.inherit = attrs & 2;     /* DUPLICATE_SAME_ATTRIBUTES, OBJ_INHERIT */
            um_lock(&tp->lock);
            int free = -1;
            for (int i = 0; i < UM_MAX_HANDLES; i++) if (tp->handles[i].kind == H_FREE) { free = i; break; }
            if (free >= 0) { tp->handles[free] = src; nh = (UINT64)(free + 1) * 4; }
            um_unlock(&tp->lock);
            if (free < 0) st = ST_TOO_MANY_HANDLES;
        }
        if (ok && (!tp || st)) {                             /* not placed: drop the reference */
            if (src.kind == H_FILE || src.kind == H_DIR) RamfsUnref(src.node);
            if (src.kind == H_OBJECT) um_ob_unref(src.obj);
        }
        DesktopUnlock();
    }
    if (!st && (options & 1)) {                              /* DUPLICATE_CLOSE_SOURCE */
        if (sp == p) um_close_handle(a2);
        else {
            DesktopLock();
            um_lock(&sp->lock);
            UmHandle *h = a2 >= 4 && !(a2 & 3) && a2 / 4 - 1 < UM_MAX_HANDLES ? &sp->handles[a2 / 4 - 1] : NULL;
            UmHandle old = h ? *h : (UmHandle){ 0 };
            if (h) memset(h, 0, sizeof(*h));
            um_unlock(&sp->lock);
            if (old.kind == H_FILE || old.kind == H_DIR) RamfsUnref(old.node);
            if (old.kind == H_OBJECT) um_ob_unref(old.obj);
            DesktopUnlock();
        }
    }
    if (sob) um_ob_unref(sob);
    if (tob && tp) um_ob_unref(tob);
    if (st) return st;
    if (a4 && !put_handle(a4, nh)) return ST_ACCESS_VIOLATION;
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * The loader's kernel half, debug output
 * ----------------------------------------------------------------------- */
/* NtNovaLoadDll(PCSTR Name, ULONG Length, PVOID *Base) */
static UINT64 sys_nova_load_dll(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    char name[RAMFS_PATH_MAX];
    if (!a2 || a2 >= sizeof(name)) return ST_INVALID_PARAMETER;
    if (!NT_SUCCESS(CopyFromUser(name, (const void *)(uintptr_t)a1, a2))) return ST_ACCESS_VIOLATION;
    name[a2] = '\0';
    UINT64 base = 0;
    UINT32 st = um_load_dll(UmCurrent(), name, &base);
    if (st) return st;
    UINT64 b = base;
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, &b, 8)) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* NtNovaDebugPrint(PCSTR, ULONG Length): to the kernel log */
static UINT64 sys_nova_debug_print(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    char buf[256];
    UINT64 n = a2 < sizeof(buf) - 1 ? a2 : sizeof(buf) - 1;
    if (!NT_SUCCESS(CopyFromUser(buf, (const void *)(uintptr_t)a1, n))) return ST_ACCESS_VIOLATION;
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    kprintf("[UM] %s: %s\n", UmCurrent()->name, buf);
    return ST_SUCCESS;
}

/* NtNovaUnimplemented(index): the program called an import NovaOS does
 * not have (the loader bound it to a stub); say which, and end it. */
static UINT64 sys_nova_unimplemented(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    const char *what = p->stub_names && a1 < (UINT64)p->nstubs ? p->stub_names[a1] : "an unknown function";
    char msg[160];
    int n = ksnprintf(msg, sizeof(msg), "\r\n%s called %s, which NovaOS does not implement yet.\r\n", p->name, what);
    kprintf("[UM] %s (PID %u): unimplemented %s\n", p->name, p->pid, what);
    if (p->con) um_console_write(p->con, msg, n);
    ksnprintf(p->why, sizeof(p->why), "unimplemented %s", what);
    um_exit_process(0xC0000139u);                   /* STATUS_ENTRYPOINT_NOT_FOUND */
}

/* NtNovaGetRandom(PVOID Buffer, ULONG Length): bytes from the kernel's
 * entropy pool (net/tls.c), for programs' own TLS and key generation */
static UINT64 sys_nova_get_random(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UINT8 buf[256];
    if (a2 > 4096) return ST_INVALID_PARAMETER;
    for (UINT64 off = 0; off < a2; off += sizeof(buf)) {
        UINT64 n = a2 - off < sizeof(buf) ? a2 - off : sizeof(buf);
        net_lock();
        TlsEntropyOutput(buf, n);
        net_unlock();
        if (!NT_SUCCESS(CopyToUser((UINT8 *)(uintptr_t)a1 + off, buf, n))) {
            memset(buf, 0, sizeof(buf));
            return ST_ACCESS_VIOLATION;
        }
    }
    memset(buf, 0, sizeof(buf));
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Sections: shared memory (backed by memory, not by a file) that any
 * process can map by handle or by name
 * ----------------------------------------------------------------------- */
#define ST_UNABLE_TO_DELETE_SECTION 0xC000001Bu
#define ST_CONFLICTING_ADDRESSES_   0xC0000018u
#define ST_NOT_MAPPED_VIEW          0xC0000019u
#define ST_NOT_SUPPORTED_           0xC00000BBu
#define ST_SECTION_TOO_BIG          0xC0000040u

typedef struct {
    UINT64 size, npages;
    PADDR *frames;
    RamNode *file;                  /* file-backed: written back on unmap, flush and destruction */
    bool writable;
} UmSection;

/* File-backed sections hold a copy of the file: it is filled at creation
 * and written back (whole) while the file is still there. Programs that
 * read the file through ReadFile meanwhile see the copy only once it is
 * written back, as on a flush. */
static void section_fill(UmSection *sec)
{
    DesktopLock();
    RamNode *f = sec->file;
    UINT64 left = f->size < sec->size ? f->size : sec->size;
    for (UINT64 i = 0; i < sec->npages && left; i++) {
        UINT64 n = left < PAGE_SIZE ? left : PAGE_SIZE;
        memcpy(um_frame_ptr(sec->frames[i]), f->data + i * PAGE_SIZE, n);
        left -= n;
    }
    DesktopUnlock();
}

static void section_writeback(UmSection *sec)
{
    if (!sec->file || !sec->writable) return;
    DesktopLock();
    RamNode *f = sec->file;
    if (f->parent || f == RamfsRoot()) {                    /* not deleted meanwhile */
        UINT64 left = sec->size;
        for (UINT64 i = 0; i < sec->npages && left; i++) {
            UINT64 n = left < PAGE_SIZE ? left : PAGE_SIZE;
            if (!RamfsWriteAt(f, (UINT32)(i * PAGE_SIZE), um_frame_ptr(sec->frames[i]), (UINT32)n)) break;
            left -= n;
        }
    }
    DesktopUnlock();
}

static void section_destroy(UmObject *o)
{
    UmSection *sec = o->ptr;
    if (!sec) return;
    section_writeback(sec);
    if (sec->file) { DesktopLock(); RamfsUnref(sec->file); DesktopUnlock(); }
    um_free_frames(sec->frames, sec->npages);
    kfree(sec);
    o->ptr = NULL;
}

void um_flush_view_at(UmProcess *p, UINT64 va)
{
    um_lock(&p->lock);
    UmRegion *r = um_region_find(p, va);
    UmObject *o = r && r->section ? um_ob_ref(r->section) : NULL;
    um_unlock(&p->lock);
    if (!o) return;
    section_writeback(o->ptr);
    um_ob_unref(o);
}

static bool get_u64_(UINT64 ptr, UINT64 *v) { return ptr && NT_SUCCESS(CopyFromUser(v, (const void *)(uintptr_t)ptr, 8)); }
static bool put_u64_(UINT64 ptr, UINT64 v) { return ptr && NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, &v, 8)); }

/* NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER MaximumSize,
 *                 ULONG PageProtection, ULONG AllocationAttributes, HANDLE File) */
static UINT64 sys_create_section(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2;
    UmProcess *p = UmCurrent();
    UINT64 fileh = um_stack_arg(7);
    UINT32 prot = (UINT32)um_stack_arg(5);
    char name[NS_NAME_MAX];
    if (!ns_name(a3, name)) return ST_ACCESS_VIOLATION;
    UINT64 r = open_existing(name, UO_SECTION, a1);
    if (r) return r;
    UINT64 size = 0;
    if (a4 && !get_u64_(a4, &size)) return ST_INVALID_PARAMETER;
    RamNode *file = NULL;
    if (fileh) {                                            /* the file's size when none is given */
        DesktopLock();
        um_lock(&p->lock);
        file = um_handle_file(p, fileh);
        if (file) { RamfsRef(file); if (!size) size = file->size; }
        um_unlock(&p->lock);
        DesktopUnlock();
        if (!file) return ST_INVALID_HANDLE;
    }
    if (!size) { if (file) { DesktopLock(); RamfsUnref(file); DesktopUnlock(); } return file ? 0xC000011EU /* MAPPED_FILE_SIZE_ZERO */ : ST_INVALID_PARAMETER; }
    UINT64 n = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    UmSection *sec = kzalloc(sizeof(*sec));
    if (sec) sec->frames = um_alloc_frames(n);
    if (!sec || !sec->frames) {
        kfree(sec);
        if (file) { DesktopLock(); RamfsUnref(file); DesktopUnlock(); }
        return n > (UINT64_C(256) << 20) / PAGE_SIZE ? ST_SECTION_TOO_BIG : ST_NO_MEMORY;
    }
    sec->size = size;
    sec->npages = n;
    sec->file = file;
    sec->writable = (prot & 0xFF) == 0x04 || (prot & 0xFF) == 0x40;   /* READWRITE, EXECUTE_READWRITE */
    if (file) section_fill(sec);
    UmObject *o = ob_new(UO_SECTION);
    if (!o) { section_destroy(&(UmObject){ .ptr = sec }); return ST_NO_MEMORY; }
    o->ptr = sec;
    o->destroy = section_destroy;
    return finish_create(o, name, a1);
}

static UINT64 sys_open_section(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a2; (void)a4; return open_named(UO_SECTION, a1, a3); }

/* NtMapViewOfSection(HANDLE Section, HANDLE Process, PVOID *Base, ULONG_PTR ZeroBits, SIZE_T CommitSize,
 *                    PLARGE_INTEGER Offset, PSIZE_T ViewSize, InheritDisposition, AllocationType, Win32Protect) */
static UINT64 sys_map_view(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UmProcess *p = UmCurrent();
    if (a2 != UINT64_C(0xFFFFFFFFFFFFFFFF)) return ST_NOT_SUPPORTED_;
    UINT64 off_ptr = um_stack_arg(6), size_ptr = um_stack_arg(7);
    UINT32 prot = (UINT32)um_stack_arg(10);
    UINT64 base = 0, off = 0, view = 0;
    if (!get_u64_(a3, &base) || !get_u64_(size_ptr, &view)) return ST_ACCESS_VIOLATION;
    if (off_ptr && !get_u64_(off_ptr, &off)) return ST_ACCESS_VIOLATION;
    UmObject *o = um_handle_object(p, a1, UO_SECTION);
    if (!o) return ST_INVALID_HANDLE;
    UmSection *sec = o->ptr;
    if (off & 0xFFF) { um_ob_unref(o); return ST_INVALID_PARAMETER; }
    if (off >= sec->npages * PAGE_SIZE) { um_ob_unref(o); return ST_INVALID_PARAMETER; }
    if (!view) view = sec->size - off;
    if (off + view > sec->npages * PAGE_SIZE) { um_ob_unref(o); return ST_INVALID_PARAMETER; }
    UINT64 npages = (view + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT64 bytes = npages * PAGE_SIZE;
    if ((prot & 0xFF) == 0) prot = 0x04;
    um_lock(&p->lock);
    UINT64 r = ST_SUCCESS;
    if (base) {
        base &= ~0xFFFFULL;
        if (!um_is_free(p, base, bytes)) r = ST_CONFLICTING_ADDRESSES_;
    } else {
        base = um_find_free(p, bytes, p->lay.alloc_min, p->lay.alloc_max);
        if (!base) r = ST_NO_MEMORY;
    }
    UmRegion *reg = NULL;
    if (r == ST_SUCCESS) {
        reg = um_region_add(p, base, bytes, prot, false);
        if (!reg) r = ST_NO_MEMORY;
    }
    if (r == ST_SUCCESS && !um_map_frames(p, base, sec->frames + off / PAGE_SIZE, npages, prot)) {
        um_region_remove(p, reg);
        r = ST_NO_MEMORY;
    }
    if (r == ST_SUCCESS) reg->section = o;                  /* the view keeps the reference */
    um_unlock(&p->lock);
    if (r != ST_SUCCESS) { um_ob_unref(o); return r; }
    put_u64_(a3, base);
    put_u64_(size_ptr, bytes);
    return ST_SUCCESS;
}

/* NtUnmapViewOfSection(HANDLE Process, PVOID Base) */
static UINT64 sys_unmap_view(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    if (a1 != UINT64_C(0xFFFFFFFFFFFFFFFF)) return ST_NOT_SUPPORTED_;
    um_lock(&p->lock);
    UmRegion *r = um_region_find(p, a2);
    if (!r || !r->section) { um_unlock(&p->lock); return ST_NOT_MAPPED_VIEW; }
    UmObject *o = r->section;
    um_unmap_frames(p, r->base, r->size / PAGE_SIZE);
    um_region_remove(p, r);
    um_unlock(&p->lock);
    section_writeback(o->ptr);
    um_ob_unref(o);
    return ST_SUCCESS;
}

/* NtNovaFlushView(PVOID Base): a file-backed view's section goes back to its file */
static UINT64 sys_flush_view(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    um_flush_view_at(UmCurrent(), a1);
    return ST_SUCCESS;
}

void um_release_views(UmProcess *p)
{
    for (int i = 0; i < p->nregions; i++) {
        UmObject *o = p->regions[i].section;
        if (!o) continue;
        p->regions[i].section = NULL;
        um_ob_unref(o);
    }
}

void um_thread_syscalls_init(void)
{
    um_install(SYSCALL_NtCreateEvent,             sys_create_event);
    um_install(SYSCALL_NtCreateSection,           sys_create_section);
    um_install(SYSCALL_NtOpenSection,             sys_open_section);
    um_install(SYSCALL_NtMapViewOfSection,        sys_map_view);
    um_install(SYSCALL_NtUnmapViewOfSection,      sys_unmap_view);
    um_install(SYSCALL_NtNovaFlushView,           sys_flush_view);
    um_install(SYSCALL_NtOpenEvent,               sys_open_event);
    um_install(SYSCALL_NtOpenMutant,              sys_open_mutant);
    um_install(SYSCALL_NtOpenSemaphore,           sys_open_semaphore);
    um_install(SYSCALL_NtSetEvent,                sys_set_event);
    um_install(SYSCALL_NtResetEvent,              sys_reset_event);
    um_install(SYSCALL_NtClearEvent,              sys_clear_event);
    um_install(SYSCALL_NtCreateMutant,            sys_create_mutant);
    um_install(SYSCALL_NtReleaseMutant,           sys_release_mutant);
    um_install(SYSCALL_NtCreateSemaphore,         sys_create_semaphore);
    um_install(SYSCALL_NtReleaseSemaphore,        sys_release_semaphore);
    um_install(SYSCALL_NtWaitForSingleObject,     sys_wait_single);
    um_install(SYSCALL_NtWaitForMultipleObjects,  sys_wait_multiple);
    um_install(SYSCALL_NtCreateThreadEx,          sys_create_thread_ex);
    um_install(SYSCALL_NtTerminateThread,         sys_terminate_thread);
    um_install(SYSCALL_NtResumeThread,            sys_resume_thread);
    um_install(SYSCALL_NtSuspendThread,           sys_suspend_thread);
    um_install(SYSCALL_NtQueryInformationThread,  sys_query_info_thread);
    um_install(SYSCALL_NtSetInformationThread,    sys_set_info_thread);
    um_install(SYSCALL_NtQueryInformationProcess, sys_query_info_process);
    um_install(SYSCALL_NtDuplicateObject,         sys_duplicate_object);
    um_install(SYSCALL_NtNovaLoadDll,             sys_nova_load_dll);
    um_install(SYSCALL_NtNovaDebugPrint,          sys_nova_debug_print);
    um_install(SYSCALL_NtNovaGetRandom,           sys_nova_get_random);
    um_install(SYSCALL_NtNovaUnimplemented,       sys_nova_unimplemented);
}
