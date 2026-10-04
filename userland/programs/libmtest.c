/*
 * libmtest.exe — the 32-bit C runtime's math entry points that MSVC's
 * compiler calls instead of the C names (SDL2 and other x86 builds import
 * them from ucrtbase.dll through api-ms-win-crt-math):
 *
 *   _libm_sse2_NAME_precise   /arch:SSE2 code: the argument in xmm0 (pow: xmm0
 *                             and xmm1), the result in xmm0
 *   _CINAME                   x87 code: the arguments on the x87 stack (the
 *                             first in st(1)), the result in st(0)
 *
 * The 64-bit runtime has neither (x64 code calls the C names).
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

static int near(double a, double b) { return fabs(a - b) <= 1e-12 * (fabs(b) > 1 ? fabs(b) : 1); }

#ifndef _WIN64
static double sse2(FARPROC f, double x, double y)
{
    double r;
    __asm__ volatile("movsd %1, %%xmm0\n\tmovsd %2, %%xmm1\n\tcall *%3\n\tmovsd %%xmm0, %0"
                     : "=m"(r) : "m"(x), "m"(y), "r"(f)
                     : "eax", "ecx", "edx", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "memory");
    return r;
}

static double x87_1(FARPROC f, double x)
{
    double r;
    __asm__ volatile("fldl %1\n\tcall *%2\n\tfstpl %0"
                     : "=m"(r) : "m"(x), "r"(f)
                     : "eax", "ecx", "edx", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "memory");
    return r;
}

static double x87_2(FARPROC f, double x, double y)
{
    double r;
    __asm__ volatile("fldl %1\n\tfldl %2\n\tcall *%3\n\tfstpl %0"
                     : "=m"(r) : "m"(x), "m"(y), "r"(f)
                     : "eax", "ecx", "edx", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "memory");
    return r;
}

typedef double (*Fn1)(double);
static const struct { const char *name; Fn1 c; } ONE[] = {
    { "acos", acos }, { "asin", asin }, { "atan", atan }, { "cos", cos }, { "exp", exp },
    { "log", log }, { "log10", log10 }, { "sin", sin }, { "sqrt", sqrt }, { "tan", tan },
};
#endif

int main(void)
{
#ifndef _WIN64
    HMODULE u = LoadLibraryA("ucrtbase.dll");
    check(u != NULL, "ucrtbase.dll");
    char name[64], what[96];
    for (unsigned i = 0; u && i < sizeof(ONE) / sizeof(ONE[0]); i++) {
        double x = 0.375;
        snprintf(name, sizeof(name), "_libm_sse2_%s_precise", ONE[i].name);
        FARPROC f = GetProcAddress(u, name);
        snprintf(what, sizeof(what), "%s(%g)", name, x);
        check(f && near(sse2(f, x, 0), ONE[i].c(x)), what);
        snprintf(name, sizeof(name), "_CI%s", ONE[i].name);
        f = GetProcAddress(u, name);
        snprintf(what, sizeof(what), "%s(%g)", name, x);
        check(f && near(x87_1(f, x), ONE[i].c(x)), what);
    }
    FARPROC f = u ? GetProcAddress(u, "_libm_sse2_pow_precise") : NULL;
    check(f && near(sse2(f, 1.5, 2.5), pow(1.5, 2.5)), "_libm_sse2_pow_precise(1.5, 2.5)");
    f = u ? GetProcAddress(u, "_CIpow") : NULL;
    check(f && near(x87_2(f, 1.5, 2.5), pow(1.5, 2.5)), "_CIpow(1.5, 2.5)");
    f = u ? GetProcAddress(u, "_CIfmod") : NULL;
    check(f && near(x87_2(f, 7.5, 2.0), 1.5), "_CIfmod(7.5, 2)");
    f = u ? GetProcAddress(u, "_CIatan2") : NULL;
    check(f && near(x87_2(f, 1.0, -1.0), atan2(1.0, -1.0)), "_CIatan2(1, -1)");
    f = u ? GetProcAddress(u, "_CIsinh") : NULL;
    check(f && near(x87_1(f, 0.5), sinh(0.5)), "_CIsinh(0.5)");
#else
    printf("(the 64-bit runtime has no x86 math entry points)\n");
#endif
    printf("libmtest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
