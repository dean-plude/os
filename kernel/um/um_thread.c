/*
 * um_thread.c — threads, synchronization objects and waits for programs
 *
 * Objects: events (manual/auto reset), mutants (recursive, abandoned when
 * the owner ends), semaphores and threads (signaled once ended).  Object
 * state changes happen with interrupts off (one CPU), so a check and the
 * acquisition that follows it are atomic.  Waiting threads yield until an
 * object is signaled, the timeout passes or the thread is being ended.
 */

#include "um_internal.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../arch/x86_64/cpu.h"

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
UmObject *um_ob_ref(UmObject *o)
{
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    return o;
}

void um_ob_unref(UmObject *o)
{
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (o->destroy) o->destroy(o);
    kfree(o);                                       /* UmThread: ob is its first member */
}

static UmObject *ob_new(UmObType type)
{
    UmObject *o = kzalloc(sizeof(*o));
    if (o) { o->type = type; o->refs = 1; }
    return o;
}

/* With interrupts off: can @o be acquired by @me right now? */
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

/* With interrupts off: take @o (it is ready).  True if it was abandoned. */
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

/* Wait on @n objects: any one (index returned) or all of them. */
static UINT32 wait_objects(UmObject **o, int n, bool all, INT64 timeout_100ns)
{
    UmThread *me = UmCurrentThread();
    UINT64 until = deadline_ticks(timeout_100ns);
    for (;;) {
        IrqState s = irq_save();
        if (all) {
            bool ready = true;
            for (int i = 0; i < n && ready; i++) ready = ob_ready(o[i], me);
            if (ready) {
                bool ab = false;
                for (int i = 0; i < n; i++) ab |= ob_acquire(o[i], me);
                irq_restore(s);
                return ab ? ST_ABANDONED : ST_SUCCESS;
            }
        } else {
            for (int i = 0; i < n; i++) {
                if (ob_ready(o[i], me)) {
                    bool ab = ob_acquire(o[i], me);
                    irq_restore(s);
                    return (ab ? ST_ABANDONED : ST_SUCCESS) + (UINT32)i;
                }
            }
        }
        irq_restore(s);
        if (um_stopping()) return ST_THREAD_IS_TERMINATING;
        if (timeout_100ns == 0 || sched_ticks() >= until) return ST_TIMEOUT;
        sched_yield();
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
        IrqState s = irq_save();
        h->obj->owner = NULL;
        h->obj->recursion = 0;
        h->obj->abandoned = true;
        irq_restore(s);
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
static UINT64 sys_create_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3;
    UmObject *o = ob_new(UO_EVENT);
    if (!o) return ST_NO_MEMORY;
    o->manual = a4 == 0;                             /* NotificationEvent */
    o->signaled = um_stack_arg(5) & 0xFF;
    return new_handle(UmCurrent(), o, a1);
}

static UINT64 event_op(UINT64 h, UINT64 prev_ptr, int op)
{
    UmObject *o = um_handle_object(UmCurrent(), h, UO_EVENT);
    if (!o) return ST_INVALID_HANDLE;
    IrqState s = irq_save();
    UINT32 prev = o->signaled;
    o->signaled = op == 1;
    irq_restore(s);
    um_ob_unref(o);
    return put_u32(prev_ptr, prev) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

static UINT64 sys_set_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)   { (void)a3; (void)a4; return event_op(a1, a2, 1); }
static UINT64 sys_reset_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a3; (void)a4; return event_op(a1, a2, 0); }
static UINT64 sys_clear_event(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a2; (void)a3; (void)a4; return event_op(a1, 0, 0); }

/* NtCreateMutant(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN InitialOwner) */
static UINT64 sys_create_mutant(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3;
    UmObject *o = ob_new(UO_MUTANT);
    if (!o) return ST_NO_MEMORY;
    if (a4 & 0xFF) { o->owner = UmCurrentThread(); o->recursion = 1; }
    return new_handle(UmCurrent(), o, a1);
}

