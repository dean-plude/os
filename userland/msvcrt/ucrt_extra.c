/* ucrtbase: entry points the Visual C++ runtime (msvcp140.dll) and
 * programs built with it call that the other files do not provide —
 * locale time names, stream buffer views, the _s variants and the
 * "dispatch" forms of _sopen.
 */
#define NOVA_BUILD_MSVCRT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <windows.h>
#include "msvcrt_internal.h"

#define CRTEXP __declspec(dllexport)
typedef int errno_t;

/* -----------------------------------------------------------------------
 * <ctype.h>: Microsoft's classification table (index -1 is EOF)
 * ----------------------------------------------------------------------- */
#define C_UPPER 0x1
#define C_LOWER 0x2
#define C_DIGIT 0x4
#define C_SPACE 0x8
#define C_PUNCT 0x10
#define C_CNTRL 0x20
#define C_BLANK 0x40
#define C_HEX   0x80
#define C_ALPHA 0x100

/* The "C" locale's character classes, [0] for EOF (-1): the table old
 * programs read directly as msvcrt's _ctype and _pctype data exports */
CRTEXP const unsigned short _ctype[257] = {
    0,                                                      /* EOF */
    0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x068, 0x028, 0x028, 0x028, 0x028, 0x020, 0x020,
    0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020,
    0x048, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010,
    0x084, 0x084, 0x084, 0x084, 0x084, 0x084, 0x084, 0x084, 0x084, 0x084, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010,
    0x010, 0x181, 0x181, 0x181, 0x181, 0x181, 0x181, 0x101, 0x101, 0x101, 0x101, 0x101, 0x101, 0x101, 0x101, 0x101,
    0x101, 0x101, 0x101, 0x101, 0x101, 0x101, 0x101, 0x101, 0x101, 0x101, 0x101, 0x010, 0x010, 0x010, 0x010, 0x010,
    0x010, 0x182, 0x182, 0x182, 0x182, 0x182, 0x182, 0x102, 0x102, 0x102, 0x102, 0x102, 0x102, 0x102, 0x102, 0x102,
    0x102, 0x102, 0x102, 0x102, 0x102, 0x102, 0x102, 0x102, 0x102, 0x102, 0x102, 0x010, 0x010, 0x010, 0x010, 0x020,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
};
CRTEXP const unsigned short *_pctype = _ctype + 1;

static const unsigned short *ctype_table(void)
{
    return _ctype + 1;
}

CRTEXP const unsigned short *__pctype_func(void) { return ctype_table(); }
CRTEXP const unsigned short *__pwctype_func(void) { return ctype_table(); }   /* the first 256 wide characters */
CRTEXP const unsigned short **__p__pctype(void) { return &_pctype; }

/* -----------------------------------------------------------------------
 * Locale time names (the "C" locale), as msvcp140's time_get/time_put
 * read them
 * ----------------------------------------------------------------------- */
