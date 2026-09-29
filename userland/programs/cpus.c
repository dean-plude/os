/*
 * cpus.exe [threads] — shows the processors and checks that threads run on
 * them at the same time: the same amount of work per thread, first on one
 * thread, then on several at once.  With N CPUs, N threads should take
 * about as long as one (speedup close to N).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static volatile unsigned long long g_sink;

static unsigned long long work(void)
{
    /* integer work only: no system calls, no shared memory */
    unsigned long long x = 88172645463325252ULL, sum = 0;
    for (int i = 0; i < 120000000; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        sum += x & 0xFF;
    }
    return sum;
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    g_sink += work();
    return 0;
}

static double now_s(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
}

int main(int argc, char **argv)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = argc > 1 ? atoi(argv[1]) : (int)si.dwNumberOfProcessors;
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    printf("Processors: %lu\n", si.dwNumberOfProcessors);

    double t0 = now_s();
    worker(NULL);
    double one = now_s() - t0;
    printf("1 thread:  %.2f s\n", one);

    HANDLE h[64];
    t0 = now_s();
    for (int i = 0; i < n; i++) h[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForMultipleObjects((DWORD)n, h, TRUE, INFINITE);
    double many = now_s() - t0;
    for (int i = 0; i < n; i++) CloseHandle(h[i]);
    printf("%d threads: %.2f s  (speedup %.1fx)\n", n, many, many > 0 ? n * one / many : 0.0);
    return 0;
}