/* NtReleaseMutant(HANDLE, PLONG PreviousCount) */
static UINT64 sys_release_mutant(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmObject *o = um_handle_object(UmCurrent(), a1, UO_MUTANT);
    if (!o) return ST_INVALID_HANDLE;
    UINT32 st = ST_SUCCESS, prev = 0;
    IrqState s = irq_save();
    if (o->owner != UmCurrentThread()) st = ST_MUTANT_NOT_OWNED;
    else {
        prev = o->recursion;
        if (--o->recursion == 0) o->owner = NULL;
    }
    irq_restore(s);
    um_ob_unref(o);
    if (st) return st;
    return put_u32(a2, (UINT32)(1 - (INT32)prev)) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* NtCreateSemaphore(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, LONG Initial, LONG Maximum) */
static UINT64 sys_create_semaphore(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3;
    INT32 init = (INT32)a4, max = (INT32)um_stack_arg(5);
    if (max <= 0 || init < 0 || init > max) return ST_INVALID_PARAMETER;
    UmObject *o = ob_new(UO_SEMAPHORE);
    if (!o) return ST_NO_MEMORY;
    o->count = init;
    o->max = max;
    return new_handle(UmCurrent(), o, a1);
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
    IrqState s = irq_save();
    prev = o->count;
    if (o->count > o->max - n) st = ST_SEMAPHORE_LIMIT;
    else o->count += n;
    irq_restore(s);
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
    IrqState s = irq_save();
    INT32 prev = t->suspend;
    if (delta > 0 && prev < 127) t->suspend = prev + 1;
    if (delta < 0 && prev > 0) t->suspend = prev - 1;
    irq_restore(s);
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
    UINT64 b[6] = { ST_STILL_ACTIVE, UM_PEB_VA, 1, 8, p->pid, 0 };
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, sizeof(b)))) return ST_ACCESS_VIOLATION;
    return put_u32(um_stack_arg(5), 48) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* NtDuplicateObject(HANDLE SourceProcess, HANDLE Source, HANDLE TargetProcess,
 *                   PHANDLE Target, ACCESS_MASK, ULONG Attributes, ULONG Options) */
static UINT64 sys_duplicate_object(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT64 self = UINT64_C(0xFFFFFFFFFFFFFFFF);
    if (a1 != self || (a3 != self && a3)) return ST_INVALID_HANDLE;
    UINT32 options = (UINT32)um_stack_arg(7);
    UINT64 nh = 0;
    UINT32 st = ST_SUCCESS;
    if (a2 == UINT64_C(0xFFFFFFFFFFFFFFFE)) {                 /* GetCurrentThread() */
        nh = um_handle_new_object(p, &UmCurrentThread()->ob);
        if (!nh) return ST_TOO_MANY_HANDLES;
    } else {
        DesktopLock();
        um_lock(&p->lock);
        UmHandle *src = NULL;
        if (a2 >= 4 && !(a2 & 3) && a2 / 4 - 1 < UM_MAX_HANDLES && p->handles[a2 / 4 - 1].kind != H_FREE)
            src = &p->handles[a2 / 4 - 1];
        if (!src) st = ST_INVALID_HANDLE;
        else {
            int free = -1;
            for (int i = 0; i < UM_MAX_HANDLES; i++) if (p->handles[i].kind == H_FREE) { free = i; break; }
            if (free < 0) st = ST_TOO_MANY_HANDLES;
            else {
                UmHandle *d = &p->handles[free];
                *d = *src;
                if (d->kind == H_FILE || d->kind == H_DIR) RamfsRef(d->node);
                if (d->kind == H_OBJECT) um_ob_ref(d->obj);
                nh = (UINT64)(free + 1) * 4;
            }
        }
        um_unlock(&p->lock);
        DesktopUnlock();
        if (st) return st;
    }
    if (options & 1) {                                         /* DUPLICATE_CLOSE_SOURCE */
        um_close_handle(a2);
    }
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

void um_thread_syscalls_init(void)
{
    um_install(SYSCALL_NtCreateEvent,             sys_create_event);
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
}
