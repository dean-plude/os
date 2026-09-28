/* msvcrt: FILE streams over Win32 handles */
#define NOVA_BUILD_MSVCRT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <windows.h>
#include "msvcrt_internal.h"

#define F_READ     0x001
#define F_WRITE    0x002
#define F_EOF      0x004
#define F_ERR      0x008
#define F_APPEND   0x010
#define F_LINEBUF  0x020
#define F_NOBUF    0x040
#define F_OWNBUF   0x080
#define F_WRITING  0x100       /* buffer holds pending output */
#define F_READING  0x200       /* buffer holds read-ahead input */
#define F_CONSOLE  0x400
#define F_OPEN     0x800
#define F_HASFD    0x1000      /* _fd is a descriptor naming this stream's handle */
#define F_OWNFD    0x2000      /* from fdopen: closing the stream closes _fd */

static FILE g_iob[3];
static FILE *g_files[FOPEN_MAX];
static int  g_iob_ready;

static void set_errno_from_win32(void)
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
    default:                   errno = EINVAL; break;
    }
}

static void init_std(FILE *f, DWORD which, int flags)
{
    f->_handle = GetStdHandle(which);
    f->_ungot = -1;
    f->_flags = flags | F_OPEN;
    if (GetFileType(f->_handle) == FILE_TYPE_CHAR) {
        f->_flags |= F_CONSOLE;
        if (which == STD_OUTPUT_HANDLE) f->_flags |= F_LINEBUF;
    }
}

FILE *__iob_func(void)
{
    if (!g_iob_ready) {
        g_iob_ready = 1;
        init_std(&g_iob[0], STD_INPUT_HANDLE, F_READ);
        init_std(&g_iob[1], STD_OUTPUT_HANDLE, F_WRITE);
        init_std(&g_iob[2], STD_ERROR_HANDLE, F_WRITE | F_NOBUF);
    }
    return g_iob;
}

static int ensure_buf(FILE *f)
{
    if (f->_buf || (f->_flags & F_NOBUF)) return 1;
    f->_buf = malloc(BUFSIZ);
    if (!f->_buf) { f->_flags |= F_NOBUF; return 1; }
    f->_bufsize = BUFSIZ;
    f->_flags |= F_OWNBUF;
    return 1;
}

static int flush_write(FILE *f)
{
    if (!(f->_flags & F_WRITING)) return 0;
    int off = 0;
    while (off < f->_pos) {
        DWORD w;
        if (!WriteFile(f->_handle, f->_buf + off, (DWORD)(f->_pos - off), &w, 0) || !w) {
            f->_flags |= F_ERR;
            set_errno_from_win32();
            f->_pos = 0;
            f->_flags &= ~F_WRITING;
            return EOF;
        }
        off += (int)w;
    }
    f->_pos = 0;
    f->_flags &= ~F_WRITING;
    return 0;
}

/* Drop read-ahead and move the OS file position back to the logical one */
static void drop_read(FILE *f)
{
    if (!(f->_flags & F_READING)) return;
    int ahead = f->_len - f->_pos + (f->_ungot >= 0);
    if (ahead && !(f->_flags & F_CONSOLE)) {
        LARGE_INTEGER d; d.QuadPart = -ahead;
        SetFilePointerEx(f->_handle, d, 0, FILE_CURRENT);
    }
    f->_pos = f->_len = 0;
    f->_ungot = -1;
    f->_flags &= ~F_READING;
}

int fflush(FILE *f)
{
    if (!f) {
        int r = 0;
        for (int i = 0; i < 3; i++) if (flush_write(&__iob_func()[i])) r = EOF;
        for (int i = 0; i < FOPEN_MAX; i++) if (g_files[i] && flush_write(g_files[i])) r = EOF;
        return r;
    }
    if (f->_flags & F_WRITING) return flush_write(f);
    drop_read(f);
    return 0;
}

void __nova_flush_all(void) { fflush(0); }

