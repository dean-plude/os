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
#include "../ke/ksym.h"
#include "../ke/kpcr.h"   /* KVMDBG */
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
#define NS_MAX      1024
#define NS_NAME_MAX 260
static struct { char name[NS_NAME_MAX]; UmObject *o; } g_ns[NS_MAX];
static KSpinLock g_ns_lock = KSPINLOCK_INIT;

static void ns_remove_locked(UmObject *o)
{
    for (int i = 0; i < NS_MAX; i++) if (g_ns[i].o == o) { g_ns[i].o = NULL; g_ns[i].name[0] = 0; }
}

/* UTF-16 -> UTF-8 into @out (cap bytes, NUL-terminated); the length */
static int ns_utf8(const UINT16 *w, UINT32 n, char *out, int cap)
{
    int k = 0;
    for (UINT32 i = 0; i < n && k < cap - 4; i++) {
        UINT32 c = w[i];
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800) { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
        else { out[k++] = (char)(0xE0 | c >> 12); out[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[k] = 0;
    return k;
}

/* The name in OBJECT_ATTRIBUTES @oa_ptr (UTF-16 kept as UTF-8 bytes; empty
 * if unnamed), without the session prefixes.  A name relative to a
 * directory object (RootDirectory) is "directory\name".  False if
 * unreadable or the directory handle is bad. */
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
    if (oa[1]) {                                    /* relative to a directory object */
        UmObject *d = um_handle_object(UmCurrent(), oa[1], UO_DIRECTORY);
        if (!d) return false;
        int k = (int)strlen((const char *)d->ptr);
        if (k > NS_NAME_MAX - 8) k = NS_NAME_MAX - 8;
        memcpy(out, d->ptr, (size_t)k);
        um_ob_unref(d);
        out[k++] = '\\';
        ns_utf8(w, n, out + k, NS_NAME_MAX - k);
        return true;
    }
    ns_utf8(w, n, out, NS_NAME_MAX);
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

/* The namespace name of @o ("" if unnamed) */
void um_object_name(UmObject *o, char *buf, int cap)
{
    buf[0] = 0;
    if (!o || !o->named || cap < 2) return;
    IrqState s = spin_lock_irqsave(&g_ns_lock);
    for (int i = 0; i < NS_MAX; i++)
        if (g_ns[i].o == o) { strncpy(buf, g_ns[i].name, (size_t)cap - 1); buf[cap - 1] = 0; break; }
    spin_unlock_irqrestore(&g_ns_lock, s);
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
     * kernel lock; most destructors (a process's, a socket's...) want it */
    bool big = !o->free_unlocked;
    if (big) bkl_acquire();
    if (o->destroy) o->destroy(o);
    um_sd_free(o->sd);
    kfree(o);                                       /* UmThread: ob is its first member */
    if (big) bkl_release();
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
    case UO_TIMER:
        if (o->due && rdtsc() >= o->due) {                  /* fired (checked lazily) */
            o->signaled = true;
            if (o->period) {                                /* (on its own grid, so it doesn't drift) */
                UINT64 now = rdtsc();
                o->due += (now - o->due) / o->period * o->period + o->period;
            } else o->due = 0;
        }
        return o->signaled;
    default:           return o->signaled;
    }
}

/* Under g_um_oblock: take @o (it is ready).  True if it was abandoned. */
static bool ob_acquire(UmObject *o, UmThread *me)
{
    switch (o->type) {
    case UO_EVENT:     if (!o->manual) o->signaled = false; break;
    case UO_TIMER:     if (!o->manual) o->signaled = false; break;
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

/* The TSC at which a wait times out (UINT64_MAX: never) */
static UINT64 deadline_tsc(INT64 timeout_100ns)
{
    if (timeout_100ns < 0) return UINT64_MAX;
    return sched_tsc_after((UINT64)timeout_100ns);
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
            if (w->kt) {                                    /* (a timer's waiter runs at once: scheduler.h) */
                if (o->type == UO_TIMER) sched_unblock_timer(w->kt);
                else sched_unblock(w->kt);
            }
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
    UINT64 until = deadline_tsc(timeout_100ns);
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
        if (timeout_100ns == 0 || rdtsc() >= until) { ob_unlock(s); return ST_TIMEOUT; }
        me->wait_objs = o;
        me->wait_n = n;
        me->wake = 0;
        me->wait_next = g_waiters;
        g_waiters = me;
        ob_unlock(s);
        UINT64 nap = sched_tick_tsc(sched_ticks() + 10);
        for (int i = 0; i < n; i++)                         /* a timer wakes it when due */
            if (o[i]->type == UO_TIMER && o[i]->due && o[i]->due < nap) nap = o[i]->due;
        UINT64 target = until < nap ? until : nap, sl0 = rdtsc();   /* KVMDBG */
        bool tmr = false;
        for (int i = 0; i < n; i++) if (o[i]->type == UO_TIMER && o[i]->due == target) tmr = true;
        sched_sleep_until_tsc(&me->wake, target);
        {   /* KVMDBG: a timer's waiter running late */
            extern uint64_t g_tsc_per_tick;
            Thread *kt = sched_current();
            UINT64 now = rdtsc();
            UINT64 base = me->wake && kt->dbg_rdy > sl0 && kt->dbg_rdy < target ? kt->dbg_rdy : target;
            if (g_tsc_per_tick && now > base + g_tsc_per_tick / 5) {
                #define US(x) ((unsigned long)((x) * 10000 / g_tsc_per_tick))
                kprintf("[KVMDBG] tid %lu %s late %lu us (wake %u tmr %d): ready +%ld us how %x from cpu %u, ran +%ld us on cpu %u, sleep cpu %u, slept %lu us\n",
                        (unsigned long)kt->tid, kt->name, US(now - base), me->wake, tmr,
                        (long)(kt->dbg_rdy > base ? (long)US(kt->dbg_rdy - base) : -(long)US(base - kt->dbg_rdy)), kt->dbg_how, kt->dbg_from,
                        (long)(kt->dbg_run > kt->dbg_rdy ? (long)US(kt->dbg_run - kt->dbg_rdy) : -1), (unsigned)KiGetCurrentKpcr()->CpuNumber, kt->sleep_cpu, US(now - sl0));
                #undef US
            }
        }
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
    um_lock_excl(&p->lock);
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
    um_unlock_excl(&p->lock);
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
static UINT64 open_existing(const char *name, UmObType type, UINT64 handle_ptr, UINT32 access)
{
    if (!name[0]) return 0;
    UmObject *o = ns_lookup(name);
    if (!o) return 0;
    if (o->type != type) { um_ob_unref(o); return ST_OBJECT_TYPE_MISMATCH; }
    UINT32 st = um_check_object(o, access);                 /* its descriptor against our token */
    if (st) { um_ob_unref(o); return st; }
    UINT64 r = new_handle(UmCurrent(), o, handle_ptr);
    return r ? r : ST_OBJECT_NAME_EXISTS;
}

static UINT64 finish_create(UmObject *o, const char *name, UINT64 handle_ptr, UINT64 oa)
{
    UINT32 st = um_oa_security(oa, &o->sd);                 /* the descriptor it was created with */
    if (st) { um_ob_unref(o); return st; }
    if (name[0]) ns_add(name, o);
    return new_handle(UmCurrent(), o, handle_ptr);
}

/* NtOpenEvent / NtOpenMutant / NtOpenSemaphore(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES) */
/* The device objects that exist without being in the namespace table:
 * opening one as another type is a type mismatch, not "not found" (Cygwin
 * asks NtOpenSymbolicLinkObject whether \Device\Null exists). */
static bool builtin_device(const char *name)
{
    static const char *const devs[] = { "\\device\\null", "\\device\\namedpipe", "\\device\\condrv",
                                        "\\device\\afd", "\\device\\beep", "\\device\\mup" };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        const char *a = name, *b = devs[i];
        while (*a && *b && ((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a) == *b) a++, b++;
        if (!*a && !*b) return true;
    }
    return false;
}

static UINT64 open_named(UmObType type, UINT64 handle_ptr, UINT64 oa, UINT32 access)
{
    char name[NS_NAME_MAX];
    if (!ns_name(oa, name)) return ST_ACCESS_VIOLATION;
    if (!name[0]) return ST_INVALID_PARAMETER;
    UmObject *o = ns_lookup(name);
    if (!o) return builtin_device(name) ? ST_OBJECT_TYPE_MISMATCH : ST_OBJECT_NAME_NOT_FOUND;
    if (o->type != type) { um_ob_unref(o); return ST_OBJECT_TYPE_MISMATCH; }
    UINT32 st = um_check_object(o, access);
    if (st) { um_ob_unref(o); return st; }
    return new_handle(UmCurrent(), o, handle_ptr);
}
static UINT64 sys_open_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)     { (void)a4; return open_named(UO_EVENT, a1, a3, (UINT32)a2); }
static UINT64 sys_open_mutant(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)    { (void)a4; return open_named(UO_MUTANT, a1, a3, (UINT32)a2); }
static UINT64 sys_open_semaphore(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a4; return open_named(UO_SEMAPHORE, a1, a3, (UINT32)a2); }

static UINT64 sys_create_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2;
    char name[NS_NAME_MAX];
    if (!ns_name(a3, name)) return ST_ACCESS_VIOLATION;
    UINT64 r = open_existing(name, UO_EVENT, a1, (UINT32)a2);
    if (r) return r;
    UmObject *o = ob_new(UO_EVENT);
    if (!o) return ST_NO_MEMORY;
    o->manual = a4 == 0;                             /* NotificationEvent */
    o->signaled = um_stack_arg(5) & 0xFF;
    return finish_create(o, name, a1, a3);
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
    UINT64 r = open_existing(name, UO_MUTANT, a1, (UINT32)a2);
    if (r) return r;
    UmObject *o = ob_new(UO_MUTANT);
    if (!o) return ST_NO_MEMORY;
    if (a4 & 0xFF) { o->owner = UmCurrentThread(); o->recursion = 1; }
    return finish_create(o, name, a1, a3);
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
    UINT64 r = open_existing(name, UO_SEMAPHORE, a1, (UINT32)a2);
    if (r) return r;
    UmObject *o = ob_new(UO_SEMAPHORE);
    if (!o) return ST_NO_MEMORY;
    o->count = init;
    o->max = max;
    return finish_create(o, name, a1, a3);
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

/* NtAlertThreadByThreadId(HANDLE ThreadId): wake that thread of this
 * process from NtWaitForAlertByThreadId, or make its next one return at
 * once.  ntdll builds WaitOnAddress, SRW locks and condition variables on
 * this pair, as Windows 8 and later do. */
static UINT64 sys_alert_by_tid(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    UINT64 st = 0xC000000Bu;                                /* STATUS_INVALID_CID */
    um_lock_excl(&p->lock);
    for (int i = 0; i < UM_MAX_THREADS; i++) {
        UmThread *t = p->threads[i];
        if (!t || t->tid != (UINT32)a1 || t->exited) continue;
        IrqState s = ob_lock();
        t->alerted = 1;
        if (t->kt) sched_unblock(t->kt);
        ob_unlock(s);
        st = ST_SUCCESS;
        break;
    }
    um_unlock_excl(&p->lock);
    return st;
}

/* NtWaitForAlertByThreadId(PVOID Address (a hint only), PLARGE_INTEGER Timeout):
 * STATUS_ALERTED once alerted, else STATUS_TIMEOUT */
static UINT64 sys_wait_alert(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a3; (void)a4;
    INT64 t;
    if (!get_timeout(a2, &t)) return ST_ACCESS_VIOLATION;
    UmThread *me = UmCurrentThread();
    UINT64 until = deadline_tsc(t);
    for (;;) {
        if (__atomic_exchange_n(&me->alerted, 0, __ATOMIC_ACQ_REL)) return 0x101;   /* STATUS_ALERTED */
        if (um_stopping()) return ST_THREAD_IS_TERMINATING;
        if (t == 0 || rdtsc() >= until) return ST_TIMEOUT;
        UINT64 nap = sched_tick_tsc(sched_ticks() + 10);
        sched_sleep_until_tsc(&me->alerted, until < nap ? until : nap);
    }
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
    /* As on Windows, a thread's stack is at least the image's stack reserve
     * (programs built to need 1 or 2 MB overflow a smaller one) */
    if (stack < p->stack_reserve) stack = p->stack_reserve;
    if (stack < UM_THREAD_STACK) stack = UM_THREAD_STACK;
    UmThread *t = um_create_thread(p, start, arg, stack, true, &st);
    if (!t) { kprintf("[UM] %s: no new thread (%08x, stack %llu KB)\n", p->name, st, (unsigned long long)(stack >> 10)); return st; }
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
    UmObject *o = um_handle_object(UmCurrent(), a1, UO_THREAD);
    if (!o) return ST_INVALID_HANDLE;
    UINT32 st = ST_SUCCESS;
    if (a2 == 5) st = um_set_thread_token((UmThread *)o, a3, (UINT32)a4);   /* ThreadImpersonationToken */
    um_ob_unref(o);
    return st;
}

/* NtQueryInformationProcess(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG) */
static UINT64 query_info_process(UmProcess *p, UINT64 a2, UINT64 a3, UINT64 a4);
/* Mitigation policies, priorities and the like: accepted, none of them change anything here */
static UINT64 sys_set_info_process(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmObject *ob;
    UmProcess *p = um_proc_of(UmCurrent(), a1, &ob);
    if (!p) return ST_INVALID_HANDLE;
    if (ob) um_ob_unref(ob);
    return ST_SUCCESS;
}

static UINT64 sys_query_info_process(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *ob;
    UmProcess *p = um_proc_of(UmCurrent(), a1, &ob);          /* this process or another (a launcher's child) */
    if (!p) return ST_INVALID_HANDLE;
    UINT64 r = query_info_process(p, a2, a3, a4);
    if (ob) um_ob_unref(ob);
    return r;
}

static UINT64 query_info_process(UmProcess *p, UINT64 a2, UINT64 a3, UINT64 a4)
{
    if (a2 == 23) {                                            /* ProcessDeviceMap: the drive letters */
        if (a4 < 36) return ST_INFO_LENGTH_MISMATCH;
        UINT8 b[36];
        memset(b, 0, sizeof(b));
        UINT32 map = RamfsDriveMask();
        memcpy(b, &map, 4);
        for (int i = 0; i < 26; i++) if (map & (1u << i)) b[4 + i] = 3;   /* DRIVE_FIXED */
        if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, sizeof(b)))) return ST_ACCESS_VIOLATION;
        UINT64 ret = um_stack_arg(5);
        return !ret || put_u32(ret, 36) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
    }
    if (a2 == 20 || a2 == 58) {                                /* ProcessHandleCount, ProcessHandleTable */
        static UINT32 vals[UM_MAX_HANDLES];
        static UmLock tlock;
        um_lock(&tlock);
        UINT32 n = 0;
        um_lock_excl(&p->lock);
        for (UINT32 i = 0; i < UM_MAX_HANDLES; i++) if (p->handles[i].kind != H_FREE) vals[n++] = (i + 1) * 4;
        um_unlock_excl(&p->lock);
        UINT32 need = a2 == 20 ? 4 : n * 4;
        UINT64 st = ST_SUCCESS, ret = um_stack_arg(5);
        if (a4 < need) st = ST_INFO_LENGTH_MISMATCH;
        else if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, a2 == 20 ? &n : vals, need))) st = ST_ACCESS_VIOLATION;
        um_unlock(&tlock);
        if (ret && !put_u32(ret, need)) return ST_ACCESS_VIOLATION;
        return st;
    }
    if (a2 != 0) return ST_INVALID_INFO_CLASS;                 /* ProcessBasicInformation */
    if (a4 < 48) return ST_INFO_LENGTH_MISMATCH;
    UINT64 b[6] = { p->exited ? p->exit_status : ST_STILL_ACTIVE, p->lay.peb, 1, 8, p->pid, p->parent_pid };
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, sizeof(b)))) return ST_ACCESS_VIOLATION;
    return put_u32(um_stack_arg(5), 48) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* A process named by a handle (-1: the caller); referenced by @ob if not
 * the caller */
