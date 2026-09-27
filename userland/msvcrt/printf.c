/* msvcrt: the printf family (C99 + MSVC's I64/I32 length modifiers) */
#define NOVA_BUILD_MSVCRT
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    void (*put)(void *ctx, const char *s, size_t n);
    void *ctx;
    size_t total;
} Sink;

static void out(Sink *k, const char *s, size_t n) { if (n) k->put(k->ctx, s, n); k->total += n; }
static void pad(Sink *k, char c, int n) { char b[32]; memset(b, c, sizeof(b)); while (n > 0) { int m = n > 32 ? 32 : n; out(k, b, (size_t)m); n -= m; } }

static double p10(int e)                  /* 10^e */
{
    double r = 1, b = 10;
    int n = e < 0 ? -e : e;
    while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
    return e < 0 ? 1 / r : r;
}

/* Digits of x (>= 0) with @prec decimals into buf; returns length */
static int fixed_digits(double x, int prec, char *buf, int cap)
{
    x += 0.5 * p10(-prec);                /* round */
    int n = 0;
    double ip = __builtin_floor(x), fp = x - ip;
    char tmp[340];
    int t = 0;
    if (ip < 1e19) {
        uint64_t u = (uint64_t)ip;
        do { tmp[t++] = (char)('0' + u % 10); u /= 10; } while (u);
    } else {
        int e = 0;
        while (ip >= 10 && t < 330) { double q = __builtin_floor(ip / 10); tmp[t++] = (char)('0' + (int)(ip - q * 10)); ip = q; e++; }
        tmp[t++] = (char)('0' + (int)ip);
        for (int i = 0; i < t && i < e - 17; i++) tmp[i] = '0';   /* beyond double precision */
    }
    while (t && n < cap) buf[n++] = tmp[--t];
    if (prec > 0 && n < cap) buf[n++] = '.';
    for (int i = 0; i < prec && n < cap; i++) {
        fp *= 10;
        int d = (int)fp;
        if (d > 9) d = 9;
        buf[n++] = (char)('0' + d);
        fp -= d;
    }
    return n;
}

/* x = m * 10^e with 1 <= m < 10 (x > 0) */
static int exponent10(double x)
{
    int e = 0;
    if (x >= 10) { while (x >= 1e16) { x /= 1e16; e += 16; } while (x >= 10) { x /= 10; e++; } }
    else if (x < 1) { while (x < 1e-16) { x *= 1e16; e -= 16; } while (x < 1) { x *= 10; e--; } }
    return e;
}

static int exp_digits(double x, int prec, char *buf, int cap, char echar)
{
    int e = x > 0 ? exponent10(x) : 0;
    double m = x > 0 ? x / p10(e) : 0;
    if (m + 0.5 * p10(-prec) >= 10) { m /= 10; e++; }       /* rounding carried */
    int n = fixed_digits(m, prec, buf, cap - 6);
    buf[n++] = echar;
    buf[n++] = e < 0 ? '-' : '+';
    int a = e < 0 ? -e : e;
    if (a >= 100) buf[n++] = (char)('0' + a / 100);
    buf[n++] = (char)('0' + a / 10 % 10);
    buf[n++] = (char)('0' + a % 10);
    return n;
}

static int fmt_double(double x, char conv, int prec, int alt, char *buf, int cap)
{
    int upper = conv == 'F' || conv == 'E' || conv == 'G';
    if (x != x) { memcpy(buf, upper ? "NAN" : "nan", 3); return 3; }
    if (x == __builtin_inf()) { memcpy(buf, upper ? "INF" : "inf", 3); return 3; }
    if (prec < 0) prec = 6;
    char c = (char)(conv | 0x20);
    if (c == 'f') return fixed_digits(x, prec, buf, cap);
    if (c == 'e') return exp_digits(x, prec, buf, cap, upper ? 'E' : 'e');
    /* %g */
    int P = prec ? prec : 1;
    int X = 0;
    if (x > 0) {
        X = exponent10(x);
        double m = x / p10(X);
        if (m + 0.5 * p10(-(P - 1)) >= 10) X++;
    }
    int n;
    if (P > X && X >= -4) n = fixed_digits(x, P - 1 - X, buf, cap);
    else n = exp_digits(x, P - 1, buf, cap, upper ? 'E' : 'e');
    if (!alt) {                                             /* strip trailing zeros */
        int e = 0;
        while (e < n && (buf[e] | 0x20) != 'e') e++;
        int dot = -1;
        for (int i = 0; i < e; i++) if (buf[i] == '.') dot = i;
        if (dot >= 0) {
            int z = e;
            while (z > dot + 1 && buf[z - 1] == '0') z--;
            if (z == dot + 1) z = dot;
            memmove(buf + z, buf + e, (size_t)(n - e));
            n -= e - z;
        }
    }
    return n;
}

