/*
 * normaliz.dll — internationalized domain names: IdnToAscii turns each
 * label with non-ASCII characters into its "xn--" Punycode form (RFC 3492),
 * IdnToUnicode turns them back.  Labels are lower-cased first (the part of
 * nameprep that matters for ordinary names).
 */
#include <windows.h>
#include <string.h>

#define NZ __declspec(dllexport)
#define ERROR_INVALID_NAME_ 123
#define IDN_USE_STD3_ASCII_RULES 0x02


enum { BASE = 36, TMIN = 1, TMAX = 26, SKEW = 38, DAMP = 700, INITIAL_BIAS = 72, INITIAL_N = 0x80 };

static unsigned adapt(unsigned delta, unsigned npoints, int first)
{
    delta = first ? delta / DAMP : delta / 2;
    delta += delta / npoints;
    unsigned k = 0;
    while (delta > ((BASE - TMIN) * TMAX) / 2) { delta /= BASE - TMIN; k += BASE; }
    return k + (BASE - TMIN + 1) * delta / (delta + SKEW);
}
static WCHAR digit_char(unsigned d) { return (WCHAR)(d < 26 ? 'a' + d : '0' + d - 26); }
static int char_digit(WCHAR c)
{
    if (c >= '0' && c <= '9') return c - '0' + 26;
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    return -1;
}

/* Code points of a UTF-16 label */
static int to_cps(const WCHAR *s, int n, unsigned *cp, int cap)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        unsigned c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (k == cap) return -1;
        cp[k++] = c;
    }
    return k;
}
static int put_cp(WCHAR *out, int k, int cap, unsigned c)
{
    if (c >= 0x10000) {
        if (k + 2 > cap) return -1;
        c -= 0x10000;
        out[k++] = (WCHAR)(0xD800 + (c >> 10));
        out[k++] = (WCHAR)(0xDC00 + (c & 0x3FF));
        return k;
    }
    if (k + 1 > cap) return -1;
    out[k++] = (WCHAR)c;
    return k;
}

/* RFC 3492 section 6.3: encode one label (code points) after "xn--" */
static int encode(const unsigned *cp, int n, WCHAR *out, int k, int cap)
{
    unsigned b = 0;
    for (int i = 0; i < n; i++)
        if (cp[i] < 0x80) { if (k == cap) return -1; out[k++] = (WCHAR)cp[i]; b++; }
    unsigned h = b;
    if (b) { if (k == cap) return -1; out[k++] = '-'; }
    unsigned nn = INITIAL_N, delta = 0, bias = INITIAL_BIAS;
    while (h < (unsigned)n) {
        unsigned m = 0xFFFFFFFF;
        for (int i = 0; i < n; i++) if (cp[i] >= nn && cp[i] < m) m = cp[i];
        if ((m - nn) > (0xFFFFFFFF - delta) / (h + 1)) return -1;
        delta += (m - nn) * (h + 1);
        nn = m;
        for (int i = 0; i < n; i++) {
            if (cp[i] < nn && ++delta == 0) return -1;
            if (cp[i] == nn) {
                unsigned q = delta;
                for (unsigned kk = BASE;; kk += BASE) {
                    unsigned t = kk <= bias ? TMIN : kk >= bias + TMAX ? TMAX : kk - bias;
                    if (q < t) break;
                    if (k == cap) return -1;
                    out[k++] = digit_char(t + (q - t) % (BASE - t));
                    q = (q - t) / (BASE - t);
                }
                if (k == cap) return -1;
                out[k++] = digit_char(q);
                bias = adapt(delta, h + 1, h == b);
                delta = 0;
                h++;
            }
        }
        delta++; nn++;
    }
    return k;
}

/* RFC 3492 section 6.2: decode the part after "xn--" into code points */
static int decode(const WCHAR *s, int n, unsigned *out, int cap)
{
    int b = 0;
    for (int i = 0; i < n; i++) if (s[i] == '-') b = i;
    int len = 0;
    for (int i = 0; i < b; i++) { if (s[i] >= 0x80 || len == cap) return -1; out[len++] = s[i]; }
    unsigned nn = INITIAL_N, i = 0, bias = INITIAL_BIAS;
    for (int in = b > 0 ? b + 1 : 0; in < n;) {
        unsigned oldi = i, w = 1;
        for (unsigned k = BASE;; k += BASE) {
            if (in >= n) return -1;
            int d = char_digit(s[in++]);
            if (d < 0 || (unsigned)d > (0xFFFFFFFF - i) / w) return -1;
            i += (unsigned)d * w;
            unsigned t = k <= bias ? TMIN : k >= bias + TMAX ? TMAX : k - bias;
            if ((unsigned)d < t) break;
            w *= BASE - t;
        }
        bias = adapt(i - oldi, (unsigned)len + 1, oldi == 0);
        nn += i / ((unsigned)len + 1);
        i %= (unsigned)len + 1;
        if (len == cap || nn > 0x10FFFF) return -1;
        for (int j = len; j > (int)i; j--) out[j] = out[j - 1];
        out[i++] = nn;
        len++;
    }
    return len;
}

