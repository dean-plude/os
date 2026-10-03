/*
 * prioritytest.exe — SetThreadPriority and SetPriorityClass (kernel/um/
 * um_thread.c, kernel/ke/scheduler.c)
 *
 * First the query functions: for each priority class (IDLE through HIGH)
 * and each thread priority level (THREAD_PRIORITY_IDLE through
 * TIME_CRITICAL), GetPriorityClass and GetThreadPriority must give back
 * what was set, and the thread's actual priority (NtQueryInformationThread)
 * must be NT's: the class base (4, 6, 8, 10, 13) plus the level, kept
 * within 1-15, with IDLE and TIME_CRITICAL saturating at 1 and 15.
 * REALTIME_PRIORITY_CLASS without SeIncreaseBasePriorityPrivilege gives
 * HIGH, as on Windows; the boost switches round-trip too.
 *
 * Then the scheduling: with wake-up boosts switched off for the process
 * (SetProcessPriorityBoost) and a busy NORMAL thread on every processor,
 * a thread woken by an event runs at once when it is THREAD_PRIORITY_HIGHEST
 * (it preempts a busy thread of lower priority), and only once a busy
 * thread's 20 ms time slice ends when it is NORMAL like them.
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

/* A thread's current priority (boosts are off: its base) */
static LONG priority_of(HANDLE t)
{
    TBI b;
    ULONG got = 0;
    memset(&b, 0, sizeof(b));
    return g_qit && g_qit(t, 0, &b, sizeof(b), &got) >= 0 ? b.Priority : -1;
}

static const struct { DWORD cls; const char *name; LONG base; } k_classes[] = {
    { IDLE_PRIORITY_CLASS, "IDLE", 4 },
    { BELOW_NORMAL_PRIORITY_CLASS, "BELOW_NORMAL", 6 },
    { NORMAL_PRIORITY_CLASS, "NORMAL", 8 },
    { ABOVE_NORMAL_PRIORITY_CLASS, "ABOVE_NORMAL", 10 },
    { HIGH_PRIORITY_CLASS, "HIGH", 13 },
};
static const int k_levels[] = {
    THREAD_PRIORITY_IDLE, THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL,
    THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST, THREAD_PRIORITY_TIME_CRITICAL,
};

static LONG expected(LONG base, int level)
{
    if (level == THREAD_PRIORITY_IDLE) return 1;
    if (level == THREAD_PRIORITY_TIME_CRITICAL) return 15;
    LONG p = base + level;
    return p < 1 ? 1 : p > 15 ? 15 : p;
}

static int check_queries(void)
{
    int ok = 1, n = 0;
    HANDLE me = GetCurrentThread(), proc = GetCurrentProcess();
    if (GetPriorityClass(proc) != NORMAL_PRIORITY_CLASS || GetThreadPriority(me) != THREAD_PRIORITY_NORMAL ||
        priority_of(me) != 8) {
        printf("  at start: class 0x%lx, level %d, priority %ld (expected NORMAL, 0, 8)\n",
               GetPriorityClass(proc), GetThreadPriority(me), priority_of(me));
        ok = 0;
    }
    for (size_t c = 0; c < sizeof(k_classes) / sizeof(k_classes[0]); c++) {
        if (!SetPriorityClass(proc, k_classes[c].cls) || GetPriorityClass(proc) != k_classes[c].cls) {
            printf("  SetPriorityClass(%s): reads back 0x%lx\n", k_classes[c].name, GetPriorityClass(proc));
            ok = 0;
        }
        printf("  %-13s", k_classes[c].name);
        for (size_t l = 0; l < sizeof(k_levels) / sizeof(k_levels[0]); l++) {
            int lv = k_levels[l];
            BOOL set = SetThreadPriority(me, lv);
            int back = GetThreadPriority(me);
            LONG prio = priority_of(me), want = expected(k_classes[c].base, lv);
            printf(" %3ld", prio);
            if (!set || back != lv || prio != want) {
                printf(" (level %d: set %d, reads back %d, priority %ld, expected %ld)", lv, set, back, prio, want);
                ok = 0;
            }
            n++;
        }
        printf("\n");
    }
    SetThreadPriority(me, THREAD_PRIORITY_NORMAL);
    /* No SeIncreaseBasePriorityPrivilege: REALTIME is HIGH, TIME_CRITICAL 15 */
    SetPriorityClass(proc, REALTIME_PRIORITY_CLASS);
    SetThreadPriority(me, THREAD_PRIORITY_TIME_CRITICAL);
    DWORD rc = GetPriorityClass(proc);
    LONG rp = priority_of(me);
    printf("  REALTIME asked for: class 0x%lx, TIME_CRITICAL thread at %ld (expected HIGH 0x80, 15)\n", rc, rp);
    if (rc != HIGH_PRIORITY_CLASS || rp != 15) ok = 0;
    SetThreadPriority(me, THREAD_PRIORITY_NORMAL);
    SetPriorityClass(proc, NORMAL_PRIORITY_CLASS);
    /* Out-of-range requests fail */
    SetLastError(0);
    BOOL bad_level = SetThreadPriority(me, 5);
    DWORD e1 = GetLastError();
    BOOL bad_class = SetPriorityClass(proc, 0x12345);
    DWORD e2 = GetLastError();
    if (bad_level || e1 != ERROR_INVALID_PARAMETER || bad_class || e2 != ERROR_INVALID_PARAMETER) {
        printf("  invalid level/class accepted (%d, error %lu; %d, error %lu)\n", bad_level, e1, bad_class, e2);
        ok = 0;
    }
    /* The boost switches */
    BOOL tb = FALSE, pb = FALSE;
    SetThreadPriorityBoost(me, TRUE);
    GetThreadPriorityBoost(me, &tb);
    SetProcessPriorityBoost(proc, TRUE);
    GetProcessPriorityBoost(proc, &pb);
    BOOL tb2 = TRUE;
    SetThreadPriorityBoost(me, FALSE);
    GetThreadPriorityBoost(me, &tb2);
    SetProcessPriorityBoost(proc, FALSE);
    if (!tb || !pb || tb2) {
        printf("  boost switches read back thread %d, process %d, thread after clearing %d\n", tb, pb, tb2);
        ok = 0;
    }
    if (GetPriorityClass(proc) != NORMAL_PRIORITY_CLASS || priority_of(me) != 8) ok = 0;
    printf("prioritytest: %d class/level combinations %s\n", n, ok ? "match NT" : "DIFFER");
    return ok;
}