/* Refill the read buffer; 0 at EOF/error */
static int fill(FILE *f)
{
    if (f->_flags & F_WRITING) flush_write(f);
    ensure_buf(f);
    char one;
    char *dst = f->_buf ? f->_buf : &one;
    DWORD cap = f->_buf ? (DWORD)f->_bufsize : 1, got = 0;
    if (!ReadFile(f->_handle, dst, cap, &got, 0)) { f->_flags |= F_ERR; set_errno_from_win32(); return 0; }
    if (!got) { f->_flags |= F_EOF; return 0; }
    if (f->_flags & F_CONSOLE) {                         /* console lines end "\r\n" */
        DWORD o = 0;
        for (DWORD i = 0; i < got; i++) if (dst[i] != '\r') dst[o++] = dst[i];
        got = o;
        if (!got) return fill(f);
    }
    if (!f->_buf) { f->_ungot = (unsigned char)one; return 1; }
    f->_pos = 0;
    f->_len = (int)got;
    f->_flags |= F_READING;
    return 1;
}

int fgetc(FILE *f)
{
    if (!(f->_flags & F_READ)) { f->_flags |= F_ERR; errno = EBADF; return EOF; }
    if (f == stdin) fflush(stdout);                     /* show prompts before waiting */
    if (f->_ungot >= 0) { int c = f->_ungot; f->_ungot = -1; return c; }
    if (f->_pos >= f->_len && !fill(f)) return EOF;
    if (f->_ungot >= 0) { int c = f->_ungot; f->_ungot = -1; return c; }
    return (unsigned char)f->_buf[f->_pos++];
}

int getc(FILE *f)     { return fgetc(f); }
int getchar(void)     { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF || f->_ungot >= 0) return EOF;
    f->_ungot = (unsigned char)c;
    f->_flags &= ~F_EOF;
    return c;
}

char *fgets(char *s, int n, FILE *f)
{
    if (n <= 0) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return 0;
    s[i] = 0;
    return s;
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    size_t want = size * n, got = 0;
    unsigned char *d = p;
    if (!want) return 0;
    while (got < want) {
        if (f->_ungot >= 0 || f->_pos < f->_len) {
            int c = fgetc(f);
            if (c == EOF) break;
            d[got++] = (unsigned char)c;
            continue;
        }
        if (want - got >= (size_t)BUFSIZ && !(f->_flags & F_CONSOLE)) {   /* large: read directly */
            if (f->_flags & F_WRITING) flush_write(f);
            DWORD r = 0;
            if (!ReadFile(f->_handle, d + got, (DWORD)(want - got > 0x40000000 ? 0x40000000 : want - got), &r, 0)) {
                f->_flags |= F_ERR; break;
            }
            if (!r) { f->_flags |= F_EOF; break; }
            got += r;
            continue;
        }
        if (!fill(f)) break;
    }
    return got / size;
}

static int put_bytes(FILE *f, const char *p, size_t n)
{
    if (!(f->_flags & F_WRITE)) { f->_flags |= F_ERR; errno = EBADF; return EOF; }
    drop_read(f);
    if (f->_flags & F_APPEND && !(f->_flags & F_WRITING)) {
        LARGE_INTEGER z; z.QuadPart = 0;
        SetFilePointerEx(f->_handle, z, 0, FILE_END);
    }
    ensure_buf(f);
    if (!f->_buf) {
        DWORD w;
        if (!WriteFile(f->_handle, p, (DWORD)n, &w, 0)) { f->_flags |= F_ERR; set_errno_from_win32(); return EOF; }
        return 0;
    }
    int line = 0;
    for (size_t i = 0; i < n; i++) {
        if (f->_pos == f->_bufsize && flush_write(f)) return EOF;
        f->_buf[f->_pos++] = p[i];
        f->_flags |= F_WRITING;
        if (p[i] == '\n' && (f->_flags & F_LINEBUF)) line = 1;
    }
    if (line) return flush_write(f);
    return 0;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    if (!size || !n) return 0;
    return put_bytes(f, p, size * n) ? 0 : n;
}

int fputc(int c, FILE *f) { char ch = (char)c; return put_bytes(f, &ch, 1) ? EOF : (unsigned char)c; }
int putc(int c, FILE *f)  { return fputc(c, f); }
int putchar(int c)        { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) { return put_bytes(f, s, strlen(s)) ? EOF : 0; }
int puts(const char *s)   { return fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF ? EOF : 0; }

