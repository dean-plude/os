/*
 * smpstress.exe — works the kernel's multiprocessor locking from many
 * threads at once and checks the results:
 *
 *   1. critical sections: a plain counter, incremented under one lock by
 *      every thread (contention parks threads on the kernel event);
 *   2. events: pairs of threads ping-pong through two auto-reset events;
 *   3. semaphores: producers release, consumers wait, the counts balance;
 *   4. memory: threads allocate, fill, check and free their own blocks;
 *   5. handles: threads create and close events as fast as they can;
 *   6. a system call writing into memory that another thread frees at the
 *      same time: the kernel must fail the call, not crash;
 *   7. throughput: files (open, write, read back, close) and the registry
 *      (open a key, set and read a value, close), each thread on its own
 *      file or key, by one thread and then by one per CPU.  With
 *      "smpstress scaling X" it fails unless both scale by X or more.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ITER 3000

static int g_threads;
static CRITICAL_SECTION g_cs;
static volatile long g_plain;                 /* only touched under g_cs */
static volatile long g_bad;

static DWORD WINAPI cs_worker(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < ITER; i++) {
        EnterCriticalSection(&g_cs);
        long v = g_plain;
        if (i % 64 == 0) SwitchToThread();        /* hold it a while sometimes */
        g_plain = v + 1;
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

typedef struct { HANDLE ping, pong; int side; long count; } Pair;

static DWORD WINAPI pingpong(LPVOID arg)
{
    Pair *p = arg;
    for (int i = 0; i < ITER / 3; i++) {
        if (p->side == 0) {
            SetEvent(p->ping);
            if (WaitForSingleObject(p->pong, 5000) != WAIT_OBJECT_0) { InterlockedIncrement(&g_bad); return 1; }
        } else {
            if (WaitForSingleObject(p->ping, 5000) != WAIT_OBJECT_0) { InterlockedIncrement(&g_bad); return 1; }
            SetEvent(p->pong);
        }
        InterlockedIncrement(&p->count);
    }
    return 0;
}

static HANDLE g_sem;
static volatile long g_produced, g_consumed;

static DWORD WINAPI producer(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < ITER; i++) {
        InterlockedIncrement(&g_produced);
        if (!ReleaseSemaphore(g_sem, 1, NULL)) { InterlockedDecrement(&g_produced); SwitchToThread(); i--; }
    }
    return 0;
}

static DWORD WINAPI consumer(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < ITER; i++) {
        if (WaitForSingleObject(g_sem, 10000) != WAIT_OBJECT_0) { InterlockedIncrement(&g_bad); return 1; }
        InterlockedIncrement(&g_consumed);
    }
    return 0;
}

static DWORD WINAPI mem_worker(LPVOID arg)
{
    unsigned seed = (unsigned)(ULONG_PTR)arg * 2654435761u;
    for (int i = 0; i < ITER / 10; i++) {
        seed = seed * 1103515245u + 12345u;
        SIZE_T n = 4096 * (1 + seed % 16);
        unsigned char *b = VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!b) { InterlockedIncrement(&g_bad); return 1; }
        for (SIZE_T k = 0; k < n; k += 512) b[k] = (unsigned char)(k ^ seed);
        for (SIZE_T k = 0; k < n; k += 512) if (b[k] != (unsigned char)(k ^ seed)) { InterlockedIncrement(&g_bad); break; }
        VirtualFree(b, 0, MEM_RELEASE);
    }
    return 0;
}

static DWORD WINAPI handle_worker(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < ITER / 3; i++) {
        HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!e) { InterlockedIncrement(&g_bad); return 1; }
        SetEvent(e);
        if (WaitForSingleObject(e, 0) != WAIT_OBJECT_0) InterlockedIncrement(&g_bad);
        CloseHandle(e);
    }
    return 0;
}

/* 6: one thread keeps freeing and re-allocating a page at a fixed address;
 * others ask the kernel to write there meanwhile */
