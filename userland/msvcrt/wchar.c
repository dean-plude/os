/* msvcrt: wide (UTF-16) strings, classification and multibyte conversion
 *
 * The C locale's multibyte encoding is UTF-8 (NovaOS's native text
 * encoding); wchar_t is a UTF-16 code unit.  Characters outside the Basic
 * Multilingual Plane are surrogate pairs: the mbrtowc family hands them out
 * one unit at a time, as Microsoft's CRT does.
 */
#define NOVA_BUILD_MSVCRT
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

size_t wcslen(const wchar_t *s) { const wchar_t *p = s; while (*p) p++; return (size_t)(p - s); }
size_t wcsnlen(const wchar_t *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
wchar_t *wcscpy(wchar_t *d, const wchar_t *s) { wchar_t *r = d; while ((*d++ = *s++)) ; return r; }
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
wchar_t *wcscat(wchar_t *d, const wchar_t *s) { wcscpy(d + wcslen(d), s); return d; }
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n)
{
    wchar_t *e = d + wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}
int wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned short)*a - (int)(unsigned short)*b;
}
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) return (int)(unsigned short)*a - (int)(unsigned short)*b;
    }
    return 0;
}
static wchar_t lower1(wchar_t c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c; }
static wchar_t upper1(wchar_t c) { return (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? c - 32 : c; }
int _wcsicmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && lower1(*a) == lower1(*b)) { a++; b++; }
    return (int)lower1(*a) - (int)lower1(*b);
}
int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        wchar_t x = lower1(*a), y = lower1(*b);
        if (x != y || !x) return (int)x - (int)y;
    }
    return 0;
}
int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t len = wcslen(s);
    if (n) { wcsncpy(d, s, n); if (len >= n) d[n - 1] = 0; }
    return len;
}
wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
    for (;; s++) { if (*s == c) return (wchar_t *)s; if (!*s) return NULL; }
}
wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *r = NULL;
    for (;; s++) { if (*s == c) r = s; if (!*s) return (wchar_t *)r; }
}
wchar_t *wcsstr(const wchar_t *h, const wchar_t *n)
{
    size_t ln = wcslen(n);
    if (!ln) return (wchar_t *)h;
    for (; *h; h++) if (*h == *n && !wcsncmp(h, n, ln)) return (wchar_t *)h;
    return NULL;
}
size_t wcsspn(const wchar_t *s, const wchar_t *a)  { size_t i = 0; while (s[i] && wcschr(a, s[i])) i++; return i; }
size_t wcscspn(const wchar_t *s, const wchar_t *r) { size_t i = 0; while (s[i] && !wcschr(r, s[i])) i++; return i; }
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *a)
{
    for (; *s; s++) if (wcschr(a, *s)) return (wchar_t *)s;
    return NULL;
}
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **ctx)
{
    if (!s) s = *ctx;
    if (!s) return NULL;
    s += wcsspn(s, delim);
    if (!*s) { *ctx = NULL; return NULL; }
    wchar_t *e = s + wcscspn(s, delim);
    if (*e) { *e = 0; *ctx = e + 1; } else *ctx = NULL;
    return s;
}
wchar_t *_wcstok(wchar_t *s, const wchar_t *delim)
{
    static wchar_t *ctx;
    return wcstok(s, delim, &ctx);
}
wchar_t *_wcsdup(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
wchar_t *_wcslwr(wchar_t *s) { for (wchar_t *p = s; *p; p++) *p = lower1(*p); return s; }
wchar_t *_wcsupr(wchar_t *s) { for (wchar_t *p = s; *p; p++) *p = upper1(*p); return s; }
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n)  { return memcpy(d, s, n * sizeof(wchar_t)); }
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n) { return memmove(d, s, n * sizeof(wchar_t)); }
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n) { for (size_t i = 0; i < n; i++) d[i] = c; return d; }
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++) if (s[i] == c) return (wchar_t *)s + i;
    return NULL;
}

