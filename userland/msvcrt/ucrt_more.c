/* ucrtbase: more of the Universal CRT — the _s conversions, the _nolock
 * stream forms, temporary names, _creat, file times (_utime), the wide
 * PATH-searching spawns and the multibyte string functions (the ANSI code
 * page is single-byte for these, so they are the byte string ones).
 */
#define NOVA_BUILD_MSVCRT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include <ctype.h>
#include <windows.h>
#include "msvcrt_internal.h"
#include "ptd.h"

#define CRTEXP __declspec(dllexport)
typedef int errno_t;
#define STRUNCATE_ 80

extern int _open(const char *path, int flags, ...);
extern int _wopen(const wchar_t *path, int flags, ...);
extern intptr_t _get_osfhandle(int fd);
extern intptr_t _spawnvpe(int, const char *, const char *const *, const char *const *);
extern FILE *_wfreopen(const wchar_t *, const wchar_t *, FILE *);
WINBASEAPI BOOL WINAPI SetFileTime(HANDLE, const FILETIME *, const FILETIME *, const FILETIME *);
#define FILE_WRITE_ATTRIBUTES_      0x0100
#define FILE_FLAG_BACKUP_SEMANTICS_ 0x02000000

/* -----------------------------------------------------------------------
 * mbstowcs_s / wcstombs_s
 * ----------------------------------------------------------------------- */
CRTEXP errno_t mbstowcs_s(size_t *done, wchar_t *dst, size_t cap, const char *src, size_t count)
{
    if (done) *done = 0;
    if ((!dst && cap) || (dst && !cap) || !src) { if (dst && cap) dst[0] = 0; errno = EINVAL; return EINVAL; }
    size_t need = mbstowcs(NULL, src, 0);
    if (need == (size_t)-1) { if (dst) dst[0] = 0; errno = EILSEQ; return EILSEQ; }
    size_t n = need < count ? need : count;                 /* count == _TRUNCATE (-1) takes all */
    if (!dst) { if (done) *done = n + 1; return 0; }
    int trunc = 0;
    if (n >= cap) {
        if (count != (size_t)-1) { dst[0] = 0; errno = ERANGE; return ERANGE; }
        n = cap - 1; trunc = 1;
    }
    wchar_t *tmp = malloc((need + 1) * sizeof(wchar_t));
    if (!tmp) { dst[0] = 0; errno = ENOMEM; return ENOMEM; }
    mbstowcs(tmp, src, need + 1);
    memcpy(dst, tmp, n * sizeof(wchar_t));
    dst[n] = 0;
    free(tmp);
    if (done) *done = n + 1;
    return trunc ? STRUNCATE_ : 0;
}

CRTEXP errno_t wcstombs_s(size_t *done, char *dst, size_t cap, const wchar_t *src, size_t count)
{
    if (done) *done = 0;
    if ((!dst && cap) || (dst && !cap) || !src) { if (dst && cap) dst[0] = 0; errno = EINVAL; return EINVAL; }
    size_t need = wcstombs(NULL, src, 0);
    if (need == (size_t)-1) { if (dst) dst[0] = 0; errno = EILSEQ; return EILSEQ; }
    size_t n = need < count ? need : count;                 /* count is in bytes here */
    if (!dst) { if (done) *done = n + 1; return 0; }
    int trunc = 0;
    if (n >= cap) {
        if (count != (size_t)-1) { dst[0] = 0; errno = ERANGE; return ERANGE; }
        n = cap - 1; trunc = 1;
    }
    char *tmp = malloc(need + 1);
    if (!tmp) { dst[0] = 0; errno = ENOMEM; return ENOMEM; }
    wcstombs(tmp, src, need + 1);
    /* never cut a UTF-8 sequence in half */
    while (n && (tmp[n] & 0xC0) == 0x80) n--;
    memcpy(dst, tmp, n);
    dst[n] = 0;
    free(tmp);
    if (done) *done = n + 1;
    return trunc ? STRUNCATE_ : 0;
}

/* -----------------------------------------------------------------------
 * The _nolock stream functions: NovaOS's streams take no per-stream lock
 * ----------------------------------------------------------------------- */
