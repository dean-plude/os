/* msvcrt: the POSIX subset (file descriptors, stat, directories, getopt,
 * gettimeofday) over the Win32 API.  Descriptors 0-2 are the standard
 * handles; the rest index a table of shared open-file objects so that
 * dup() and fdopen() behave. */
#define NOVA_BUILD_MSVCRT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <windows.h>
#include "msvcrt_internal.h"
#include <wchar.h>
WINBASEAPI DWORD WINAPI SearchPathA(LPCSTR path, LPCSTR name, LPCSTR ext, DWORD n, LPSTR buf, LPSTR *file);

/* -----------------------------------------------------------------------
 * Descriptor table
 * ----------------------------------------------------------------------- */
#define MAX_FD 64

typedef struct {
    HANDLE h;
    int    refs;
    int    owns;        /* CloseHandle when the last descriptor goes */
    int    append;
} OpenFile;

static OpenFile *g_fd[MAX_FD];
static OpenFile g_std_fd[3];                   /* 0-2 before anything replaces them */
static char g_std_closed[3];                   /* 0-2 closed: free for the next descriptor */

static OpenFile *fd_get(int fd)
{
    if (fd < 0 || fd >= MAX_FD) { errno = EBADF; return 0; }
    if (!g_fd[fd] && fd < 3 && !g_std_closed[fd]) {
        OpenFile *std = g_std_fd;
        std[fd].h = GetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        std[fd].refs = 1;
        g_fd[fd] = &std[fd];
    }
    if (!g_fd[fd]) errno = EBADF;
    return g_fd[fd];
}

/* Descriptors 0-2 carry the standard handles with them (as Windows' CRT
 * does for a console program): the console streams follow too. */
static void std_follow(int fd, HANDLE h)
{
    extern void __nova_std_changed(int fd, void *h);
    g_std_closed[fd] = 0;
    SetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, h);
    __nova_std_changed(fd, h);
}

/* The lowest free descriptor from @lowest gets @o (0-2 only once closed) */
static int fd_install(OpenFile *o, int lowest)
{
    for (int i = lowest; i < MAX_FD; i++) {
        if (!g_fd[i] && !(i < 3 && fd_get(i))) {
            g_fd[i] = o;
            o->refs++;
            if (i < 3) std_follow(i, o->h);
            return i;
        }
    }
    errno = EMFILE;
    return -1;
}

int __nova_fd_new(void *handle, int owns)
{
    OpenFile *o = calloc(1, sizeof(*o));
    if (!o) { errno = ENOMEM; return -1; }
    o->h = handle;
    o->owns = owns;
    int fd = fd_install(o, 0);
    if (fd < 0) free(o);
    return fd;
}

void *__nova_fd_handle(int fd)
{
    OpenFile *o = fd_get(fd);
    return o ? o->h : INVALID_HANDLE_VALUE;
}

void __nova_set_errno_win32(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ACCESS_DENIED:  errno = EACCES; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_TOO_MANY_OPEN_FILES: errno = EMFILE; break;
    case ERROR_NOT_ENOUGH_MEMORY: errno = ENOMEM; break;
    case ERROR_DISK_FULL:      errno = ENOSPC; break;
    case ERROR_DIR_NOT_EMPTY:  errno = ENOTEMPTY; break;
    case ERROR_INVALID_HANDLE: errno = EBADF; break;
    case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: errno = EPIPE; break;
    default:                   errno = EINVAL; break;
    }
}