static void *volatile g_racy;
static volatile long g_race_stop, g_race_ok, g_race_fault;

static DWORD WINAPI race_freer(LPVOID arg)
{
    (void)arg;
    void *base = g_racy;
    while (!g_race_stop) {
        VirtualFree(base, 0, MEM_RELEASE);
        if (!VirtualAlloc(base, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) SwitchToThread();
    }
    return 0;
}

static DWORD WINAPI race_writer(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < ITER; i++) {
        LARGE_INTEGER *p = (LARGE_INTEGER *)g_racy;
        /* NtQueryPerformanceCounter copies its result out: into memory that
         * may be gone by then */
        typedef LONG (WINAPI *QPC)(LARGE_INTEGER *, LARGE_INTEGER *);
        static QPC q;
        if (!q) q = (QPC)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryPerformanceCounter");
        LONG st = q(p, NULL);
        if (st == 0) InterlockedIncrement(&g_race_ok); else InterlockedIncrement(&g_race_fault);
    }
    return 0;
}

/* 7: throughput.  Each worker repeats its operation on a file or key of
 * its own until g_tp_stop, counting in g_tp_ops; any failure counts in
 * g_bad. */
#define TP_MS     2000
#define TP_CHUNK  4096
#define TP_CHUNKS 4
static volatile long g_tp_stop, g_tp_ops;

static DWORD WINAPI file_worker(LPVOID arg)
{
    char path[MAX_PATH], buf[TP_CHUNK], back[TP_CHUNK];
    int id = (int)(ULONG_PTR)arg;
    wsprintfA(path, "C:\\Temp\\smpstress\\f%d.dat", id);
    for (int i = 0; i < TP_CHUNK; i++) buf[i] = (char)(i * 7 + id);
    while (!g_tp_stop) {
        HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) { InterlockedIncrement(&g_bad); return 1; }
        DWORD n;
        int ok = 1;
        for (int k = 0; k < TP_CHUNKS; k++) ok &= WriteFile(f, buf, TP_CHUNK, &n, NULL) && n == TP_CHUNK;
        SetFilePointer(f, 0, NULL, FILE_BEGIN);
        for (int k = 0; k < TP_CHUNKS; k++) ok &= ReadFile(f, back, TP_CHUNK, &n, NULL) && n == TP_CHUNK && !memcmp(buf, back, TP_CHUNK);
        CloseHandle(f);
        if (!ok) { InterlockedIncrement(&g_bad); return 1; }
        InterlockedIncrement(&g_tp_ops);
    }
    return 0;
}

static DWORD WINAPI reg_worker(LPVOID arg)
{
    char path[80];
    int id = (int)(ULONG_PTR)arg;
    wsprintfA(path, "Software\\NovaOS\\smpstress\\k%d", id);
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL)) { InterlockedIncrement(&g_bad); return 1; }
    RegCloseKey(k);
    for (DWORD v = 0; !g_tp_stop; v++) {
        DWORD got = 0, size = sizeof(got), type = 0;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k)) { InterlockedIncrement(&g_bad); return 1; }
        int ok = !RegSetValueExA(k, "n", 0, REG_DWORD, (const BYTE *)&v, sizeof(v)) &&
                 !RegQueryValueExA(k, "n", NULL, &type, (BYTE *)&got, &size) && type == REG_DWORD && got == v;
        RegCloseKey(k);
        if (!ok) { InterlockedIncrement(&g_bad); return 1; }
        InterlockedIncrement(&g_tp_ops);
    }
    return 0;
}