/* -----------------------------------------------------------------------
 * Classification (ASCII + Latin-1; other letters count as alphabetic)
 * ----------------------------------------------------------------------- */
int iswupper(wint_t c)  { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7); }
int iswlower(wint_t c)  { return (c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7); }
int iswdigit(wint_t c)  { return c >= '0' && c <= '9'; }
int iswxdigit(wint_t c) { return iswdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iswspace(wint_t c)  { return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x2028 || c == 0x2029 || c == 0x3000; }
int iswblank(wint_t c)  { return c == ' ' || c == '\t'; }
int iswcntrl(wint_t c)  { return c < 32 || (c >= 127 && c < 160); }
int iswalpha(wint_t c)  { return iswupper(c) || iswlower(c) || c == 0xAA || c == 0xB5 || c == 0xBA || (c >= 0x100 && !iswspace(c) && c != WEOF && !(c >= 0x2000 && c <= 0x2BFF) && !(c >= 0x3000 && c <= 0x303F) && !(c >= 0xD800 && c <= 0xDFFF) && !(c >= 0xFF00 && c <= 0xFF0F)); }
int iswalnum(wint_t c)  { return iswalpha(c) || iswdigit(c); }
int iswpunct(wint_t c)  { return (c > 32 && c < 127 && !iswalnum(c)) || (c >= 0xA1 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA) || c == 0xD7 || c == 0xF7 || (c >= 0x2010 && c <= 0x2027) || (c >= 0x3001 && c <= 0x3003); }
int iswgraph(wint_t c)  { return c != WEOF && !iswspace(c) && !iswcntrl(c) && c != 0xA0; }
int iswprint(wint_t c)  { return iswgraph(c) || c == ' '; }
wint_t towlower(wint_t c) { return (wint_t)lower1((wchar_t)c); }
wint_t towupper(wint_t c) { return (wint_t)upper1((wchar_t)c); }

/* Microsoft's _ctype bits, used by iswctype and wctype */
#define W_UPPER 0x1
#define W_LOWER 0x2
#define W_DIGIT 0x4
#define W_SPACE 0x8
#define W_PUNCT 0x10
#define W_CNTRL 0x20
#define W_BLANK 0x40
#define W_HEX   0x80
#define W_ALPHA 0x103
int iswctype(wint_t c, wctype_t t)
{
    int r = 0;
    if ((t & W_UPPER) && iswupper(c)) r = 1;
    if ((t & W_LOWER) && iswlower(c)) r = 1;
    if ((t & 0x100) && iswalpha(c)) r = 1;
    if ((t & W_DIGIT) && iswdigit(c)) r = 1;
    if ((t & W_SPACE) && iswspace(c)) r = 1;
    if ((t & W_PUNCT) && iswpunct(c)) r = 1;
    if ((t & W_CNTRL) && iswcntrl(c)) r = 1;
    if ((t & W_BLANK) && iswblank(c)) r = 1;
    if ((t & W_HEX) && iswxdigit(c)) r = 1;
    return r;
}
int is_wctype(wint_t c, wctype_t t) { return iswctype(c, t); }
wctype_t wctype(const char *n)
{
    static const struct { const char *n; wctype_t t; } tab[] = {
        { "alnum", W_ALPHA | W_DIGIT }, { "alpha", W_ALPHA }, { "blank", W_BLANK }, { "cntrl", W_CNTRL },
        { "digit", W_DIGIT }, { "graph", W_ALPHA | W_DIGIT | W_PUNCT }, { "lower", W_LOWER },
        { "print", W_ALPHA | W_DIGIT | W_PUNCT | W_BLANK }, { "punct", W_PUNCT }, { "space", W_SPACE },
        { "upper", W_UPPER }, { "xdigit", W_HEX },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) if (!strcmp(n, tab[i].n)) return tab[i].t;
    return 0;
}

/* -----------------------------------------------------------------------
 * Numbers
 * ----------------------------------------------------------------------- */
/* Narrow copy of a wide number (numbers are ASCII); *used = characters */
static const char *narrow_num(const wchar_t *s, char *buf, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap && s[i] && s[i] < 128) { buf[i] = (char)s[i]; i++; }
    buf[i] = 0;
    return buf;
}
#define WNUM(name, type, conv) \
    type name(const wchar_t *s, wchar_t **end, int base) \
    { \
        char b[128]; char *e; \
        type r = conv(narrow_num(s, b, sizeof(b)), &e, base); \
        if (end) *end = (wchar_t *)s + (e - b); \
        return r; \
    }
