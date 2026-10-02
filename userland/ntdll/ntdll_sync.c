#define NOVA_BUILD_NTDLL
/*
 * ntdll_sync.c — critical sections, SRW locks, condition variables
 *
 * Critical sections use the classic NT algorithm: an interlocked LockCount
 * for the fast, uncontended path and a lazily created auto-reset event to
 * park a contending thread.  SRW locks are a compact reader/writer word.
 * Condition variables use a signal counter (spurious wakeups are allowed
 * by the contract, so callers re-check their predicate).
 */

#include <winternl.h>
#include <winnt.h>

static ULONG cur_tid(void) { return *(ULONG *)(NtCurrentTebBytes() + TEB_CLIENT_ID + sizeof(HANDLE)); }
static void  yield(void)   { NtYieldExecution(); }
/* Waiting on another thread: yield the first few rounds, then sleep a tick
 * per round (on several CPUs, spinning on yields keeps them all busy) */
static void  backoff(ULONG *round)
{
    if ((*round)++ < 4) { yield(); return; }
    LARGE_INTEGER iv; iv.QuadPart = -10000;     /* 1 ms: the next 10 ms tick */
    NtDelayExecution(FALSE, &iv);
}
/* Milliseconds since boot (KUSER_SHARED_DATA.TickCount, 10 ms ticks) */
static ULONGLONG now_ms(void) { return (ULONGLONG)*(volatile ULONG *)(ULONG_PTR)0x7FFE0320 * 10; }

/* -----------------------------------------------------------------------
 * Critical sections
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI RtlInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION cs, ULONG spin)
{
    cs->DebugInfo = 0;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    cs->LockSemaphore = 0;
    cs->SpinCount = spin;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs)
{
    return RtlInitializeCriticalSectionAndSpinCount(cs, 0);
}

static HANDLE cs_event(PRTL_CRITICAL_SECTION cs)
{
    HANDLE e = cs->LockSemaphore;
    if (e) return e;
    HANDLE ne = 0;
    if (!NT_SUCCESS(NtCreateEvent(&ne, 0, 0, SynchronizationEvent, FALSE))) return 0;
    HANDLE old = InterlockedCompareExchangePointer(&cs->LockSemaphore, ne, 0);
    if (old) { NtClose(ne); return old; }
    return ne;
}

NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs)
{
    ULONG tid = cur_tid();
    if ((ULONG)(ULONG_PTR)cs->OwningThread == tid) { cs->RecursionCount++; return STATUS_SUCCESS; }
    if (InterlockedIncrement(&cs->LockCount) == 0) {
        cs->OwningThread = (HANDLE)(ULONG_PTR)tid;
        cs->RecursionCount = 1;
        return STATUS_SUCCESS;
    }
    /* Contended: spin briefly, then park on the event. */
    for (ULONG_PTR i = 0; i < cs->SpinCount; i++) {
        if (cs->OwningThread == 0 && InterlockedCompareExchange(&cs->LockCount, 0, -1) < 0) {
            /* raced to free — unlikely with our increment; fall through to wait */
        }
        YieldProcessor();
    }
    NtWaitForSingleObject(cs_event(cs), FALSE, 0);
    cs->OwningThread = (HANDLE)(ULONG_PTR)tid;        /* we were handed ownership */
    cs->RecursionCount = 1;
    return STATUS_SUCCESS;
}

BOOLEAN NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs)
{
    ULONG tid = cur_tid();
    if ((ULONG)(ULONG_PTR)cs->OwningThread == tid) { cs->RecursionCount++; return TRUE; }
    if (InterlockedCompareExchange(&cs->LockCount, 0, -1) == -1) {
        cs->OwningThread = (HANDLE)(ULONG_PTR)tid;
        cs->RecursionCount = 1;
        return TRUE;
    }
    return FALSE;
}

NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs)
{
    if (--cs->RecursionCount > 0) return STATUS_SUCCESS;
    cs->OwningThread = 0;
    if (InterlockedDecrement(&cs->LockCount) >= 0) {         /* waiters remain */
        HANDLE e = cs_event(cs);
        if (e) NtSetEvent(e, 0);
    }
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION cs)
{
    if (cs->LockSemaphore) NtClose(cs->LockSemaphore);
    cs->LockSemaphore = 0;
    cs->LockCount = -1;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Slim reader/writer locks (bit 0: writer held; bits 1..: reader count)
 * ----------------------------------------------------------------------- */
/* pointer-sized (8 bytes, or 4 in 32-bit programs) */
static volatile LONG_PTR *srw(PRTL_SRWLOCK l) { return (volatile LONG_PTR *)&l->Ptr; }
static LONG_PTR cas(volatile LONG_PTR *p, LONG_PTR v, LONG_PTR cmp) { return __sync_val_compare_and_swap(p, cmp, v); }

VOID NTAPI RtlInitializeSRWLock(PRTL_SRWLOCK l) { l->Ptr = 0; }

VOID NTAPI RtlAcquireSRWLockExclusive(PRTL_SRWLOCK l)
{
    ULONG round = 0;
    while (cas(srw(l), 1, 0) != 0) backoff(&round);
}

BOOLEAN NTAPI RtlTryAcquireSRWLockExclusive(PRTL_SRWLOCK l)
{
    return cas(srw(l), 1, 0) == 0;
}

VOID NTAPI RtlReleaseSRWLockExclusive(PRTL_SRWLOCK l)
{
    __atomic_store_n(srw(l), 0, __ATOMIC_RELEASE);
}

VOID NTAPI RtlAcquireSRWLockShared(PRTL_SRWLOCK l)
{
    for (ULONG round = 0;;) {
        LONG_PTR v = *srw(l);
        if (!(v & 1) && cas(srw(l), v + 2, v) == v) return;
        backoff(&round);
    }
}

BOOLEAN NTAPI RtlTryAcquireSRWLockShared(PRTL_SRWLOCK l)
{
    LONG_PTR v = *srw(l);
    return !(v & 1) && cas(srw(l), v + 2, v) == v;
}

VOID NTAPI RtlReleaseSRWLockShared(PRTL_SRWLOCK l)
{
    __sync_fetch_and_sub(srw(l), 2);
}

/* -----------------------------------------------------------------------
 * Condition variables (signal counter; spurious wakeups permitted)
 * ----------------------------------------------------------------------- */
VOID NTAPI RtlInitializeConditionVariable(PRTL_CONDITION_VARIABLE cv) { cv->Ptr = 0; }
VOID NTAPI RtlWakeConditionVariable(PRTL_CONDITION_VARIABLE cv)    { __sync_fetch_and_add((volatile LONG_PTR *)&cv->Ptr, 1); }
VOID NTAPI RtlWakeAllConditionVariable(PRTL_CONDITION_VARIABLE cv) { __sync_fetch_and_add((volatile LONG_PTR *)&cv->Ptr, 1); }

static NTSTATUS cv_sleep(PRTL_CONDITION_VARIABLE cv, PLARGE_INTEGER timeout,
                         void (*unlock)(void *), void (*lock)(void *), void *obj)
{
    LONG_PTR seen = *(volatile LONG_PTR *)&cv->Ptr;
    /* Timeout: relative (negative, 100 ns units) is the common case; an
     * absolute one is treated as already due */
    ULONGLONG start = now_ms(), limit = 0;
    if (timeout) limit = timeout->QuadPart < 0 ? (ULONGLONG)(-timeout->QuadPart) / 10000 : 0;
    unlock(obj);
    NTSTATUS s = STATUS_SUCCESS;
    ULONG round = 0;
    while (*(volatile LONG_PTR *)&cv->Ptr == seen) {
        if (timeout && now_ms() - start >= limit) { s = STATUS_TIMEOUT; break; }
        backoff(&round);
    }
    lock(obj);
    return s;
}

static void cs_unlock(void *o) { RtlLeaveCriticalSection(o); }
static void cs_lock(void *o)   { RtlEnterCriticalSection(o); }
static void srw_ex_unlock(void *o) { RtlReleaseSRWLockExclusive(o); }
static void srw_ex_lock(void *o)   { RtlAcquireSRWLockExclusive(o); }
static void srw_sh_unlock(void *o) { RtlReleaseSRWLockShared(o); }
static void srw_sh_lock(void *o)   { RtlAcquireSRWLockShared(o); }

NTSTATUS NTAPI RtlSleepConditionVariableCS(PRTL_CONDITION_VARIABLE cv, PRTL_CRITICAL_SECTION cs, PLARGE_INTEGER t)
{
    return cv_sleep(cv, t, cs_unlock, cs_lock, cs);
}

NTSTATUS NTAPI RtlSleepConditionVariableSRW(PRTL_CONDITION_VARIABLE cv, PRTL_SRWLOCK l, PLARGE_INTEGER t, ULONG flags)
{
    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED)
        return cv_sleep(cv, t, srw_sh_unlock, srw_sh_lock, l);
    return cv_sleep(cv, t, srw_ex_unlock, srw_ex_lock, l);
}

/* -----------------------------------------------------------------------
 * One-time initialization (synchronous)
 * ----------------------------------------------------------------------- */
VOID NTAPI RtlRunOnceInitialize(PRTL_RUN_ONCE once) { once->Ptr = 0; }

#define RUNONCE_RUNNING ((PVOID)1)
#define RUNONCE_DONE    ((PVOID)2)

NTSTATUS NTAPI RtlRunOnceBeginInitialize(PRTL_RUN_ONCE once, ULONG flags, PVOID *ctx)
{
    (void)flags;
    for (ULONG round = 0;;) {
        PVOID v = once->Ptr;
        if (v == RUNONCE_DONE) { if (ctx) *ctx = 0; return STATUS_SUCCESS; }   /* already done */
        if (v == 0) {
            if (InterlockedCompareExchangePointer(&once->Ptr, RUNONCE_RUNNING, 0) == 0)
                return STATUS_PENDING;                 /* we run the initializer */
        } else {
            backoff(&round);                           /* another thread is running it */
        }
    }
}

NTSTATUS NTAPI RtlRunOnceComplete(PRTL_RUN_ONCE once, ULONG flags, PVOID ctx)
{
    (void)ctx;
    once->Ptr = (flags & 4 /* INIT_ONCE_INIT_FAILED */) ? 0 : RUNONCE_DONE;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlRunOnceExecuteOnce(PRTL_RUN_ONCE once, PRTL_RUN_ONCE_INIT_FN fn, PVOID param, PVOID *ctx)
{
    NTSTATUS s = RtlRunOnceBeginInitialize(once, 0, ctx);
    if (s != STATUS_PENDING) return s;
    if (!fn(once, param, ctx)) {
        RtlRunOnceComplete(once, 4 /* INIT_ONCE_INIT_FAILED */, 0);
        return (NTSTATUS)0xC0000001;                    /* STATUS_UNSUCCESSFUL */
    }
    return RtlRunOnceComplete(once, 0, ctx ? *ctx : 0);
}
