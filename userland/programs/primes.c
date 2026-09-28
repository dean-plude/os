/* primes.exe [N] — sieve of Eratosthenes, timed with QueryPerformanceCounter */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(int argc, char **argv)
{
    long n = argc > 1 ? strtol(argv[1], 0, 10) : 1000000;
    if (n < 2 || n > 100000000) { fprintf(stderr, "usage: primes [2..100000000]\n"); return 1; }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    char *sieve = malloc((size_t)n + 1);
    if (!sieve) { fprintf(stderr, "out of memory\n"); return 1; }
    memset(sieve, 1, (size_t)n + 1);
    sieve[0] = sieve[1] = 0;
    for (long i = 2; i * i <= n; i++)
        if (sieve[i]) for (long j = i * i; j <= n; j += i) sieve[j] = 0;
    long count = 0, last = 0;
    for (long i = 2; i <= n; i++) if (sieve[i]) { count++; last = i; }
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    printf("%ld primes up to %ld (largest %ld) in %.1f ms\n", count, n, last, ms);
    free(sieve);
    return 0;
}