int open(const char *path, int flags, ...)
{
    DWORD access, disp;
    switch (flags & 3) {
    case O_WRONLY: access = GENERIC_WRITE; break;
    case O_RDWR:   access = GENERIC_READ | GENERIC_WRITE; break;
    default:       access = GENERIC_READ; break;
    }
    if (flags & O_CREAT)
        disp = (flags & O_EXCL) ? CREATE_NEW : (flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else
        disp = (flags & O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, disp, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { __nova_set_errno_win32(); return -1; }
    int fd = __nova_fd_new(h, 1);
    if (fd < 0) { CloseHandle(h); return -1; }
    g_fd[fd]->append = (flags & O_APPEND) != 0;
    return fd;
}
int _open(const char *path, int flags, ...) { return open(path, flags); }

int close(int fd)
{
    OpenFile *o = fd_get(fd);
    if (!o) return -1;
    g_fd[fd] = 0;
    if (fd < 3) g_std_closed[fd] = 1;
    if (--o->refs == 0) {                       /* the last descriptor: the handle goes */
        if (o->owns) CloseHandle(o->h);
        if (o < g_std_fd || o >= g_std_fd + 3) free(o);
    }
    return 0;
}
int _close(int fd) { return close(fd); }

int dup(int fd)
{
    OpenFile *o = fd_get(fd);
    return o ? fd_install(o, 0) : -1;
}
int _dup(int fd) { return dup(fd); }

int dup2(int fd, int fd2)
{
    OpenFile *o = fd_get(fd);
    if (!o) return -1;
    if (fd2 < 0 || fd2 >= MAX_FD) { errno = EBADF; return -1; }
    if (fd == fd2) return fd2;
    if (g_fd[fd2] || fd2 < 3) { if (fd_get(fd2)) close(fd2); }
    g_fd[fd2] = o;
    o->refs++;
    if (fd2 < 3) std_follow(fd2, o->h);
    return fd2;
}
int _dup2(int fd, int fd2) { return dup2(fd, fd2); }

ssize_t read(int fd, void *buf, size_t n)
{
    OpenFile *o = fd_get(fd);
    if (!o) return -1;
    DWORD got = 0;
    if (!ReadFile(o->h, buf, (DWORD)(n > 0x40000000 ? 0x40000000 : n), &got, 0)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) return 0;     /* the writer is gone: end of file */
        __nova_set_errno_win32();
        return -1;
    }
    return got;
}
int _read(int fd, void *buf, unsigned n) { return (int)read(fd, buf, n); }

ssize_t write(int fd, const void *buf, size_t n)
{
    OpenFile *o = fd_get(fd);
    if (!o) return -1;
    if (o->append) {
        LARGE_INTEGER z; z.QuadPart = 0;
        SetFilePointerEx(o->h, z, 0, FILE_END);
    }
    DWORD put = 0;
    if (!WriteFile(o->h, buf, (DWORD)(n > 0x40000000 ? 0x40000000 : n), &put, 0)) { __nova_set_errno_win32(); return -1; }
    return put;
}
int _write(int fd, const void *buf, unsigned n) { return (int)write(fd, buf, n); }

off_t lseek(int fd, off_t off, int whence)
{
    OpenFile *o = fd_get(fd);
    if (!o) return -1;
    LARGE_INTEGER d, r;
    d.QuadPart = off;
    DWORD m = whence == SEEK_SET ? FILE_BEGIN : whence == SEEK_CUR ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx(o->h, d, &r, m)) { __nova_set_errno_win32(); return -1; }
    return r.QuadPart;
}
long      _lseek(int fd, long off, int whence)          { return (long)lseek(fd, off, whence); }
long long _lseeki64(int fd, long long off, int whence)  { return lseek(fd, off, whence); }

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
    ssize_t r = read(fd, buf, n);
    lseek(fd, cur, SEEK_SET);
    return r;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    OpenFile *o = fd_get(fd);
    if (!o) return -1;
    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
    int app = o->append;
    o->append = 0;
    ssize_t r = write(fd, buf, n);
    o->append = app;
    lseek(fd, cur, SEEK_SET);
    return r;
}

int ftruncate(int fd, off_t len)
{
    OpenFile *o = fd_get(fd);
    if (!o) return -1;
    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (lseek(fd, len, SEEK_SET) < 0) return -1;
    BOOL ok = SetEndOfFile(o->h);
    lseek(fd, cur < len ? cur : len, SEEK_SET);
    if (!ok) { __nova_set_errno_win32(); return -1; }
    return 0;
}