/* Operations a second by @n threads of @fn */
static double throughput(LPTHREAD_START_ROUTINE fn, int n)
{
    HANDLE h[64];
    g_tp_stop = 0;
    g_tp_ops = 0;
    for (int i = 0; i < n; i++) h[i] = CreateThread(NULL, 0, fn, (void *)(ULONG_PTR)i, 0, NULL);
    Sleep(200);                                     /* (all started and warm) */
    long ops0 = g_tp_ops;
    DWORD t0 = GetTickCount();
    Sleep(TP_MS);
    long ops = g_tp_ops - ops0;
    DWORD ms = GetTickCount() - t0;
    g_tp_stop = 1;
    WaitForMultipleObjects((DWORD)n, h, TRUE, INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(h[i]);
    return ms ? ops * 1000.0 / ms : 0;
}

/* @fn's throughput by one thread and by one per CPU; false if it failed
 * or scaled by less than @want (0: no target) */
/* Yardsticks for the two above: plain computation, and a system call that
 * touches nothing shared (NtQuerySystemTime: kernel32 reads the time from
 * shared memory instead) */
static DWORD WINAPI cpu_worker(LPVOID arg)
{
    (void)arg;
    volatile unsigned x = 1;
    while (!g_tp_stop) {
        for (int i = 0; i < 20000; i++) x = x * 1103515245u + 12345u;
        InterlockedIncrement(&g_tp_ops);
    }
    return 0;
}

typedef LONG (WINAPI *QST)(LARGE_INTEGER *);
static QST g_qst;
static DWORD WINAPI call_worker(LPVOID arg)
{
    (void)arg;
    LARGE_INTEGER c;
    while (!g_tp_stop) {
        for (int i = 0; i < 16; i++) g_qst(&c);
        InterlockedIncrement(&g_tp_ops);
    }
    return 0;
}

static int g_only_many;                             /* "smpstress throughput X NAME many|N": that many threads only */
static int cpus_of(const SYSTEM_INFO *si) { return si->dwNumberOfProcessors > 16 ? 16 : (int)si->dwNumberOfProcessors; }

static int scaling(const char *name, LPTHREAD_START_ROUTINE fn, int cpus, double want)
{
    long bad0 = g_bad;
    if (g_only_many) { printf("  %-16s %2d threads %8.0f/s\n", name, g_only_many, throughput(fn, g_only_many)); return 1; }
    double one = throughput(fn, 1), all = throughput(fn, cpus);
    double x = one > 0 ? all / one : 0;
    printf("  %-16s 1 thread %8.0f/s, %2d threads %8.0f/s: %.2fx\n", name, one, cpus, all, x);
    if (g_bad != bad0) { printf("  %s: %ld failed\n", name, g_bad - bad0); return 0; }
    return !(want > 0 && x < want);
}

static void run(const char *name, LPTHREAD_START_ROUTINE fn, int n, void **args)
{
    HANDLE h[64];
    DWORD t0 = GetTickCount();
    for (int i = 0; i < n; i++) h[i] = CreateThread(NULL, 0, fn, args ? args[i] : (void *)(ULONG_PTR)i, 0, NULL);
    WaitForMultipleObjects((DWORD)n, h, TRUE, INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(h[i]);
    printf("  %-16s %2d threads, %5lu ms\n", name, n, GetTickCount() - t0);
}

int main(int argc, char **argv)
{
    /* "smpstress scaling X": also require X times one thread's file and
     * registry throughput; "smpstress throughput [X]": only measure that */
    BOOL only_tp = argc > 1 && !strcmp(argv[1], "throughput");
    double want = argc > 2 && (only_tp || !strcmp(argv[1], "scaling")) ? atof(argv[2]) : 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_threads = (int)si.dwNumberOfProcessors * 2;
    if (g_threads < 4) g_threads = 4;
    if (g_threads > 16) g_threads = 16;
    printf("smpstress: %lu CPUs\n", si.dwNumberOfProcessors);
    int pass = 0, fail = 0;

    if (only_tp) goto throughput;
    InitializeCriticalSection(&g_cs);
    run("critical section", cs_worker, g_threads, NULL);
    if (g_plain == (long)g_threads * ITER) pass++; else { fail++; printf("  counter %ld, expected %ld\n", g_plain, (long)g_threads * ITER); }

    Pair pairs[8];
    void *pa[16];
    int np = g_threads / 2;
    for (int i = 0; i < np; i++) {
        pairs[i].ping = CreateEventA(NULL, FALSE, FALSE, NULL);
        pairs[i].pong = CreateEventA(NULL, FALSE, FALSE, NULL);
        pairs[i].count = 0;
    }
    Pair sides[16];
    for (int i = 0; i < np; i++) {
        sides[2 * i] = pairs[i]; sides[2 * i].side = 0;
        sides[2 * i + 1] = pairs[i]; sides[2 * i + 1].side = 1;
        pa[2 * i] = &sides[2 * i]; pa[2 * i + 1] = &sides[2 * i + 1];
    }
    long bad0 = g_bad;
    run("event ping-pong", pingpong, np * 2, pa);
    int ok = g_bad == bad0;
    for (int i = 0; i < np * 2; i++) if (sides[i].count != ITER / 3) ok = 0;
    if (ok) pass++; else { fail++; printf("  ping-pong lost a wake-up\n"); }

    g_sem = CreateSemaphoreA(NULL, 0, 64, NULL);
    HANDLE h[32];
    DWORD t0 = GetTickCount();
    int half = g_threads / 2;
    for (int i = 0; i < half; i++) h[i] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    for (int i = 0; i < half; i++) h[half + i] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    WaitForMultipleObjects((DWORD)(2 * half), h, TRUE, INFINITE);
    for (int i = 0; i < 2 * half; i++) CloseHandle(h[i]);
    printf("  %-16s %2d threads, %5lu ms\n", "semaphore", 2 * half, GetTickCount() - t0);
    if (g_produced == (long)half * ITER && g_consumed == (long)half * ITER && WaitForSingleObject(g_sem, 0) == WAIT_TIMEOUT) pass++;
    else { fail++; printf("  semaphore: produced %ld consumed %ld\n", g_produced, g_consumed); }

    bad0 = g_bad;
    run("memory", mem_worker, g_threads, NULL);
    if (g_bad == bad0) pass++; else { fail++; printf("  memory: %ld bad\n", g_bad - bad0); }

    bad0 = g_bad;
    run("handles", handle_worker, g_threads, NULL);
    if (g_bad == bad0) pass++; else { fail++; printf("  handles: %ld bad\n", g_bad - bad0); }

    g_racy = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE fr = CreateThread(NULL, 0, race_freer, NULL, 0, NULL);
    run("free while copying", race_writer, 3, NULL);
    g_race_stop = 1;
    WaitForSingleObject(fr, INFINITE);
    CloseHandle(fr);
    printf("  (%ld copies landed, %ld refused: the page was gone)\n", g_race_ok, g_race_fault);
    if (g_race_ok + g_race_fault == 3L * ITER) pass++; else fail++;

throughput:
    CreateDirectoryA("C:\\Temp", NULL);
    CreateDirectoryA("C:\\Temp\\smpstress", NULL);
    int cpus = (int)si.dwNumberOfProcessors;
    if (cpus > 16) cpus = 16;
    const char *only = only_tp && argc > 3 ? argv[3] : NULL;    /* "smpstress throughput X files" */
    g_only_many = only && argc > 4 ? (!strcmp(argv[4], "many") ? cpus_of(&si) : atoi(argv[4])) : 0;
    g_qst = (QST)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemTime");
    if (!only) {
        scaling("(computing)", cpu_worker, cpus, 0);
        if (g_qst) scaling("(system calls)", call_worker, cpus, 0);
    }
    if (!only || !strcmp(only, "files")) { if (scaling("files", file_worker, cpus, want)) pass++; else fail++; }
    if (!only || !strcmp(only, "registry")) { if (scaling("registry", reg_worker, cpus, want)) pass++; else fail++; }
    if (want > 0) printf("  (target: %.1fx on %d CPUs)\n", want, cpus);

    printf("smpstress: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