UmProcess *um_proc_of(UmProcess *self, UINT64 h, UmObject **ob)
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
    UmProcess *sp = um_proc_of(p, a1, &sob);
    if (!sp) return ST_INVALID_HANDLE;
    UmProcess *tp = a3 ? um_proc_of(p, a3, &tob) : NULL;
    if (a3 && !tp) { if (sob) um_ob_unref(sob); return ST_INVALID_HANDLE; }
    UINT64 nh = 0;
    UINT32 st = ST_SUCCESS;
    if (a2 == UINT64_C(0xFFFFFFFFFFFFFFFE) && sp == p) {     /* GetCurrentThread() */
        nh = tp ? um_handle_new_object(tp, &UmCurrentThread()->ob) : 0;
        if (tp && !nh) st = ST_TOO_MANY_HANDLES;
    } else if (a2 == UINT64_C(0xFFFFFFFFFFFFFFFF) && sp == p) {   /* GetCurrentProcess(): a real handle to it */
        UmObject *self = um_open_process(p->pid);
        if (!self) st = ST_INVALID_HANDLE;
        else if (tp) {
            nh = um_handle_new_object(tp, self);
            if (!nh) st = ST_TOO_MANY_HANDLES;
            else if (attrs & 2) {                            /* OBJ_INHERIT */
                um_lock_excl(&tp->lock);
                tp->handles[nh / 4 - 1].inherit = true;
                um_unlock_excl(&tp->lock);
            }
        }
        if (self) um_ob_unref(self);
    } else {
        DesktopLock();
        um_lock_excl(&sp->lock);
        UmHandle src;
        bool ok = a2 >= 4 && !(a2 & 3) && a2 / 4 - 1 < UM_MAX_HANDLES && sp->handles[a2 / 4 - 1].kind != H_FREE;
        if (ok) {
            src = sp->handles[a2 / 4 - 1];
            if (src.kind == H_FILE || src.kind == H_DIR) RamfsRef(src.node);
            if (src.kind == H_FILE) um_fpos_ref(src.fp);         /* a duplicate shares the position */
            if (src.kind == H_OBJECT) um_ob_ref(src.obj);
        }
        um_unlock_excl(&sp->lock);
        if (!ok) st = ST_INVALID_HANDLE;
        else if (tp) {
            if (!(options & 4)) src.inherit = attrs & 2;     /* DUPLICATE_SAME_ATTRIBUTES, OBJ_INHERIT */
            um_lock_excl(&tp->lock);
            int free = -1;
            for (int i = 0; i < UM_MAX_HANDLES; i++) if (tp->handles[i].kind == H_FREE) { free = i; break; }
            if (free >= 0) { tp->handles[free] = src; nh = (UINT64)(free + 1) * 4; }
            um_unlock_excl(&tp->lock);
            if (free < 0) st = ST_TOO_MANY_HANDLES;
        }
        if (ok && (!tp || st)) {                             /* not placed: drop the reference */
            if (src.kind == H_FILE || src.kind == H_DIR) RamfsUnref(src.node);
            if (src.kind == H_FILE) um_fpos_unref(src.fp);
            if (src.kind == H_OBJECT) um_ob_unref(src.obj);
        }
        DesktopUnlock();
    }
    if (!st && (options & 1)) {                              /* DUPLICATE_CLOSE_SOURCE */
        if (sp == p) um_close_handle(a2);
        else {
            DesktopLock();
            um_lock_excl(&sp->lock);
            UmHandle *h = a2 >= 4 && !(a2 & 3) && a2 / 4 - 1 < UM_MAX_HANDLES ? &sp->handles[a2 / 4 - 1] : NULL;
            UmHandle old = h ? *h : (UmHandle){ 0 };
            if (h) memset(h, 0, sizeof(*h));
            um_unlock_excl(&sp->lock);
            if (old.kind == H_FILE || old.kind == H_DIR) RamfsUnref(old.node);
            if (old.kind == H_FILE) um_fpos_unref(old.fp);
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
/* NtNovaLoadDll(PCSTR Name, ULONG Length, PVOID *Base, ULONG Flags): Flags are
 * LoadLibraryEx's (AS_DATAFILE / AS_IMAGE_RESOURCE map the module as data).
 * With NOVA_LDR_DIR_OP (0x80000000) it changes the search path instead:
 * Flags & 3 is um_dll_directory's operation, Name the folder and *Base the
 * cookie (AddDllDirectory gets it, RemoveDllDirectory passes it) */
static UINT64 sys_nova_load_dll(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    char name[RAMFS_PATH_MAX];
    if (a4 & 0x80000000u) {
        UINT64 cookie = 0;
        UINT32 op = (UINT32)a4 & 3;
        if (a2 >= sizeof(name) || op == 3 || (op == 0 && !a2)) return ST_INVALID_PARAMETER;
        if (a2 && !NT_SUCCESS(CopyFromUser(name, (const void *)(uintptr_t)a1, a2))) return ST_ACCESS_VIOLATION;
        name[a2] = '\0';
        if (op == 1 && !NT_SUCCESS(CopyFromUser(&cookie, (const void *)(uintptr_t)a3, 8))) return ST_ACCESS_VIOLATION;
        UINT32 st = um_dll_directory(UmCurrent(), op, name, &cookie);
        if (st || op != 0) return st;
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, &cookie, 8)) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
    }
    if (!a2 || a2 >= sizeof(name)) return ST_INVALID_PARAMETER;
    if (!NT_SUCCESS(CopyFromUser(name, (const void *)(uintptr_t)a1, a2))) return ST_ACCESS_VIOLATION;
    name[a2] = '\0';
    UINT64 base = 0;
    UINT32 st = um_load_dll(UmCurrent(), name, &base, (UINT32)a4);
    if (st) return st;
    UINT64 b = base;
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, &b, 8)) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* NtNovaBugCheck(ULONG Magic): crash the kernel on purpose ("crash
 * kernel"), to check that a kernel fault prints a readable backtrace.
 * Windows has the same idea in NotMyFault; here it takes 'NOVA' as the
 * argument so no stray call does it. */
static UINT64 sys_nova_bugcheck(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    if ((UINT32)a1 != 0x4E4F5641u) return 0xC000000Du;      /* STATUS_INVALID_PARAMETER */
    kprintf("[UM] %s (PID %u) asked for a kernel crash (NtNovaBugCheck)\n", UmCurrent()->name, UmCurrent()->pid);
    KeCrashTest();
    return 0;
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
    um_lock_excl(&p->lock);
    UmRegion *r = um_region_find(p, va);
    UmObject *o = r && r->section ? um_ob_ref(r->section) : NULL;
    um_unlock_excl(&p->lock);
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
    UINT64 r = open_existing(name, UO_SECTION, a1, (UINT32)a2);
    if (r) return r;
    UINT64 size = 0;
    if (a4 && !get_u64_(a4, &size)) return ST_INVALID_PARAMETER;
    RamNode *file = NULL;
    if (fileh) {                                            /* the file's size when none is given */
        DesktopLock();
        um_lock_excl(&p->lock);
        file = um_handle_file(p, fileh);
        if (file) { RamfsRef(file); if (!size) size = file->size; }
        um_unlock_excl(&p->lock);
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
    return finish_create(o, name, a1, a3);
}

/* NtQuerySection(HANDLE, SECTION_INFORMATION_CLASS, PVOID, SIZE_T, PSIZE_T): SectionBasicInformation
 * { PVOID BaseAddress; ULONG AllocationAttributes; LARGE_INTEGER MaximumSize } */
static UINT64 sys_query_section(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    if (a2 == 1) return 0xC0000049U;                        /* SectionImageInformation: SECTION_NOT_IMAGE */
    if (a2 != 0) return ST_INVALID_INFO_CLASS;
    if (a4 < 24) return ST_INFO_LENGTH_MISMATCH;
    UmObject *o = um_handle_object(UmCurrent(), a1, UO_SECTION);
    if (!o) return ST_INVALID_HANDLE;
    UINT64 b[3] = { 0, 0x8000000 /* SEC_COMMIT */, ((UmSection *)o->ptr)->size };
    if (((UmSection *)o->ptr)->file) b[1] |= 0x800000;     /* SEC_FILE */
    um_ob_unref(o);
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, 24))) return ST_ACCESS_VIOLATION;
    UINT64 ret = um_stack_arg(5);
    return !ret || put_u64_(ret, 24) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

