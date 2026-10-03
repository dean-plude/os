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
 *
 * Then the foreground process.  Run from the Terminal (the Terminal
 * active), this console program is the foreground process without a
 * window of its own, as a console's programs are on Windows.  Its threads
 * get three times the time slice: with twice as many busy NORMAL threads
 * as processors, each timing its turns with the performance counter, they
 * run 60 ms at a time, a background copy's ("prioritytest slices") 20 ms
 * (NT's quantum stretching, "Programs" in System Properties).
 *
 * Then the foreground boost, as the Terminal's console program and again
 * with a window of its own active: this process
 * is the foreground one (NtQueryInformationProcess(ProcessPriorityClass)
 * says so), and a thread of it woken by an event gets 2 more on top of the
 * event's boost (NT's PsPrioritySeparation on client Windows).  A copy of
 * this program in the background (no window) runs a THREAD_PRIORITY_
 * HIGHEST (10) busy thread on every processor: a woken NORMAL thread of
 * the foreground process (8 + 1 + 2 = 11) preempts them at once, one of
 * the background process (8 + 1 = 9) waits for the balance set.
 *
 * "prioritytest net" (the network suite: the core boot has no network
 * adapter) checks the kernel's network thread runs above programs: with a
 * HIGH_PRIORITY_CLASS busy thread on every processor, a byte sent over a
 * 127.0.0.1 TCP connection and back takes at most 250 ms every time (with
 * the network thread at 8, as before, it waited for the balance set: 3 s).
 */
#include <winsock2.h>
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

/* -------------------------------------------------------------------- */
typedef LONG (WINAPI *NtQipFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

/* PROCESS_PRIORITY_CLASS.Foreground of this process (-1: no answer) */
static int is_foreground(void)
{
    NtQipFn qip = (NtQipFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    UCHAR v[2] = { 0xFF, 0 };
    ULONG got = 0;
    return qip && qip(GetCurrentProcess(), 18, v, sizeof(v), &got) >= 0 ? v[0] : -1;
}

static LRESULT CALLBACK fg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(10);
    }
}

/* The background copy ("prioritytest bg"): a thread waiting on the
 * "prioritytest-bg-ev" event, which answers with "prioritytest-bg-back",
 * and a THREAD_PRIORITY_HIGHEST (10) busy thread on every processor until
 * the foreground process sets "prioritytest-done" */
static HANDLE g_done;

static DWORD WINAPI bg_spin(LPVOID p)
{
    (void)p;
    DWORD start = GetTickCount();
    for (;;) {
        for (volatile int i = 0; i < 100000; i++) {}
        if (WaitForSingleObject(g_done, 0) == WAIT_OBJECT_0 || GetTickCount() - start > 30000) return 0;
    }
}

static DWORD WINAPI bg_waiter(LPVOID p)
{
    HANDLE ev = OpenEventA(EVENT_ALL_ACCESS, FALSE, "prioritytest-bg-ev");
    HANDLE back = OpenEventA(EVENT_ALL_ACCESS, FALSE, "prioritytest-bg-back");
    SetEvent((HANDLE)p);
    if (!ev || !back) return 1;
    WaitForSingleObject(ev, 30000);
    SetEvent(back);
    return 0;
}

static int background(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int nspin = si.dwNumberOfProcessors > MAX_SPIN ? MAX_SPIN : (int)si.dwNumberOfProcessors;
    HANDLE ready = OpenEventA(EVENT_ALL_ACCESS, FALSE, "prioritytest-ready");
    g_done = OpenEventA(SYNCHRONIZE, FALSE, "prioritytest-done");
    if (!ready || !g_done) return 1;
    HANDLE up = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE w = CreateThread(NULL, 0, bg_waiter, up, 0, NULL);
    WaitForSingleObject(up, 5000);                          /* (waiting before the busy threads start) */
    HANDLE th[MAX_SPIN];
    for (int i = 0; i < nspin; i++) {
        th[i] = CreateThread(NULL, 0, bg_spin, NULL, CREATE_SUSPENDED, NULL);
        SetThreadPriority(th[i], THREAD_PRIORITY_HIGHEST);
    }
    SetEvent(ready);
    for (int i = 0; i < nspin; i++) ResumeThread(th[i]);
    WaitForMultipleObjects(nspin, th, TRUE, 60000);
    WaitForSingleObject(w, 5000);
    return 0;
}

