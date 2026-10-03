/*
 * boosttest.exe — priority boosts on wake-up (kernel/ke/scheduler.c)
 *
 * With a busy thread on every processor, all of the same base priority as
 * the threads measured, a thread waiting on an event, a semaphore, a
 * condition variable (NtAlertThreadByThreadId underneath) or a thread
 * message is woken by another thread, and notes how long it took to run.
 * NT raises a woken thread's priority above its base by the waker's
 * increment (+1 for events, semaphores and alerts, +2 for window
 * messages), so it preempts a busy thread of the same base priority
 * instead of waiting for that thread's 20 ms time slice to end.  Each
 * case passes when its 95th percentile is 2 ms or less.
 *
 * Then the boost itself: the woken thread reads its own priority
 * (NtQueryInformationThread), which must be its base plus the increment,
 * spins for 200 ms, and reads it again: the boost decays one level per
 * quantum it runs, so it must be back at its base, and every busy thread
 * must have run meanwhile (a boosted thread doesn't starve the others).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    LONG ExitStatus; PVOID TebBaseAddress; HANDLE UniqueProcess; HANDLE UniqueThread;
    ULONG_PTR AffinityMask; LONG Priority; LONG BasePriority;
} TBI;
typedef LONG (WINAPI *NtQitFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
static NtQitFn g_qit;

/* The calling thread's current (dynamic) and base priority */
static void my_priority(LONG *prio, LONG *base)
{
    TBI b;
    memset(&b, 0, sizeof(b));
    ULONG got = 0;
    *prio = *base = -1;
    if (g_qit && g_qit(GetCurrentThread(), 0, &b, sizeof(b), &got) >= 0) { *prio = b.Priority; *base = b.BasePriority; }
}

#define MAX_SPIN 16
static volatile LONG g_stop;
static volatile LONGLONG g_spun[MAX_SPIN];
static DWORD WINAPI spin(LPVOID p)
{
    volatile LONGLONG *n = &g_spun[(INT_PTR)p];
    while (!g_stop) (*n)++;
    return 0;
}

static int cmp_ll(const void *a, const void *b)
{
    LONGLONG x = *(const LONGLONG *)a, y = *(const LONGLONG *)b;
    return x < y ? -1 : x > y;
}

#define ROUNDS 200
enum { K_EVENT, K_SEMAPHORE, K_CONDVAR, K_MESSAGE };
static const char *const k_name[] = { "event", "semaphore", "condition variable", "thread message" };

static int g_kind;
static HANDLE g_ev, g_sem, g_back;
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static volatile LONG g_round;
static LARGE_INTEGER g_set_at, g_freq;
static LONGLONG g_woke[ROUNDS];
static LONG g_prio_woken, g_prio_after, g_base;

static void wait_one(int round)
{
    switch (g_kind) {
    case K_EVENT:     WaitForSingleObject(g_ev, INFINITE); break;
    case K_SEMAPHORE: WaitForSingleObject(g_sem, INFINITE); break;
    case K_CONDVAR:
        AcquireSRWLockExclusive(&g_lock);
        while (g_round <= round) SleepConditionVariableSRW(&g_cv, &g_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&g_lock);
        break;
    case K_MESSAGE: {
        MSG m;
        GetMessageA(&m, NULL, 0, 0);
        break;
    }
    }
}

static DWORD WINAPI waiter(LPVOID p)
{
    (void)p;
    MSG m;
    PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE);              /* (makes the thread's message queue) */
    SetEvent(g_back);
    for (int i = 0; i < ROUNDS; i++) {
        wait_one(i);
        LARGE_INTEGER b;
        QueryPerformanceCounter(&b);
        g_woke[i] = (b.QuadPart - g_set_at.QuadPart) * 1000000 / g_freq.QuadPart;
        if (i == ROUNDS - 1) {                              /* the boost, and its decay */
            LONG base;
            my_priority(&g_prio_woken, &g_base);
            QueryPerformanceCounter(&b);
            LARGE_INTEGER c;
            volatile unsigned x = 0;
            do { x++; QueryPerformanceCounter(&c); } while ((c.QuadPart - b.QuadPart) * 1000 / g_freq.QuadPart < 200);
            my_priority(&g_prio_after, &base);
        }
        SetEvent(g_back);
    }
    return 0;
}

