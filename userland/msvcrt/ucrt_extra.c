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

static unsigned short g_ctype[257];

static const unsigned short *ctype_table(void)
{
    if (!g_ctype[1 + 'A']) {
        for (int c = 0; c < 256; c++) {
            unsigned short v = 0;
            if (c < 0x20 || c == 0x7F) v |= C_CNTRL;
            if (c == ' ' || (c >= 9 && c <= 13)) v |= C_SPACE;
            if (c == ' ' || c == '\t') v |= C_BLANK;
            if (c >= 'A' && c <= 'Z') v |= C_UPPER | C_ALPHA;
            if (c >= 'a' && c <= 'z') v |= C_LOWER | C_ALPHA;
            if (c >= '0' && c <= '9') v |= C_DIGIT | C_HEX;
            if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) v |= C_HEX;
            if (c > 0x20 && c < 0x7F && !(v & (C_ALPHA | C_DIGIT))) v |= C_PUNCT;
            g_ctype[1 + c] = v;
        }
    }
    return g_ctype + 1;
}
CRTEXP const unsigned short *__pctype_func(void) { return ctype_table(); }
CRTEXP const unsigned short *__pwctype_func(void) { return ctype_table(); }   /* the first 256 wide characters */
CRTEXP const unsigned short **__p__pctype(void) { static const unsigned short *p; p = ctype_table(); return &p; }

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
