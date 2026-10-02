/* threads.exe — threads, synchronization and SEH self-test */
#include <stdio.h>
#include <windows.h>

static int pass, fail;
#define CHECK(cond) do { if (cond) pass++; else { fail++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

/* ---- threads and a shared counter guarded by a critical section ---- */
static CRITICAL_SECTION g_cs;
static volatile LONG g_atomic;
static long g_guarded;

static DWORD WINAPI worker(LPVOID arg)
{
    int n = (int)(LONG_PTR)arg;
    for (int i = 0; i < n; i++) {
        InterlockedIncrement(&g_atomic);
        EnterCriticalSection(&g_cs);
        g_guarded++;
        LeaveCriticalSection(&g_cs);
    }
    return (DWORD)n;
}

/* ---- a producer/consumer over an auto-reset event and a semaphore ---- */
static HANDLE g_ev;
static volatile LONG g_seen;
static DWORD WINAPI waiter(LPVOID arg)
{
    (void)arg;
    WaitForSingleObject(g_ev, INFINITE);
    InterlockedIncrement(&g_seen);
    return 0;
}

/* ---- TLS ---- */
/* SRW lock + condition variable: a bounded queue between producers and consumers */
static SRWLOCK g_srw = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv_put = CONDITION_VARIABLE_INIT, g_cv_get = CONDITION_VARIABLE_INIT;
static int g_q[4], g_qn;
static long g_sum, g_shared_hits;
static DWORD WINAPI producer(LPVOID arg)
{
    for (int i = 1; i <= 500; i++) {
        AcquireSRWLockExclusive(&g_srw);
        while (g_qn == 4) SleepConditionVariableSRW(&g_cv_put, &g_srw, INFINITE, 0);
        g_q[g_qn++] = i;
        WakeConditionVariable(&g_cv_get);
        ReleaseSRWLockExclusive(&g_srw);
    }
    (void)arg;
    return 0;
}
static DWORD WINAPI consumer(LPVOID arg)
{
    for (int i = 0; i < 500; i++) {
        AcquireSRWLockExclusive(&g_srw);
        while (g_qn == 0) SleepConditionVariableSRW(&g_cv_get, &g_srw, INFINITE, 0);
        g_sum += g_q[--g_qn];
        WakeConditionVariable(&g_cv_put);
        ReleaseSRWLockExclusive(&g_srw);
        AcquireSRWLockShared(&g_srw);                     /* readers share it */
        InterlockedIncrement(&g_shared_hits);
        ReleaseSRWLockShared(&g_srw);
    }
    (void)arg;
    return 0;
}

/* WaitOnAddress: a waiter sleeps until the value changes and it is woken */
static volatile LONG g_addr_val;
static DWORD WINAPI addr_waiter(LPVOID arg)
{
    LONG zero = 0;
    while (g_addr_val == 0) WaitOnAddress(&g_addr_val, &zero, sizeof(zero), INFINITE);
    (void)arg;
    return (DWORD)g_addr_val;
}

static DWORD g_tls;
static DWORD WINAPI tls_worker(LPVOID arg)
{
    TlsSetValue(g_tls, arg);
    Sleep(1);
    return TlsGetValue(g_tls) == arg ? 1 : 0;
}

/* ---- SEH ---- */
static volatile int g_zero;      /* 0, but opaque to the optimizer */
static int  *volatile g_nullp;   /* NULL, but opaque */
__declspec(noinline) static int divide(int a, int b) { return a / b; }

