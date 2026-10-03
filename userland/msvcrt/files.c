/* msvcrt / ucrtbase: file status in Microsoft's layouts, OS handles for
 * descriptors, full paths, and the 32/64-bit time function names
 *
 * Microsoft's CRT has several stat structures (_stat32, _stat64i32,
 * _stat64, ...) that differ in the widths of st_size and the times; each
 * entry point fills its own layout.
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

/* What every layout is filled from */
typedef struct {
    unsigned mode;
    long long size;
    long long atime, mtime, ctime;
    unsigned dev;
} StatInfo;

#define S_IFDIR 0x4000
#define S_IFCHR 0x2000
#define S_IFIFO 0x1000
#define S_IFREG 0x8000

static long long ft_unix(const FILETIME *ft)
{
    unsigned long long t = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    return t ? (long long)(t / 10000000ULL) - 11644473600LL : 0;
}

static int info_from_find(const WIN32_FIND_DATAW *fd, const wchar_t *path, StatInfo *si)
{
    memset(si, 0, sizeof(*si));
    int dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    unsigned perm = (fd->dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666;
    const wchar_t *dot = wcsrchr(path, L'.');
    if (dir || (dot && (!_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".com") || !_wcsicmp(dot, L".bat") || !_wcsicmp(dot, L".cmd"))))
        perm |= 0111;
    si->mode = (dir ? S_IFDIR : S_IFREG) | perm;
    si->size = dir ? 0 : ((long long)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    si->mtime = ft_unix(&fd->ftLastWriteTime);
    si->atime = fd->ftLastAccessTime.dwLowDateTime || fd->ftLastAccessTime.dwHighDateTime ? ft_unix(&fd->ftLastAccessTime) : si->mtime;
    si->ctime = fd->ftCreationTime.dwLowDateTime || fd->ftCreationTime.dwHighDateTime ? ft_unix(&fd->ftCreationTime) : si->mtime;
    wchar_t drive = path[0] && path[1] == L':' ? (wchar_t)(towupper(path[0]) - 'A') : 2;
    si->dev = drive;
    return 0;
}

static int info_path_w(const wchar_t *path, StatInfo *si)
{
    size_t n = wcslen(path);
    if (!n) { errno = ENOENT; return -1; }
    /* the root of a drive, and paths ending in a separator, name directories */
    wchar_t tmp[MAX_PATH];
    if (n >= MAX_PATH) { errno = ENAMETOOLONG; return -1; }
    wcscpy(tmp, path);
    while (n > 1 && (tmp[n - 1] == L'\\' || tmp[n - 1] == L'/') && !(n == 3 && tmp[1] == L':')) tmp[--n] = 0;
    if ((n == 2 && tmp[1] == L':') || (n == 3 && tmp[1] == L':') || (n == 1 && (tmp[0] == L'\\' || tmp[0] == L'/'))) {
        memset(si, 0, sizeof(*si));
        si->mode = S_IFDIR | 0777;
        return 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(tmp, &fd);
    if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; return -1; }
    FindClose(h);
    return info_from_find(&fd, tmp, si);
}

static int info_path_a(const char *path, StatInfo *si)
{
    wchar_t w[MAX_PATH];
    size_t r = mbstowcs(w, path, MAX_PATH - 1);
    if (r == (size_t)-1) { errno = EINVAL; return -1; }
    w[r] = 0;
    return info_path_w(w, si);
}

static int info_fd(int fd, StatInfo *si)
{
    HANDLE h = __nova_fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    memset(si, 0, sizeof(*si));
    DWORD t = GetFileType(h);
    if (t == FILE_TYPE_CHAR) { si->mode = S_IFCHR | 0666; si->dev = (unsigned)fd; return 0; }
    if (t == FILE_TYPE_PIPE) { si->mode = S_IFIFO | 0666; return 0; }
    LARGE_INTEGER sz;
    si->mode = S_IFREG | 0666;
    if (GetFileSizeEx(h, &sz)) si->size = sz.QuadPart;
    else si->mode = S_IFDIR | 0777;
    si->dev = 2;
    si->mtime = si->atime = si->ctime = time(NULL);
    return 0;
}

/* The layouts */
struct ms_stat64    { unsigned st_dev; unsigned short st_ino, st_mode; short st_nlink, st_uid, st_gid; unsigned st_rdev; long long st_size; long long st_atime_, st_mtime_, st_ctime_; };
struct ms_stat64i32 { unsigned st_dev; unsigned short st_ino, st_mode; short st_nlink, st_uid, st_gid; unsigned st_rdev; long st_size; long long st_atime_, st_mtime_, st_ctime_; };
struct ms_stat32    { unsigned st_dev; unsigned short st_ino, st_mode; short st_nlink, st_uid, st_gid; unsigned st_rdev; long st_size; long st_atime_, st_mtime_, st_ctime_; };
struct ms_stat32i64 { unsigned st_dev; unsigned short st_ino, st_mode; short st_nlink, st_uid, st_gid; unsigned st_rdev; long long st_size; long st_atime_, st_mtime_, st_ctime_; };

#define FILL(st, si) do { memset(st, 0, sizeof(*st)); \
    (st)->st_dev = (st)->st_rdev = (si).dev; (st)->st_mode = (unsigned short)(si).mode; (st)->st_nlink = 1; \
    (st)->st_size = (si).size; (st)->st_atime_ = (si).atime; (st)->st_mtime_ = (si).mtime; (st)->st_ctime_ = (si).ctime; } while (0)