/* -------------------------------------------------------------------- */
#define MAX_SPIN 16
#define ROUNDS   100
static volatile LONG g_stop;
static volatile LONGLONG g_spun[MAX_SPIN];
static HANDLE g_ev, g_back;
static LARGE_INTEGER g_set_at, g_freq;
static LONGLONG g_woke[ROUNDS];

static DWORD WINAPI spin(LPVOID p)
{
    volatile LONGLONG *n = &g_spun[(INT_PTR)p];
    while (!g_stop) (*n)++;
    return 0;
}

static DWORD WINAPI waiter(LPVOID p)
{
    (void)p;
    SetEvent(g_back);
    for (int i = 0; i < ROUNDS; i++) {
        WaitForSingleObject(g_ev, INFINITE);
        LARGE_INTEGER b;
        QueryPerformanceCounter(&b);
        g_woke[i] = (b.QuadPart - g_set_at.QuadPart) * 1000000 / g_freq.QuadPart;
        SetEvent(g_back);
    }
    return 0;
}

static int cmp_ll(const void *a, const void *b)
{
    LONGLONG x = *(const LONGLONG *)a, y = *(const LONGLONG *)b;
    return x < y ? -1 : x > y;
}

/* The median and 95th percentile wake-up, in microseconds, of a waiter at
 * @level */
static void measure(int level, LONGLONG *median, LONGLONG *p95, LONG *prio)
{
    HANDLE w = CreateThread(NULL, 0, waiter, NULL, CREATE_SUSPENDED, NULL);
    SetThreadPriority(w, level);
    *prio = priority_of(w);
    ResumeThread(w);
    WaitForSingleObject(g_back, 5000);
    for (int i = 0; i < ROUNDS; i++) {
        Sleep(1);                                           /* (the waiter is waiting by now) */
        QueryPerformanceCounter(&g_set_at);
        SetEvent(g_ev);
        WaitForSingleObject(g_back, 5000);
    }
    WaitForSingleObject(w, 5000);
    CloseHandle(w);
    qsort(g_woke, ROUNDS, sizeof(g_woke[0]), cmp_ll);
    *median = g_woke[ROUNDS / 2];
    *p95 = g_woke[ROUNDS * 95 / 100];
}

static int check_scheduling(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int nspin = si.dwNumberOfProcessors > MAX_SPIN ? MAX_SPIN : (int)si.dwNumberOfProcessors;
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_back = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetProcessPriorityBoost(GetCurrentProcess(), TRUE);     /* priorities alone, no wake-up boosts */
    HANDLE th[MAX_SPIN];
    for (int i = 0; i < nspin; i++) th[i] = CreateThread(NULL, 0, spin, (LPVOID)(INT_PTR)i, 0, NULL);
    Sleep(20);
    LONGLONG hi_med, hi_p95, lo_med, lo_p95;
    LONG hi_prio, lo_prio;
    measure(THREAD_PRIORITY_HIGHEST, &hi_med, &hi_p95, &hi_prio);
    for (int s = 0; s < nspin; s++) g_spun[s] = 0;
    measure(THREAD_PRIORITY_NORMAL, &lo_med, &lo_p95, &lo_prio);
    LONGLONG least = -1;
    for (int s = 0; s < nspin; s++) if (least < 0 || g_spun[s] < least) least = g_spun[s];
    g_stop = 1;
    WaitForMultipleObjects(nspin, th, TRUE, 5000);
    for (int i = 0; i < nspin; i++) CloseHandle(th[i]);
    SetProcessPriorityBoost(GetCurrentProcess(), FALSE);
    printf("  HIGHEST waiter (priority %ld) runs after: median %lld us, 95%% %lld us\n", hi_prio, hi_med, hi_p95);
    printf("  NORMAL waiter (priority %ld), same as the busy threads: median %lld us, 95%% %lld us\n",
           lo_prio, lo_med, lo_p95);
    int ok = hi_prio == 10 && lo_prio == 8 && hi_p95 <= 2000 && lo_med > hi_p95 && least > 0;
    printf("prioritytest: a higher-priority thread runs within %lld.%03lld ms against %d busy threads "
           "(same priority: %lld.%03lld ms median)\n",
           hi_p95 / 1000, hi_p95 % 1000, nspin, lo_med / 1000, lo_med % 1000);
    return ok;
}

int main(void)
{
    g_qit = (NtQitFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    QueryPerformanceFrequency(&g_freq);
    printf("Thread priorities (columns IDLE, LOWEST, BELOW_NORMAL, NORMAL, ABOVE_NORMAL, HIGHEST, TIME_CRITICAL):\n");
    int ok = check_queries();
    printf("Scheduling, wake-up boosts off, a busy NORMAL thread on every processor:\n");
    ok &= check_scheduling();
    printf("prioritytest: %s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    return ok ? 0 : 1;
}
