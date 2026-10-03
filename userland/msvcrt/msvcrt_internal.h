/* msvcrt_internal.h — helpers shared by the msvcrt sources (not exported) */
#pragma once

/* posix.c: descriptor table */
int   __nova_fd_new(void *handle, int owns);    /* new descriptor; -1 on failure */
void *__nova_fd_handle(int fd);                 /* INVALID_HANDLE_VALUE if bad */
int   close(int fd);
void  __nova_set_errno_win32(void);           /* errno and _doserrno from GetLastError */
int   _set_doserrno(unsigned long v);

/* printf.c: the formatter, shared by the narrow, wide and UCRT entry points */
#include <stdarg.h>
#include <stddef.h>
typedef struct {
    void (*put)(void *ctx, const char *s, size_t n);
    void *ctx;
    size_t total;
} PrintSink;
/* 1 in msvcrt.dll (legacy behaviour), 0 in ucrtbase.dll (crt_flavor.c) */
extern const int __nova_crt_legacy;
#define PF_WIDE 1               /* %s/%c are wide (the w functions) */
struct _iobuf;
int __nova_printf_core(PrintSink *k, const char *fmt, va_list ap, int flags);
int __nova_vsnprintf(char *s, size_t n, const char *fmt, va_list ap, int flags);
int __nova_vfprintf(struct _iobuf *f, const char *fmt, va_list ap, int flags);
int __nova_vsnwprintf(unsigned short *s, size_t n, const unsigned short *fmt, va_list ap, int flags);
int __nova_vfwprintf(struct _iobuf *f, const unsigned short *fmt, va_list ap, int flags);