int fsync(int fd)
{
    OpenFile *o = fd_get(fd);
    if (!o) return -1;
    FlushFileBuffers(o->h);
    return 0;
}

int isatty(int fd)
{
    OpenFile *o = fd_get(fd);
    return o && GetFileType(o->h) == FILE_TYPE_CHAR;
}
int _isatty(int fd) { return isatty(fd); }

/* -----------------------------------------------------------------------
 * Paths
 * ----------------------------------------------------------------------- */
int access(const char *path, int mode)
{
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) { __nova_set_errno_win32(); return -1; }
    if ((mode & W_OK) && (a & FILE_ATTRIBUTE_READONLY)) { errno = EACCES; return -1; }
    return 0;
}
int _access(const char *path, int mode) { return access(path, mode); }

int unlink(const char *path)
{
    if (!DeleteFileA(path)) { __nova_set_errno_win32(); return -1; }
    return 0;
}
int _unlink(const char *path) { return unlink(path); }

int rmdir(const char *path)
{
    if (!RemoveDirectoryA(path)) { __nova_set_errno_win32(); return -1; }
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode;
    if (!CreateDirectoryA(path, 0)) { __nova_set_errno_win32(); return -1; }
    return 0;
}

int chmod(const char *path, mode_t mode)
{
    (void)mode;
    return access(path, F_OK);
}

mode_t umask(mode_t m) { (void)m; return 022; }

char *getcwd(char *buf, size_t size)
{
    char tmp[MAX_PATH];
    DWORD n = GetCurrentDirectoryA(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) { errno = ERANGE; return 0; }
    if (!buf) {
        size = size > n ? size : n + 1;
        buf = malloc(size);
        if (!buf) { errno = ENOMEM; return 0; }
    }
    if (size <= n) { errno = ERANGE; return 0; }
    memcpy(buf, tmp, n + 1);
    return buf;
}

int chdir(const char *path)
{
    if (!SetCurrentDirectoryA(path)) { __nova_set_errno_win32(); return -1; }
    return 0;
}

/* FILETIME (100 ns since 1601) to Unix seconds */
static time_t ft_to_unix(const FILETIME *ft)
{
    unsigned long long t = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (!t) return 0;
    return (time_t)(t / 10000000ULL - 11644473600ULL);
}

static int fill_stat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) { __nova_set_errno_win32(); return -1; }
    st->st_nlink = 1;
    st->st_blksize = 4096;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        st->st_mode = S_IFDIR | 0755;
    } else {
        st->st_mode = S_IFREG | ((a & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, 0);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(h, &sz)) st->st_size = sz.QuadPart;
            CloseHandle(h);
        }
    }
    st->st_blocks = (st->st_size + 511) / 512;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    st->st_atime = st->st_mtime = st->st_ctime = ft_to_unix(&now);
    return 0;
}

int stat(const char *path, struct stat *st)  { return fill_stat(path, st); }
int lstat(const char *path, struct stat *st) { return fill_stat(path, st); }

int fstat(int fd, struct stat *st)
{
    OpenFile *o = fd_get(fd);
    if (!o) return -1;
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_blksize = 4096;
    if (GetFileType(o->h) == FILE_TYPE_CHAR) {
        st->st_mode = S_IFCHR | 0666;
        return 0;
    }
    st->st_mode = S_IFREG | 0644;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(o->h, &sz)) st->st_size = sz.QuadPart;
    st->st_blocks = (st->st_size + 511) / 512;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    st->st_atime = st->st_mtime = st->st_ctime = ft_to_unix(&now);
    return 0;
}

/* -----------------------------------------------------------------------
 * Directory streams
 * ----------------------------------------------------------------------- */
struct __nova_dir {
    HANDLE           find;
    int              first;      /* fd holds an entry not yet returned */
    int              done;
    WIN32_FIND_DATAA fd;
    struct dirent    ent;
    char             pattern[MAX_PATH];
};