static UINT64 sys_open_section(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a4; return open_named(UO_SECTION, a1, a3, (UINT32)a2); }

/* NtMapViewOfSection(HANDLE Section, HANDLE Process, PVOID *Base, ULONG_PTR ZeroBits, SIZE_T CommitSize,
 *                    PLARGE_INTEGER Offset, PSIZE_T ViewSize, InheritDisposition, AllocationType, Win32Protect) */
static UINT64 map_view(UINT64 a1, UINT64 a3, UINT64 off_ptr, UINT64 size_ptr, UINT32 prot, UINT64 lo, UINT64 hi, UINT64 align);

static UINT64 sys_map_view(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UmProcess *p = UmCurrent();
    if (a2 != UINT64_C(0xFFFFFFFFFFFFFFFF)) return ST_NOT_SUPPORTED_;
    return map_view(a1, a3, um_stack_arg(6), um_stack_arg(7), (UINT32)um_stack_arg(10), p->lay.alloc_min, p->lay.alloc_max, 0);
}

/* NtMapViewOfSectionEx(HANDLE Section, HANDLE Process, PVOID *Base, PLARGE_INTEGER Offset,
 *                      PSIZE_T ViewSize, ULONG AllocationType, ULONG Protect,
 *                      MEM_EXTENDED_PARAMETER *, ULONG Count): an address range too */
