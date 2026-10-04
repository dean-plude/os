/* msvcrt: the printf family (C99 + MSVC's I64/I32 length modifiers)
 *
 * One formatter for narrow and wide output.  Floating point is converted
 * exactly (the double's binary value expanded to all its decimal digits
 * with big-integer arithmetic, then rounded half-to-even), so %.17g and
 * friends print what the value really is, as glibc and the UCRT do.
 * In wide mode (the w functions) %s and %c take wide arguments and %S/%C
 * narrow ones, as in Microsoft's CRT; %hs/%ls pick explicitly.
 */
#define NOVA_BUILD_MSVCRT
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include "msvcrt_internal.h"

static void out(PrintSink *k, const char *s, size_t n) { if (n) k->put(k->ctx, s, n); k->total += n; }
static void pad(PrintSink *k, char c, int n)
{
    char b[32];
    memset(b, c, sizeof(b));
    while (n > 0) { int m = n > 32 ? 32 : n; out(k, b, (size_t)m); n -= m; }
}

/* -----------------------------------------------------------------------
 * Exact decimal expansion of a double
 * ----------------------------------------------------------------------- */
#define MAX_DIGITS 800            /* 2^-1074 needs 751 significant digits */

/* All significant digits of x > 0 (finite): x = 0.D1D2...Dn * 10^*dexp.
 * Returns n (trailing zeros removed). */
static int exact_digits(double x, char *dig, int *dexp)
{
    uint64_t bits;
    memcpy(&bits, &x, 8);
    int be = (int)((bits >> 52) & 0x7FF);
    uint64_t m = bits & 0xFFFFFFFFFFFFFull;
    int e;
    if (be) { m |= 1ull << 52; e = be - 1075; } else e = -1074;
    while (!(m & 1)) { m >>= 1; e++; }            /* smaller numbers, same value */

    /* big number in base 1e9, least significant limb first */
    uint32_t n[100];
    int len = 0;
    while (m) { n[len++] = (uint32_t)(m % 1000000000u); m /= 1000000000u; }
    int k = 0;                                     /* value = N * 10^-k */
    if (e > 0) {
        for (int i = 0; i < e; i += 29) {          /* multiply by 2^e, 29 bits at a time */
            int sh = e - i < 29 ? e - i : 29;
            uint64_t carry = 0;
            for (int j = 0; j < len; j++) {
                uint64_t v = ((uint64_t)n[j] << sh) + carry;
                n[j] = (uint32_t)(v % 1000000000u);
                carry = v / 1000000000u;
            }
            while (carry) { n[len++] = (uint32_t)(carry % 1000000000u); carry /= 1000000000u; }
        }
    } else if (e < 0) {
        k = -e;                                    /* x = m / 2^k = m * 5^k / 10^k */
        for (int i = 0; i < k; i += 13) {
            uint32_t mul = 1;
            for (int j = 0; j < 13 && i + j < k; j++) mul *= 5;
            uint64_t carry = 0;
            for (int j = 0; j < len; j++) {
                uint64_t v = (uint64_t)n[j] * mul + carry;
                n[j] = (uint32_t)(v % 1000000000u);
                carry = v / 1000000000u;
            }
            while (carry) { n[len++] = (uint32_t)(carry % 1000000000u); carry /= 1000000000u; }
        }
    }
    /* limbs to a digit string, most significant first */
    int d = 0;
    char tmp[12];
    for (int j = len - 1; j >= 0; j--) {
        uint32_t v = n[j];
        for (int t = 8; t >= 0; t--) { tmp[t] = (char)('0' + v % 10); v /= 10; }
        int s = 0;
        if (j == len - 1) while (s < 8 && tmp[s] == '0') s++;
        for (; s < 9; s++) dig[d++] = tmp[s];
    }
    *dexp = d - k;
    while (d > 1 && dig[d - 1] == '0') d--;
    return d;
}

/* Keep @keep digits of dig[0..n), rounding half to even (the digits are
 * exact, so a 5 followed by nothing is a true tie) as the Universal CRT
 * does, or half away from zero as msvcrt.dll does (__nova_crt_legacy,
 * set per DLL).  May carry into a new leading digit: then *dexp grows.
 * Returns the new length (>= 0). */