static int dir_start(DIR *d)
{
    d->find = FindFirstFileA(d->pattern, &d->fd);
    d->done = d->find == INVALID_HANDLE_VALUE;
    d->first = !d->done;
    return !d->done || GetLastError() == ERROR_FILE_NOT_FOUND;
}

DIR *opendir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) { __nova_set_errno_win32(); return 0; }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { errno = ENOTDIR; return 0; }
    DIR *d = calloc(1, sizeof(*d));
    if (!d) { errno = ENOMEM; return 0; }
    size_t n = strlen(path);
    if (n + 3 > sizeof(d->pattern)) { free(d); errno = ENAMETOOLONG; return 0; }
    memcpy(d->pattern, path, n);
    if (n && path[n - 1] != '\\' && path[n - 1] != '/') d->pattern[n++] = '\\';
    d->pattern[n++] = '*';
    d->pattern[n] = 0;
    if (!dir_start(d)) { __nova_set_errno_win32(); free(d); return 0; }
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (d->done) return 0;
    if (!d->first && !FindNextFileA(d->find, &d->fd)) { d->done = 1; return 0; }
    d->first = 0;
    memset(&d->ent, 0, sizeof(d->ent));
    strncpy(d->ent.d_name, d->fd.cFileName, sizeof(d->ent.d_name) - 1);
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    d->ent.d_ino = 1;
    return &d->ent;
}

int closedir(DIR *d)
{
    if (!d) { errno = EBADF; return -1; }
    if (d->find != INVALID_HANDLE_VALUE) FindClose(d->find);
    free(d);
    return 0;
}

void rewinddir(DIR *d)
{
    if (d->find != INVALID_HANDLE_VALUE) FindClose(d->find);
    dir_start(d);
}

int alphasort(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char *dir, struct dirent ***list, int (*filter)(const struct dirent *),
            int (*cmp)(const struct dirent **, const struct dirent **))
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent **v = 0, *e;
    int n = 0, cap = 0;
    while ((e = readdir(d))) {
        if (filter && !filter(e)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            struct dirent **nv = realloc(v, cap * sizeof(*v));
            if (!nv) goto oom;
            v = nv;
        }
        if (!(v[n] = malloc(sizeof(*e)))) goto oom;
        *v[n++] = *e;
    }
    closedir(d);
    if (cmp && n > 1) qsort(v, n, sizeof(*v), (int (*)(const void *, const void *))cmp);
    *list = v;
    return n;
oom:
    while (n) free(v[--n]);
    free(v);
    closedir(d);
    errno = ENOMEM;
    return -1;
}

/* -----------------------------------------------------------------------
 * Process and time
 * ----------------------------------------------------------------------- */
unsigned sleep(unsigned sec) { Sleep(sec * 1000); return 0; }
int usleep(useconds_t usec)  { Sleep((usec + 999) / 1000); return 0; }
pid_t getpid(void)           { return (pid_t)GetCurrentProcessId(); }
uid_t getuid(void)           { return 0; }
uid_t geteuid(void)          { return 0; }

long sysconf(int name)
{
    if (name == _SC_PAGESIZE) return 4096;
    errno = EINVAL;
    return -1;
}