static UINT64 sys_map_view_ex(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (a2 != UINT64_C(0xFFFFFFFFFFFFFFFF)) return ST_NOT_SUPPORTED_;
    UINT64 lo = p->lay.alloc_min, hi = p->lay.alloc_max, align = 0;
    if (!um_addr_requirements(um_stack_arg(8), (UINT32)um_stack_arg(9), &lo, &hi, &align)) return ST_ACCESS_VIOLATION;
    return map_view(a1, a3, a4, um_stack_arg(5), (UINT32)um_stack_arg(7), lo, hi, align);
}

static UINT64 map_view(UINT64 a1, UINT64 a3, UINT64 off_ptr, UINT64 size_ptr, UINT32 prot, UINT64 lo, UINT64 hi, UINT64 align)
{
    UmProcess *p = UmCurrent();
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
    um_lock_excl(&p->lock);
    UINT64 r = ST_SUCCESS;
    if (base) {
        base &= ~0xFFFFULL;
        if (!um_is_free(p, base, bytes)) r = ST_CONFLICTING_ADDRESSES_;
    } else {
        base = um_find_free_aligned(p, bytes, lo, hi, align);
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
    um_unlock_excl(&p->lock);
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
    um_lock_excl(&p->lock);
    UmRegion *r = um_region_find(p, a2);
    if (!r || !r->section) { um_unlock_excl(&p->lock); return ST_NOT_MAPPED_VIEW; }
    UmObject *o = r->section;
    um_unmap_frames(p, r->base, r->size / PAGE_SIZE);
    um_region_remove(p, r);
    um_unlock_excl(&p->lock);
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

/* -----------------------------------------------------------------------
 * Object directories, symbolic links, timers, and querying events and
 * semaphores.  A directory is a named object whose children are the
 * names under it ("dir\child"); a program that creates objects relative
 * to it (RootDirectory) and lists it, as Cygwin does, sees them there.
 * ----------------------------------------------------------------------- */
#define ST_MORE_ENTRIES     0x00000105u
#define ST_NO_MORE_ENTRIES  0x8000001Au
#define ST_BUFFER_TOO_SMALL 0xC0000023u

typedef struct { UINT32 n; UINT16 w[]; } UmLinkTarget;    /* a symbolic link's target, UTF-16 */

static void ptr_destroy(UmObject *o) { kfree(o->ptr); o->ptr = NULL; }

/* NtCreateDirectoryObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES) */
static UINT64 sys_create_directory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a4;
    char name[NS_NAME_MAX];
    if (!ns_name(a3, name)) return ST_ACCESS_VIOLATION;
    UINT64 r = open_existing(name, UO_DIRECTORY, a1, (UINT32)a2);
    if (r) return r;
    UmObject *o = ob_new(UO_DIRECTORY);
    char *copy = kmalloc(strlen(name) + 1);
    if (!o || !copy) { kfree(o); kfree(copy); return ST_NO_MEMORY; }
    memcpy(copy, name, strlen(name) + 1);
    o->ptr = copy;
    o->destroy = ptr_destroy;
    o->free_unlocked = true;
    return finish_create(o, name, a1, a3);
}
static UINT64 sys_open_directory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a4; return open_named(UO_DIRECTORY, a1, a3, (UINT32)a2); }