static int round_digits(char *dig, int n, int keep, int *dexp)
{
    if (keep >= n) return n;
    if (keep < 0) return 0;
    int up = dig[keep] > '5';
    if (dig[keep] == '5' && __nova_crt_legacy) {
        up = 1;
    } else if (dig[keep] == '5') {
        int more = 0;
        for (int i = keep + 1; i < n; i++) if (dig[i] != '0') { more = 1; break; }
        /* a tie goes to the even neighbour (with no digit kept, that is 0) */
        up = more || (keep > 0 && ((dig[keep - 1] - '0') & 1));
    }
    n = keep;
    if (up) {
        int i = keep - 1;
        while (i >= 0 && dig[i] == '9') dig[i--] = '0';
        if (i >= 0) dig[i]++;
        else {                                         /* 999 -> 1000 */
            memmove(dig + 1, dig, (size_t)n);
            dig[0] = '1';
            n++;
            (*dexp)++;
        }
    }
    return n;
}

/* Format a finite x >= 0 for %f %e %g %a (upper case variants via @c).
 * Writes into @b (cap >= 1200), returns length. */
static int fmt_float(double x, char c, int prec, int alt, char *b)
{
    int upper = c == 'E' || c == 'G' || c == 'F' || c == 'A';
    char lc = (char)(c | 0x20);
    int o = 0;
    if (lc == 'a') {                                   /* hex: 0x1.hhhp+d */
        uint64_t bits;
        memcpy(&bits, &x, 8);
        int be = (int)((bits >> 52) & 0x7FF);
        uint64_t m = bits & 0xFFFFFFFFFFFFFull;
        int e = be ? be - 1023 : (m ? -1022 : 0);
        int lead = be ? 1 : 0;
        const char *hx = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        if (prec >= 0 && prec < 13) {                  /* round to prec hex digits */
            int drop = (13 - prec) * 4;
            uint64_t full = ((uint64_t)lead << 52) | m, half = 1ull << (drop - 1);
            uint64_t rem = full & ((1ull << drop) - 1);
            full >>= drop;
            if (rem > half || (rem == half && (full & 1))) full++;
            full <<= drop;
            lead = (int)(full >> 52);
            m = full & 0xFFFFFFFFFFFFFull;
            if (lead > 1) { lead = 1; e++; }
        }
        b[o++] = '0'; b[o++] = upper ? 'X' : 'x';
        b[o++] = (char)('0' + lead);
        int nd = prec < 0 ? 13 : prec;
        if (prec < 0) while (nd > 0 && !((m >> ((13 - nd) * 4)) & 0xF)) nd--;
        if (nd || alt) b[o++] = '.';
        for (int i = 0; i < nd; i++) b[o++] = i < 13 ? hx[(m >> ((12 - i) * 4)) & 0xF] : '0';
        o += sprintf(b + o, "%c%+d", upper ? 'P' : 'p', x == 0 ? 0 : e);
        return o;
    }
    if (prec < 0) prec = 6;
    if (prec > 350) prec = 350;
    char dig[MAX_DIGITS + 4];
    int n = 0, dexp = 1;
    if (x > 0) n = exact_digits(x, dig, &dexp);
    else { dig[0] = '0'; n = 1; dexp = 1; }

    if (lc == 'g') {
        int P = prec == 0 ? 1 : prec;
        int rn = x > 0 ? round_digits(dig, n, P, &dexp) : 1;
        int X = x > 0 ? dexp - 1 : 0;
        if (P > X && X >= -4) { lc = 'f'; prec = P - 1 - X; }
        else { lc = 'e'; prec = P - 1; }
        n = rn;
        if (!alt) {                                    /* %g drops trailing zeros */
            int sig = n;
            while (sig > 1 && dig[sig - 1] == '0') sig--;
            int shown = lc == 'e' ? sig - 1 : sig - dexp;
            if (shown < 0) shown = 0;
            if (shown < prec) prec = shown;
        }
    }
    if (lc == 'e') {
        if (x > 0) n = round_digits(dig, n, prec + 1, &dexp);
        b[o++] = n > 0 ? dig[0] : '0';
        if (prec || alt) b[o++] = '.';
        for (int i = 1; i <= prec; i++) b[o++] = i < n ? dig[i] : '0';
        int ex = x > 0 ? dexp - 1 : 0;
        b[o++] = upper ? 'E' : 'e';
        b[o++] = ex < 0 ? '-' : '+';
        if (ex < 0) ex = -ex;
        if (ex >= 100) b[o++] = (char)('0' + ex / 100);
        b[o++] = (char)('0' + ex / 10 % 10);
        b[o++] = (char)('0' + ex % 10);
        return o;
    }
    /* %f */
    if (x > 0) n = round_digits(dig, n, dexp + prec, &dexp);
    if (n == 0) { dexp = 1; }
    if (dexp <= 0) b[o++] = '0';
    else for (int i = 0; i < dexp; i++) b[o++] = i < n ? dig[i] : '0';
    if (prec || alt) b[o++] = '.';
    for (int i = 0; i < prec; i++) {
        int di = dexp + i;
        b[o++] = di >= 0 && di < n ? dig[di] : '0';
    }
    return o;
}