int gethostname(char *name, size_t len)
{
    const char *h = "novaos";
    if (len < strlen(h) + 1) { errno = ENAMETOOLONG; return -1; }
    strcpy(name, h);
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = (((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10;
    t -= 11644473600000000ULL;
    tv->tv_sec = (long)(t / 1000000);
    tv->tv_usec = (long)(t % 1000000);
    return 0;
}

/* -----------------------------------------------------------------------
 * getopt / getopt_long
 * ----------------------------------------------------------------------- */
char *optarg;
int   optind = 1, opterr = 1, optopt;
static int g_optpos;     /* position inside a group of short options */

static int short_opt(int argc, char *const argv[], const char *spec)
{
    char *arg = argv[optind];
    if (!g_optpos) g_optpos = 1;
    int c = (unsigned char)arg[g_optpos++];
    const char *s = c == ':' ? 0 : strchr(spec, c);
    optopt = c;
    if (!s) {
        if (!arg[g_optpos]) { optind++; g_optpos = 0; }
        if (opterr && *spec != ':') fprintf(stderr, "%s: unknown option -%c\n", argv[0], c);
        return '?';
    }
    optarg = 0;
    if (s[1] == ':') {
        if (arg[g_optpos]) {
            optarg = arg + g_optpos;
        } else if (s[2] != ':') {
            if (optind + 1 >= argc) {
                optind++; g_optpos = 0;
                if (opterr && *spec != ':') fprintf(stderr, "%s: option -%c needs an argument\n", argv[0], c);
                return *spec == ':' ? ':' : '?';
            }
            optarg = argv[++optind];
        }
        optind++; g_optpos = 0;
        return c;
    }
    if (!arg[g_optpos]) { optind++; g_optpos = 0; }
    return c;
}

int getopt_long(int argc, char *const argv[], const char *spec, const struct option *longopts, int *longindex)
{
    optarg = 0;
    if (optind >= argc || !argv[optind]) return -1;
    char *arg = argv[optind];
    if (!g_optpos) {
        if (arg[0] != '-' || !arg[1]) return -1;
        if (!strcmp(arg, "--")) { optind++; return -1; }
        if (arg[1] == '-' && longopts) {
            const char *name = arg + 2, *eq = strchr(name, '=');
            size_t len = eq ? (size_t)(eq - name) : strlen(name);
            for (int i = 0; longopts[i].name; i++) {
                const struct option *o = &longopts[i];
                if (strlen(o->name) != len || strncmp(o->name, name, len)) continue;
                optind++;
                if (longindex) *longindex = i;
                if (o->has_arg == required_argument) {
                    if (eq) optarg = (char *)eq + 1;
                    else if (optind < argc) optarg = argv[optind++];
                    else {
                        if (opterr) fprintf(stderr, "%s: option --%s needs an argument\n", argv[0], o->name);
                        return '?';
                    }
                } else if (o->has_arg == optional_argument && eq) {
                    optarg = (char *)eq + 1;
                }
                if (o->flag) { *o->flag = o->val; return 0; }
                return o->val;
            }
            optind++;
            if (opterr) fprintf(stderr, "%s: unknown option %s\n", argv[0], arg);
            return '?';
        }
    }
    return short_opt(argc, argv, spec);
}

int getopt(int argc, char *const argv[], const char *spec)
{
    return getopt_long(argc, argv, spec, 0, 0);
}

/* -----------------------------------------------------------------------
 * Pipes and child processes: _pipe, _popen, _pclose
 * ----------------------------------------------------------------------- */
#define O_NOINHERIT_ 0x0080

__declspec(dllexport) int _pipe(int *fds, unsigned size, int mode)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, !(mode & O_NOINHERIT_) };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, size)) { __nova_set_errno_win32(); return -1; }
    int a = __nova_fd_new(r, 1), b = a < 0 ? -1 : __nova_fd_new(w, 1);
    if (a < 0 || b < 0) {
        if (a >= 0) close(a); else CloseHandle(r);
        CloseHandle(w);
        return -1;
    }
    fds[0] = a;
    fds[1] = b;
    return 0;
}

/* The command interpreter: %ComSpec%, else cmd.exe */
static void comspec(char *out, int cap)
{
    DWORD n = GetEnvironmentVariableA("ComSpec", out, (DWORD)cap);
    if (!n || n >= (DWORD)cap) strcpy(out, "C:\\Windows\\System32\\cmd.exe");
}

/* Run "cmd.exe /c @cmd" with the given standard handles; the process
 * handle, or 0 */
