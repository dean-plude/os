/* msvcrt: <stdlib.h>, errno, assert, program startup support */
#define NOVA_BUILD_MSVCRT
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <windows.h>
#include <assert.h>

/* -----------------------------------------------------------------------
 * errno
 * ----------------------------------------------------------------------- */
static int g_errno;
int *_errno(void) { return &g_errno; }

/* -----------------------------------------------------------------------
 * Memory
 * ----------------------------------------------------------------------- */
void *malloc(size_t n)
{
    void *p = HeapAlloc(GetProcessHeap(), 0, n);
    if (!p) errno = ENOMEM;
    return p;
}

void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) { errno = ENOMEM; return 0; }
    void *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * size);
    if (!p) errno = ENOMEM;
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    void *q = HeapReAlloc(GetProcessHeap(), 0, p, n);
    if (!q) errno = ENOMEM;
    return q;
}

void   free(void *p)    { if (p) HeapFree(GetProcessHeap(), 0, p); }
size_t _msize(void *p)  { return HeapSize(GetProcessHeap(), 0, p); }

/* -----------------------------------------------------------------------
 * Conversions
 * ----------------------------------------------------------------------- */
static unsigned long long parse_ull(const char *s, char **end, int base, int *neg, int *overflow)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    *neg = 0;
    if (*p == '+' || *p == '-') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 0x20) == 'x' && isxdigit((unsigned char)p[2])) { p += 2; base = 16; }
    else if (base == 0) base = *p == '0' ? 8 : 10;
    unsigned long long v = 0;
    const char *start = p;
    *overflow = 0;
    for (;; p++) {
        int d = isdigit((unsigned char)*p) ? *p - '0' : isalpha((unsigned char)*p) ? tolower((unsigned char)*p) - 'a' + 10 : 99;
        if (d >= base) break;
        if (v > (~0ULL - (unsigned)d) / (unsigned)base) *overflow = 1;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return v;
}

long long strtoll(const char *s, char **end, int base)
{
    int neg, of;
    unsigned long long v = parse_ull(s, end, base, &neg, &of);
    if (of || v > (neg ? 0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFULL)) {
        errno = ERANGE;
        return neg ? (long long)0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFLL;
    }
    return neg ? -(long long)v : (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    int neg, of;
    unsigned long long v = parse_ull(s, end, base, &neg, &of);
    if (of) { errno = ERANGE; return ~0ULL; }
    return neg ? -v : v;
}

long strtol(const char *s, char **end, int base)
{
    long long v = strtoll(s, end, base);
    if (v > 0x7FFFFFFFL) { errno = ERANGE; return 0x7FFFFFFFL; }
    if (v < -0x80000000LL) { errno = ERANGE; return (long)-0x80000000LL; }
    return (long)v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long long v = strtoull(s, end, base);
    if (v > 0xFFFFFFFFULL && v < 0xFFFFFFFF00000000ULL) { errno = ERANGE; return 0xFFFFFFFFUL; }
    return (unsigned long)v;
}

int       atoi(const char *s)  { return (int)strtol(s, 0, 10); }
long      atol(const char *s)  { return strtol(s, 0, 10); }
long long atoll(const char *s) { return strtoll(s, 0, 10); }

double strtod(const char *s, char **end)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if (!_strnicmp(p, "inf", 3)) {
        p += _strnicmp(p, "infinity", 8) ? 3 : 8;
        if (end) *end = (char *)p;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if (!_strnicmp(p, "nan", 3)) {
        if (end) *end = (char *)(p + 3);
        return __builtin_nan("");
    }
    double v = 0;
    int digits = 0, exp10 = 0;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p++ - '0'); digits++; }
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) { v = v * 10 + (*p++ - '0'); exp10--; digits++; }
    }
    if (!digits) { if (end) *end = (char *)s; return 0; }
    if ((*p | 0x20) == 'e') {
        const char *q = p + 1;
        int en = 0, eneg = 0, ed = 0;
        if (*q == '+' || *q == '-') eneg = *q++ == '-';
        while (isdigit((unsigned char)*q)) { if (en < 10000) en = en * 10 + (*q - '0'); q++; ed++; }
        if (ed) { exp10 += eneg ? -en : en; p = q; }
    }
    /* scale by 10^exp10 using exact powers where possible */
    double scale = 1;
    int e = exp10 < 0 ? -exp10 : exp10;
    double b = 10;
    while (e) { if (e & 1) scale *= b; b *= b; e >>= 1; }
    v = exp10 < 0 ? v / scale : v * scale;
    if (end) *end = (char *)p;
    if (v == __builtin_inf()) errno = ERANGE;
    return neg ? -v : v;
}

float  strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s)               { return strtod(s, 0); }

static char *utoa_base(unsigned long long v, char *buf, int radix, int neg)
{
    char tmp[72];
    int n = 0;
    if (radix < 2 || radix > 36) { buf[0] = 0; return buf; }
    do { int d = (int)(v % (unsigned)radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)radix; } while (v);
    int o = 0;
    if (neg) buf[o++] = '-';
    while (n) buf[o++] = tmp[--n];
    buf[o] = 0;
    return buf;
}

char *_itoa(int v, char *buf, int radix)
{
    return radix == 10 && v < 0 ? utoa_base((unsigned)(-(long long)v), buf, 10, 1) : utoa_base((unsigned)v, buf, radix, 0);
}
char *_ltoa(long v, char *buf, int radix)           { return _itoa((int)v, buf, radix); }
char *_ultoa(unsigned long v, char *buf, int radix) { return utoa_base(v, buf, radix, 0); }