/* -----------------------------------------------------------------------
 * The formatter
 * ----------------------------------------------------------------------- */
/* UTF-16 -> UTF-8 of at most @max code units (and @max_out bytes) */
static int w2u8(const unsigned short *w, int max, char *out8, int max_out)
{
    int o = 0;
    for (int i = 0; (max < 0 || i < max) && w[i]; i++) {
        unsigned c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000 && (max < 0 || i + 1 < max)) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            i++;
        }
        int need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (o + need > max_out) break;
        if (need == 1) out8[o++] = (char)c;
        else if (need == 2) { out8[o++] = (char)(0xC0 | c >> 6); out8[o++] = (char)(0x80 | (c & 63)); }
        else if (need == 3) { out8[o++] = (char)(0xE0 | c >> 12); out8[o++] = (char)(0x80 | (c >> 6 & 63)); out8[o++] = (char)(0x80 | (c & 63)); }
        else { out8[o++] = (char)(0xF0 | c >> 18); out8[o++] = (char)(0x80 | (c >> 12 & 63)); out8[o++] = (char)(0x80 | (c >> 6 & 63)); out8[o++] = (char)(0x80 | (c & 63)); }
    }
    return o;
}

int __nova_printf_core(PrintSink *k, const char *f, va_list ap, int flags)
{
    int wide = flags & PF_WIDE;
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
        int len = 0;                  /* 0 int, 1 long, 2 long long, -1 short, -2 char */
        int hlen = 0, llen = 0;       /* h / l seen (for %s %c width of characters) */
        for (;;) {
            if (*f == 'h') { len = len == -1 ? -2 : -1; hlen = 1; f++; }
            else if (*f == 'l') { len++; llen = 1; f++; }
            else if (*f == 'w') { llen = 1; f++; }
            else if (*f == 'j') { len = 2; f++; }
            else if (*f == 'z' || *f == 't') { len = sizeof(size_t) == 8 ? 2 : 0; f++; }   /* pointer-sized */
            else if (*f == 'L') { f++; }
            else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { len = 2; f += 3; }
            else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') { len = 0; f += 3; }
            else if (*f == 'I') { len = sizeof(size_t) == 8 ? 2 : 0; f++; }
            else break;
        }
        char c = *f;
        if (!c) break;
        char buf[1400];
        const char *s = buf;
        int n = 0;
        char sign = 0;
        const char *prefix = "";
        switch (c) {
        case '%': out(k, "%", 1); continue;
        case 'c': case 'C': {
            int wch = hlen ? 0 : llen ? 1 : (c == 'C') ^ (wide != 0);
            if (wch) { unsigned short w[2] = { (unsigned short)va_arg(ap, int), 0 }; n = w2u8(w, 1, buf, 4); }
            else { buf[0] = (char)va_arg(ap, int); n = 1; }
            zero = 0;
            break;
        }
        case 's': case 'S': {
            int wstr = hlen ? 0 : llen ? 1 : (c == 'S') ^ (wide != 0);
            if (wstr) {
                const unsigned short *w = va_arg(ap, const unsigned short *);
                if (!w) { s = "(null)"; n = 6; break; }
                /* precision counts characters here (wide units) */
                int units = 0;
                while (w[units] && (prec < 0 || units < prec)) units++;
                if (units <= (int)sizeof(buf) / 4) n = w2u8(w, units, buf, sizeof(buf));
                else {                                     /* long: stream it */
                    int fill = width - units;
                    if (!left) pad(k, ' ', fill);
                    char tmp[256];
                    for (int i = 0; i < units; i += 60) {
                        int m = w2u8(w + i, units - i < 60 ? units - i : 60, tmp, sizeof(tmp));
                        out(k, tmp, (size_t)m);
                    }
                    if (left) pad(k, ' ', fill);
                    continue;
                }
            } else {
                s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                n = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            }
            zero = 0;
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': case 'b': {
            uint64_t v;
            int neg = 0, base = 10;
            if (c == 'p') { v = (uint64_t)(uintptr_t)va_arg(ap, void *); base = 16; prec = 2 * (int)sizeof(void *); }
            else if (c == 'd' || c == 'i') {
                int64_t sv = len >= 2 ? va_arg(ap, long long) : len == 1 ? va_arg(ap, long) : va_arg(ap, int);
                if (len == -1) sv = (short)sv; else if (len == -2) sv = (signed char)sv;
                neg = sv < 0;
                v = neg ? (uint64_t)0 - (uint64_t)sv : (uint64_t)sv;
            } else {
                v = len >= 2 ? va_arg(ap, unsigned long long) : len == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
                if (len == -1) v = (unsigned short)v; else if (len == -2) v = (unsigned char)v;
                base = c == 'o' ? 8 : c == 'u' ? 10 : c == 'b' ? 2 : 16;
            }
            const char *dig = c == 'X' || c == 'p' ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[72];
            int t = 0;
            while (v) { tmp[t++] = dig[v % (unsigned)base]; v /= (unsigned)base; }
            if (prec < 0) prec = 1; else zero = 0;
            while (t < prec && t < (int)sizeof(tmp)) tmp[t++] = '0';
            if (c == 'o' && alt && (t == 0 || tmp[t - 1] != '0')) tmp[t++] = '0';
            while (t) buf[n++] = tmp[--t];
            sign = neg ? '-' : (c == 'd' || c == 'i') ? (plus ? '+' : space ? ' ' : 0) : 0;
            if (alt && base == 16 && n && c != 'p') prefix = c == 'X' ? "0X" : "0x";
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double x = va_arg(ap, double);
            uint64_t bits;
            memcpy(&bits, &x, 8);
            if (bits >> 63) { sign = '-'; x = -x; }
            else sign = plus ? '+' : space ? ' ' : 0;
            int upper = c == 'F' || c == 'E' || c == 'G' || c == 'A';
            if (x != x) { s = upper ? "NAN" : "nan"; n = 3; zero = 0; }
            else if (x == __builtin_inf()) { s = upper ? "INF" : "inf"; n = 3; zero = 0; }
            else n = fmt_float(x, c, prec, alt, buf);
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

/* -----------------------------------------------------------------------
 * Sinks and the narrow API
 * ----------------------------------------------------------------------- */
typedef struct { char *s; size_t n, pos; } StrCtx;
static void str_put(void *ctx, const char *s, size_t n)
{
    StrCtx *c = ctx;
    for (size_t i = 0; i < n; i++, c->pos++) if (c->pos + 1 < c->n) c->s[c->pos] = s[i];
}
/* A stream: the output gathers in a buffer on the stack and goes to the
 * stream in one piece, as msvcrt's _stbuf does, so an unbuffered stream
 * (stderr) gets one write per call, not one per fragment of the format.
 * VLC logs to stderr this way: a fragment at a time, each a system call. */
typedef struct { FILE *f; size_t n; char buf[512]; } FileCtx;
static void file_flush(FileCtx *c)
{
    if (c->n) fwrite(c->buf, 1, c->n, c->f);
    c->n = 0;
}
static void file_put(void *ctx, const char *s, size_t n)
{
    FileCtx *c = ctx;
    while (n) {
        if (c->n == sizeof(c->buf)) file_flush(c);
        size_t m = sizeof(c->buf) - c->n;
        if (m > n) m = n;
        memcpy(c->buf + c->n, s, m);
        c->n += m; s += m; n -= m;
    }
}

int __nova_vsnprintf(char *s, size_t n, const char *fmt, va_list ap, int flags)
{
    StrCtx c = { s, n, 0 };
    PrintSink k = { str_put, &c, 0 };
    int r = __nova_printf_core(&k, fmt, ap, flags);
    if (n) s[c.pos < n ? c.pos : n - 1] = 0;
    return r;
}

int __nova_vfprintf(FILE *f, const char *fmt, va_list ap, int flags)
{
    FileCtx c = { f, 0 };
    PrintSink k = { file_put, &c, 0 };
    int r = __nova_printf_core(&k, fmt, ap, flags);
    file_flush(&c);
    return r;
}

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap) { return __nova_vsnprintf(s, n, fmt, ap, 0); }

int _vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
    int r = vsnprintf(s, n, fmt, ap);
    return (size_t)r >= n ? -1 : r;                         /* MSVC semantics */
}

int vsprintf(char *s, const char *fmt, va_list ap)   { return vsnprintf(s, (size_t)-1 >> 1, fmt, ap); }
int vfprintf(FILE *f, const char *fmt, va_list ap)   { return __nova_vfprintf(f, fmt, ap, 0); }
int vprintf(const char *fmt, va_list ap)             { return vfprintf(stdout, fmt, ap); }
__declspec(dllexport) int _vscprintf(const char *fmt, va_list ap)          { return vsnprintf(NULL, 0, fmt, ap); }

int printf(const char *fmt, ...)            { va_list a; va_start(a, fmt); int r = vfprintf(stdout, fmt, a); va_end(a); return r; }
int fprintf(FILE *f, const char *fmt, ...)  { va_list a; va_start(a, fmt); int r = vfprintf(f, fmt, a); va_end(a); return r; }
int sprintf(char *s, const char *fmt, ...)  { va_list a; va_start(a, fmt); int r = vsprintf(s, fmt, a); va_end(a); return r; }
int snprintf(char *s, size_t n, const char *fmt, ...)  { va_list a; va_start(a, fmt); int r = vsnprintf(s, n, fmt, a); va_end(a); return r; }
int _snprintf(char *s, size_t n, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = _vsnprintf(s, n, fmt, a); va_end(a); return r; }
__declspec(dllexport) int _scprintf(const char *fmt, ...)         { va_list a; va_start(a, fmt); int r = _vscprintf(fmt, a); va_end(a); return r; }
int sprintf_s(char *s, size_t n, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vsnprintf(s, n, fmt, a); va_end(a); return r; }
int vsprintf_s(char *s, size_t n, const char *fmt, va_list ap) { return vsnprintf(s, n, fmt, ap); }
int _snprintf_s(char *s, size_t n, size_t cnt, const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    int r = _vsnprintf(s, cnt < n ? cnt + 1 : n, fmt, a);
    va_end(a);
    return r;
}

/* -----------------------------------------------------------------------
 * Wide output: format as UTF-8, deliver UTF-16
 * ----------------------------------------------------------------------- */
/* The wide format, as UTF-8 (conversion specifications are ASCII) */
static char *narrow_format(const wchar_t *fmt, char *small, size_t cap)
{
    size_t need = wcslen(fmt) * 3 + 1;
    char *b = need <= cap ? small : malloc(need);
    if (!b) return NULL;
    int n = w2u8((const unsigned short *)fmt, -1, b, (int)need - 1);
    b[n] = 0;
    return b;
}

typedef struct { wchar_t *s; size_t n, pos; unsigned pend, want; } WStrCtx;
/* UTF-8 bytes in, UTF-16 units out (to a buffer of n units) */
static void wstr_put(void *ctx, const char *s, size_t len)
{
    WStrCtx *c = ctx;
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)s[i];
        if (c->want) {
            c->pend = (c->pend << 6) | (b & 63);
            if (--c->want) continue;
        } else if (b >= 0xF0) { c->pend = b & 7; c->want = 3; continue; }
        else if (b >= 0xE0) { c->pend = b & 15; c->want = 2; continue; }
        else if (b >= 0xC0) { c->pend = b & 31; c->want = 1; continue; }
        else c->pend = b;
        unsigned cp = c->pend;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            if (c->pos + 1 < c->n) c->s[c->pos] = (wchar_t)(0xD800 + (cp >> 10));
            c->pos++;
            cp = 0xDC00 + (cp & 0x3FF);
        }
        if (c->pos + 1 < c->n) c->s[c->pos] = (wchar_t)cp;
        c->pos++;
    }
}