/* One case: the 95th percentile wake-up in microseconds; *ok cleared on a
 * failed check */
static LONGLONG measure(int kind, int incr, int load, int nspin, int *ok)
{
    g_kind = kind;
    g_round = 0;
    DWORD tid;
    HANDLE w = CreateThread(NULL, 0, waiter, NULL, 0, &tid);
    WaitForSingleObject(g_back, 5000);
    for (int i = 0; i < ROUNDS; i++) {
        Sleep(1);                                           /* (the waiter is waiting by now) */
        if (i == ROUNDS - 1)
            for (int s = 0; s < nspin; s++) g_spun[s] = 0;
        QueryPerformanceCounter(&g_set_at);
        switch (kind) {
        case K_EVENT:     SetEvent(g_ev); break;
        case K_SEMAPHORE: ReleaseSemaphore(g_sem, 1, NULL); break;
        case K_CONDVAR:
            AcquireSRWLockExclusive(&g_lock);
            QueryPerformanceCounter(&g_set_at);
            g_round = i + 1;
            ReleaseSRWLockExclusive(&g_lock);
            WakeConditionVariable(&g_cv);
            break;
        case K_MESSAGE:   PostThreadMessageA(tid, WM_USER, 0, 0); break;
        }
        WaitForSingleObject(g_back, 5000);
    }
    WaitForSingleObject(w, 5000);
    CloseHandle(w);
    qsort(g_woke, ROUNDS, sizeof(g_woke[0]), cmp_ll);
    LONGLONG p95 = g_woke[ROUNDS * 95 / 100];
    printf("  %-20s runs after: min %lld us, median %lld us, 95%% %lld us, max %lld us\n", k_name[kind],
           g_woke[0], g_woke[ROUNDS / 2], p95, g_woke[ROUNDS - 1]);
    if (!load) return p95;
    LONGLONG least = -1;
    for (int s = 0; s < nspin; s++) if (least < 0 || g_spun[s] < least) least = g_spun[s];
    printf("  %-20s priority %ld on waking (base %ld, +%d expected), %ld after 200 ms busy; busy threads ran %s\n", "",
           g_prio_woken, g_base, incr, g_prio_after, least > 0 ? "meanwhile" : "NOT AT ALL");
    if (p95 > 2000 || g_prio_woken != g_base + incr || g_prio_after != g_base || least <= 0) *ok = 0;
    return p95;
}

int main(void)
{
    g_qit = (NtQitFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    QueryPerformanceFrequency(&g_freq);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int nspin = si.dwNumberOfProcessors > MAX_SPIN ? MAX_SPIN : (int)si.dwNumberOfProcessors;
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_sem = CreateSemaphoreA(NULL, 0, 1000, NULL);
    g_back = CreateEventA(NULL, FALSE, FALSE, NULL);
    static const int incr[] = { 1, 1, 1, 2 };
    int ok = 1;
    LONGLONG worst = 0;
    for (int load = 0; load < 2; load++) {
        HANDLE th[MAX_SPIN];
        int n = load ? nspin : 0;
        g_stop = 0;
        for (int i = 0; i < n; i++) th[i] = CreateThread(NULL, 0, spin, (LPVOID)(INT_PTR)i, 0, NULL);
        printf("%s:\n", load ? "Under load (a busy thread of the same priority on every processor)" : "Idle");
        Sleep(20);
        for (int k = K_EVENT; k <= K_MESSAGE; k++) {
            LONGLONG p = measure(k, incr[k], load, n, &ok);
            if (load && p > worst) worst = p;
        }
        g_stop = 1;
        WaitForMultipleObjects(n, th, TRUE, 5000);
        for (int i = 0; i < n; i++) CloseHandle(th[i]);
    }
    printf("boosttest: woken threads run within %lld.%03lld ms under load on %lu processors\n",
           worst / 1000, worst % 1000, si.dwNumberOfProcessors);
    printf("boosttest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
