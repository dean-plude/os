/*
 * mmcsstest.exe — the Multimedia Class Scheduler (userland/avrt/avrt.c,
 * kernel/ke/scheduler.c)
 *
 * First the priorities: a thread registered for "Pro Audio" runs at 18
 * (NtQueryInformationThread), at 16 with AVRT_PRIORITY_LOW, a "Games"
 * thread at 16, a task Windows does not have fails with
 * ERROR_INVALID_TASK_NAME, and reverting gives the thread back the
 * priority SetThreadPriority gave it (TIME_CRITICAL: 15).  None of this
 * needs SeIncreaseBasePriorityPrivilege, which a program does not hold.
 *
 * Then the point of it: a busy TIME_CRITICAL thread on every processor
 * stands in for a foreground program's window threads, which wake-up
 * boosts lift as high as 15.  A thread woken by an event every 5 ms runs
 * within 2 ms when it is registered with MMCSS ("Audio": 18 preempts
 * them), and waits for a busy thread's time slice to end when it is a
 * plain TIME_CRITICAL thread like them (the sound threads' place before
 * MMCSS).  The thread setting the event is registered too, so it is never
 * held up itself.
 *
 * Then the budget: a "Pro Audio" thread spinning on every processor for a
 * second must not freeze the rest of the machine.  Each may use 80% of a
 * processor in each 100 ms period (Windows' SystemResponsiveness 20), so
 * this NORMAL thread, sleeping 1 ms at a time meanwhile, is never kept
 * waiting more than 250 ms (without the budget it waits the whole second).
 */
#include <windows.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    LONG ExitStatus; PVOID TebBaseAddress; HANDLE UniqueProcess; HANDLE UniqueThread;
    ULONG_PTR AffinityMask; LONG Priority; LONG BasePriority;
} TBI;
typedef LONG (WINAPI *NtQitFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
static NtQitFn g_qit;

static LONG priority_of(HANDLE t)
{
    TBI b;
    ULONG got = 0;
    memset(&b, 0, sizeof(b));
    return g_qit && g_qit(t, 0, &b, sizeof(b), &got) >= 0 ? b.Priority : -1;
}

/* -------------------------------------------------------------------- */
static int check_priorities(void)
{
    int ok = 1;
    HANDLE me = GetCurrentThread();
    SetThreadPriority(me, THREAD_PRIORITY_TIME_CRITICAL);
    LONG before = priority_of(me);
    DWORD idx = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    LONG pro = priority_of(me);
    BOOL low_ok = h && AvSetMmThreadPriority(h, AVRT_PRIORITY_LOW);
    LONG low = priority_of(me);
    BOOL crit_ok = h && AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
    LONG crit = priority_of(me);
    BOOL rev = h && AvRevertMmThreadCharacteristics(h);
    LONG after = priority_of(me);
    printf("  TIME_CRITICAL %ld; \"Pro Audio\" %ld (task index %lu), LOW %ld, CRITICAL %ld; reverted %ld\n",
           before, pro, idx, low, crit, after);
    ok &= before == 15 && h && idx != 0 && pro == 18 && low_ok && low == 16 && crit_ok && crit == 18 && rev && after == 15;

    idx = 0;
    HANDLE g = AvSetMmThreadCharacteristicsA("Games", &idx);
    LONG games = priority_of(me);
    if (g) AvRevertMmThreadCharacteristics(g);
    idx = 0;
    SetLastError(0);
    HANDLE bad = AvSetMmThreadCharacteristicsW(L"No Such Task", &idx);
    DWORD err = GetLastError();
    printf("  \"Games\" %ld; \"No Such Task\" %s (error %lu)\n", games, bad ? "registered" : "refused", err);
    ok &= g && games == 16 && !bad && err == 1550 /* ERROR_INVALID_TASK_NAME */;
    SetThreadPriority(me, THREAD_PRIORITY_NORMAL);
    printf("mmcsstest: MMCSS priorities %s\n", ok ? "match" : "DO NOT match");
    return ok;
}

/* -------------------------------------------------------------------- */
#define MAX_SPIN 16
#define ROUNDS   40
static volatile LONG g_stop;
static HANDLE g_ev, g_ready;
static LARGE_INTEGER g_freq, g_set_at;
static LONGLONG g_woke[ROUNDS];
static volatile LONG g_round;

static DWORD WINAPI spin(LPVOID p)
{
    (void)p;
    while (!g_stop) { }
    return 0;
}

static DWORD WINAPI waiter(LPVOID p)
{
    DWORD idx = 0;
    if (p) AvSetMmThreadCharacteristicsW(L"Audio", &idx);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetEvent(g_ready);
    for (int i = 0; i < ROUNDS; i++) {
        WaitForSingleObject(g_ev, INFINITE);
        LARGE_INTEGER b;
        QueryPerformanceCounter(&b);
        g_woke[i] = (b.QuadPart - g_set_at.QuadPart) * 1000000 / g_freq.QuadPart;
        InterlockedIncrement(&g_round);
    }
    return 0;
}

static int cmp_ll(const void *a, const void *b)
{
    LONGLONG x = *(const LONGLONG *)a, y = *(const LONGLONG *)b;
    return x < y ? -1 : x > y;
}

/* The waiter's median and 95th percentile wake-up (microseconds) against
 * @nspin busy TIME_CRITICAL threads; @mm: the waiter is registered */
static void measure(int nspin, BOOL mm, LONGLONG *median, LONGLONG *p95, LONG *prio)
{
    DWORD idx = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);   /* (the setter must not wait itself) */
    g_stop = 0;
    g_round = 0;
    HANDLE w = CreateThread(NULL, 0, waiter, (LPVOID)(INT_PTR)mm, 0, NULL);
    WaitForSingleObject(g_ready, 5000);                          /* (registered, before the processors fill up) */
    *prio = priority_of(w);
    HANDLE th[MAX_SPIN];
    for (int i = 0; i < nspin; i++) {
        th[i] = CreateThread(NULL, 0, spin, NULL, CREATE_SUSPENDED, NULL);
        SetThreadPriority(th[i], THREAD_PRIORITY_TIME_CRITICAL);
        ResumeThread(th[i]);
    }
    Sleep(30);
    for (int i = 0; i < ROUNDS; i++) {
        Sleep(5);
        QueryPerformanceCounter(&g_set_at);
        SetEvent(g_ev);
        for (DWORD t0 = GetTickCount(); g_round <= i && GetTickCount() - t0 < 2000; ) Sleep(1);
    }
    g_stop = 1;
    WaitForSingleObject(w, 5000);
    WaitForMultipleObjects(nspin, th, TRUE, 5000);
    for (int i = 0; i < nspin; i++) CloseHandle(th[i]);
    CloseHandle(w);
    if (task) AvRevertMmThreadCharacteristics(task);
    qsort(g_woke, ROUNDS, sizeof(g_woke[0]), cmp_ll);
    *median = g_woke[ROUNDS / 2];
    *p95 = g_woke[ROUNDS * 95 / 100];
}

