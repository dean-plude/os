/* mbedtls_nova_platform.h — the libc-style hooks Mbed TLS calls in the
 * NovaOS kernel (implemented in net/tls_platform.c) */
#pragma once
#include <stddef.h>
#include <stdarg.h>

void     *nova_calloc(size_t n, size_t size);
void      nova_free(void *p);
int       nova_snprintf(char *s, size_t n, const char *fmt, ...);
int       nova_vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int       nova_printf(const char *fmt, ...);
int       nova_fprintf(void *f, const char *fmt, ...);
void      nova_setbuf(void *f, char *buf);
void      nova_exit(int status);
long long nova_time(long long *t);