FILE *fopen(const char *path, const char *mode)
{
    int slot = -1;
    for (int i = 0; i < FOPEN_MAX; i++) if (!g_files[i]) { slot = i; break; }
    if (slot < 0) { errno = EMFILE; return 0; }
    DWORD access = 0, disp = 0;
    int flags = F_OPEN;
    int plus = strchr(mode, '+') != 0;
    switch (mode[0]) {
    case 'r': access = GENERIC_READ | (plus ? GENERIC_WRITE : 0); disp = OPEN_EXISTING; flags |= F_READ | (plus ? F_WRITE : 0); break;
    case 'w': access = GENERIC_WRITE | (plus ? GENERIC_READ : 0); disp = CREATE_ALWAYS; flags |= F_WRITE | (plus ? F_READ : 0); break;
    case 'a': access = GENERIC_WRITE | (plus ? GENERIC_READ : 0); disp = OPEN_ALWAYS; flags |= F_WRITE | F_APPEND | (plus ? F_READ : 0); break;
    default:  errno = EINVAL; return 0;
    }
    if (mode[1] == 'x' || (mode[1] && mode[2] == 'x')) disp = CREATE_NEW;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, disp, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { set_errno_from_win32(); return 0; }
    FILE *f = calloc(1, sizeof(FILE));
    if (!f) { CloseHandle(h); errno = ENOMEM; return 0; }
    f->_handle = h;
    f->_flags = flags;
    f->_ungot = -1;
    if (GetFileType(h) == FILE_TYPE_CHAR) f->_flags |= F_CONSOLE | F_LINEBUF;
    g_files[slot] = f;
    return f;
}

int fclose(FILE *f)
{
    if (!f) return EOF;
    int r = fflush(f);
    if (f >= g_iob && f < g_iob + 3) { f->_flags = 0; return r; }
    if (f->_flags & F_OWNFD) {
        if (close(f->_fd)) r = EOF;
    } else {
        if (f->_flags & F_HASFD) close(f->_fd);
        if (!CloseHandle(f->_handle)) r = EOF;
    }
    if (f->_flags & F_OWNBUF) free(f->_buf);
    for (int i = 0; i < FOPEN_MAX; i++) if (g_files[i] == f) g_files[i] = 0;
    free(f);
    return r;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    FILE *n = fopen(path, mode);
    if (!n) return 0;
    fflush(f);
    if (!(f >= g_iob && f < g_iob + 3)) CloseHandle(f->_handle);
    if (f->_flags & F_OWNBUF) free(f->_buf);
    for (int i = 0; i < FOPEN_MAX; i++) if (g_files[i] == n) g_files[i] = f;
    *f = *n;
    free(n);
    return f;
}

long long _ftelli64(FILE *f)
{
    LARGE_INTEGER z, cur;
    z.QuadPart = 0;
    if (f->_flags & F_CONSOLE) { errno = ESPIPE; return -1; }
    if (!SetFilePointerEx(f->_handle, z, &cur, FILE_CURRENT)) { set_errno_from_win32(); return -1; }
    long long pos = cur.QuadPart;
    if (f->_flags & F_WRITING) pos += f->_pos;
    if (f->_flags & F_READING) pos -= f->_len - f->_pos;
    if (f->_ungot >= 0) pos--;
    return pos;
}

int _fseeki64(FILE *f, long long off, int whence)
{
    if (f->_flags & F_CONSOLE) { errno = ESPIPE; return -1; }
    if (whence == SEEK_CUR) { off += _ftelli64(f); whence = SEEK_SET; }
    fflush(f);
    f->_ungot = -1;
    LARGE_INTEGER d;
    d.QuadPart = off;
    if (!SetFilePointerEx(f->_handle, d, 0, whence == SEEK_SET ? FILE_BEGIN : FILE_END)) {
        set_errno_from_win32();
        return -1;
    }
    f->_flags &= ~F_EOF;
    return 0;
}