CRTEXP int _fflush_nolock(FILE *f) { return fflush(f); }
CRTEXP int _fclose_nolock(FILE *f) { return fclose(f); }
CRTEXP size_t _fread_nolock(void *p, size_t sz, size_t n, FILE *f) { return fread(p, sz, n, f); }
CRTEXP size_t _fwrite_nolock(const void *p, size_t sz, size_t n, FILE *f) { return fwrite(p, sz, n, f); }
CRTEXP int _fseek_nolock(FILE *f, long off, int whence) { return fseek(f, off, whence); }
CRTEXP long _ftell_nolock(FILE *f) { return ftell(f); }
CRTEXP int _fgetc_nolock(FILE *f) { return fgetc(f); }
CRTEXP int _fputc_nolock(int c, FILE *f) { return fputc(c, f); }
CRTEXP int _getc_nolock(FILE *f) { return fgetc(f); }
CRTEXP int _putc_nolock(int c, FILE *f) { return fputc(c, f); }
CRTEXP int _ungetc_nolock(int c, FILE *f) { return ungetc(c, f); }

/* -----------------------------------------------------------------------
 * freopen_s, temporary names, _creat
 * ----------------------------------------------------------------------- */
CRTEXP errno_t freopen_s(FILE **out, const char *path, const char *mode, FILE *f)
{
    if (!out || !mode || !f) { errno = EINVAL; return EINVAL; }
    *out = freopen(path, mode, f);
    return *out ? 0 : errno ? errno : EINVAL;
}
CRTEXP errno_t _wfreopen_s(FILE **out, const wchar_t *path, const wchar_t *mode, FILE *f)
{
    if (!out || !mode || !f) { errno = EINVAL; return EINVAL; }
    *out = _wfreopen(path, mode, f);
    return *out ? 0 : errno ? errno : EINVAL;
}

/* "\sNNNN.0" names in the temporary folder, like the UCRT's: each call
 * moves on to a name no file has */
static LONG g_tmp_seq;
static int tmp_name(wchar_t *buf, size_t cap)
{
    wchar_t dir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (!n || n >= MAX_PATH) return ENOENT;
    for (int tries = 0; tries < 10000; tries++) {
        LONG k = InterlockedIncrement(&g_tmp_seq);
        unsigned v = (unsigned)(GetCurrentProcessId() * 7919u + (unsigned)k);
        wchar_t name[MAX_PATH + 24];
        int len = 0;
        for (DWORD i = 0; i < n; i++) name[len++] = dir[i];
        name[len++] = L's';
        for (int b = 28; b >= 0; b -= 4) name[len++] = L"0123456789abcdef"[(v >> b) & 15];
        name[len++] = L'.'; name[len++] = L'0'; name[len] = 0;
        if (GetFileAttributesW(name) != INVALID_FILE_ATTRIBUTES) continue;
        if ((size_t)len + 1 > cap) return ERANGE;
        memcpy(buf, name, (len + 1) * sizeof(wchar_t));
        return 0;
    }
    return EEXIST;
}
CRTEXP errno_t _wtmpnam_s(wchar_t *buf, size_t cap)
{
    if (!buf || !cap) { errno = EINVAL; return EINVAL; }
    int e = tmp_name(buf, cap);
    if (e) { buf[0] = 0; errno = e; }
    return e;
}
CRTEXP errno_t tmpnam_s(char *buf, size_t cap)
{
    wchar_t w[MAX_PATH + 24];
    if (!buf || !cap) { errno = EINVAL; return EINVAL; }
    int e = tmp_name(w, MAX_PATH + 24);
    if (!e && !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, 0, 0)) e = ERANGE;
    if (e) { buf[0] = 0; errno = e; }
    return e;
}
/* tmpnam(NULL) fills a per-thread buffer, as on Windows */
CRTEXP char *tmpnam(char *buf)
{
    size_t n = MAX_PATH;
    if (!buf) { buf = __nova_ptd()->tmpnam; n = sizeof(__nova_ptd()->tmpnam); }
    return tmpnam_s(buf, n) ? 0 : buf;
}
CRTEXP wchar_t *_wtmpnam(wchar_t *buf)
{
    if (!buf) buf = __nova_ptd()->wtmpnam;
    return _wtmpnam_s(buf, MAX_PATH) ? 0 : buf;
}