/* The foreground boost: this process's window active, a background
 * process with a THREAD_PRIORITY_HIGHEST (10) busy thread on every
 * processor.  A NORMAL thread of this process woken by an event runs at
 * 8 + 1 + 2 = 11 and preempts them at once; the background process's
 * NORMAL thread, at 8 + 1 = 9, waits until the balance set lifts it
 * (seconds). */
static int check_foreground(int own_window)
{
    HWND h = NULL;
    if (own_window) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = fg_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "PriorityTestFg";
        RegisterClassA(&wc);
        h = CreateWindowExA(0, "PriorityTestFg", "prioritytest (foreground)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            100, 100, 320, 160, NULL, NULL, wc.hInstance, NULL);
        SetForegroundWindow(h);
        pump(200);                                          /* (the desktop notices at its next tick) */
    }
    int fg = is_foreground();

    HANDLE ready = CreateEventA(NULL, FALSE, FALSE, "prioritytest-ready");
    HANDLE done = CreateEventA(NULL, TRUE, FALSE, "prioritytest-done");
    HANDLE bg_ev = CreateEventA(NULL, FALSE, FALSE, "prioritytest-bg-ev");
    HANDLE bg_back = CreateEventA(NULL, FALSE, FALSE, "prioritytest-bg-back");
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_back = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE w = CreateThread(NULL, 0, waiter, NULL, 0, NULL);    /* (waiting before the busy threads start) */
    WaitForSingleObject(g_back, 5000);
    LONG prio = priority_of(w);

    char exe[MAX_PATH], cmd[MAX_PATH + 8];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    snprintf(cmd, sizeof(cmd), "\"%s\" bg", exe);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("  CreateProcess(%s) failed: %lu\n", cmd, GetLastError());
        if (h) DestroyWindow(h);
        return 0;
    }
    LONGLONG med = -1, p95 = -1, bg = -1;
    if (WaitForSingleObject(ready, 30000) == WAIT_OBJECT_0) {
        Sleep(100);                                         /* (the busy threads are running) */
        for (int i = 0; i < ROUNDS; i++) {
            Sleep(1);
            QueryPerformanceCounter(&g_set_at);
            SetEvent(g_ev);
            if (WaitForSingleObject(g_back, 10000) != WAIT_OBJECT_0) { g_woke[i] = 10000000; break; }
        }
        qsort(g_woke, ROUNDS, sizeof(g_woke[0]), cmp_ll);
        med = g_woke[ROUNDS / 2];
        p95 = g_woke[ROUNDS * 95 / 100];
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        SetEvent(bg_ev);
        if (WaitForSingleObject(bg_back, 15000) == WAIT_OBJECT_0) {
            QueryPerformanceCounter(&t1);
            bg = (t1.QuadPart - t0.QuadPart) * 1000000 / g_freq.QuadPart;
        }
    }
    int still_fg = is_foreground();
    SetEvent(done);
    WaitForSingleObject(w, 5000);
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(ready);
    CloseHandle(done); CloseHandle(bg_ev); CloseHandle(bg_back); CloseHandle(g_ev); CloseHandle(g_back);
    if (h) { DestroyWindow(h); pump(200); }
    printf("  this process (%s): Foreground %d (and %d after); its NORMAL waiter (priority %ld) runs after: "
           "median %lld us, 95%% %lld us\n", own_window ? "its own window active" : "a console program, its Terminal active",
           fg, still_fg, prio, med, p95);
    printf("  the background process's NORMAL waiter runs after %lld us\n", bg);
    int ok = fg == 1 && still_fg == 1 && prio == 8 && p95 >= 0 && p95 <= 3000 && (bg < 0 || bg >= 100000);
    printf("prioritytest: the foreground %s's woken thread runs within %lld.%03lld ms against busy "
           "priority-10 threads of a background process (its own: %lld ms)\n",
           own_window ? "process" : "console program", p95 / 1000, p95 % 1000, bg < 0 ? -1 : bg / 1000);
    return ok;
}

/* -------------------------------------------------------------------- */
#define NET_ROUNDS 40

/* With a HIGH_PRIORITY_CLASS busy thread on every processor, a byte sent
 * over a loopback TCP connection and back must arrive in bounded time: the
 * kernel's network thread runs above every program thread */