/* Returns the number of UTF-16 units the full output needs */
int __nova_vsnwprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap, int flags)
{
    char small[256];
    char *f8 = narrow_format(fmt, small, sizeof(small));
    if (!f8) return -1;
    WStrCtx c = { s, n, 0, 0, 0 };
    PrintSink k = { wstr_put, &c, 0 };
    __nova_printf_core(&k, f8, ap, flags | PF_WIDE);
    if (f8 != small) free(f8);
    if (n) s[c.pos < n ? c.pos : n - 1] = 0;
    return (int)c.pos;
}

/* (wide streams on NovaOS carry UTF-8 text, files and the console alike,
 * so the output gathers and goes out as the narrow one does) */
int __nova_vfwprintf(FILE *f, const wchar_t *fmt, va_list ap, int flags)
{
    char small[256];
    char *f8 = narrow_format(fmt, small, sizeof(small));
    if (!f8) return -1;
    FileCtx c = { f, 0 };
    PrintSink k = { file_put, &c, 0 };
    int r = __nova_printf_core(&k, f8, ap, flags | PF_WIDE);
    file_flush(&c);
    if (f8 != small) free(f8);
    return r;
}

int _vsnwprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap)
{
    int r = __nova_vsnwprintf(s, n, fmt, ap, 0);
    return (size_t)r >= n ? -1 : r;
}
/* msvcrt.dll's swprintf and vswprintf (also exported as _swprintf and
 * _vswprintf): no buffer size, as programs built for msvcrt.dll call them */