static const char *ob_type_name(UmObType t)
{
    switch (t) {
    case UO_EVENT: return "Event";          case UO_MUTANT: return "Mutant";
    case UO_SEMAPHORE: return "Semaphore";  case UO_SECTION: return "Section";
    case UO_DIRECTORY: return "Directory";  case UO_SYMLINK: return "SymbolicLink";
    case UO_TIMER: return "Timer";          default: return "Unknown";
    }
}

/* Put ASCII/UTF-8 @str as UTF-16 at user @at; the byte count */
static UINT32 put_wide(UINT64 at, const char *str)
{
    UINT16 w[NS_NAME_MAX];
    UINT32 n = 0;
    for (const unsigned char *c = (const unsigned char *)str; *c && n < NS_NAME_MAX - 1; ) {
        UINT32 cp = *c++;
        if (cp >= 0xE0 && c[0] && c[1]) { cp = (cp & 0x0F) << 12 | (UINT32)(c[0] & 0x3F) << 6 | (c[1] & 0x3F); c += 2; }
        else if (cp >= 0xC0 && c[0]) { cp = (cp & 0x1F) << 6 | (c[0] & 0x3F); c++; }
        w[n++] = (UINT16)cp;
    }
    w[n] = 0;
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)at, w, 2 * (n + 1))) ? 2 * n : (UINT32)-1;
}

static UINT64 directory_entries(char (*names)[NS_NAME_MAX], const UmObType *types, int n, bool more,
                                UINT64 a2, UINT64 a3, UINT32 ctx, UINT64 ctx_ptr, UINT64 ret_ptr);

