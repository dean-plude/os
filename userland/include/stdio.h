#pragma once
#include <_nova.h>
#include <stddef.h>
#include <stdarg.h>
_NOVA_BEGIN
/* 48 bytes, the size of Microsoft's FILE: programs built with other
 * headers (MinGW) find stdout/stderr as &__iob_func()[1] and [2] */
typedef struct _iobuf {
    void  *_handle;        /* Win32 HANDLE */
    char  *_buf;           /* buffer (NULL: unbuffered) */
    long long _offset;     /* file offset of _buf[0] */
    int    _bufsize;
    int    _pos, _len;     /* read: next/valid bytes; write: pending bytes */
#ifdef __x86_64__
    int    _flags;
    int    _ungot;         /* ungetc character, or -1 */
    int    _fd;            /* POSIX descriptor, once one is made */
#else                      /* 32 bytes in 32-bit programs */
    int    _flags : 14;
    int    _ungot : 10;
    int    _fd : 8;
#endif
} FILE;
#ifdef __x86_64__
_Static_assert(sizeof(FILE) == 48, "FILE must match Microsoft's layout size");
#else
_Static_assert(sizeof(FILE) == 32, "FILE must match Microsoft's layout size");
#endif
typedef long long fpos_t;
#define EOF       (-1)
#define BUFSIZ    4096
#define FILENAME_MAX 260
#define FOPEN_MAX 32
#define L_tmpnam  260
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#define _IOFBF    0x0000
#define _IOLBF    0x0040
#define _IONBF    0x0004
_CRTIMP FILE *__iob_func(void);
#define stdin  (&__iob_func()[0])
#define stdout (&__iob_func()[1])
#define stderr (&__iob_func()[2])
_CRTIMP FILE  *fopen(const char *path, const char *mode);
_CRTIMP FILE  *freopen(const char *path, const char *mode, FILE *f);
_CRTIMP int    fclose(FILE *f);
_CRTIMP size_t fread(void *p, size_t size, size_t n, FILE *f);
_CRTIMP size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
_CRTIMP int    fgetc(FILE *f);
_CRTIMP int    getc(FILE *f);
_CRTIMP int    getchar(void);
_CRTIMP char  *fgets(char *s, int n, FILE *f);
_CRTIMP int    ungetc(int c, FILE *f);
_CRTIMP int    fputc(int c, FILE *f);
_CRTIMP int    putc(int c, FILE *f);
_CRTIMP int    putchar(int c);
_CRTIMP int    fputs(const char *s, FILE *f);
_CRTIMP int    puts(const char *s);
_CRTIMP int    fflush(FILE *f);
_CRTIMP int    fseek(FILE *f, long off, int whence);
_CRTIMP int    _fseeki64(FILE *f, long long off, int whence);
_CRTIMP long   ftell(FILE *f);
_CRTIMP long long _ftelli64(FILE *f);
_CRTIMP void   rewind(FILE *f);
_CRTIMP int    fgetpos(FILE *f, fpos_t *pos);
_CRTIMP int    fsetpos(FILE *f, const fpos_t *pos);
_CRTIMP int    feof(FILE *f);
_CRTIMP int    ferror(FILE *f);
_CRTIMP void   clearerr(FILE *f);
_CRTIMP int    setvbuf(FILE *f, char *buf, int mode, size_t size);
_CRTIMP void   setbuf(FILE *f, char *buf);
_CRTIMP int    _fileno(FILE *f);
_CRTIMP int    remove(const char *path);
_CRTIMP int    rename(const char *from, const char *to);
_CRTIMP void   perror(const char *s);
_CRTIMP int    printf(const char *fmt, ...);
_CRTIMP int    fprintf(FILE *f, const char *fmt, ...);
_CRTIMP int    sprintf(char *s, const char *fmt, ...);
_CRTIMP int    snprintf(char *s, size_t n, const char *fmt, ...);
_CRTIMP int    _snprintf(char *s, size_t n, const char *fmt, ...);
_CRTIMP int    vprintf(const char *fmt, va_list ap);
_CRTIMP int    vfprintf(FILE *f, const char *fmt, va_list ap);
_CRTIMP int    vsprintf(char *s, const char *fmt, va_list ap);
_CRTIMP int    vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
_CRTIMP int    _vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
_CRTIMP int    scanf(const char *fmt, ...);
_CRTIMP int    fscanf(FILE *f, const char *fmt, ...);
_CRTIMP int    sscanf(const char *s, const char *fmt, ...);
_CRTIMP int    vsscanf(const char *s, const char *fmt, va_list ap);
_CRTIMP int    vfscanf(FILE *f, const char *fmt, va_list ap);
#define fileno _fileno
_CRTIMP FILE  *_fdopen(int fd, const char *mode);
#define fdopen _fdopen
_CRTIMP FILE  *_popen(const char *cmd, const char *mode);
_CRTIMP FILE  *_wpopen(const unsigned short *cmd, const unsigned short *mode);
_CRTIMP int    _pclose(FILE *f);
#define popen _popen
#define pclose _pclose
_CRTIMP FILE  *tmpfile(void);
_CRTIMP int    vasprintf(char **out, const char *fmt, va_list ap);
_CRTIMP int    asprintf(char **out, const char *fmt, ...);
_NOVA_END