#define STAT_FUNCS(sfx, T) \
    CRTEXP int _stat##sfx(const char *p, struct T *st) { StatInfo si; if (info_path_a(p, &si)) return -1; FILL(st, si); return 0; } \
    CRTEXP int _wstat##sfx(const wchar_t *p, struct T *st) { StatInfo si; if (info_path_w(p, &si)) return -1; FILL(st, si); return 0; } \
    CRTEXP int _fstat##sfx(int fd, struct T *st) { StatInfo si; if (info_fd(fd, &si)) return -1; FILL(st, si); return 0; }
STAT_FUNCS(64, ms_stat64)
STAT_FUNCS(64i32, ms_stat64i32)
STAT_FUNCS(32, ms_stat32)
STAT_FUNCS(32i64, ms_stat32i64)
/* msvcrt's plain names use the 32-bit-time layout */
CRTEXP int _stat(const char *p, struct ms_stat32 *st) { return _stat32(p, st); }
CRTEXP int _wstat(const wchar_t *p, struct ms_stat32 *st) { return _wstat32(p, st); }
CRTEXP int _wstati64(const wchar_t *p, struct ms_stat32i64 *st) { return _wstat32i64(p, st); }
CRTEXP int _fstat(int fd, struct ms_stat32 *st) { return _fstat32(fd, st); }
CRTEXP int _stati64(const char *p, struct ms_stat32i64 *st) { return _stat32i64(p, st); }
CRTEXP int _fstati64(int fd, struct ms_stat32i64 *st) { return _fstat32i64(fd, st); }

CRTEXP long long _filelengthi64(int fd)
{
    StatInfo si;
    return info_fd(fd, &si) ? -1 : si.size;
}
CRTEXP long _filelength(int fd) { return (long)_filelengthi64(fd); }