#define O_WRONLY_ 0x0001
#define O_CREAT_  0x0100
#define O_TRUNC_  0x0200
CRTEXP int _creat(const char *path, int perm) { return _open(path, O_WRONLY_ | O_CREAT_ | O_TRUNC_, perm); }
CRTEXP int _wcreat(const wchar_t *path, int perm) { return _wopen(path, O_WRONLY_ | O_CREAT_ | O_TRUNC_, perm); }
CRTEXP int _wsopen(const wchar_t *path, int flags, int share, ...) { (void)share; return _wopen(path, flags); }

/* -----------------------------------------------------------------------
 * File times: _utime / _futime (seconds since 1970, NULL = now)
 * ----------------------------------------------------------------------- */
struct utimbuf64_ { long long actime, modtime; };
struct utimbuf32_ { long actime, modtime; };

static FILETIME ft_of(long long t)
{
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(t * 10000000LL + 116444736000000000LL);
    FILETIME f = { u.LowPart, u.HighPart };
    return f;
}
static int set_times(HANDLE h, const struct utimbuf64_ *t)
{
    FILETIME a, m;
    if (t) { a = ft_of(t->actime); m = ft_of(t->modtime); }
    else { GetSystemTimeAsFileTime(&a); m = a; }
    if (!SetFileTime(h, 0, &a, &m)) { errno = EINVAL; return -1; }
    return 0;
}
CRTEXP int _futime64(int fd, struct utimbuf64_ *t)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || !h) { errno = EBADF; return -1; }
    return set_times(h, t);
}
CRTEXP int _wutime64(const wchar_t *path, struct utimbuf64_ *t)
{
    if (!path) { errno = EINVAL; return -1; }
    HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES_, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS_, 0);
    if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; return -1; }
    int r = set_times(h, t);
    CloseHandle(h);
    return r;
}
CRTEXP int _utime64(const char *path, struct utimbuf64_ *t)
{
    wchar_t w[MAX_PATH * 2];
    if (!path || !MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH * 2)) { errno = EINVAL; return -1; }
    return _wutime64(w, t);
}
static struct utimbuf64_ *widen(const struct utimbuf32_ *t, struct utimbuf64_ *o)
{
    if (!t) return 0;
    o->actime = t->actime; o->modtime = t->modtime;
    return o;
}
CRTEXP int _utime32(const char *p, struct utimbuf32_ *t) { struct utimbuf64_ o; return _utime64(p, widen(t, &o)); }
CRTEXP int _wutime32(const wchar_t *p, struct utimbuf32_ *t) { struct utimbuf64_ o; return _wutime64(p, widen(t, &o)); }
CRTEXP int _futime32(int fd, struct utimbuf32_ *t) { struct utimbuf64_ o; return _futime64(fd, widen(t, &o)); }

/* -----------------------------------------------------------------------
 * _wspawnvp / _wspawnvpe: the narrow forms in UTF-8
 * ----------------------------------------------------------------------- */
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
CRTEXP intptr_t _wspawnvpe(int m, const wchar_t *p, const wchar_t *const *a, const wchar_t *const *e)
{
    char *pp = u8(p), **aa = u8v(a), **ee = u8v(e);
    intptr_t r = _spawnvpe(m, pp, (const char *const *)aa, (const char *const *)ee);
    free(pp); free_v(aa); free_v(ee);
    return r;
}
CRTEXP intptr_t _wspawnvp(int m, const wchar_t *p, const wchar_t *const *a) { return _wspawnvpe(m, p, a, 0); }

/* -----------------------------------------------------------------------
 * <mbstring.h>: single-byte code page, so these are the byte functions
 * ----------------------------------------------------------------------- */
