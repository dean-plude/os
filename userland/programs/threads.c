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