long ftell(FILE *f)                           { return (long)_ftelli64(f); }
int  fseek(FILE *f, long off, int whence)     { return _fseeki64(f, off, whence); }
void rewind(FILE *f)                          { _fseeki64(f, 0, SEEK_SET); f->_flags &= ~F_ERR; }
int  fgetpos(FILE *f, fpos_t *pos)            { *pos = _ftelli64(f); return *pos < 0 ? -1 : 0; }
int  fsetpos(FILE *f, const fpos_t *pos)      { return _fseeki64(f, *pos, SEEK_SET); }
int  feof(FILE *f)                            { return (f->_flags & F_EOF) != 0; }
int  ferror(FILE *f)                          { return (f->_flags & F_ERR) != 0; }
void clearerr(FILE *f)                        { f->_flags &= ~(F_EOF | F_ERR); }
int _fileno(FILE *f)
{
    if (f >= g_iob && f < g_iob + 3) return (int)(f - g_iob);
    if (!(f->_flags & F_HASFD)) {
        int fd = __nova_fd_new(f->_handle, 0);
        if (fd < 0) return -1;
        f->_fd = fd;
        f->_flags |= F_HASFD;
    }
    return f->_fd;
}

static FILE *add_stream(HANDLE h, int flags)
{
    int slot = -1;
    for (int i = 0; i < FOPEN_MAX; i++) if (!g_files[i]) { slot = i; break; }
    if (slot < 0) { errno = EMFILE; return 0; }
    FILE *f = calloc(1, sizeof(FILE));
    if (!f) { errno = ENOMEM; return 0; }
    f->_handle = h;
    f->_flags = flags | F_OPEN;
    f->_ungot = -1;
    if (GetFileType(h) == FILE_TYPE_CHAR) f->_flags |= F_CONSOLE | F_LINEBUF;
    g_files[slot] = f;
    return f;
}

FILE *_fdopen(int fd, const char *mode)
{
    HANDLE h = __nova_fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int flags = F_OWNFD;
    int plus = strchr(mode, '+') != 0;
    switch (mode[0]) {
    case 'r': flags |= F_READ | (plus ? F_WRITE : 0); break;
    case 'w': flags |= F_WRITE | (plus ? F_READ : 0); break;
    case 'a': flags |= F_WRITE | F_APPEND | (plus ? F_READ : 0); break;
    default:  errno = EINVAL; return 0;
    }
    FILE *f = add_stream(h, flags);
    if (f) f->_fd = fd;
    return f;
}

FILE *tmpfile(void)
{
    static unsigned n;
    char name[64];
    for (int tries = 0; tries < 100; tries++) {
        snprintf(name, sizeof(name), "C:\\Temp\\tmp%u_%u.tmp", (unsigned)GetCurrentProcessId(), n++);
        CreateDirectoryA("C:\\Temp", 0);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, 0, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        FILE *f = add_stream(h, F_READ | F_WRITE);
        if (!f) CloseHandle(h);
        return f;
    }
    errno = EEXIST;
    return 0;
}

int vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(0, 0, fmt, ap2);
    va_end(ap2);
    *out = 0;
    if (n < 0) return -1;
    char *s = malloc((size_t)n + 1);
    if (!s) { errno = ENOMEM; return -1; }
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    *out = s;
    return n;
}

int asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    fflush(f);
    if (f->_flags & F_OWNBUF) free(f->_buf);
    f->_flags &= ~(F_OWNBUF | F_LINEBUF | F_NOBUF);
    f->_buf = 0;
    f->_bufsize = 0;
    if (mode == _IONBF) { f->_flags |= F_NOBUF; return 0; }
    if (mode == _IOLBF) f->_flags |= F_LINEBUF;
    if (buf && size) { f->_buf = buf; f->_bufsize = (int)size; }
    return 0;
}

void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }

int remove(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    BOOL ok = a != INVALID_FILE_ATTRIBUTES && ((a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryA(path) : DeleteFileA(path));
    if (!ok) { set_errno_from_win32(); return -1; }
    return 0;
}

int rename(const char *from, const char *to)
{
    /* copy then delete (NovaOS has no rename call yet) */
    FILE *a = fopen(from, "rb");
    if (!a) return -1;
    FILE *b = fopen(to, "wb");
    if (!b) { fclose(a); return -1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    if (fclose(b)) return -1;
    return remove(from);
}

void perror(const char *s)
{
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

/* conio */
int _getch(void) { return fgetc(stdin); }
int _kbhit(void) { return 0; }