HANDLE __nova_shell(const char *cmd, HANDLE in, HANDLE out, HANDLE err)
{
    char shell[MAX_PATH];
    comspec(shell, sizeof(shell));
    size_t n = strlen(shell) + strlen(cmd) + 16;
    char *line = malloc(n);
    if (!line) { errno = ENOMEM; return 0; }
    snprintf(line, n, "\"%s\" /c %s", shell, cmd);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(shell, line, 0, 0, TRUE, 0, 0, 0, &si, &pi);
    free(line);
    if (!ok) { __nova_set_errno_win32(); if (errno == EINVAL) errno = ENOENT; return 0; }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static struct { FILE *f; HANDLE proc; } g_popen[16];

__declspec(dllexport) FILE *_popen(const char *cmd, const char *mode)
{
    int rd = mode[0] == 'r';
    if (!rd && mode[0] != 'w') { errno = EINVAL; return 0; }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, TRUE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) { __nova_set_errno_win32(); return 0; }
    HANDLE mine = rd ? r : w, theirs = rd ? w : r;
    SetHandleInformation(mine, HANDLE_FLAG_INHERIT, 0);
    fflush(0);
    HANDLE proc = __nova_shell(cmd, rd ? GetStdHandle(STD_INPUT_HANDLE) : theirs,
                               rd ? theirs : GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE));
    CloseHandle(theirs);
    if (!proc) { CloseHandle(mine); return 0; }
    int fd = __nova_fd_new(mine, 1);
    FILE *f = fd < 0 ? 0 : _fdopen(fd, rd ? "r" : "w");
    if (!f) { if (fd >= 0) close(fd); else CloseHandle(mine); CloseHandle(proc); return 0; }
    for (int i = 0; i < 16; i++) if (!g_popen[i].f) { g_popen[i].f = f; g_popen[i].proc = proc; return f; }
    fclose(f);
    CloseHandle(proc);
    errno = EMFILE;
    return 0;
}

__declspec(dllexport) FILE *_wpopen(const wchar_t *cmd, const wchar_t *mode)
{
    char c[4096], m[8];
    if (wcstombs(c, cmd, sizeof(c)) == (size_t)-1 || wcstombs(m, mode, sizeof(m)) == (size_t)-1) { errno = EINVAL; return 0; }
    return _popen(c, m);
}

__declspec(dllexport) int _pclose(FILE *f)
{
    for (int i = 0; i < 16; i++) {
        if (g_popen[i].f != f) continue;
        HANDLE proc = g_popen[i].proc;
        g_popen[i].f = 0;
        fclose(f);
        DWORD code = (DWORD)-1;
        WaitForSingleObject(proc, INFINITE);
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        return (int)code;
    }
    errno = EINVAL;
    return -1;
}


/* -----------------------------------------------------------------------
 * _spawn* / _exec*: a new process (CreateProcess); _P_OVERLAY and the
 * _exec functions wait for it and exit with its code, as Windows' CRT does
 * ----------------------------------------------------------------------- */
#define P_WAIT_    0
#define P_NOWAIT_  1
#define P_OVERLAY_ 2
#define P_NOWAITO_ 3
#define P_DETACH_  4

/* One argument, quoted the way CommandLineToArgv reads it back */
static void quote_arg(char *out, size_t *o, size_t cap, const char *a)
{
    int need = !*a || strpbrk(a, " \t\"") != 0;
    if (need && *o < cap) out[(*o)++] = '"';
    for (const char *p = a; *p; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) { for (size_t i = 0; i < (need ? bs * 2 : bs) && *o < cap; i++) out[(*o)++] = '\\'; break; }
        if (*p == '"') { for (size_t i = 0; i < bs * 2 + 1 && *o < cap; i++) out[(*o)++] = '\\'; }
        else for (size_t i = 0; i < bs && *o < cap; i++) out[(*o)++] = '\\';
        if (*o < cap) out[(*o)++] = *p;
    }
    if (need && *o < cap) out[(*o)++] = '"';
}