WNUM(wcstol, long, strtol)
WNUM(wcstoul, unsigned long, strtoul)
WNUM(wcstoll, long long, strtoll)
WNUM(wcstoull, unsigned long long, strtoull)
long long _wcstoi64(const wchar_t *s, wchar_t **end, int base) { return wcstoll(s, end, base); }
unsigned long long _wcstoui64(const wchar_t *s, wchar_t **end, int base) { return wcstoull(s, end, base); }
double wcstod(const wchar_t *s, wchar_t **end)
{
    char b[400]; char *e;
    double r = strtod(narrow_num(s, b, sizeof(b)), &e);
    if (end) *end = (wchar_t *)s + (e - b);
    return r;
}
float wcstof(const wchar_t *s, wchar_t **end) { return (float)wcstod(s, end); }
long double wcstold(const wchar_t *s, wchar_t **end) { return wcstod(s, end); }
int _wtoi(const wchar_t *s)        { return (int)wcstol(s, NULL, 10); }
long _wtol(const wchar_t *s)       { return wcstol(s, NULL, 10); }
long long _wtoi64(const wchar_t *s) { return wcstoll(s, NULL, 10); }
long long _wtoll(const wchar_t *s) { return wcstoll(s, NULL, 10); }
double _wtof(const wchar_t *s)     { return wcstod(s, NULL); }

static wchar_t *u64tow(unsigned long long v, int neg, wchar_t *buf, int radix)
{
    wchar_t t[72];
    int n = 0;
    if (radix < 2 || radix > 36) { buf[0] = 0; return buf; }
    do { int d = (int)(v % (unsigned)radix); t[n++] = (wchar_t)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)radix; } while (v);
    int o = 0;
    if (neg) buf[o++] = '-';
    while (n) buf[o++] = t[--n];
    buf[o] = 0;
    return buf;
}
wchar_t *_i64tow(long long v, wchar_t *b, int r)  { return r == 10 && v < 0 ? u64tow(0ull - (unsigned long long)v, 1, b, r) : u64tow((unsigned long long)v, 0, b, r); }
wchar_t *_ui64tow(unsigned long long v, wchar_t *b, int r) { return u64tow(v, 0, b, r); }
wchar_t *_itow(int v, wchar_t *b, int r)  { return r == 10 ? _i64tow(v, b, r) : u64tow((unsigned)v, 0, b, r); }
wchar_t *_ltow(long v, wchar_t *b, int r) { return r == 10 ? _i64tow(v, b, r) : u64tow((unsigned long)v, 0, b, r); }
wchar_t *_ultow(unsigned long v, wchar_t *b, int r) { return u64tow(v, 0, b, r); }

/* -----------------------------------------------------------------------
 * Multibyte (UTF-8) <-> wide (UTF-16)
 *
 * mbstate_t holds a pending low surrogate (mbrtowc) or a pending high
 * surrogate (wcrtomb).
 * ----------------------------------------------------------------------- */
static mbstate_t g_mbstate_rtowc, g_mbstate_rtomb, g_mbstate_len;

/* Decode one UTF-8 sequence; returns its length (0 for NUL), -1 invalid,
 * -2 incomplete */