static const char *const g_wday_abbr[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const g_wday[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const g_mon_abbr[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const g_mon[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                       "September", "October", "November", "December" };
static const char *const g_ampm[2] = { "AM", "PM" };

/* ":Sun:Sunday:Mon:Monday..." (malloc'd; the caller frees it) */
static char *names_list(const char *const *abbr, const char *const *full, int n)
{
    size_t len = 1;
    for (int i = 0; i < n; i++) len += strlen(abbr[i]) + strlen(full[i]) + 2;
    char *s = malloc(len), *p = s;
    if (!s) return 0;
    for (int i = 0; i < n; i++) p += sprintf(p, ":%s:%s", abbr[i], full[i]);
    *p = 0;
    return s;
}
static wchar_t *wide_dup(const char *s)
{
    size_t n = strlen(s);
    wchar_t *w = malloc((n + 1) * sizeof(wchar_t));
    if (w) for (size_t i = 0; i <= n; i++) w[i] = (unsigned char)s[i];
    return w;
}
CRTEXP char *_Getdays(void)   { return names_list(g_wday_abbr, g_wday, 7); }
CRTEXP char *_Getmonths(void) { return names_list(g_mon_abbr, g_mon, 12); }
CRTEXP wchar_t *_W_Getdays(void)   { char *s = _Getdays();   wchar_t *w = s ? wide_dup(s) : 0; free(s); return w; }
CRTEXP wchar_t *_W_Getmonths(void) { char *s = _Getmonths(); wchar_t *w = s ? wide_dup(s) : 0; free(s); return w; }

/* __crt_lc_time_data, in one block (strings after the structure) */
typedef struct {
    char *wday_abbr[7], *wday[7], *month_abbr[12], *month[12], *ampm[2];
    char *sdatefmt, *ldatefmt, *timefmt;
    int caltype;
    long refcount;
    wchar_t *w_wday_abbr[7], *w_wday[7], *w_month_abbr[12], *w_month[12], *w_ampm[2];
    wchar_t *w_sdatefmt, *w_ldatefmt, *w_timefmt, *w_locale_name;
} LcTime;

CRTEXP void *_Gettnames(void)
{
    const char *all[43];
    int k = 0;
    for (int i = 0; i < 7; i++) all[k++] = g_wday_abbr[i];
    for (int i = 0; i < 7; i++) all[k++] = g_wday[i];
    for (int i = 0; i < 12; i++) all[k++] = g_mon_abbr[i];
    for (int i = 0; i < 12; i++) all[k++] = g_mon[i];
    all[k++] = g_ampm[0]; all[k++] = g_ampm[1];
    all[k++] = "MM/dd/yy"; all[k++] = "dddd, MMMM dd, yyyy"; all[k++] = "HH:mm:ss";
    size_t bytes = sizeof(LcTime) + sizeof(wchar_t) * 4;          /* + the empty locale name */
    for (int i = 0; i < k; i++) bytes += (strlen(all[i]) + 1) * (1 + sizeof(wchar_t));
    LcTime *t = calloc(1, bytes);
    if (!t) return 0;
    char **np = &t->wday_abbr[0];                /* 43 narrow pointers in a row */
    wchar_t **wp = &t->w_wday_abbr[0];           /* then 43 wide ones */
    char *s = (char *)(t + 1);
    for (int i = 0; i < k; i++) {
        size_t n = strlen(all[i]) + 1;
        wchar_t *w = (wchar_t *)s;
        for (size_t j = 0; j < n; j++) w[j] = (unsigned char)all[i][j];
        wp[i] = w;
        s += n * sizeof(wchar_t);
        memcpy(s, all[i], n);
        np[i] = s;
        s += n;
    }
    s = (char *)(((ULONG_PTR)s + 1) & ~(ULONG_PTR)1);
    t->w_locale_name = (wchar_t *)s;
    t->caltype = 1;
    t->refcount = 1;
    return t;
}
CRTEXP void *_W_Gettnames(void) { return _Gettnames(); }

CRTEXP size_t _Strftime(char *s, size_t n, const char *f, const struct tm *t, void *lc) { (void)lc; return strftime(s, n, f, t); }
CRTEXP size_t _Wcsftime(wchar_t *s, size_t n, const wchar_t *f, const struct tm *t, void *lc) { (void)lc; return wcsftime(s, n, f, t); }

/* -----------------------------------------------------------------------
 * Strings, environment, files
 * ----------------------------------------------------------------------- */
CRTEXP size_t __strncnt(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
CRTEXP size_t __wcsncnt(const wchar_t *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }

CRTEXP errno_t _strupr_s(char *s, size_t n)
{
    if (!s || strnlen(s, n) >= n) return EINVAL;
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32;
    return 0;
}
CRTEXP errno_t _strlwr_s(char *s, size_t n)
{
    if (!s || strnlen(s, n) >= n) return EINVAL;
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
    return 0;
}
CRTEXP errno_t _wcslwr_s(wchar_t *s, size_t n)
{
    if (!s || wcsnlen(s, n) >= n) return EINVAL;
    for (; *s; s++) *s = (wchar_t)towlower(*s);
    return 0;
}
CRTEXP errno_t _wcsupr_s(wchar_t *s, size_t n)
{
    if (!s || wcsnlen(s, n) >= n) return EINVAL;
    for (; *s; s++) *s = (wchar_t)towupper(*s);
    return 0;
}

/* The process environment is the one store: getenv reads it live */
CRTEXP int _putenv(const char *def)
{
    const char *eq = def ? strchr(def, '=') : 0;
    if (!eq || eq == def) { errno = EINVAL; return -1; }
    char name[256];
    size_t n = (size_t)(eq - def);
    if (n >= sizeof(name)) { errno = EINVAL; return -1; }
    memcpy(name, def, n);
    name[n] = 0;
    return SetEnvironmentVariableA(name, eq[1] ? eq + 1 : 0) || !eq[1] ? 0 : -1;
}
CRTEXP int _wputenv(const wchar_t *def)
{
    const wchar_t *eq = def ? wcschr(def, L'=') : 0;
    if (!eq || eq == def) { errno = EINVAL; return -1; }
    wchar_t name[256];
    size_t n = (size_t)(eq - def);
    if (n >= 256) { errno = EINVAL; return -1; }
    memcpy(name, def, n * sizeof(wchar_t));
    name[n] = 0;
    return SetEnvironmentVariableW(name, eq[1] ? eq + 1 : 0) || !eq[1] ? 0 : -1;
}
CRTEXP errno_t _putenv_s(const char *name, const char *value)
{
    return SetEnvironmentVariableA(name, value && *value ? value : 0) || !(value && *value) ? 0 : EINVAL;
}
CRTEXP errno_t _wputenv_s(const wchar_t *name, const wchar_t *value)
{
    return SetEnvironmentVariableW(name, value && *value ? value : 0) || !(value && *value) ? 0 : EINVAL;
}
CRTEXP errno_t _dupenv_s(char **buf, size_t *len, const char *name)
{
    if (!buf || !name) return EINVAL;
    *buf = 0;
    if (len) *len = 0;
    DWORD n = GetEnvironmentVariableA(name, 0, 0);
    if (!n) return 0;
    char *v = malloc(n);
    if (!v) return ENOMEM;
    GetEnvironmentVariableA(name, v, n);
    *buf = v;
    if (len) *len = strlen(v) + 1;
    return 0;
}
CRTEXP errno_t _wdupenv_s(wchar_t **buf, size_t *len, const wchar_t *name)
{
    if (!buf || !name) return EINVAL;
    *buf = 0;
    if (len) *len = 0;
    DWORD n = GetEnvironmentVariableW(name, 0, 0);
    if (!n) return 0;
    wchar_t *v = malloc(n * sizeof(wchar_t));
    if (!v) return ENOMEM;
    GetEnvironmentVariableW(name, v, n);
    *buf = v;
    if (len) *len = wcslen(v) + 1;
    return 0;
}

/* One drive (C:): its current directory is the current directory */
CRTEXP char *_getdcwd(int drive, char *buf, int n)
{
    if (drive && drive != 3) { errno = EACCES; return 0; }
    extern char *_getcwd(char *, int);
    return _getcwd(buf, n);
}
CRTEXP wchar_t *_wgetdcwd(int drive, wchar_t *buf, int n)
{
    if (drive && drive != 3) { errno = EACCES; return 0; }
    extern wchar_t *_wgetcwd(wchar_t *, int);
    return _wgetcwd(buf, n);
}

CRTEXP FILE *_fsopen(const char *name, const char *mode, int share) { (void)share; return fopen(name, mode); }
CRTEXP FILE *_wfsopen(const wchar_t *name, const wchar_t *mode, int share) { (void)share; return _wfopen(name, mode); }
CRTEXP errno_t fopen_s(FILE **f, const char *name, const char *mode)
{
    if (!f) return EINVAL;
    *f = fopen(name, mode);
    return *f ? 0 : errno ? errno : ENOENT;
}
CRTEXP errno_t _wfopen_s(FILE **f, const wchar_t *name, const wchar_t *mode)
{
    if (!f) return EINVAL;
    *f = _wfopen(name, mode);
    return *f ? 0 : errno ? errno : ENOENT;
}

/* errno_t _sopen_dispatch(path, oflag, shflag, pmode, int *fd, int secure) */
CRTEXP errno_t _sopen_dispatch(const char *p, int flags, int share, int perm, int *fd, int secure)
{
    (void)secure;
    extern int _sopen_s(int *, const char *, int, int, int);
    return _sopen_s(fd, p, flags, share, perm);
}
CRTEXP errno_t _wsopen_dispatch(const wchar_t *p, int flags, int share, int perm, int *fd, int secure)
{
    (void)secure;
    extern int _wsopen_s(int *, const wchar_t *, int, int, int);
    return _wsopen_s(fd, p, flags, share, perm);
}

/* The CRT's view of a stream's buffer (base, next, count), which msvcp140's
 * basic_filebuf reads to work in it directly.  NovaOS's FILE keeps its
 * buffer its own way, so each stream shows an empty one: the library then
 * goes through fgetc/fputc and friends. */
typedef struct { FILE *f; char *base, *ptr; int cnt; } StreamView;
static StreamView g_views[FOPEN_MAX + 3];

CRTEXP void _get_stream_buffer_pointers(FILE *f, char ***base, char ***ptr, int **cnt)
{
    StreamView *v = 0;
    for (size_t i = 0; i < sizeof(g_views) / sizeof(g_views[0]) && !v; i++)
        if (g_views[i].f == f) v = &g_views[i];
    for (size_t i = 0; i < sizeof(g_views) / sizeof(g_views[0]) && !v; i++)
        if (!g_views[i].f) { v = &g_views[i]; v->f = f; }
    if (!v) v = &g_views[0];
    v->base = v->ptr = 0;
    v->cnt = 0;
    if (base) *base = &v->base;
    if (ptr) *ptr = &v->ptr;
    if (cnt) *cnt = &v->cnt;
}

/* -----------------------------------------------------------------------
 * Locales: one, the "C" locale; a _locale_t names it
 * ----------------------------------------------------------------------- */
typedef struct { void *locinfo, *mbcinfo; } LocaleObj;
static LocaleObj g_c_locale;
CRTEXP void *_create_locale(int category, const char *name) { (void)category; (void)name; return &g_c_locale; }
CRTEXP void *_wcreate_locale(int category, const wchar_t *name) { (void)category; (void)name; return &g_c_locale; }
CRTEXP void *_get_current_locale(void) { return &g_c_locale; }
CRTEXP void _free_locale(void *l) { (void)l; }

/* The _l forms take a _locale_t; there is only the "C" locale (LLVM's
 * libc++ calls these for every std::locale facet) */
size_t strxfrm(char *d, const char *s, size_t n);
CRTEXP int _isctype(int c, int mask) { return c >= -1 && c < 256 ? ctype_table()[c] & mask : 0; }
CRTEXP int _isctype_l(int c, int mask, void *l) { (void)l; return _isctype(c, mask); }
CRTEXP int _tolower_l(int c, void *l) { (void)l; return tolower(c); }
CRTEXP int _toupper_l(int c, void *l) { (void)l; return toupper(c); }
CRTEXP wint_t _towlower_l(wint_t c, void *l) { (void)l; return towlower(c); }
CRTEXP wint_t _towupper_l(wint_t c, void *l) { (void)l; return towupper(c); }
CRTEXP int _iswalpha_l(wint_t c, void *l) { (void)l; return iswalpha(c); }
CRTEXP int _iswcntrl_l(wint_t c, void *l) { (void)l; return iswcntrl(c); }
CRTEXP int _iswdigit_l(wint_t c, void *l) { (void)l; return iswdigit(c); }
CRTEXP int _iswlower_l(wint_t c, void *l) { (void)l; return iswlower(c); }
CRTEXP int _iswprint_l(wint_t c, void *l) { (void)l; return iswprint(c); }
CRTEXP int _iswpunct_l(wint_t c, void *l) { (void)l; return iswpunct(c); }
CRTEXP int _iswspace_l(wint_t c, void *l) { (void)l; return iswspace(c); }
CRTEXP int _iswupper_l(wint_t c, void *l) { (void)l; return iswupper(c); }
CRTEXP int _iswxdigit_l(wint_t c, void *l) { (void)l; return iswxdigit(c); }
CRTEXP size_t _strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
CRTEXP int _wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscoll(a, b); }
CRTEXP size_t _wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }
CRTEXP int _mbtowc_l(wchar_t *pwc, const char *s, size_t n, void *l) { (void)l; return mbtowc(pwc, s, n); }
CRTEXP double _strtod_l(const char *s, char **e, void *l) { (void)l; return strtod(s, e); }
CRTEXP long _strtol_l(const char *s, char **e, int b, void *l) { (void)l; return strtol(s, e, b); }
CRTEXP long long _strtoi64_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
CRTEXP unsigned long long _strtoui64_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
CRTEXP errno_t wcrtomb_s(size_t *ret, char *s, size_t n, wchar_t wc, mbstate_t *ps)
{
    char tmp[MB_LEN_MAX];
    size_t r = wcrtomb(s ? tmp : NULL, wc, ps);
    if (r == (size_t)-1) { if (ret) *ret = r; return errno = EILSEQ; }
    if (s && r > n) { if (n) s[0] = 0; if (ret) *ret = (size_t)-1; return errno = ERANGE; }
    if (s) memcpy(s, tmp, r);
    if (ret) *ret = r;
    return 0;
}
CRTEXP void _swab(char *src, char *dst, int n)
{
    for (int i = 0; i + 1 < n; i += 2) { char a = src[i], b = src[i + 1]; dst[i] = b; dst[i + 1] = a; }
}

CRTEXP int iswascii(wint_t c) { return c < 0x80; }
CRTEXP errno_t _ltow_s(long v, wchar_t *buf, size_t n, int radix)
{
    if (!buf || !n || radix < 2 || radix > 36) return EINVAL;
    wchar_t tmp[40];
    int k = 0, neg = radix == 10 && v < 0;
    unsigned long u = neg ? 0UL - (unsigned long)v : (unsigned long)v;
    do { int d = (int)(u % (unsigned)radix); tmp[k++] = (wchar_t)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned)radix; } while (u);
    if ((size_t)(k + neg + 1) > n) { buf[0] = 0; return ERANGE; }
    size_t i = 0;
    if (neg) buf[i++] = L'-';
    while (k) buf[i++] = tmp[--k];
    buf[i] = 0;
    return 0;
}

/* The other secure integer-to-wide conversions: as _ltow_s, a minus sign
 * only in base 10 */
static errno_t u64tow_s(unsigned long long u, int neg, wchar_t *buf, size_t n, int radix)
{
    if (!buf || !n) return EINVAL;
    if (radix < 2 || radix > 36) { buf[0] = 0; return EINVAL; }
    wchar_t tmp[72];
    int k = 0;
    do { int d = (int)(u % (unsigned)radix); tmp[k++] = (wchar_t)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned)radix; } while (u);
    if ((size_t)(k + neg + 1) > n) { buf[0] = 0; return ERANGE; }
    size_t i = 0;
    if (neg) buf[i++] = L'-';
    while (k) buf[i++] = tmp[--k];
    buf[i] = 0;
    return 0;
}
CRTEXP errno_t _ultow_s(unsigned long v, wchar_t *buf, size_t n, int radix) { return u64tow_s(v, 0, buf, n, radix); }
CRTEXP errno_t _ui64tow_s(unsigned long long v, wchar_t *buf, size_t n, int radix) { return u64tow_s(v, 0, buf, n, radix); }
CRTEXP errno_t _i64tow_s(long long v, wchar_t *buf, size_t n, int radix)
{
    int neg = radix == 10 && v < 0;
    return u64tow_s(neg ? 0ULL - (unsigned long long)v : (unsigned long long)v, neg, buf, n, radix);
}
CRTEXP errno_t _itow_s(int v, wchar_t *buf, size_t n, int radix)
{
    int neg = radix == 10 && v < 0;
    return u64tow_s(neg ? 0ULL - (unsigned long long)(long long)v : (unsigned)v, neg, buf, n, radix);
}