static intptr_t spawn_common(int mode, const char *path, const char *const *argv, const char *const *envp, int search)
{
    char image[MAX_PATH];
    if (search && !strpbrk(path, "\\/:")) {
        char *file;
        if (!SearchPathA(0, path, ".exe", MAX_PATH, image, &file)) { errno = ENOENT; return -1; }
    } else {
        snprintf(image, sizeof(image), "%s", path);
        if (GetFileAttributesA(image) == INVALID_FILE_ATTRIBUTES && !strchr(strrchr(image, '\\') ? strrchr(image, '\\') : image, '.'))
            strncat(image, ".exe", sizeof(image) - strlen(image) - 1);
    }
    size_t cap = 32768, o = 0;
    char *cl = malloc(cap);
    if (!cl) { errno = ENOMEM; return -1; }
    for (int i = 0; argv && argv[i]; i++) {
        if (i && o < cap) cl[o++] = ' ';
        quote_arg(cl, &o, cap - 1, argv[i]);
    }
    cl[o] = 0;
    char *env = 0;
    if (envp) {                                        /* NAME=value\0...\0 */
        size_t n = 1;
        for (int i = 0; envp[i]; i++) n += strlen(envp[i]) + 1;
        env = malloc(n);
        if (env) {
            size_t k = 0;
            for (int i = 0; envp[i]; i++) { size_t l = strlen(envp[i]) + 1; memcpy(env + k, envp[i], l); k += l; }
            env[k] = 0;
        }
    }
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    fflush(0);
    BOOL ok = CreateProcessA(image, cl, 0, 0, TRUE, mode == P_DETACH_ ? DETACHED_PROCESS : 0, env, 0, &si, &pi);
    free(cl);
    free(env);
    if (!ok) { __nova_set_errno_win32(); if (errno == EINVAL) errno = ENOENT; return -1; }
    CloseHandle(pi.hThread);
    if (mode == P_NOWAIT_ || mode == P_NOWAITO_) return (intptr_t)pi.hProcess;
    if (mode == P_DETACH_) { CloseHandle(pi.hProcess); return 0; }
    DWORD code = 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (mode == P_OVERLAY_) ExitProcess(code);      /* _exec: this program "becomes" the other */
    return (intptr_t)code;
}

#define ARGS_FROM(first, list)                                                   \
    const char *av[64]; int ac = 0; va_list list; va_start(list, first);        \
    for (const char *a = first; a && ac < 63; a = va_arg(list, const char *)) av[ac++] = a; \
    av[ac] = 0;

__declspec(dllexport) intptr_t _spawnv(int m, const char *p, const char *const *a)   { return spawn_common(m, p, a, 0, 0); }
__declspec(dllexport) intptr_t _spawnvp(int m, const char *p, const char *const *a)  { return spawn_common(m, p, a, 0, 1); }
__declspec(dllexport) intptr_t _spawnve(int m, const char *p, const char *const *a, const char *const *e)  { return spawn_common(m, p, a, e, 0); }
__declspec(dllexport) intptr_t _spawnvpe(int m, const char *p, const char *const *a, const char *const *e) { return spawn_common(m, p, a, e, 1); }
__declspec(dllexport) intptr_t _spawnl(int m, const char *p, const char *a0, ...)  { ARGS_FROM(a0, ap) va_end(ap); return spawn_common(m, p, av, 0, 0); }
__declspec(dllexport) intptr_t _spawnlp(int m, const char *p, const char *a0, ...) { ARGS_FROM(a0, ap) va_end(ap); return spawn_common(m, p, av, 0, 1); }
__declspec(dllexport) intptr_t _execv(const char *p, const char *const *a)   { return spawn_common(P_OVERLAY_, p, a, 0, 0); }
__declspec(dllexport) intptr_t _execvp(const char *p, const char *const *a)  { return spawn_common(P_OVERLAY_, p, a, 0, 1); }
__declspec(dllexport) intptr_t _execve(const char *p, const char *const *a, const char *const *e)  { return spawn_common(P_OVERLAY_, p, a, e, 0); }
__declspec(dllexport) intptr_t _execvpe(const char *p, const char *const *a, const char *const *e) { return spawn_common(P_OVERLAY_, p, a, e, 1); }
__declspec(dllexport) intptr_t _execl(const char *p, const char *a0, ...)  { ARGS_FROM(a0, ap) va_end(ap); return spawn_common(P_OVERLAY_, p, av, 0, 0); }
__declspec(dllexport) intptr_t _execlp(const char *p, const char *a0, ...) { ARGS_FROM(a0, ap) va_end(ap); return spawn_common(P_OVERLAY_, p, av, 0, 1); }
__declspec(dllexport) intptr_t _cwait(int *status, intptr_t proc, int action)
{
    (void)action;
    DWORD code = 0;
    if (WaitForSingleObject((HANDLE)proc, INFINITE) != WAIT_OBJECT_0) { errno = ECHILD; return -1; }
    GetExitCodeProcess((HANDLE)proc, &code);
    CloseHandle((HANDLE)proc);
    if (status) *status = (int)code;
    return proc;
}