/* NtQueryDirectoryObject(HANDLE, PVOID Buffer, ULONG Length, BOOLEAN ReturnSingleEntry,
 *                        BOOLEAN RestartScan, PULONG Context, PULONG ReturnLength)
 * OBJECT_DIRECTORY_INFORMATION { UNICODE_STRING Name, TypeName } entries, a
 * zeroed one after the last, then the strings. */
static UINT64 sys_query_directory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *d = um_handle_object(UmCurrent(), a1, UO_DIRECTORY);
    if (!d) return ST_INVALID_HANDLE;
    bool single = a4 & 0xFF, restart = um_stack_arg(5) & 0xFF;
    UINT64 ctx_ptr = um_stack_arg(6), ret_ptr = um_stack_arg(7);
    UINT32 ctx = 0;
    if (!restart && ctx_ptr && !NT_SUCCESS(CopyFromUser(&ctx, (const void *)(uintptr_t)ctx_ptr, 4))) { um_ob_unref(d); return ST_ACCESS_VIOLATION; }
    char prefix[NS_NAME_MAX];
    strncpy(prefix, (const char *)d->ptr, NS_NAME_MAX - 2);
    prefix[NS_NAME_MAX - 2] = 0;
    um_ob_unref(d);
    size_t pl = strlen(prefix);
    prefix[pl++] = '\\';
    prefix[pl] = 0;
    /* the children, in table order, from entry @ctx on */
    char (*names)[NS_NAME_MAX] = kmalloc(64 * NS_NAME_MAX);
    if (!names) return ST_NO_MEMORY;
    UmObType types[64];
    int n = 0;
    UINT32 idx = 0;
    bool more = false;
    IrqState s = spin_lock_irqsave(&g_ns_lock);
    for (int i = 0; i < NS_MAX; i++) {
        if (!g_ns[i].o || strncmp(g_ns[i].name, prefix, pl) || strchr(g_ns[i].name + pl, '\\')) continue;
        if (idx++ < ctx) continue;
        if (n == 64 || (single && n == 1)) { more = true; break; }
        strncpy(names[n], g_ns[i].name + pl, NS_NAME_MAX - 1);
        names[n][NS_NAME_MAX - 1] = 0;
        types[n++] = g_ns[i].o->type;
    }
    spin_unlock_irqrestore(&g_ns_lock, s);
    UINT64 st = n ? directory_entries(names, types, n, more, a2, a3, ctx, ctx_ptr, ret_ptr) : ST_NO_MORE_ENTRIES;
    kfree(names);
    return st;
}

static UINT64 directory_entries(char (*names)[NS_NAME_MAX], const UmObType *types, int n, bool more,
                                UINT64 a2, UINT64 a3, UINT32 ctx, UINT64 ctx_ptr, UINT64 ret_ptr)
{
    /* how many fit */
    int fit = 0;
    UINT64 need = 16 * 2;                                /* the terminating entry */
    for (; fit < n; fit++) {
        UINT64 add = 32 + 2 * (strlen(names[fit]) + 1) + 2 * (strlen(ob_type_name(types[fit])) + 1);
        if (need + add > a3) break;
        need += add;
    }
    if (!fit) { put_u32(ret_ptr, (UINT32)(need + 32 + 2 * NS_NAME_MAX)); return ST_BUFFER_TOO_SMALL; }
    UINT64 str = a2 + 32 * (UINT64)(fit + 1);
    for (int i = 0; i < fit; i++) {
        UINT64 e[4];
        UINT32 l = put_wide(str, names[i]);
        if (l == (UINT32)-1) return ST_ACCESS_VIOLATION;
        e[0] = l | (UINT64)(l + 2) << 16; e[1] = str; str += l + 2;
        l = put_wide(str, ob_type_name(types[i]));
        if (l == (UINT32)-1) return ST_ACCESS_VIOLATION;
        e[2] = l | (UINT64)(l + 2) << 16; e[3] = str; str += l + 2;
        if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)(a2 + 32 * (UINT64)i), e, 32))) return ST_ACCESS_VIOLATION;
    }
    UINT64 z[4] = { 0, 0, 0, 0 };
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)(a2 + 32 * (UINT64)fit), z, 32))) return ST_ACCESS_VIOLATION;
    ctx += (UINT32)fit;
    if (ctx_ptr && !put_u32(ctx_ptr, ctx)) return ST_ACCESS_VIOLATION;
    put_u32(ret_ptr, (UINT32)(str - a2));
    return more || fit < n ? ST_MORE_ENTRIES : ST_SUCCESS;
}

/* NtCreateSymbolicLinkObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PUNICODE_STRING Target) */
static UINT64 sys_create_symlink(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2;
    char name[NS_NAME_MAX];
    if (!ns_name(a3, name)) return ST_ACCESS_VIOLATION;
    UINT64 us[2];
    if (!a4 || !NT_SUCCESS(CopyFromUser(us, (const void *)(uintptr_t)a4, 16))) return ST_ACCESS_VIOLATION;
    UINT32 n = (UINT32)(us[0] & 0xFFFF) / 2;
    if (n > 1024) return ST_INVALID_PARAMETER;
    UmLinkTarget *t = kmalloc(sizeof(*t) + 2 * n + 2);
    if (!t) return ST_NO_MEMORY;
    t->n = n;
    if (n && !NT_SUCCESS(CopyFromUser(t->w, (const void *)(uintptr_t)us[1], 2 * n))) { kfree(t); return ST_ACCESS_VIOLATION; }
    if (name[0]) {
        UmObject *old = ns_lookup(name);
        if (old) { um_ob_unref(old); kfree(t); return 0xC0000035u; }   /* OBJECT_NAME_COLLISION */
    }
    UmObject *o = ob_new(UO_SYMLINK);
    if (!o) { kfree(t); return ST_NO_MEMORY; }
    o->ptr = t;
    o->destroy = ptr_destroy;
    o->free_unlocked = true;
    return finish_create(o, name, a1, a3);
}
static UINT64 sys_open_symlink(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a4; return open_named(UO_SYMLINK, a1, a3, (UINT32)a2); }