CRTEXP intptr_t _get_osfhandle(int fd)
{
    HANDLE h = __nova_fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) errno = EBADF;
    return (intptr_t)h;
}
CRTEXP int _open_osfhandle(intptr_t h, int flags) { (void)flags; return __nova_fd_new((void *)h, 1); }
CRTEXP int _setmode(int fd, int mode) { (void)fd; return mode ? 0x8000 : 0x8000; }   /* always binary */
CRTEXP int _commit(int fd) { return FlushFileBuffers(__nova_fd_handle(fd)) ? 0 : -1; }
CRTEXP int _chsize_s(int fd, long long size)
{
    extern int ftruncate(int fd, long long len);
    return ftruncate(fd, size) ? errno : 0;
}
CRTEXP int _chsize(int fd, long size) { return _chsize_s(fd, size) ? -1 : 0; }
CRTEXP int _eof(int fd)
{
    extern long long _lseeki64(int, long long, int);
    long long cur = _lseeki64(fd, 0, SEEK_CUR), len = _filelengthi64(fd);
    return cur < 0 || len < 0 ? -1 : cur >= len;
}
CRTEXP long long _telli64(int fd) { extern long long _lseeki64(int, long long, int); return _lseeki64(fd, 0, SEEK_CUR); }
CRTEXP long _tell(int fd) { return (long)_telli64(fd); }
CRTEXP int _locking(int fd, int mode, long n) { (void)fd; (void)mode; (void)n; return 0; }
CRTEXP int _wopen(const wchar_t *p, int flags, ...)
{
    char buf[MAX_PATH * 3];
    if (wcstombs(buf, p, sizeof(buf)) == (size_t)-1) { errno = EINVAL; return -1; }
    extern int open(const char *, int, ...);
    return open(buf, flags);
}
CRTEXP int _sopen(const char *p, int flags, int share, ...) { (void)share; extern int open(const char *, int, ...); return open(p, flags); }
CRTEXP int _sopen_s(int *fd, const char *p, int flags, int share, int perm) { (void)share; (void)perm; extern int open(const char *, int, ...); *fd = open(p, flags); return *fd < 0 ? errno : 0; }
CRTEXP int _wsopen_s(int *fd, const wchar_t *p, int flags, int share, int perm) { (void)share; (void)perm; *fd = _wopen(p, flags); return *fd < 0 ? errno : 0; }

CRTEXP char *_fullpath(char *buf, const char *path, size_t n)
{
    char tmp[MAX_PATH];
    DWORD r = GetFullPathNameA(path, MAX_PATH, tmp, NULL);
    if (!r || r >= MAX_PATH) { errno = ENOENT; return NULL; }
    if (!buf) return _strdup(tmp);
    if (r >= n) { errno = ERANGE; return NULL; }
    memcpy(buf, tmp, r + 1);
    return buf;
}
CRTEXP wchar_t *_wfullpath(wchar_t *buf, const wchar_t *path, size_t n)
{
    char a[MAX_PATH * 3], full[MAX_PATH];
    if (wcstombs(a, path, sizeof(a)) == (size_t)-1 || !_fullpath(full, a, sizeof(full))) return NULL;
    size_t len = mbstowcs(NULL, full, 0);
    if (!buf) { n = len + 1; buf = malloc(n * sizeof(wchar_t)); if (!buf) return NULL; }
    if (len >= n) { errno = ERANGE; return NULL; }
    mbstowcs(buf, full, n);
    return buf;
}
CRTEXP wchar_t *_wgetcwd(wchar_t *buf, int n)
{
    extern char *getcwd(char *, size_t);
    char tmp[MAX_PATH];
    if (!getcwd(tmp, sizeof(tmp))) return NULL;
    size_t len = mbstowcs(NULL, tmp, 0);
    if (!buf) { n = (int)len + 1; buf = malloc((size_t)n * sizeof(wchar_t)); if (!buf) return NULL; }
    if ((int)len >= n) { errno = ERANGE; return NULL; }
    mbstowcs(buf, tmp, (size_t)n);
    return buf;
}
CRTEXP int _wchdir(const wchar_t *p) { char a[MAX_PATH * 3]; extern int chdir(const char *); return wcstombs(a, p, sizeof(a)) == (size_t)-1 ? -1 : chdir(a); }
CRTEXP int _wmkdir(const wchar_t *p) { char a[MAX_PATH * 3]; extern int mkdir(const char *, int); return wcstombs(a, p, sizeof(a)) == (size_t)-1 ? -1 : mkdir(a, 0777); }
CRTEXP int _wrmdir(const wchar_t *p) { char a[MAX_PATH * 3]; extern int rmdir(const char *); return wcstombs(a, p, sizeof(a)) == (size_t)-1 ? -1 : rmdir(a); }
CRTEXP int _waccess(const wchar_t *p, int m) { char a[MAX_PATH * 3]; extern int access(const char *, int); return wcstombs(a, p, sizeof(a)) == (size_t)-1 ? -1 : access(a, m); }
CRTEXP int _mkdir(const char *p) { extern int mkdir(const char *, int); return mkdir(p, 0777); }
CRTEXP int _rmdir(const char *p) { extern int rmdir(const char *); return rmdir(p); }
CRTEXP int _chdir(const char *p) { extern int chdir(const char *); return chdir(p); }
CRTEXP char *_getcwd(char *b, int n) { extern char *getcwd(char *, size_t); return getcwd(b, (size_t)n); }
CRTEXP int _chmod(const char *p, int m) { extern int chmod(const char *, int); return chmod(p, m); }
CRTEXP int _getpid(void) { return (int)GetCurrentProcessId(); }
CRTEXP int _getdrive(void) { return 3; }                        /* C: */
CRTEXP unsigned long _getdrives(void) { return 1u << 2; }