static int finish(WCHAR *tmp, int k, WCHAR *dst, int cap)
{
    if (k < 0) { SetLastError(ERROR_INVALID_NAME_); return 0; }
    if (!cap) return k;
    if (k > cap) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(dst, tmp, k * sizeof(WCHAR));
    return k;
}

static int src_len(LPCWSTR src, int n) { if (n == -1) { n = 0; while (src[n]) n++; n++; } return n; }

NZ int WINAPI IdnToAscii(DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cap)
{
    if (!src || !n || n < -1 || cap < 0 || (cap && !dst)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    n = src_len(src, n);
    WCHAR out[512];
    unsigned cps[256];
    int k = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && src[j] != '.' && src[j] != 0x3002 && src[j] != 0xFF0E && src[j] != 0xFF61 && src[j]) j++;
        WCHAR label[256];
        int ln = j - i;
        if (ln > 255) return finish(out, -1, dst, cap);
        memcpy(label, src + i, ln * sizeof(WCHAR));
        CharLowerBuffW(label, (DWORD)ln);
        int ascii = 1;
        for (int t = 0; t < ln; t++) if (label[t] >= 0x80) ascii = 0;
        int start = k;
        if (ascii) {
            if (k + ln > 512) return finish(out, -1, dst, cap);
            for (int t = 0; t < ln; t++) {
                if ((flags & IDN_USE_STD3_ASCII_RULES) && !(char_digit(label[t]) >= 0 || label[t] == '-'))
                    return finish(out, -1, dst, cap);
                out[k++] = src[i + t];                      /* ASCII labels keep their case */
            }
        } else {
            int nc = to_cps(label, ln, cps, 256);
            if (nc < 0 || k + 4 > 512) return finish(out, -1, dst, cap);
            out[k++] = 'x'; out[k++] = 'n'; out[k++] = '-'; out[k++] = '-';
            k = encode(cps, nc, out, k, 512);
            if (k < 0) return finish(out, -1, dst, cap);
        }
        if (k - start > 63) return finish(out, -1, dst, cap);
        if (j < n) {                                        /* the separator (or the terminator) */
            if (k == 512) return finish(out, -1, dst, cap);
            out[k++] = src[j] ? '.' : 0;
        }
        i = j + 1;
    }
    return finish(out, k, dst, cap);
}

NZ int WINAPI IdnToUnicode(DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cap)
{
    (void)flags;
    if (!src || !n || n < -1 || cap < 0 || (cap && !dst)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    n = src_len(src, n);
    WCHAR out[512];
    unsigned cps[256];
    int k = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && src[j] != '.' && src[j]) j++;
        int ln = j - i;
        const WCHAR *l = src + i;
        if (ln > 4 && (l[0] | 0x20) == 'x' && (l[1] | 0x20) == 'n' && l[2] == '-' && l[3] == '-') {
            int nc = decode(l + 4, ln - 4, cps, 256);
            if (nc < 0) return finish(out, -1, dst, cap);
            for (int t = 0; t < nc; t++) if ((k = put_cp(out, k, 512, cps[t])) < 0) return finish(out, -1, dst, cap);
        } else {
            if (k + ln > 512) return finish(out, -1, dst, cap);
            memcpy(out + k, l, ln * sizeof(WCHAR));
            k += ln;
        }
        if (j < n) {
            if (k == 512) return finish(out, -1, dst, cap);
            out[k++] = src[j];
        }
        i = j + 1;
    }
    return finish(out, k, dst, cap);
}

/* Nameprep alone: lower-casing */
NZ int WINAPI IdnToNameprepUnicode(DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cap)
{
    (void)flags;
    if (!src || !n || n < -1 || cap < 0 || (cap && !dst)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    n = src_len(src, n);
    if (!cap) return n;
    if (n > cap) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(dst, src, n * sizeof(WCHAR));
    CharLowerBuffW(dst, (DWORD)n);
    return n;
}