static int check_network(void)
{
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) { printf("  WSAStartup failed\n"); return 0; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(a);
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0), c = socket(AF_INET, SOCK_STREAM, 0), s = INVALID_SOCKET;
    if (l != INVALID_SOCKET && !bind(l, (struct sockaddr *)&a, sizeof(a)) && !listen(l, 1) &&
        !getsockname(l, (struct sockaddr *)&a, &len) && !connect(c, (struct sockaddr *)&a, sizeof(a)))
        s = accept(l, NULL, NULL);
    if (s == INVALID_SOCKET) { printf("  no loopback connection (error %d)\n", WSAGetLastError()); return 0; }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int nspin = si.dwNumberOfProcessors > MAX_SPIN ? MAX_SPIN : (int)si.dwNumberOfProcessors;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    g_stop = 0;
    HANDLE th[MAX_SPIN];
    for (int i = 0; i < nspin; i++) th[i] = CreateThread(NULL, 0, spin, (LPVOID)(INT_PTR)i, 0, NULL);
    Sleep(50);
    LONGLONG took[NET_ROUNDS];
    int n = 0, ok = 1;
    for (; n < NET_ROUNDS; n++) {
        char b = (char)n, r = 0;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        fd_set f;
        FD_ZERO(&f);
        FD_SET(s, &f);
        struct timeval tv = { 10, 0 };
        /* (there and back: each byte carries the ACK of the one before, so
         * Nagle's algorithm never holds one back for the delayed ACK) */
        if (send(c, &b, 1, 0) != 1 || select(0, &f, NULL, NULL, &tv) != 1 || recv(s, &r, 1, 0) != 1 || r != b ||
            send(s, &r, 1, 0) != 1 || recv(c, &r, 1, 0) != 1 || r != b) {
            printf("  round %d: the byte did not come back (error %d)\n", n, WSAGetLastError());
            ok = 0;
            break;
        }
        QueryPerformanceCounter(&t1);
        took[n] = (t1.QuadPart - t0.QuadPart) * 1000000 / g_freq.QuadPart;
        if (took[n] > 1000000) { n++; ok = 0; break; }      /* (starved: no need to wait out every round) */
        Sleep(n % 3);                                        /* (sometimes at once, sometimes after a sleep) */
    }
    g_stop = 1;
    WaitForMultipleObjects(nspin, th, TRUE, 5000);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    closesocket(s); closesocket(c); closesocket(l);
    WSACleanup();
    if (!n) return 0;
    qsort(took, n, sizeof(took[0]), cmp_ll);
    LONGLONG med = took[n / 2], worst = took[n - 1];
    if (worst > 250000) ok = 0;
    printf("prioritytest: %d loopback round trips against %d busy HIGH threads: median %lld.%03lld ms, worst %lld.%03lld ms\n",
           n, nspin, med / 1000, med % 1000, worst / 1000, worst % 1000);
    return ok;
}

/* -------------------------------------------------------------------- */
/* Time slices: twice as many busy NORMAL threads as processors, each
 * timing itself with the performance counter.  A gap of over 5 ms is
 * another busy thread's turn, so a stretch between two gaps is one of
 * this thread's slices (shorter interruptions, a kernel thread's, count
 * as running: a thread preempted goes on with its slice).  The median
 * slice of the foreground process's threads is 60 ms (NT's quantum
 * stretching: three times the background's 20 ms).  (A processor left
 * with one busy thread gives it no gaps, and no slices to count.) */
#define SLICE_MS   1500
#define SLICE_GAP  5000             /* us */
#define SLICE_MAX  128
static volatile LONG g_go;
static LONGLONG g_slices[MAX_SPIN][SLICE_MAX];
static int g_nslices[MAX_SPIN];

static DWORD WINAPI slicer(LPVOID p)
{
    int i = (int)(INT_PTR)p;
    while (!g_go) {}
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    LONGLONG prev = t.QuadPart, end = prev + g_freq.QuadPart * SLICE_MS / 1000, gap = g_freq.QuadPart * SLICE_GAP / 1000000;
    LONGLONG since = -1;                    /* the current slice's start (-1: before the first gap) */
    int n = 0;
    while (prev < end) {
        QueryPerformanceCounter(&t);
        if (t.QuadPart - prev > gap) {
            if (since >= 0 && n < SLICE_MAX) g_slices[i][n++] = (prev - since) * 1000000 / g_freq.QuadPart;
            since = t.QuadPart;
        }
        prev = t.QuadPart;
    }
    g_nslices[i] = n;
    return 0;
}