__declspec(dllexport) int _vswprintf(wchar_t *s, const wchar_t *fmt, va_list ap) { return __nova_vsnwprintf(s, (size_t)1 << 30, fmt, ap, 0); }
__declspec(dllexport) int _swprintf(wchar_t *s, const wchar_t *fmt, ...) { va_list a; va_start(a, fmt); int r = _vswprintf(s, fmt, a); va_end(a); return r; }
#ifdef _WIN64
__asm__(".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:swprintf=_swprintf /EXPORT:vswprintf=_vswprintf\"\n\t.text\n");
#else                                                   /* (x86 C names have a leading underscore) */
__asm__(".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:swprintf=__swprintf /EXPORT:vswprintf=__vswprintf\"\n\t.text\n");
#endif
int _snwprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...) { va_list a; va_start(a, fmt); int r = _vsnwprintf(s, n, fmt, a); va_end(a); return r; }
int vfwprintf(FILE *f, const wchar_t *fmt, va_list ap) { return __nova_vfwprintf(f, fmt, ap, 0); }
int vwprintf(const wchar_t *fmt, va_list ap) { return vfwprintf(stdout, fmt, ap); }
int fwprintf(FILE *f, const wchar_t *fmt, ...) { va_list a; va_start(a, fmt); int r = vfwprintf(f, fmt, a); va_end(a); return r; }
int wprintf(const wchar_t *fmt, ...) { va_list a; va_start(a, fmt); int r = vfwprintf(stdout, fmt, a); va_end(a); return r; }
int _vscwprintf(const wchar_t *fmt, va_list ap) { return __nova_vsnwprintf(NULL, 0, fmt, ap, 0); }
int _scwprintf(const wchar_t *fmt, ...) { va_list a; va_start(a, fmt); int r = _vscwprintf(fmt, a); va_end(a); return r; }
__declspec(dllexport) int vswprintf_s(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap) { int r = _vsnwprintf(s, n, fmt, ap); if (n && (r < 0 || (size_t)r >= n)) { s[0] = 0; return -1; } return r; }
__declspec(dllexport) int swprintf_s(wchar_t *s, size_t n, const wchar_t *fmt, ...) { va_list a; va_start(a, fmt); int r = vswprintf_s(s, n, fmt, a); va_end(a); return r; }
/* at most @cnt characters (_TRUNCATE: as many as fit), always terminated; -1 when cut short */
__declspec(dllexport) int _vsnwprintf_s(wchar_t *s, size_t n, size_t cnt, const wchar_t *fmt, va_list ap)
{
    if (!s || !n) return -1;
    size_t room = cnt < n ? cnt : n - 1;
    int r = _vsnwprintf(s, room + 1, fmt, ap);
    if (r < 0 || (size_t)r > room) { s[room] = 0; return -1; }
    return r;
}
__declspec(dllexport) int _snwprintf_s(wchar_t *s, size_t n, size_t cnt, const wchar_t *fmt, ...)
{
    va_list a; va_start(a, fmt); int r = _vsnwprintf_s(s, n, cnt, fmt, a); va_end(a); return r;
}