/* Defined elsewhere without the export (on x86 the linker drops one
 * leading underscore from an /EXPORT name) */
#ifdef _WIN64
__asm__(".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:_wcstoui64 /EXPORT:_wcstoi64\"\n\t.text\n");
#else
__asm__(".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:__wcstoui64 /EXPORT:__wcstoi64\"\n\t.text\n");
#endif

/* -----------------------------------------------------------------------
 * <conio.h>: the console, one character at a time (no echo, no line
 * editing); a character can be pushed back
 * ----------------------------------------------------------------------- */
static wint_t g_ungot = WEOF;
CRTEXP wint_t _getwch(void)
{
    if (g_ungot != WEOF) { wint_t c = g_ungot; g_ungot = WEOF; return c; }
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0, got = 0;
    BOOL con = GetConsoleMode(in, &mode);
    if (con) SetConsoleMode(in, mode & ~(DWORD)(0x2 | 0x4)  /* ENABLE_LINE_INPUT, ENABLE_ECHO_INPUT */);
    WCHAR c = 0;
    BOOL ok = con ? ReadConsoleW(in, &c, 1, &got, 0) : ReadFile(in, &c, 1, &got, 0);
    if (con) SetConsoleMode(in, mode);
    return ok && got ? (wint_t)c : WEOF;
}
CRTEXP int _getch(void) { wint_t c = _getwch(); return c == WEOF ? EOF : (int)(c & 0xFF); }
CRTEXP wint_t _putwch(wchar_t c)
{
    DWORD w;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    return WriteConsoleW(out, &c, 1, &w, 0) || WriteFile(out, &c, 1, &w, 0) ? (wint_t)c : WEOF;
}
CRTEXP int _putch(int c) { return _putwch((wchar_t)(unsigned char)c) == WEOF ? EOF : c; }
CRTEXP wint_t _getwche(void) { wint_t c = _getwch(); if (c != WEOF) _putwch((wchar_t)c); return c; }
CRTEXP int _getche(void) { int c = _getch(); if (c != EOF) _putch(c); return c; }
CRTEXP wint_t _ungetwch(wint_t c) { if (g_ungot != WEOF || c == WEOF) return WEOF; g_ungot = c; return c; }
CRTEXP int _ungetch(int c) { return _ungetwch((wint_t)(unsigned char)c) == WEOF ? EOF : c; }
CRTEXP int _kbhit(void)
{
    if (g_ungot != WEOF) return 1;
    DWORD n = 0;
    return GetNumberOfConsoleInputEvents(GetStdHandle(STD_INPUT_HANDLE), &n) && n > 0;
}