static int utf8_decode(const unsigned char *s, size_t n, unsigned *cp)
{
    if (!n) return -2;
    unsigned c = s[0];
    if (c < 0x80) { *cp = c; return c ? 1 : 0; }
    int len = c >= 0xF0 && c < 0xF5 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : -1;
    if (len < 0 || c >= 0xF5) return -1;
    unsigned v = c & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if ((size_t)i >= n) return -2;
        if ((s[i] & 0xC0) != 0x80) return -1;
        v = (v << 6) | (s[i] & 63);
    }
    if ((len == 3 && v < 0x800) || (len == 4 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v < 0xE000)) return -1;
    *cp = v;
    return len;
}

int mbsinit(const mbstate_t *ps) { return !ps || !ps->_state; }
wint_t btowc(int c) { return c == EOF || c >= 0x80 ? WEOF : (wint_t)c; }
int wctob(wint_t c) { return c < 0x80 ? (int)c : EOF; }

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
    if (!ps) ps = &g_mbstate_rtowc;
    if (!s) { ps->_state = 0; return 0; }
    if (ps->_state) {                               /* the low half of a pair */
        if (pwc) *pwc = (wchar_t)ps->_state;
        ps->_state = 0;
        return (size_t)-3;
    }
    unsigned cp;
    int r = utf8_decode((const unsigned char *)s, n, &cp);
    if (r == -2) return (size_t)-2;
    if (r < 0) { errno = EILSEQ; return (size_t)-1; }
    if (cp >= 0x10000) {
        cp -= 0x10000;
        if (pwc) *pwc = (wchar_t)(0xD800 + (cp >> 10));
        ps->_state = 0xDC00 + (cp & 0x3FF);
    } else if (pwc) *pwc = (wchar_t)cp;
    return (size_t)r;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) { return mbrtowc(NULL, s, n, ps ? ps : &g_mbstate_len); }
int mblen(const char *s, size_t n)
{
    if (!s) return 0;
    unsigned cp;
    int r = utf8_decode((const unsigned char *)s, n, &cp);
    return r < 0 ? -1 : r;
}
int mbtowc(wchar_t *pwc, const char *s, size_t n)
{
    if (!s) return 0;
    unsigned cp;
    int r = utf8_decode((const unsigned char *)s, n, &cp);
    if (r < 0) { errno = EILSEQ; return -1; }
    if (pwc) *pwc = (wchar_t)(cp >= 0x10000 ? 0xFFFD : cp);
    return r;
}