/* NtQuerySymbolicLinkObject(HANDLE, PUNICODE_STRING Target (in: MaximumLength, Buffer), PULONG ReturnedLength) */
static UINT64 sys_query_symlink(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UmObject *o = um_handle_object(UmCurrent(), a1, UO_SYMLINK);
    if (!o) return ST_INVALID_HANDLE;
    UmLinkTarget *t = o->ptr;
    UINT64 us[2];
    UINT32 st = ST_SUCCESS;
    if (!NT_SUCCESS(CopyFromUser(us, (const void *)(uintptr_t)a2, 16))) st = ST_ACCESS_VIOLATION;
    else {
        UINT32 bytes = 2 * t->n, cap = (UINT32)(us[0] >> 16) & 0xFFFF;
        put_u32(a3, bytes);
        if (cap < bytes) st = ST_BUFFER_TOO_SMALL;
        else if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)us[1], t->w, bytes))) st = ST_ACCESS_VIOLATION;
        else {
            UINT16 len = (UINT16)bytes;
            if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, &len, 2))) st = ST_ACCESS_VIOLATION;
        }
    }
    um_ob_unref(o);
    return st;
}

/* NtCreateTimer(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, TIMER_TYPE) */
static UINT64 sys_create_timer(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2;
    char name[NS_NAME_MAX];
    if (!ns_name(a3, name)) return ST_ACCESS_VIOLATION;
    UINT64 r = open_existing(name, UO_TIMER, a1, (UINT32)a2);
    if (r) return r;
    UmObject *o = ob_new(UO_TIMER);
    if (!o) return ST_NO_MEMORY;
    o->manual = a4 == 0;                                    /* NotificationTimer */
    return finish_create(o, name, a1, a3);
}
static UINT64 sys_open_timer(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a4; return open_named(UO_TIMER, a1, a3, (UINT32)a2); }

/* Timers keep TSC deadlines, so a wait on one ends when it is due (the
 * one-shot or TSC-deadline APIC timer of Phase 18.7), not at the next
 * 10 ms tick.  TSC cycles in @t100ns 100 ns units, and back. */
static UINT64 tsc_per_tick(void) { return sched_tick_tsc(1) - sched_tick_tsc(0); }
static UINT64 tsc_of_100ns(UINT64 t100ns)
{
    UINT64 k = tsc_per_tick();
    if (t100ns > UINT64_C(1000000000000000)) t100ns = UINT64_C(1000000000000000);   /* (about 3 years) */
    return t100ns / 100000 * k + t100ns % 100000 * k / 100000;
}
static UINT64 tsc_to_100ns(UINT64 tsc)
{
    UINT64 k = tsc_per_tick();
    return k ? tsc / k * 100000 + tsc % k * 100000 / k : 0;
}

/* NtSetTimer(HANDLE, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE, PVOID, BOOLEAN Resume,
 *            LONG Period (ms), PBOOLEAN PreviousState).  (No APC routine: kernel32
 *            runs SetWaitableTimer's completion routines itself.) */
static UINT64 sys_set_timer(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    INT64 due;
    if (!a2 || !NT_SUCCESS(CopyFromUser(&due, (const void *)(uintptr_t)a2, 8))) return ST_ACCESS_VIOLATION;
    INT32 period = (INT32)um_stack_arg(6);
    UmObject *o = um_handle_object(UmCurrent(), a1, UO_TIMER);
    if (!o) return ST_INVALID_HANDLE;
    UINT64 rel;                                             /* 100 ns from now */
    if (due < 0) rel = (UINT64)-due;
    else { UINT64 now = um_now_100ns(); rel = (UINT64)due > now ? (UINT64)due - now : 0; }
    IrqState s = ob_lock();
    UINT8 prev = o->signaled;
    o->signaled = false;
    o->due = rdtsc() + tsc_of_100ns(rel);
    if (!o->due) o->due = 1;
    o->period = period > 0 ? tsc_of_100ns((UINT64)period * 10000) : 0;
    if (period > 0 && !o->period) o->period = 1;
    um_ob_wake(o);                                          /* (its waiters look again: due now, or when to wake) */
    ob_unlock(s);
    um_ob_unref(o);
    UINT64 pp = um_stack_arg(7);
    if (pp && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)pp, &prev, 1))) return ST_ACCESS_VIOLATION;
    return ST_SUCCESS;
}

/* NtCancelTimer(HANDLE, PBOOLEAN CurrentState) */
static UINT64 sys_cancel_timer(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmObject *o = um_handle_object(UmCurrent(), a1, UO_TIMER);
    if (!o) return ST_INVALID_HANDLE;
    IrqState s = ob_lock();
    ob_ready(o, NULL);
    UINT8 cur = o->signaled;
    o->due = 0;
    o->period = 0;
    ob_unlock(s);
    um_ob_unref(o);
    if (a2 && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, &cur, 1))) return ST_ACCESS_VIOLATION;
    return ST_SUCCESS;
}

/* NtQueryTimer / NtQueryEvent / NtQuerySemaphore(HANDLE, class 0, PVOID, ULONG, PULONG) */
static UINT64 query_object_state(UmObType type, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    if (a2 != 0) return ST_INVALID_INFO_CLASS;
    UINT32 size = type == UO_TIMER ? 16 : 8;
    if (a4 < size) return ST_INFO_LENGTH_MISMATCH;
    UmObject *o = um_handle_object(UmCurrent(), a1, type);
    if (!o) return ST_INVALID_HANDLE;
    UINT32 b[4] = { 0, 0, 0, 0 };
    IrqState s = ob_lock();
    if (type == UO_TIMER) {
        ob_ready(o, NULL);
        UINT64 now = rdtsc(), left = o->due > now ? tsc_to_100ns(o->due - now) : 0;
        b[0] = (UINT32)left; b[1] = (UINT32)(left >> 32);
        b[2] = o->signaled;
    } else if (type == UO_EVENT) {
        b[0] = o->manual ? 0 : 1;                           /* NotificationEvent / SynchronizationEvent */
        b[1] = o->signaled;
    } else {
        b[0] = (UINT32)o->count;
        b[1] = (UINT32)o->max;
    }
    ob_unlock(s);
    um_ob_unref(o);
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, size))) return ST_ACCESS_VIOLATION;
    return put_u32(um_stack_arg(5), size) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}