/* -----------------------------------------------------------------------
 * The wide spawn/exec forms: the narrow ones in UTF-8
 * ----------------------------------------------------------------------- */
extern intptr_t _spawnve(int, const char *, const char *const *, const char *const *);
extern intptr_t _execve(const char *, const char *const *, const char *const *);
static char *u8(const wchar_t *w)
{
    if (!w) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
    char *s = malloc(n > 0 ? (size_t)n : 1);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, 0, 0);
    return s;
}
static char **u8v(const wchar_t *const *v)
{
    if (!v) return 0;
    size_t n = 0;
    while (v[n]) n++;
    char **r = calloc(n + 1, sizeof(char *));
    if (r) for (size_t i = 0; i < n; i++) r[i] = u8(v[i]);
    return r;
}
static void free_v(char **v) { if (v) { for (size_t i = 0; v[i]; i++) free(v[i]); free(v); } }
CRTEXP intptr_t _wspawnve(int m, const wchar_t *p, const wchar_t *const *a, const wchar_t *const *e)
{
    char *pp = u8(p), **aa = u8v(a), **ee = u8v(e);
    intptr_t r = _spawnve(m, pp, (const char *const *)aa, (const char *const *)ee);
    free(pp); free_v(aa); free_v(ee);
    return r;
}
CRTEXP intptr_t _wspawnv(int m, const wchar_t *p, const wchar_t *const *a) { return _wspawnve(m, p, a, 0); }
CRTEXP intptr_t _wexecve(const wchar_t *p, const wchar_t *const *a, const wchar_t *const *e)
{
    char *pp = u8(p), **aa = u8v(a), **ee = u8v(e);
    intptr_t r = _execve(pp, (const char *const *)aa, (const char *const *)ee);
    free(pp); free_v(aa); free_v(ee);
    return r;
}
CRTEXP intptr_t _wexecv(const wchar_t *p, const wchar_t *const *a) { return _wexecve(p, a, 0); }