int       abs(int x)        { return x < 0 ? -x : x; }
long      labs(long x)      { return x < 0 ? -x : x; }
long long llabs(long long x){ return x < 0 ? -x : x; }
div_t  div(int a, int b)    { div_t r = { a / b, a % b }; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r = { a / b, a % b }; return r; }

/* -----------------------------------------------------------------------
 * Sorting, searching, random numbers
 * ----------------------------------------------------------------------- */
static void swap_bytes(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }

static void qsort_r(char *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    while (n > 1) {
        if (n < 12) {                                       /* insertion sort */
            for (size_t i = 1; i < n; i++)
                for (size_t j = i; j > 0 && cmp(base + (j - 1) * size, base + j * size) > 0; j--)
                    swap_bytes(base + (j - 1) * size, base + j * size, size);
            return;
        }
        /* median of three pivot → position 0 */
        char *a = base, *m = base + (n / 2) * size, *z = base + (n - 1) * size;
        if (cmp(m, a) < 0) swap_bytes(m, a, size);
        if (cmp(z, a) < 0) swap_bytes(z, a, size);
        if (cmp(z, m) < 0) swap_bytes(z, m, size);
        swap_bytes(a, m, size);
        size_t i = 1, j = n - 1;
        for (;;) {
            while (i <= j && cmp(base + i * size, base) < 0) i++;
            while (j >= i && cmp(base + j * size, base) > 0) j--;
            if (i >= j) break;
            swap_bytes(base + i * size, base + j * size, size);
            i++; j--;
        }
        swap_bytes(base, base + j * size, size);
        /* recurse into the smaller part, loop on the larger */
        if (j < n - j - 1) { qsort_r(base, j, size, cmp); base += (j + 1) * size; n -= j + 1; }
        else { qsort_r(base + (j + 1) * size, n - j - 1, size, cmp); n = j; }
    }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    qsort_r(base, n, size, cmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *p = (const char *)base + mid * size;
        int c = cmp(key, p);
        if (!c) return (void *)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

static unsigned long g_rand = 1;
int  rand(void)          { g_rand = g_rand * 214013 + 2531011; return (int)((g_rand >> 16) & 0x7FFF); }
void srand(unsigned s)   { g_rand = s; }

/* -----------------------------------------------------------------------
 * Exit, environment
 * ----------------------------------------------------------------------- */
static void (*g_atexit[32])(void);
static int  g_natexit;

int atexit(void (*fn)(void))
{
    if (g_natexit >= 32) return -1;
    g_atexit[g_natexit++] = fn;
    return 0;
}

void __nova_flush_all(void);     /* stdio.c */

void exit(int code)
{
    while (g_natexit) g_atexit[--g_natexit]();
    __nova_flush_all();
    ExitProcess((UINT)code);
}

void _exit(int code) { ExitProcess((UINT)code); }

void abort(void)
{
    fputs("\nabnormal program termination\n", stderr);
    __nova_flush_all();
    ExitProcess(3);
}

void _assert(const char *expr, const char *file, unsigned line)
{
    fprintf(stderr, "Assertion failed: %s, file %s, line %u\n", expr, file, line);
    abort();
}

char *getenv(const char *name)
{
    static struct { char name[64]; char value[512]; } cache[16];
    static int next;
    char buf[512];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (!n || n >= sizeof(buf)) return 0;
    for (int i = 0; i < 16; i++)
        if (!strcmp(cache[i].name, name)) { strcpy(cache[i].value, buf); return cache[i].value; }
    int i = next++ % 16;
    strncpy(cache[i].name, name, sizeof(cache[i].name) - 1);
    strcpy(cache[i].value, buf);
    return cache[i].value;
}

int system(const char *cmd)
{
    if (!cmd) return 0;                       /* no command processor */
    errno = ENOSYS;
    return -1;
}

/* -----------------------------------------------------------------------
 * Startup: __getmainargs (MSVC rules for splitting the command line)
 * ----------------------------------------------------------------------- */
__declspec(dllexport) int __getmainargs(int *argc, char ***argv, char ***envp, int glob, void *si);
int __getmainargs(int *argc, char ***argv, char ***envp, int glob, void *si)
{
    (void)glob; (void)si;
    const char *cl = GetCommandLineA();
    size_t len = strlen(cl);
    char *buf = malloc(len + 1);
    char **av = malloc(sizeof(char *) * (len / 2 + 2));
    static char *empty_env[1];
    if (!buf || !av) return -1;
    int n = 0;
    char *o = buf;
    const char *p = cl;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        av[n++] = o;
        int quoted = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            int bs = 0;
            while (*p == '\\') { bs++; p++; }
            if (*p == '"') {
                for (int i = 0; i < bs / 2; i++) *o++ = '\\';
                if (bs % 2) *o++ = '"';
                else quoted = !quoted;
                p++;
            } else {
                for (int i = 0; i < bs; i++) *o++ = '\\';
                if (*p && (quoted || (*p != ' ' && *p != '\t'))) *o++ = *p++;
            }
        }
        *o++ = 0;
    }
    av[n] = 0;
    *argc = n;
    *argv = av;
    if (envp) *envp = empty_env;
    return 0;
}