static int check_latency(int nspin)
{
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    LONGLONG mm_med, mm_p95, tc_med, tc_p95;
    LONG mm_prio, tc_prio;
    measure(nspin, TRUE, &mm_med, &mm_p95, &mm_prio);
    measure(nspin, FALSE, &tc_med, &tc_p95, &tc_prio);
    printf("  \"Audio\" waiter (priority %ld): median %lld us, 95%% %lld us\n", mm_prio, mm_med, mm_p95);
    printf("  TIME_CRITICAL waiter (priority %ld), same as the busy threads: median %lld us, 95%% %lld us\n",
           tc_prio, tc_med, tc_p95);
    int ok = mm_prio == 18 && tc_prio == 15 && mm_p95 <= 2000 && tc_med > mm_p95;
    printf("mmcsstest: an MMCSS thread runs within %lld.%03lld ms against %d busy TIME_CRITICAL threads "
           "(TIME_CRITICAL itself: %lld.%03lld ms median)\n",
           mm_p95 / 1000, mm_p95 % 1000, nspin, tc_med / 1000, tc_med % 1000);
    return ok;
}

/* -------------------------------------------------------------------- */
static LONGLONG g_until;

static DWORD WINAPI hog(LPVOID p)
{
    (void)p;
    DWORD idx = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    LARGE_INTEGER now;
    do QueryPerformanceCounter(&now); while (now.QuadPart < g_until);
    if (task) AvRevertMmThreadCharacteristics(task);
    return 0;
}

static int check_budget(int nspin)
{
    HANDLE th[MAX_SPIN];
    LARGE_INTEGER start, now, last;
    QueryPerformanceCounter(&start);
    g_until = start.QuadPart + g_freq.QuadPart;                  /* 1 s */
    for (int i = 0; i < nspin; i++) th[i] = CreateThread(NULL, 0, hog, NULL, 0, NULL);
    last = start;
    LONGLONG worst = 0;
    int runs = 0;
    do {
        Sleep(1);
        QueryPerformanceCounter(&now);
        LONGLONG gap = (now.QuadPart - last.QuadPart) * 1000 / g_freq.QuadPart;
        if (gap > worst) worst = gap;
        last = now;
        runs++;
    } while (now.QuadPart < g_until + g_freq.QuadPart / 10);
    WaitForMultipleObjects(nspin, th, TRUE, 5000);
    for (int i = 0; i < nspin; i++) CloseHandle(th[i]);
    printf("  a NORMAL thread ran %d times in 1.1 s, kept waiting at most %lld ms\n", runs, worst);
    int ok = worst <= 250;
    printf("mmcsstest: busy MMCSS threads %s a NORMAL thread (longest wait %lld ms)\n",
           ok ? "leave room for" : "starve", worst);
    return ok;
}

int main(void)
{
    g_qit = (NtQitFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    QueryPerformanceFrequency(&g_freq);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int nspin = si.dwNumberOfProcessors > MAX_SPIN ? MAX_SPIN : (int)si.dwNumberOfProcessors;
    printf("Task priorities:\n");
    int ok = check_priorities();
    printf("A thread woken every 5 ms, a busy TIME_CRITICAL thread on every processor:\n");
    fflush(stdout);
    ok &= check_latency(nspin);
    printf("A busy \"Pro Audio\" thread on every processor for a second:\n");
    fflush(stdout);
    ok &= check_budget(nspin);
    printf("mmcsstest: %s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    return ok ? 0 : 1;
}
