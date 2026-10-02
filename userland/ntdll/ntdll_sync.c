#define NOVA_BUILD_NTDLL
/*
 * ntdll_sync.c — critical sections, SRW locks, condition variables
 *
 * Critical sections use the classic NT algorithm: an interlocked LockCount
 * for the fast, uncontended path and a lazily created auto-reset event to
 * park a contending thread.  SRW locks are a compact reader/writer word
 * and condition variables a wake counter; both park on their word with
 * RtlWaitOnAddress (NtWaitForAlertByThreadId), so a waiter sleeps until it
 * is woken instead of polling.
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
 * Waiting on an address (RtlWaitOnAddress, as kernel32's WaitOnAddress).
 * As on Windows 8 and later, the waiters are listed here, in hashed
 * buckets, and sleep in NtWaitForAlertByThreadId; a waker unlinks them and
 * alerts their threads (NtAlertThreadByThreadId).  SRW locks and condition
 * variables park on their own words this way.
 * ----------------------------------------------------------------------- */
typedef struct AddrWaiter {
    const volatile void *addr;
    ULONG tid;
    volatile LONG done;                 /* set (under the bucket lock) by the waker */
    struct AddrWaiter *next;
} AddrWaiter;
static struct { volatile LONG lock; AddrWaiter *head; } g_wq[64];

static int wq_of(const volatile void *a) { return (int)(((ULONG_PTR)a >> 3) % 64); }
static void wq_lock(int b)
{
    for (ULONG n = 0; __atomic_exchange_n(&g_wq[b].lock, 1, __ATOMIC_ACQUIRE); n++)
        if (n < 64) __builtin_ia32_pause(); else yield();
}
static void wq_unlock(int b) { __atomic_store_n(&g_wq[b].lock, 0, __ATOMIC_RELEASE); }

static BOOLEAN same_value(const volatile void *a, const void *b, SIZE_T n)
{
    switch (n) {
    case 1: return *(const volatile UCHAR *)a == *(const UCHAR *)b;
    case 2: return *(const volatile USHORT *)a == *(const USHORT *)b;
    case 4: return *(const volatile ULONG *)a == *(const ULONG *)b;
    case 8: return *(const volatile ULONGLONG *)a == *(const ULONGLONG *)b;
    }
    return FALSE;
}

NTSTATUS NTAPI RtlWaitOnAddress(const volatile void *addr, const void *cmp, SIZE_T size, PLARGE_INTEGER timeout)
{
    if (size != 1 && size != 2 && size != 4 && size != 8) return STATUS_INVALID_PARAMETER;
    /* a relative timeout counts from now: what is left goes to each wait */
    LARGE_INTEGER left, *t = timeout;
    ULONGLONG start = now_ms(), limit = 0;
    BOOLEAN relative = timeout && timeout->QuadPart < 0;
    if (relative) limit = (ULONGLONG)(-timeout->QuadPart) / 10000;
    AddrWaiter w = { addr, cur_tid(), 0, 0 };
    int b = wq_of(addr);
    wq_lock(b);
    if (!same_value(addr, cmp, size)) { wq_unlock(b); return STATUS_SUCCESS; }
    w.next = g_wq[b].head;
    g_wq[b].head = &w;
    wq_unlock(b);
    for (;;) {
        if (w.done) return STATUS_SUCCESS;
        if (relative) {
            ULONGLONG gone = now_ms() - start;
            left.QuadPart = gone >= limit ? 0 : -(LONGLONG)((limit - gone) * 10000);
            t = &left;
        }
        NTSTATUS s = NtWaitForAlertByThreadId((PVOID)addr, t);
        if (w.done) return STATUS_SUCCESS;
        if (s != STATUS_TIMEOUT && NT_SUCCESS(s)) continue;     /* alerted for something else: look again */
        wq_lock(b);
        BOOLEAN done = w.done != 0;
        if (!done)
            for (AddrWaiter **pp = &g_wq[b].head; *pp; pp = &(*pp)->next)
                if (*pp == &w) { *pp = w.next; break; }
        wq_unlock(b);
        return done ? STATUS_SUCCESS : NT_SUCCESS(s) ? STATUS_TIMEOUT : s;
    }
}

static void wake_address(const volatile void *addr, BOOLEAN all)
{
    int b = wq_of(addr);
    for (;;) {
        ULONG tids[16];
        int n = 0;
        BOOLEAN more = FALSE;
        wq_lock(b);
        for (AddrWaiter **pp = &g_wq[b].head; *pp; ) {
            AddrWaiter *w = *pp;
            if (w->addr != addr) { pp = &w->next; continue; }
            if (n == 16) { more = TRUE; break; }
            *pp = w->next;
            tids[n++] = w->tid;
            w->done = 1;                    /* its owner may return (and its stack go) from here on */
            if (!all) break;
        }
        wq_unlock(b);
        for (int i = 0; i < n; i++) NtAlertThreadByThreadId((HANDLE)(ULONG_PTR)tids[i]);
        if (!more) return;
    }
}

VOID NTAPI RtlWakeAddressAll(PVOID addr)    { wake_address(addr, TRUE); }
VOID NTAPI RtlWakeAddressSingle(PVOID addr) { wake_address(addr, FALSE); }

/* Park on a pointer-sized word while it holds @v */
static void park_on(volatile LONG_PTR *p, LONG_PTR v)
{
    RtlWaitOnAddress(p, &v, sizeof(v), NULL);
}

/* -----------------------------------------------------------------------
 * Slim reader/writer locks: bit 0 a writer holds it, bit 1 threads are
 * parked on it, bits 2.. the readers.  A thread that cannot have it spins
 * a little, then sets bit 1 and parks; whoever releases it with bit 1 set
 * clears it and wakes them all to try again.
 * ----------------------------------------------------------------------- */