/* -----------------------------------------------------------------------
 * Odds and ends
 * ----------------------------------------------------------------------- */
__declspec(dllexport) int _flushall(void)
{
    fflush(0);
    return 0;
}

static int g_umask = 022;
__declspec(dllexport) int _umask(int m) { int old = g_umask; g_umask = m & 0777; return old; }
__declspec(dllexport) int _umask_s(int m, int *old) { if (old) *old = g_umask; g_umask = m & 0777; return 0; }

__declspec(dllexport) int _wchmod(const wchar_t *path, int mode)
{
    char a[MAX_PATH * 3];
    if (wcstombs(a, path, sizeof(a)) == (size_t)-1) { errno = EINVAL; return -1; }
    return chmod(a, (mode_t)mode);
}

/* _mktemp: the X's (six at the end) become a letter and the process id */
static int mktemp_fill(char *t, size_t n, int (*exists)(const char *, void *), void *ctx)
{
    if (n < 6) { errno = EINVAL; return -1; }
    char *x = t + n - 6;
    for (int i = 0; i < 6; i++) if (x[i] != 'X') { errno = EINVAL; return -1; }
    unsigned pid = (unsigned)GetCurrentProcessId();
    for (int k = 0; k < 5; k++) { x[5 - k] = (char)('0' + pid % 10); pid /= 10; }
    for (char c = 'a'; c <= 'z'; c++) {
        x[0] = c;
        if (!exists(t, ctx)) return 0;
    }
    errno = EEXIST;
    return -1;
}
static int exists_a(const char *t, void *ctx) { (void)ctx; return GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES; }
__declspec(dllexport) char *_mktemp(char *t) { return mktemp_fill(t, strlen(t), exists_a, 0) ? 0 : t; }
__declspec(dllexport) int _mktemp_s(char *t, size_t n) { (void)n; return _mktemp(t) ? 0 : errno; }

static int exists_w(const char *t, void *ctx)
{
    (void)t;
    return GetFileAttributesW((const wchar_t *)ctx) != INVALID_FILE_ATTRIBUTES;
}
__declspec(dllexport) wchar_t *_wmktemp(wchar_t *t)
{
    size_t n = wcslen(t);
    char a[MAX_PATH];
    if (n >= MAX_PATH) { errno = EINVAL; return 0; }
    for (size_t i = 0; i <= n; i++) a[i] = (char)t[i];
    /* fill the narrow copy, checking the wide name each time */
    char *x = a + n - 6;
    if (n < 6) { errno = EINVAL; return 0; }
    for (int i = 0; i < 6; i++) if (x[i] != 'X') { errno = EINVAL; return 0; }
    unsigned pid = (unsigned)GetCurrentProcessId();
    for (int k = 0; k < 5; k++) { x[5 - k] = (char)('0' + pid % 10); pid /= 10; }
    for (char c = 'a'; c <= 'z'; c++) {
        x[0] = c;
        for (size_t i = n - 6; i < n; i++) t[i] = (wchar_t)a[i];
        if (!exists_w(a, t)) return t;
    }
    errno = EEXIST;
    return 0;
}