static UINT64 sys_query_timer(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)     { return query_object_state(UO_TIMER, a1, a2, a3, a4); }
static UINT64 sys_query_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)     { return query_object_state(UO_EVENT, a1, a2, a3, a4); }
static UINT64 sys_query_semaphore(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { return query_object_state(UO_SEMAPHORE, a1, a2, a3, a4); }

/* OBJECT_ATTRIBUTES.Attributes OBJ_INHERIT: the new handle at @handle_ptr
 * goes to child processes created with handle inheritance */
static UINT64 oa_inherit(UINT64 st, UINT64 handle_ptr, UINT64 oa_ptr)
{
    UINT64 oa[4], h;
    if ((st & 0xC0000000u) == 0xC0000000u || !oa_ptr || !handle_ptr) return st;
    if (!NT_SUCCESS(CopyFromUser(oa, (const void *)(uintptr_t)oa_ptr, sizeof(oa))) || !(oa[3] & 2)) return st;
    if (!NT_SUCCESS(CopyFromUser(&h, (const void *)(uintptr_t)handle_ptr, 8))) return st;
    UmProcess *p = UmCurrent();
    um_lock_excl(&p->lock);
    if (h && h % 4 == 0 && h / 4 - 1 < UM_MAX_HANDLES && p->handles[h / 4 - 1].kind != H_FREE)
        p->handles[h / 4 - 1].inherit = true;
    um_unlock_excl(&p->lock);
    return st;
}
#define INHERITABLE(fn) \
    static UINT64 fn##_oa(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { return oa_inherit(fn(a1, a2, a3, a4), a1, a3); }
INHERITABLE(sys_create_directory) INHERITABLE(sys_open_directory)
INHERITABLE(sys_create_symlink)   INHERITABLE(sys_open_symlink)
INHERITABLE(sys_create_timer)     INHERITABLE(sys_open_timer)
INHERITABLE(sys_create_event)     INHERITABLE(sys_open_event)
INHERITABLE(sys_create_mutant)    INHERITABLE(sys_open_mutant)
INHERITABLE(sys_create_semaphore) INHERITABLE(sys_open_semaphore)
INHERITABLE(sys_create_section)   INHERITABLE(sys_open_section)

void um_thread_syscalls_init(void)
{
    um_install(SYSCALL_NtCreateDirectoryObject,   sys_create_directory_oa);
    um_install(SYSCALL_NtOpenDirectoryObject,     sys_open_directory_oa);
    um_install(SYSCALL_NtQueryDirectoryObject,    sys_query_directory);
    um_install(SYSCALL_NtCreateSymbolicLinkObject, sys_create_symlink_oa);
    um_install(SYSCALL_NtOpenSymbolicLinkObject,  sys_open_symlink_oa);
    um_install(SYSCALL_NtQuerySymbolicLinkObject, sys_query_symlink);
    um_install(SYSCALL_NtCreateTimer,             sys_create_timer_oa);
    um_install(SYSCALL_NtOpenTimer,               sys_open_timer_oa);
    um_install(SYSCALL_NtSetTimer,                sys_set_timer);
    um_install(SYSCALL_NtCancelTimer,             sys_cancel_timer);
    um_install(SYSCALL_NtQueryTimer,              sys_query_timer);
    um_install(SYSCALL_NtQueryEvent,              sys_query_event);
    um_install(SYSCALL_NtQuerySemaphore,          sys_query_semaphore);
    um_install(SYSCALL_NtQuerySection,            sys_query_section);
    um_install(SYSCALL_NtCreateEvent,             sys_create_event_oa);
    um_install(SYSCALL_NtCreateSection,           sys_create_section_oa);
    um_install(SYSCALL_NtOpenSection,             sys_open_section_oa);
    um_install(SYSCALL_NtMapViewOfSection,        sys_map_view);
    um_install(SYSCALL_NtMapViewOfSectionEx,      sys_map_view_ex);
    um_install(SYSCALL_NtUnmapViewOfSection,      sys_unmap_view);
    um_install(SYSCALL_NtNovaFlushView,           sys_flush_view);
    um_install(SYSCALL_NtOpenEvent,               sys_open_event_oa);
    um_install(SYSCALL_NtOpenMutant,              sys_open_mutant_oa);
    um_install(SYSCALL_NtOpenSemaphore,           sys_open_semaphore_oa);
    um_install(SYSCALL_NtSetEvent,                sys_set_event);
    um_install(SYSCALL_NtResetEvent,              sys_reset_event);
    um_install(SYSCALL_NtClearEvent,              sys_clear_event);
    um_install(SYSCALL_NtCreateMutant,            sys_create_mutant_oa);
    um_install(SYSCALL_NtReleaseMutant,           sys_release_mutant);
    um_install(SYSCALL_NtCreateSemaphore,         sys_create_semaphore_oa);
    um_install(SYSCALL_NtReleaseSemaphore,        sys_release_semaphore);
    um_install(SYSCALL_NtWaitForSingleObject,     sys_wait_single);
    um_install(SYSCALL_NtAlertThreadByThreadId,   sys_alert_by_tid);
    um_install(SYSCALL_NtWaitForAlertByThreadId,  sys_wait_alert);
    um_install(SYSCALL_NtWaitForMultipleObjects,  sys_wait_multiple);
    um_install(SYSCALL_NtCreateThreadEx,          sys_create_thread_ex);
    um_install(SYSCALL_NtTerminateThread,         sys_terminate_thread);
    um_install(SYSCALL_NtResumeThread,            sys_resume_thread);
    um_install(SYSCALL_NtSuspendThread,           sys_suspend_thread);
    um_install(SYSCALL_NtQueryInformationThread,  sys_query_info_thread);
    um_install(SYSCALL_NtSetInformationThread,    sys_set_info_thread);
    um_install(SYSCALL_NtQueryInformationProcess, sys_query_info_process);
    um_install(SYSCALL_NtSetInformationProcess, sys_set_info_process);
    um_install(SYSCALL_NtDuplicateObject,         sys_duplicate_object);
    um_install(SYSCALL_NtNovaLoadDll,             sys_nova_load_dll);
    um_install(SYSCALL_NtNovaDebugPrint,          sys_nova_debug_print);
    um_install(SYSCALL_NtNovaBugCheck,            sys_nova_bugcheck);
    um_install(SYSCALL_NtNovaGetRandom,           sys_nova_get_random);
    um_install(SYSCALL_NtNovaUnimplemented,       sys_nova_unimplemented);
}