/* -----------------------------------------------------------------------
 * Error message table and floating-point rounding mode
 * ----------------------------------------------------------------------- */
#define NSYSERR 43
static char *g_errlist[NSYSERR];
static int g_nerr = NSYSERR;
CRTEXP char **__sys_errlist(void)
{
    if (!g_errlist[0]) for (int i = 0; i < NSYSERR; i++) g_errlist[i] = strerror(i);
    return g_errlist;
}
CRTEXP int *__sys_nerr(void) { return &g_nerr; }
/* msvcrt's own names are the table and the count themselves (MinGW's
 * sys_errlist / sys_nerr); the table fills in when the DLL starts */
CRTEXP char *_sys_errlist[NSYSERR];
CRTEXP int _sys_nerr = NSYSERR;
void __nova_init_errlist(void) { for (int i = 0; i < NSYSERR; i++) _sys_errlist[i] = strerror(i); }
CRTEXP int __fpe_flt_rounds(void)
{
    unsigned m;
    __asm__ volatile("stmxcsr %0" : "=m"(m));
    switch ((m >> 13) & 3) { case 0: return 1; case 1: return 3; case 2: return 2; default: return 0; }
}

/* -----------------------------------------------------------------------
 * Odds and ends Firefox (xul, nss3) imports
 * ----------------------------------------------------------------------- */