static int core(Sink *k, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') {
            const char *s = f;
            while (*f && *f != '%') f++;
            out(k, s, (size_t)(f - s));
            f--;
            continue;
        }
        f++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        for (;; f++) {
            if (*f == '-') left = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1; else if (*f == '0') zero = 1; else break;
        }
        int width = 0, prec = -1;
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; if (prec < 0) prec = -1; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int len = 0;                                        /* 0 int, 1 long, 2 long long, -1 short, -2 char */
        for (;;) {
            if (*f == 'h') { len = len == -1 ? -2 : -1; f++; }
            else if (*f == 'l') { len++; f++; }
            else if (*f == 'z' || *f == 'j' || *f == 't') { len = 2; f++; }
            else if (*f == 'L') { f++; }
            else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { len = 2; f += 3; }
            else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') { len = 0; f += 3; }
            else if (*f == 'I') { len = 2; f++; }
            else break;
        }
        char c = *f;
        if (!c) break;
        char buf[400];
        const char *s = buf;
        int n = 0;
        char sign = 0;
        const char *prefix = "";
        switch (c) {
        case '%': out(k, "%", 1); continue;
        case 'c': buf[0] = (char)va_arg(ap, int); n = 1; zero = 0; break;
        case 's': case 'S': {
            if (len >= 1 || c == 'S') {                     /* wide string: to UTF-8 (ASCII part) */
                const unsigned short *w = va_arg(ap, const unsigned short *);
                if (!w) { s = "(null)"; n = 6; break; }
                while (w[n] && n < (int)sizeof(buf) && (prec < 0 || n < prec)) { buf[n] = w[n] < 128 ? (char)w[n] : '?'; n++; }
            } else {
                s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                n = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            }
            zero = 0;
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            uint64_t v;
            int neg = 0, base = 10;
            if (c == 'p') { v = (uint64_t)(uintptr_t)va_arg(ap, void *); base = 16; prec = 16; }
            else if (c == 'd' || c == 'i') {
                int64_t sv = len >= 2 ? va_arg(ap, long long) : len == 1 ? va_arg(ap, long) : va_arg(ap, int);
                if (len == -1) sv = (short)sv; else if (len == -2) sv = (signed char)sv;
                neg = sv < 0;
                v = neg ? (uint64_t)0 - (uint64_t)sv : (uint64_t)sv;
            } else {
                v = len >= 2 ? va_arg(ap, unsigned long long) : len == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
                if (len == -1) v = (unsigned short)v; else if (len == -2) v = (unsigned char)v;
                base = c == 'o' ? 8 : c == 'u' ? 10 : 16;
            }
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[24];
            int t = 0;
            while (v) { tmp[t++] = dig[v % (unsigned)base]; v /= (unsigned)base; }
            if (prec < 0) prec = 1; else zero = 0;
            while (t < prec) tmp[t++] = '0';
            if (c == 'o' && alt && (t == 0 || tmp[t - 1] != '0')) tmp[t++] = '0';
            while (t) buf[n++] = tmp[--t];
            sign = neg ? '-' : (c == 'd' || c == 'i') ? (plus ? '+' : space ? ' ' : 0) : 0;
            if (alt && base == 16 && n && c != 'p') prefix = c == 'X' ? "0X" : "0x";
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double x = va_arg(ap, double);
            if (x < 0 || (x == 0 && 1 / x < 0)) { sign = '-'; x = -x; }
            else sign = plus ? '+' : space ? ' ' : 0;
            n = fmt_double(x, c, prec, alt, buf, sizeof(buf));
            if (x != x || x == __builtin_inf()) zero = 0;
            break;
        }
        case 'n': {
            void *p = va_arg(ap, void *);
            if (len >= 2) *(long long *)p = (long long)k->total;
            else if (len == -1) *(short *)p = (short)k->total;
            else *(int *)p = (int)k->total;
            continue;
        }
        default:
            out(k, f - 1, 2);
            continue;
        }
        int plen = (int)strlen(prefix) + (sign != 0);
        int fill = width - n - plen;
        if (!left && !zero) pad(k, ' ', fill);
        if (sign) out(k, &sign, 1);
        out(k, prefix, strlen(prefix));
        if (!left && zero) pad(k, '0', fill);
        out(k, s, (size_t)n);
        if (left) pad(k, ' ', fill);
    }
    return (int)k->total;
}

/* sinks */
typedef struct { char *s; size_t n, pos; } StrCtx;
static void str_put(void *ctx, const char *s, size_t n)
{
    StrCtx *c = ctx;
    for (size_t i = 0; i < n; i++, c->pos++) if (c->pos + 1 < c->n) c->s[c->pos] = s[i];
}
static void file_put(void *ctx, const char *s, size_t n) { fwrite(s, 1, n, (FILE *)ctx); }

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
    StrCtx c = { s, n, 0 };
    Sink k = { str_put, &c, 0 };
    int r = core(&k, fmt, ap);
    if (n) s[c.pos < n ? c.pos : n - 1] = 0;
    return r;
}

int _vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
    int r = vsnprintf(s, n, fmt, ap);
    return (size_t)r >= n ? -1 : r;                         /* MSVC semantics */
}

int vsprintf(char *s, const char *fmt, va_list ap)   { return vsnprintf(s, (size_t)-1 >> 1, fmt, ap); }
int vfprintf(FILE *f, const char *fmt, va_list ap)   { Sink k = { file_put, f, 0 }; return core(&k, fmt, ap); }
int vprintf(const char *fmt, va_list ap)             { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...)            { va_list a; va_start(a, fmt); int r = vfprintf(stdout, fmt, a); va_end(a); return r; }
int fprintf(FILE *f, const char *fmt, ...)  { va_list a; va_start(a, fmt); int r = vfprintf(f, fmt, a); va_end(a); return r; }
int sprintf(char *s, const char *fmt, ...)  { va_list a; va_start(a, fmt); int r = vsprintf(s, fmt, a); va_end(a); return r; }
int snprintf(char *s, size_t n, const char *fmt, ...)  { va_list a; va_start(a, fmt); int r = vsnprintf(s, n, fmt, a); va_end(a); return r; }
int _snprintf(char *s, size_t n, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = _vsnprintf(s, n, fmt, a); va_end(a); return r; }