#define SRW_WRITER  1
#define SRW_PARKED  2
#define SRW_READER  4
#define SRW_SPINS   100

/* pointer-sized (8 bytes, or 4 in 32-bit programs) */
static volatile LONG_PTR *srw(PRTL_SRWLOCK l) { return (volatile LONG_PTR *)&l->Ptr; }
static LONG_PTR cas(volatile LONG_PTR *p, LONG_PTR v, LONG_PTR cmp) { return __sync_val_compare_and_swap(p, cmp, v); }

VOID NTAPI RtlInitializeSRWLock(PRTL_SRWLOCK l) { l->Ptr = 0; }

/* it is taken: spin, or mark it and park until it changes */
static void srw_contend(volatile LONG_PTR *w, LONG_PTR v, ULONG *spins)
{
    if ((*spins)++ < SRW_SPINS) { __builtin_ia32_pause(); return; }
    if (!(v & SRW_PARKED) && cas(w, v | SRW_PARKED, v) != v) return;
    park_on(w, v | SRW_PARKED);
}

VOID NTAPI RtlAcquireSRWLockExclusive(PRTL_SRWLOCK l)
{
    volatile LONG_PTR *w = srw(l);
    for (ULONG spins = 0;;) {
        LONG_PTR v = *w;
        if (!(v & ~(LONG_PTR)SRW_PARKED)) { if (cas(w, v | SRW_WRITER, v) == v) return; continue; }
        srw_contend(w, v, &spins);
    }
}

BOOLEAN NTAPI RtlTryAcquireSRWLockExclusive(PRTL_SRWLOCK l)
{
    LONG_PTR v = *srw(l);
    return !(v & ~(LONG_PTR)SRW_PARKED) && cas(srw(l), v | SRW_WRITER, v) == v;
}

VOID NTAPI RtlReleaseSRWLockExclusive(PRTL_SRWLOCK l)
{
    LONG_PTR old = __atomic_fetch_and(srw(l), ~(LONG_PTR)(SRW_WRITER | SRW_PARKED), __ATOMIC_RELEASE);
    if (old & SRW_PARKED) wake_address(srw(l), TRUE);
}

VOID NTAPI RtlAcquireSRWLockShared(PRTL_SRWLOCK l)
{
    volatile LONG_PTR *w = srw(l);
    for (ULONG spins = 0;;) {
        LONG_PTR v = *w;
        if (!(v & SRW_WRITER)) { if (cas(w, v + SRW_READER, v) == v) return; continue; }
        srw_contend(w, v, &spins);
    }
}

BOOLEAN NTAPI RtlTryAcquireSRWLockShared(PRTL_SRWLOCK l)
{
    LONG_PTR v = *srw(l);
    return !(v & SRW_WRITER) && cas(srw(l), v + SRW_READER, v) == v;
}

VOID NTAPI RtlReleaseSRWLockShared(PRTL_SRWLOCK l)
{
    LONG_PTR old = __sync_fetch_and_sub(srw(l), SRW_READER);
    /* the last reader out with threads parked: free it and wake them */
    if (old == SRW_READER + SRW_PARKED && cas(srw(l), 0, SRW_PARKED) == SRW_PARKED) wake_address(srw(l), TRUE);
}

/* -----------------------------------------------------------------------
 * Condition variables: a wake counter that sleepers park on (spurious
 * wakeups are allowed by the contract, so callers re-check their predicate)
 * ----------------------------------------------------------------------- */
VOID NTAPI RtlInitializeConditionVariable(PRTL_CONDITION_VARIABLE cv) { cv->Ptr = 0; }
VOID NTAPI RtlWakeConditionVariable(PRTL_CONDITION_VARIABLE cv)
{
    __sync_fetch_and_add((volatile LONG_PTR *)&cv->Ptr, 1);
    wake_address(&cv->Ptr, FALSE);
}
VOID NTAPI RtlWakeAllConditionVariable(PRTL_CONDITION_VARIABLE cv)
{
    __sync_fetch_and_add((volatile LONG_PTR *)&cv->Ptr, 1);
    wake_address(&cv->Ptr, TRUE);
}

static NTSTATUS cv_sleep(PRTL_CONDITION_VARIABLE cv, PLARGE_INTEGER timeout,
                         void (*unlock)(void *), void (*lock)(void *), void *obj)
{
    LONG_PTR seen = *(volatile LONG_PTR *)&cv->Ptr;
    unlock(obj);
    NTSTATUS s = RtlWaitOnAddress(&cv->Ptr, &seen, sizeof(seen), timeout);
    lock(obj);
    return s == STATUS_TIMEOUT ? STATUS_TIMEOUT : STATUS_SUCCESS;
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

NTSYSAPI NTSTATUS NTAPI RtlRunOnceExecuteOnce(PRTL_RUN_ONCE once, PRTL_RUN_ONCE_INIT_FN fn, PVOID param, PVOID *ctx)
{
    PVOID c = 0;
    NTSTATUS s = RtlRunOnceBeginInitialize(once, 0, &c);
    if (s == STATUS_SUCCESS) { if (ctx) *ctx = c; return s; }
    if (s != STATUS_PENDING) return s;
    ULONG ok = fn(once, param, ctx);
    RtlRunOnceComplete(once, ok ? 0 : 4, ctx ? *ctx : 0);
    return ok ? STATUS_SUCCESS : (NTSTATUS)0xC0000001;   /* STATUS_UNSUCCESSFUL */
}