static LONG filter_seen;
static LONG WINAPI veh(PEXCEPTION_POINTERS info)
{
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) filter_seen++;
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(void)
{
    /* Threads with a critical section and interlocked ops */
    InitializeCriticalSection(&g_cs);
    HANDLE t[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(0, 0, worker, (LPVOID)(LONG_PTR)10000, 0, 0);
    for (int i = 0; i < 4; i++) CHECK(t[i] != NULL);
    DWORD w = WaitForMultipleObjects(4, t, TRUE, 5000);
    CHECK(w == WAIT_OBJECT_0);
    CHECK(g_atomic == 40000);
    CHECK(g_guarded == 40000);
    for (int i = 0; i < 4; i++) { DWORD ec = 0; GetExitCodeThread(t[i], &ec); CHECK(ec == 10000); CloseHandle(t[i]); }
    DeleteCriticalSection(&g_cs);

    /* Manual-reset event: one SetEvent releases all three waiters */
    g_ev = CreateEventA(0, TRUE, FALSE, 0);             /* manual-reset */
    HANDLE ww[3];
    for (int i = 0; i < 3; i++) ww[i] = CreateThread(0, 0, waiter, 0, 0, 0);
    SetEvent(g_ev);
    CHECK(WaitForMultipleObjects(3, ww, TRUE, 5000) == WAIT_OBJECT_0);
    CHECK(g_seen == 3);
    for (int i = 0; i < 3; i++) CloseHandle(ww[i]);
    CloseHandle(g_ev);

    /* Auto-reset event: each SetEvent releases exactly one waiter */
    HANDLE aev = CreateEventA(0, FALSE, FALSE, 0);
    HANDLE aw[3];
    g_seen = 0;
    g_ev = aev;
    for (int i = 0; i < 3; i++) aw[i] = CreateThread(0, 0, waiter, 0, 0, 0);
    Sleep(20);                                           /* let them park on the wait */
    SetEvent(aev); Sleep(20); CHECK(g_seen == 1);
    SetEvent(aev); Sleep(20); CHECK(g_seen == 2);
    SetEvent(aev);
    CHECK(WaitForMultipleObjects(3, aw, TRUE, 5000) == WAIT_OBJECT_0);
    CHECK(g_seen == 3);
    for (int i = 0; i < 3; i++) CloseHandle(aw[i]);
    CloseHandle(aev);

    /* Semaphore */
    HANDLE sem = CreateSemaphoreA(0, 2, 5, 0);
    CHECK(sem != NULL);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_TIMEOUT);
    LONG prev = 0;
    CHECK(ReleaseSemaphore(sem, 2, &prev) && prev == 0);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0);
    CloseHandle(sem);

    /* Mutex: recursive acquisition */
    HANDLE mtx = CreateMutexA(0, FALSE, 0);
    CHECK(WaitForSingleObject(mtx, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(mtx, 0) == WAIT_OBJECT_0);   /* recursive */
    CHECK(ReleaseMutex(mtx) && ReleaseMutex(mtx));
    CloseHandle(mtx);

    /* SRW locks and condition variables: 2 producers, 2 consumers */
    HANDLE pc[4];
    pc[0] = CreateThread(0, 0, producer, 0, 0, 0);
    pc[1] = CreateThread(0, 0, producer, 0, 0, 0);
    pc[2] = CreateThread(0, 0, consumer, 0, 0, 0);
    pc[3] = CreateThread(0, 0, consumer, 0, 0, 0);
    CHECK(WaitForMultipleObjects(4, pc, TRUE, 20000) == WAIT_OBJECT_0);
    CHECK(g_sum == 2L * 500 * 501 / 2);
    CHECK(g_shared_hits == 1000);
    CHECK(TryAcquireSRWLockExclusive(&g_srw));
    CHECK(!TryAcquireSRWLockShared(&g_srw));
    ReleaseSRWLockExclusive(&g_srw);
    for (int i = 0; i < 4; i++) CloseHandle(pc[i]);
    /* a condition variable times out */
    AcquireSRWLockExclusive(&g_srw);
    CHECK(!SleepConditionVariableSRW(&g_cv_get, &g_srw, 30, 0) && GetLastError() == ERROR_TIMEOUT);
    ReleaseSRWLockExclusive(&g_srw);

    /* WaitOnAddress / WakeByAddressAll; a wait times out, or returns at once on another value */
    LONG zero = 0, one = 1;
    CHECK(!WaitOnAddress(&g_addr_val, &zero, sizeof(zero), 30) && GetLastError() == ERROR_TIMEOUT);
    CHECK(WaitOnAddress(&g_addr_val, &one, sizeof(one), INFINITE));
    HANDLE aw2[3];
    for (int i = 0; i < 3; i++) aw2[i] = CreateThread(0, 0, addr_waiter, 0, 0, 0);
    Sleep(50);
    InterlockedExchange(&g_addr_val, 7);
    WakeByAddressAll((PVOID)&g_addr_val);
    CHECK(WaitForMultipleObjects(3, aw2, TRUE, 5000) == WAIT_OBJECT_0);
    for (int i = 0; i < 3; i++) { DWORD ec = 0; GetExitCodeThread(aw2[i], &ec); CHECK(ec == 7); CloseHandle(aw2[i]); }

    /* TLS per-thread isolation */
    g_tls = TlsAlloc();
    CHECK(g_tls != TLS_OUT_OF_INDEXES);
    TlsSetValue(g_tls, (LPVOID)0x1111);
    HANDLE tt[3];
    for (int i = 0; i < 3; i++) tt[i] = CreateThread(0, 0, tls_worker, (LPVOID)(LONG_PTR)(0x2000 + i), 0, 0);
    WaitForMultipleObjects(3, tt, TRUE, 5000);
    for (int i = 0; i < 3; i++) { DWORD ec = 0; GetExitCodeThread(tt[i], &ec); CHECK(ec == 1); CloseHandle(tt[i]); }
    CHECK(TlsGetValue(g_tls) == (LPVOID)0x1111);           /* our slot untouched */
    TlsFree(g_tls);

    /* SEH: __try / __except catches a divide by zero */
    PVOID h = AddVectoredExceptionHandler(1, veh);
    volatile int caught = 0, code = 0, z = 0;
    __try {
        z = divide(10, g_zero);
    } __except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO
                ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        caught = 1;
        code = GetExceptionCode();
    }
    CHECK(caught == 1);
    CHECK((DWORD)code == EXCEPTION_INT_DIVIDE_BY_ZERO);

    /* SEH: access violation, with __finally running during the unwind */
    volatile int finally_ran = 0, caught2 = 0;
    __try {
        __try {
            *g_nullp = 1;
        } __finally {
            finally_ran = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        caught2 = 1;
    }
    CHECK(finally_ran == 1);
    CHECK(caught2 == 1);
    CHECK(filter_seen >= 1);                                /* the VEH saw the AV */
    RemoveVectoredExceptionHandler(h);

    /* RaiseException with a custom code */
    volatile int custom = 0;
    __try {
        RaiseException(0xE0001234, 0, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        custom = (int)(GetExceptionCode() == 0xE0001234);
    }
    CHECK(custom == 1);

    printf("threads/sync/SEH self-test: %d passed, %d failed\n", pass, fail);
    return fail;
}