static int utf8_encode(unsigned c, char *s)
{
    if (c < 0x80) { s[0] = (char)c; return 1; }
    if (c < 0x800) { s[0] = (char)(0xC0 | c >> 6); s[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { s[0] = (char)(0xE0 | c >> 12); s[1] = (char)(0x80 | (c >> 6 & 63)); s[2] = (char)(0x80 | (c & 63)); return 3; }
    s[0] = (char)(0xF0 | c >> 18); s[1] = (char)(0x80 | (c >> 12 & 63)); s[2] = (char)(0x80 | (c >> 6 & 63)); s[3] = (char)(0x80 | (c & 63));
    return 4;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
    if (!ps) ps = &g_mbstate_rtomb;
    char tmp[8];
    if (!s) { s = tmp; wc = 0; }
    unsigned c = (unsigned short)wc;
    if (c >= 0xD800 && c < 0xDC00) { ps->_state = c; return 0; }   /* wait for the low half */
    if (c >= 0xDC00 && c < 0xE000) {
        if (!ps->_state) { errno = EILSEQ; return (size_t)-1; }
        c = 0x10000 + ((ps->_state - 0xD800) << 10) + (c - 0xDC00);
    }
    ps->_state = 0;
    return (size_t)utf8_encode(c, s);
}

int wctomb(char *s, wchar_t wc)
{
    if (!s) return 0;
    unsigned c = (unsigned short)wc;
    if (c >= 0xD800 && c < 0xE000) { errno = EILSEQ; return -1; }
    return utf8_encode(c, s);
}

size_t mbsrtowcs(wchar_t *d, const char **src, size_t n, mbstate_t *ps)
{
    const char *s = *src;
    size_t o = 0;
    mbstate_t st = ps ? *ps : (mbstate_t){ 0 };
    for (;;) {
        if (d && o >= n) break;
        wchar_t w;
        size_t r = mbrtowc(&w, s, 4, &st);
        if (r == (size_t)-1) { *src = s; return (size_t)-1; }
        if (r == (size_t)-3) { if (d) d[o] = w; o++; continue; }
        if (r == 0) { if (d) { d[o] = 0; *src = NULL; } break; }
        if (d) d[o] = w;
        o++;
        s += r;
        if (d && st._state && o >= n) break;
    }
    if (d && *src) *src = s;
    if (ps) *ps = st;
    return o;
}

size_t wcsrtombs(char *d, const wchar_t **src, size_t n, mbstate_t *ps)
{
    const wchar_t *s = *src;
    size_t o = 0;
    mbstate_t st = ps ? *ps : (mbstate_t){ 0 };
    for (;; s++) {
        char b[8];
        if (!*s) { if (d && o < n) { d[o] = 0; *src = NULL; } break; }
        size_t r = wcrtomb(b, *s, &st);
        if (r == (size_t)-1) { if (d) *src = s; return (size_t)-1; }
        if (d && o + r > n) break;
        if (d) memcpy(d + o, b, r);
        o += r;
    }
    if (d && *src) *src = s;
    if (ps) *ps = st;
    return o;
}

size_t mbstowcs(wchar_t *d, const char *s, size_t n)
{
    const char *p = s;
    return mbsrtowcs(d, &p, n, NULL);
}
size_t wcstombs(char *d, const wchar_t *s, size_t n)
{
    const wchar_t *p = s;
    return wcsrtombs(d, &p, n, NULL);
}

/* -----------------------------------------------------------------------
 * Wide streams: NovaOS files and the console hold UTF-8, so wide
 * characters are encoded to and decoded from UTF-8 bytes.
 * ----------------------------------------------------------------------- */
wint_t fputwc(wchar_t c, FILE *f)
{
    static unsigned short hi;
    char b[4];
    unsigned cp = (unsigned short)c;
    if (cp >= 0xD800 && cp < 0xDC00) { hi = (unsigned short)cp; return c; }
    if (cp >= 0xDC00 && cp < 0xE000 && hi) { cp = 0x10000 + ((hi - 0xD800u) << 10) + (cp - 0xDC00); hi = 0; }
    int n = utf8_encode(cp, b);
    return fwrite(b, 1, (size_t)n, f) == (size_t)n ? c : WEOF;
}
wint_t putwc(wchar_t c, FILE *f) { return fputwc(c, f); }
wint_t putwchar(wchar_t c) { return fputwc(c, stdout); }

static int g_wpend = -1;                            /* a low surrogate still to return */
wint_t fgetwc(FILE *f)
{
    if (g_wpend >= 0) { wint_t r = (wint_t)g_wpend; g_wpend = -1; return r; }
    unsigned char b[4];
    int c = fgetc(f);
    if (c == EOF) return WEOF;
    b[0] = (unsigned char)c;
    int len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    for (int i = 1; i < len; i++) {
        int d = fgetc(f);
        if (d == EOF) return 0xFFFD;
        b[i] = (unsigned char)d;
    }
    unsigned cp;
    if (utf8_decode(b, (size_t)len, &cp) <= 0) return c < 0x80 ? (wint_t)c : 0xFFFD;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        g_wpend = (int)(0xDC00 + (cp & 0x3FF));
        return (wint_t)(0xD800 + (cp >> 10));
    }
    return (wint_t)cp;
}
wint_t getwc(FILE *f) { return fgetwc(f); }
wint_t getwchar(void) { return fgetwc(stdin); }
wint_t ungetwc(wint_t c, FILE *f)
{
    if (c == WEOF) return WEOF;
    char b[4];
    int n = utf8_encode(c, b);
    if (n == 1) return ungetc((unsigned char)b[0], f) == EOF ? WEOF : c;
    g_wpend = (int)c;                               /* multi-byte: hold it here */
    return c;
}
int fputws(const wchar_t *s, FILE *f)
{
    for (; *s; s++) if (fputwc(*s, f) == WEOF) return EOF;
    return 0;
}
int _putws(const wchar_t *s) { return fputws(s, stdout) == EOF ? EOF : (fputwc(L'\n', stdout) == WEOF ? EOF : 0); }
wchar_t *fgetws(wchar_t *s, int n, FILE *f)
{
    int i = 0;
    while (i < n - 1) {
        wint_t c = fgetwc(f);
        if (c == WEOF) break;
        s[i++] = (wchar_t)c;
        if (c == L'\n') break;
    }
    if (!i) return NULL;
    s[i] = 0;
    return s;
}

/* Wide file names: UTF-8 for the narrow functions */
static char *w2a(const wchar_t *w)
{
    size_t n = wcstombs(NULL, w, 0);
    if (n == (size_t)-1) return NULL;
    char *s = malloc(n + 1);
    if (s) wcstombs(s, w, n + 1);
    return s;
}
FILE *_wfopen(const wchar_t *name, const wchar_t *mode)
{
    char *n = w2a(name), *m = w2a(mode);
    FILE *f = n && m ? fopen(n, m) : NULL;
    free(n); free(m);
    return f;
}
__declspec(dllexport) FILE *_wfreopen(const wchar_t *name, const wchar_t *mode, FILE *f)
{
    char *n = w2a(name), *m = w2a(mode);
    FILE *r = n && m ? freopen(n, m, f) : NULL;
    free(n); free(m);
    return r;
}
int _wremove(const wchar_t *name) { char *n = w2a(name); int r = n ? remove(n) : -1; free(n); return r; }
int _wunlink(const wchar_t *name) { return _wremove(name); }
int _wrename(const wchar_t *a, const wchar_t *b)
{
    char *x = w2a(a), *y = w2a(b);
    int r = x && y ? rename(x, y) : -1;
    free(x); free(y);
    return r;
}
wchar_t *_wgetenv(const wchar_t *name)
{
    /* one cached result per name is enough for programs that read, not keep */
    static wchar_t *last;
    char *n = w2a(name);
    const char *v = n ? getenv(n) : NULL;
    free(n);
    if (!v) return NULL;
    size_t len = mbstowcs(NULL, v, 0);
    if (len == (size_t)-1) return NULL;
    free(last);
    last = malloc((len + 1) * sizeof(wchar_t));
    if (last) mbstowcs(last, v, len + 1);
    return last;
}

size_t wcsftime(wchar_t *s, size_t n, const wchar_t *fmt, const struct tm *t)
{
    char *f = w2a(fmt);
    if (!f || !n) { free(f); return 0; }
    char *buf = malloc(n * 4 + 1);
    size_t r = buf ? strftime(buf, n * 4 + 1, f, t) : 0;
    free(f);
    if (!r) { free(buf); return 0; }
    size_t w = mbstowcs(s, buf, n);
    free(buf);
    if (w == (size_t)-1 || w >= n) return 0;
    s[w] = 0;
    return w;
}

/* The locale-taking forms: one locale, the "C" one */
__declspec(dllexport) long _wcstol_l(const wchar_t *s, wchar_t **end, int base, void *loc) { (void)loc; return wcstol(s, end, base); }
__declspec(dllexport) unsigned long _wcstoul_l(const wchar_t *s, wchar_t **end, int base, void *loc) { (void)loc; return wcstoul(s, end, base); }
__declspec(dllexport) double _wcstod_l(const wchar_t *s, wchar_t **end, void *loc) { (void)loc; return wcstod(s, end); }
__declspec(dllexport) float _wcstof_l(const wchar_t *s, wchar_t **end, void *loc) { (void)loc; return wcstof(s, end); }