CRTEXP void _wassert(const wchar_t *expr, const wchar_t *file, unsigned line)
{
    fprintf(stderr, "Assertion failed: %ls, file %ls, line %u\n", expr, file, line);
    abort();
}

/* -----------------------------------------------------------------------
 * Time: _time64 and friends (time_t is already 64-bit), _time32 & co.,
 * and the time zone (NovaOS keeps UTC)
 * ----------------------------------------------------------------------- */
/* the variables themselves are exported too (old msvcrt.dll programs and
 * MinGW-built DLLs such as icu.dll import them as data) */
CRTEXP int   _daylight;
CRTEXP long  _timezone;
CRTEXP char *_tzname[2] = { "UTC", "UTC" };
#define g_daylight _daylight
#define g_timezone _timezone
#define g_tzname   _tzname
CRTEXP int   *__daylight(void) { return &g_daylight; }
CRTEXP long  *__timezone(void) { return &g_timezone; }
CRTEXP char **__tzname(void)   { return g_tzname; }
CRTEXP long  *__dstbias(void)  { static long b; return &b; }
CRTEXP int _get_daylight(int *v) { *v = g_daylight; return 0; }
CRTEXP int _get_timezone(long *v) { *v = g_timezone; return 0; }
CRTEXP int _get_dstbias(long *v) { *v = 0; return 0; }
CRTEXP int _get_tzname(size_t *ret, char *buf, size_t n, int i)
{
    const char *s = g_tzname[i ? 1 : 0];
    if (ret) *ret = strlen(s) + 1;
    if (buf && n) { strncpy(buf, s, n - 1); buf[n - 1] = 0; }
    return 0;
}
CRTEXP void _tzset(void) { }
CRTEXP void tzset(void) { }