/* This process's threads' median time slice, in microseconds (-1: no
 * thread waited for another) */
static LONGLONG measure_slices(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = 2 * (int)si.dwNumberOfProcessors;
    if (n > MAX_SPIN) n = MAX_SPIN;
    HANDLE th[MAX_SPIN];
    g_go = 0;
    for (int i = 0; i < n; i++) { g_nslices[i] = 0; th[i] = CreateThread(NULL, 0, slicer, (LPVOID)(INT_PTR)i, 0, NULL); }
    Sleep(50);
    g_go = 1;
    WaitForMultipleObjects(n, th, TRUE, 30000);
    static LONGLONG all[MAX_SPIN * SLICE_MAX];
    int total = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < g_nslices[i]; k++) all[total++] = g_slices[i][k];
        CloseHandle(th[i]);
    }
    if (!total) return -1;
    qsort(all, total, sizeof(all[0]), cmp_ll);
    return all[total / 2];
}

/* The background copy ("prioritytest slices"): its slice in microseconds
 * as its exit code (1: it was the foreground process, which it must not be) */
static int background_slices(void)
{
    if (is_foreground() != 0) return 1;
    LONGLONG us = measure_slices();
    return us < 2 ? 2 : (int)us;
}

static int check_slices(void)
{
    LONGLONG fg = measure_slices(), bg = -1;
    char exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    snprintf(cmd, sizeof(cmd), "\"%s\" slices", exe);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        DWORD code = 0;
        if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code))
            bg = code == 1 ? -2 : (LONGLONG)code;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    printf("  this process (foreground): %lld us a slice; a background process: %lld us%s\n", fg, bg,
           bg == -2 ? " (it was the foreground process)" : "");
    int ok = fg >= 40000 && bg > 0 && bg <= 30000 && fg >= 2 * bg;
    printf("prioritytest: the foreground process's threads run %lld ms at a time, a background process's %lld ms\n",
           fg / 1000, bg / 1000);
    return ok;
}

/* The Terminal is active, so this console program is the foreground
 * process (wait for the desktop to see it started) */
static int wait_foreground(void)
{
    int fg = 0;
    for (int i = 0; i < 100 && (fg = is_foreground()) != 1; i++) Sleep(10);
    return fg;
}

int main(int argc, char **argv)
{
    g_qit = (NtQitFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    QueryPerformanceFrequency(&g_freq);
    if (argc > 1 && !strcmp(argv[1], "bg")) return background();
    if (argc > 1 && !strcmp(argv[1], "slices")) return background_slices();
    if (argc > 1 && !strcmp(argv[1], "net")) {
        printf("The network thread against a busy HIGH_PRIORITY_CLASS thread on every processor:\n");
        int ok = check_network();
        printf("prioritytest: %s\n", ok ? "PASS" : "FAIL");
        fflush(stdout);
        return ok ? 0 : 1;
    }
    printf("Thread priorities (columns IDLE, LOWEST, BELOW_NORMAL, NORMAL, ABOVE_NORMAL, HIGHEST, TIME_CRITICAL):\n");
    int ok = check_queries();
    printf("Scheduling, wake-up boosts off, a busy NORMAL thread on every processor:\n");
    ok &= check_scheduling();
    int con_fg = wait_foreground();
    printf("A console program in the Terminal, the Terminal active: Foreground %d\n", con_fg);
    ok &= con_fg == 1;
    printf("Time slices, twice as many busy NORMAL threads as processors:\n");
    fflush(stdout);
    ok &= check_slices();
    printf("The foreground boost of the Terminal's console program, a busy HIGHEST thread of a background process on every processor:\n");
    fflush(stdout);
    ok &= check_foreground(0);
    printf("The foreground boost, its own window active, a busy HIGHEST thread of a background process on every processor:\n");
    fflush(stdout);
    ok &= check_foreground(1);
    printf("prioritytest: %s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    return ok ? 0 : 1;
}
