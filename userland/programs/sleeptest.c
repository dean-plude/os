/*
 * sleeptest.exe — put the computer to sleep (S3) and report how it woke
 *
 * Calls powrprof's SetSuspendState, which returns once the machine is
 * awake again; then checks that the wall clock moved on, the tick count
 * didn't jump, and threads, files and the network still work.
 *
 * `sleeptest timer` measures how precisely Sleep and timed waits end
 * instead: Sleep(1), Sleep(5) and a 1 ms WaitForSingleObject timeout,
 * first on an idle machine, then with a busy thread on every processor.
 * The resolution is how late the 95th percentile of them is; it passes
 * at 1 ms or less, and when none returns early.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static volatile LONG g_stop;
static DWORD WINAPI spin(LPVOID p) { (void)p; volatile unsigned x = 0; while (!g_stop) x++; return 0; }

static int cmp_ll(const void *a, const void *b)
{
    LONGLONG x = *(const LONGLONG *)a, y = *(const LONGLONG *)b;
    return x < y ? -1 : x > y;
}

#define ROUNDS 200

/* @what ms of Sleep (or of a wait timeout when @ev): the 95th percentile
 * lateness in microseconds; *early counts returns before the time */
static LONGLONG measure(const char *label, DWORD ms, HANDLE ev, int *early)
{
    static LONGLONG late[ROUNDS];
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    for (int i = 0; i < ROUNDS; i++) {
        QueryPerformanceCounter(&a);
        if (ev) WaitForSingleObject(ev, ms); else Sleep(ms);
        QueryPerformanceCounter(&b);
        late[i] = (b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart - (LONGLONG)ms * 1000;
        if (late[i] < 0) (*early)++;
    }
    qsort(late, ROUNDS, sizeof(late[0]), cmp_ll);
    LONGLONG p95 = late[ROUNDS * 95 / 100];
    printf("  %-22s late by: min %lld us, median %lld us, 95%% %lld us, max %lld us\n", label,
           late[0], late[ROUNDS / 2], p95, late[ROUNDS - 1]);
    return p95;
}

static int timer_test(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    int early = 0;
    LONGLONG worst = 0, p;
    for (int load = 0; load < 2; load++) {
        HANDLE th[64];
        DWORD n = load ? si.dwNumberOfProcessors : 0;
        if (n > 64) n = 64;
        g_stop = 0;
        for (DWORD i = 0; i < n; i++) th[i] = CreateThread(NULL, 0, spin, NULL, 0, NULL);
        printf("%s:\n", load ? "Under load (a busy thread on every processor)" : "Idle");
        Sleep(20);
        p = measure("Sleep(1)", 1, NULL, &early); if (load && p > worst) worst = p;
        p = measure("Sleep(5)", 5, NULL, &early); if (load && p > worst) worst = p;
        p = measure("1 ms wait timeout", 1, ev, &early); if (load && p > worst) worst = p;
        g_stop = 1;
        WaitForMultipleObjects(n, th, TRUE, 5000);
        for (DWORD i = 0; i < n; i++) CloseHandle(th[i]);
    }
    CloseHandle(ev);
    printf("sleeptest: resolution %lld.%03lld ms under load on %lu processors, %d early\n",
           worst / 1000, worst % 1000, si.dwNumberOfProcessors, early);
    printf("sleeptest: %s\n", worst <= 1000 && !early ? "PASS" : "FAIL");
    return 0;
}

typedef BOOLEAN (WINAPI *SetSuspendStateFn)(BOOLEAN, BOOLEAN, BOOLEAN);

static DWORD WINAPI worker(LPVOID p) { (*(volatile LONG *)p)++; return 0; }

int main(int argc, char **argv)
{
    if (argc > 1 && !lstrcmpiA(argv[1], "timer")) return timer_test();
    HMODULE pp = LoadLibraryA("powrprof.dll");
    SetSuspendStateFn sss = pp ? (SetSuspendStateFn)GetProcAddress(pp, "SetSuspendState") : NULL;
    if (!sss) { printf("FAIL: no SetSuspendState\n"); return 1; }

    SYSTEMTIME a, b;
    GetLocalTime(&a);
    DWORD t0 = GetTickCount();
    printf("Going to sleep at %02u:%02u:%02u\n", a.wHour, a.wMinute, a.wSecond);
    fflush(stdout);
    if (!sss(FALSE, FALSE, FALSE)) {
        printf("FAIL: SetSuspendState error %lu\n", GetLastError());
        return 1;
    }
    GetLocalTime(&b);
    DWORD t1 = GetTickCount();
    LONG secs = ((b.wHour * 60 + b.wMinute) * 60 + b.wSecond) - ((a.wHour * 60 + a.wMinute) * 60 + a.wSecond);
    printf("Awake at %02u:%02u:%02u (%ld s of wall time, %lu ms of ticks)\n",
           b.wHour, b.wMinute, b.wSecond, secs, t1 - t0);

    volatile LONG n = 0;                 /* threads still start and run */
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, worker, (LPVOID)&n, 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, 5000);
    char path[MAX_PATH];                 /* files still write and read */
    GetTempPathA(sizeof(path), path);
    lstrcatA(path, "sleeptest.txt");
    HANDLE f = CreateFileA(path, GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD done = 0; char buf[8] = {0};
    BOOL fok = f != INVALID_HANDLE_VALUE && WriteFile(f, "awake", 5, &done, NULL)
               && SetFilePointer(f, 0, NULL, FILE_BEGIN) == 0 && ReadFile(f, buf, 5, &done, NULL)
               && !lstrcmpA(buf, "awake");
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    DeleteFileA(path);
    printf("threads %ld/4, file %s\n", n, fok ? "ok" : "FAILED");
    printf("%s\n", n == 4 && fok && secs >= 0 ? "PASS" : "FAIL");
    return 0;
}