CRTEXP time_t _time64(time_t *t) { return time(t); }
CRTEXP long   _time32(long *t) { long v = (long)time(NULL); if (t) *t = v; return v; }
CRTEXP struct tm *_gmtime64(const time_t *t) { return gmtime(t); }
CRTEXP struct tm *_localtime64(const time_t *t) { return localtime(t); }
CRTEXP struct tm *_gmtime32(const long *t) { time_t v = *t; return gmtime(&v); }
CRTEXP struct tm *_localtime32(const long *t) { time_t v = *t; return localtime(&v); }
CRTEXP int _gmtime64_s(struct tm *r, const time_t *t)
{
    if (!r || !t) return EINVAL;
    struct tm *g = gmtime(t);
    if (!g) return EINVAL;
    *r = *g;
    return 0;
}
CRTEXP int _localtime64_s(struct tm *r, const time_t *t) { return _gmtime64_s(r, t); }
CRTEXP int _gmtime32_s(struct tm *r, const long *t) { time_t v = *t; return _gmtime64_s(r, &v); }
CRTEXP int _localtime32_s(struct tm *r, const long *t) { time_t v = *t; return _gmtime64_s(r, &v); }
CRTEXP struct tm *gmtime_r(const time_t *t, struct tm *r) { return _gmtime64_s(r, t) ? NULL : r; }
CRTEXP struct tm *localtime_r(const time_t *t, struct tm *r) { return _gmtime64_s(r, t) ? NULL : r; }
CRTEXP time_t _mktime64(struct tm *tm) { return mktime(tm); }
CRTEXP long   _mktime32(struct tm *tm) { return (long)mktime(tm); }
CRTEXP time_t _mkgmtime64(struct tm *tm) { return mktime(tm); }         /* local time is UTC */
CRTEXP long   _mkgmtime32(struct tm *tm) { return (long)mktime(tm); }
CRTEXP time_t timegm(struct tm *tm) { return mktime(tm); }
CRTEXP double _difftime64(time_t a, time_t b) { return difftime(a, b); }
CRTEXP double _difftime32(long a, long b) { return (double)(a - b); }
CRTEXP char *_ctime64(const time_t *t) { return ctime(t); }
CRTEXP char *_ctime32(const long *t) { time_t v = *t; return ctime(&v); }
CRTEXP int _ctime64_s(char *buf, size_t n, const time_t *t) { const char *s = ctime(t); if (!s || strlen(s) >= n) return EINVAL; strcpy(buf, s); return 0; }
CRTEXP int asctime_s(char *buf, size_t n, const struct tm *tm) { const char *s = asctime(tm); if (!s || strlen(s) >= n) return EINVAL; strcpy(buf, s); return 0; }
CRTEXP size_t _strftime_l(char *s, size_t n, const char *f, const struct tm *t, void *l) { (void)l; return strftime(s, n, f, t); }
CRTEXP int timespec_get(struct timespec *ts, int base)
{
    if (base != 1) return 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL;
    ts->tv_sec = (time_t)(t / 10000000ULL);
    ts->tv_nsec = (long)(t % 10000000ULL) * 100;
    return base;
}
CRTEXP int _timespec64_get(struct timespec *ts, int base) { return timespec_get(ts, base); }
CRTEXP char *_strdate(char *buf)
{
    time_t t = time(NULL);
    strftime(buf, 9, "%m/%d/%y", gmtime(&t));
    return buf;
}
CRTEXP char *_strtime(char *buf)
{
    time_t t = time(NULL);
    strftime(buf, 9, "%H:%M:%S", gmtime(&t));
    return buf;
}
struct __timeb64 { long long time; unsigned short millitm; short timezone, dstflag; };
CRTEXP void _ftime64(struct __timeb64 *tb)
{
    struct timespec ts;
    timespec_get(&ts, 1);
    tb->time = ts.tv_sec;
    tb->millitm = (unsigned short)(ts.tv_nsec / 1000000);
    tb->timezone = 0;
    tb->dstflag = 0;
}
CRTEXP void _ftime(struct __timeb64 *tb) { _ftime64(tb); }
CRTEXP int _ftime64_s(struct __timeb64 *tb) { _ftime64(tb); return 0; }

/* -----------------------------------------------------------------------
 * _findfirst / _findnext / _findclose in every layout: the attributes,
 * three times (32- or 64-bit), the size (32- or 64-bit) and the name
 * (char or wchar_t [260]).  The handle is the FindFirstFile one.
 * ----------------------------------------------------------------------- */
typedef struct { int t64, s64; } FindLayout;       /* (MSVC alignment: 8-byte fields on 8) */

static void find_fill(const WIN32_FIND_DATAW *fd, void *out, FindLayout l, int wide)
{
    unsigned char *b = out;
    unsigned long long size = ((unsigned long long)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    long long mt = ft_unix(&fd->ftLastWriteTime);
    long long at = fd->ftLastAccessTime.dwLowDateTime || fd->ftLastAccessTime.dwHighDateTime ? ft_unix(&fd->ftLastAccessTime) : mt;
    long long ct = fd->ftCreationTime.dwLowDateTime || fd->ftCreationTime.dwHighDateTime ? ft_unix(&fd->ftCreationTime) : mt;
    *(unsigned *)b = fd->dwFileAttributes & 0x37;   /* _A_RDONLY, _HIDDEN, _SYSTEM, _SUBDIR, _ARCH */
    size_t off;
    if (l.t64) {
        *(long long *)(b + 8) = ct; *(long long *)(b + 16) = at; *(long long *)(b + 24) = mt;
        off = 32;
    } else {
        *(int *)(b + 4) = (int)ct; *(int *)(b + 8) = (int)at; *(int *)(b + 12) = (int)mt;
        off = 16;
    }
    if (l.s64) { *(unsigned long long *)(b + off) = size; off += 8; }
    else { *(unsigned *)(b + off) = (unsigned)size; off += 4; }
    if (wide) memcpy(b + off, fd->cFileName, sizeof(fd->cFileName));
    else WideCharToMultiByte(CP_ACP, 0, fd->cFileName, -1, (char *)(b + off), MAX_PATH, 0, 0);
}

static void find_errno(void)
{
    DWORD e = GetLastError();
    errno = e == ERROR_NO_MORE_FILES || e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : EINVAL;
}

static intptr_t find_first(const wchar_t *spec, void *out, FindLayout l, int wide)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) { find_errno(); return -1; }
    find_fill(&fd, out, l, wide);
    return (intptr_t)h;
}