CRTEXP errno_t getenv_s(size_t *len, char *buf, size_t size, const char *name)
{
    if (!len || !name || (!buf && size)) return EINVAL;
    *len = 0;
    if (buf && size) buf[0] = 0;
    DWORD n = GetEnvironmentVariableA(name, 0, 0);
    if (!n) return 0;
    *len = n;                                   /* includes the terminator */
    if (!buf) return 0;
    if (n > size) return ERANGE;
    GetEnvironmentVariableA(name, buf, (DWORD)size);
    return 0;
}
CRTEXP errno_t _wgetenv_s(size_t *len, wchar_t *buf, size_t size, const wchar_t *name)
{
    if (!len || !name || (!buf && size)) return EINVAL;
    *len = 0;
    if (buf && size) buf[0] = 0;
    DWORD n = GetEnvironmentVariableW(name, 0, 0);
    if (!n) return 0;
    *len = n;
    if (!buf) return 0;
    if (n > size) return ERANGE;
    GetEnvironmentVariableW(name, buf, (DWORD)size);
    return 0;
}

wchar_t *_wmktemp(wchar_t *t);
CRTEXP errno_t _wmktemp_s(wchar_t *t, size_t n)
{
    if (!t || !n || wcsnlen(t, n) >= n) return EINVAL;
    return _wmktemp(t) ? 0 : errno;
}

typedef struct { long long quot, rem; } lldiv_t_;
CRTEXP lldiv_t_ lldiv(long long a, long long b) { lldiv_t_ r = { a / b, a % b }; return r; }
CRTEXP lldiv_t_ imaxdiv(long long a, long long b) { lldiv_t_ r = { a / b, a % b }; return r; }