typedef unsigned char uc;
CRTEXP uc *_mbschr(const uc *s, unsigned c) { return (uc *)strchr((const char *)s, (int)c); }
CRTEXP uc *_mbsrchr(const uc *s, unsigned c) { return (uc *)strrchr((const char *)s, (int)c); }
CRTEXP uc *_mbsstr(const uc *s, const uc *t) { return (uc *)strstr((const char *)s, (const char *)t); }
CRTEXP size_t _mbslen(const uc *s) { return strlen((const char *)s); }
CRTEXP size_t _mbsnlen(const uc *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
CRTEXP int _mbscmp(const uc *a, const uc *b) { return strcmp((const char *)a, (const char *)b); }
CRTEXP int _mbsncmp(const uc *a, const uc *b, size_t n) { return strncmp((const char *)a, (const char *)b, n); }
CRTEXP int _mbsicmp(const uc *a, const uc *b)
{
    for (;; a++, b++) {
        int x = tolower(*a), y = tolower(*b);
        if (x != y || !x) return x - y;
    }
}
CRTEXP int _mbsnicmp(const uc *a, const uc *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower(*a), y = tolower(*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}
CRTEXP uc *_mbsinc(const uc *s) { return (uc *)s + 1; }
CRTEXP uc *_mbsdec(const uc *start, const uc *s) { return s > start ? (uc *)s - 1 : 0; }
CRTEXP uc *_mbscpy(uc *d, const uc *s) { return (uc *)strcpy((char *)d, (const char *)s); }
CRTEXP uc *_mbscat(uc *d, const uc *s) { return (uc *)strcat((char *)d, (const char *)s); }
CRTEXP uc *_mbsdup(const uc *s) { return (uc *)_strdup((const char *)s); }
CRTEXP uc *_mbslwr(uc *s) { for (uc *p = s; *p; p++) *p = (uc)tolower(*p); return s; }
CRTEXP uc *_mbsupr(uc *s) { for (uc *p = s; *p; p++) *p = (uc)toupper(*p); return s; }
CRTEXP size_t _mbsspn(const uc *s, const uc *set) { return strspn((const char *)s, (const char *)set); }
CRTEXP size_t _mbscspn(const uc *s, const uc *set) { return strcspn((const char *)s, (const char *)set); }
CRTEXP uc *_mbspbrk(const uc *s, const uc *set) { return (uc *)strpbrk((const char *)s, (const char *)set); }
CRTEXP uc *_mbstok(uc *s, const uc *set) { return (uc *)strtok((char *)s, (const char *)set); }
CRTEXP int _ismbblead(unsigned c) { (void)c; return 0; }
CRTEXP int _ismbbtrail(unsigned c) { (void)c; return 0; }
CRTEXP int _ismbslead(const uc *s, const uc *p) { (void)s; (void)p; return 0; }
CRTEXP int _ismbstrail(const uc *s, const uc *p) { (void)s; (void)p; return 0; }

/* -----------------------------------------------------------------------
 * MinGW programs: its CRT start-up and a few older msvcrt names (VLC)
 * ----------------------------------------------------------------------- */
CRTEXP int _setmaxstdio(int n) { return n >= 20 && n <= 8192 ? n : -1; }
CRTEXP int _getmaxstdio(void) { return 512; }
int __stdio_common_vsnprintf_s(unsigned long long opt, char *buf, size_t count, size_t max, const char *fmt, void *loc, va_list ap);
CRTEXP int _vsnprintf_s(char *buf, size_t size, size_t count, const char *fmt, va_list ap)
{
    return __stdio_common_vsnprintf_s(0, buf, size, count, fmt, 0, ap);
}
CRTEXP wchar_t *_wtempnam(const wchar_t *dir, const wchar_t *prefix)
{
    wchar_t d[MAX_PATH];
    if (!dir || !*dir || GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
        if (!GetTempPathW(MAX_PATH, d)) return 0;
        dir = d;
    }
    for (unsigned i = 0; i < 10000; i++) {
        size_t n = wcslen(dir) + (prefix ? wcslen(prefix) : 0) + 16;
        wchar_t *p = malloc(n * sizeof(wchar_t));
        if (!p) return 0;
        swprintf(p, n, L"%ls%ls%ls%u", dir, dir[wcslen(dir) - 1] == L'\\' ? L"" : L"\\", prefix ? prefix : L"",
                 GetTickCount() % 100000 + i);
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return p;
        free(p);
    }
    return 0;
}
CRTEXP char *_tempnam(const char *dir, const char *prefix)
{
    wchar_t wd[MAX_PATH], wp[64];
    if (dir) MultiByteToWideChar(CP_UTF8, 0, dir, -1, wd, MAX_PATH);
    if (prefix) MultiByteToWideChar(CP_UTF8, 0, prefix, -1, wp, 64);
    wchar_t *w = _wtempnam(dir ? wd : 0, prefix ? wp : 0);
    if (!w) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
    char *p = malloc((size_t)n);
    if (p) WideCharToMultiByte(CP_UTF8, 0, w, -1, p, n, 0, 0);
    free(w);
    return p;
}