static intptr_t find_first_a(const char *spec, void *out, FindLayout l)
{
    wchar_t w[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, spec, -1, w, MAX_PATH)) { errno = EINVAL; return -1; }
    return find_first(w, out, l, 0);
}

static int find_next(intptr_t h, void *out, FindLayout l, int wide)
{
    WIN32_FIND_DATAW fd;
    if (h == -1 || !FindNextFileW((HANDLE)h, &fd)) { find_errno(); return -1; }
    find_fill(&fd, out, l, wide);
    return 0;
}

static const FindLayout L32 = { 0, 0 }, L32I64 = { 0, 1 }, L64I32 = { 1, 0 }, L64 = { 1, 1 };
/* msvcrt's own names: time_t is 64-bit on x64, 32-bit on x86 */
#ifdef _WIN64
#define LT   L64I32
#define LTI  L64
#else
#define LT   L32
#define LTI  L32I64
#endif

CRTEXP intptr_t _findfirst(const char *s, void *d)         { return find_first_a(s, d, LT); }
CRTEXP intptr_t _findfirst32(const char *s, void *d)       { return find_first_a(s, d, L32); }
CRTEXP intptr_t _findfirsti64(const char *s, void *d)      { return find_first_a(s, d, LTI); }
CRTEXP intptr_t _findfirst32i64(const char *s, void *d)    { return find_first_a(s, d, L32I64); }
CRTEXP intptr_t _findfirst64i32(const char *s, void *d)    { return find_first_a(s, d, L64I32); }
CRTEXP intptr_t _findfirst64(const char *s, void *d)       { return find_first_a(s, d, L64); }
CRTEXP intptr_t _wfindfirst(const wchar_t *s, void *d)     { return find_first(s, d, LT, 1); }
CRTEXP intptr_t _wfindfirst32(const wchar_t *s, void *d)   { return find_first(s, d, L32, 1); }
CRTEXP intptr_t _wfindfirsti64(const wchar_t *s, void *d)  { return find_first(s, d, LTI, 1); }
CRTEXP intptr_t _wfindfirst32i64(const wchar_t *s, void *d) { return find_first(s, d, L32I64, 1); }
CRTEXP intptr_t _wfindfirst64i32(const wchar_t *s, void *d) { return find_first(s, d, L64I32, 1); }
CRTEXP intptr_t _wfindfirst64(const wchar_t *s, void *d)   { return find_first(s, d, L64, 1); }
CRTEXP int _findnext(intptr_t h, void *d)                  { return find_next(h, d, LT, 0); }
CRTEXP int _findnext32(intptr_t h, void *d)                { return find_next(h, d, L32, 0); }
CRTEXP int _findnexti64(intptr_t h, void *d)               { return find_next(h, d, LTI, 0); }
CRTEXP int _findnext32i64(intptr_t h, void *d)             { return find_next(h, d, L32I64, 0); }
CRTEXP int _findnext64i32(intptr_t h, void *d)             { return find_next(h, d, L64I32, 0); }
CRTEXP int _findnext64(intptr_t h, void *d)                { return find_next(h, d, L64, 0); }
CRTEXP int _wfindnext(intptr_t h, void *d)                 { return find_next(h, d, LT, 1); }
CRTEXP int _wfindnext32(intptr_t h, void *d)               { return find_next(h, d, L32, 1); }
CRTEXP int _wfindnexti64(intptr_t h, void *d)              { return find_next(h, d, LTI, 1); }
CRTEXP int _wfindnext32i64(intptr_t h, void *d)            { return find_next(h, d, L32I64, 1); }
CRTEXP int _wfindnext64i32(intptr_t h, void *d)            { return find_next(h, d, L64I32, 1); }
CRTEXP int _wfindnext64(intptr_t h, void *d)               { return find_next(h, d, L64, 1); }
CRTEXP int _findclose(intptr_t h)
{
    if (h == -1 || !FindClose((HANDLE)h)) { errno = ENOENT; return -1; }
    return 0;
}
