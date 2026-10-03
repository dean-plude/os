#pragma once
#include <_nova.h>
#include <stddef.h>
_NOVA_BEGIN
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7FFF
#define MB_CUR_MAX   4
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
_CRTIMP void  *malloc(size_t n);
_CRTIMP void  *calloc(size_t n, size_t size);
_CRTIMP void  *realloc(void *p, size_t n);
_CRTIMP void   free(void *p);
_CRTIMP size_t _msize(void *p);
_CRTIMP int    atoi(const char *s);
_CRTIMP long   atol(const char *s);
_CRTIMP long long atoll(const char *s);
_CRTIMP double atof(const char *s);
_CRTIMP long   strtol(const char *s, char **end, int base);
_CRTIMP unsigned long strtoul(const char *s, char **end, int base);
_CRTIMP long long strtoll(const char *s, char **end, int base);
_CRTIMP unsigned long long strtoull(const char *s, char **end, int base);
_CRTIMP double strtod(const char *s, char **end);
_CRTIMP float  strtof(const char *s, char **end);
_CRTIMP int    abs(int x);
_CRTIMP long   labs(long x);
_CRTIMP long long llabs(long long x);
_CRTIMP div_t  div(int a, int b);
_CRTIMP ldiv_t ldiv(long a, long b);
_CRTIMP void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
_CRTIMP void   qsort_s(void *base, size_t n, size_t size, int (*cmp)(void *, const void *, const void *), void *ctx);
_CRTIMP void  *bsearch(const void *key, const void *base, size_t n, size_t size,
                       int (*cmp)(const void *, const void *));
_CRTIMP int    rand(void);
_CRTIMP void   srand(unsigned seed);
_CRTIMP __declspec(noreturn) void exit(int code);
_CRTIMP __declspec(noreturn) void _exit(int code);
_CRTIMP __declspec(noreturn) void abort(void);
_CRTIMP int    atexit(void (*fn)(void));
_CRTIMP char  *getenv(const char *name);
_CRTIMP int    system(const char *cmd);
_CRTIMP char  *_itoa(int v, char *buf, int radix);
_CRTIMP char  *_ltoa(long v, char *buf, int radix);
_CRTIMP char  *_ultoa(unsigned long v, char *buf, int radix);
#define itoa _itoa
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
/* multibyte (UTF-8) and wide conversions */
_CRTIMP size_t mbstowcs(wchar_t *d, const char *s, size_t n);
_CRTIMP size_t wcstombs(char *d, const wchar_t *s, size_t n);
_CRTIMP int    mbtowc(wchar_t *pwc, const char *s, size_t n);
_CRTIMP int    wctomb(char *s, wchar_t wc);
_CRTIMP int    mblen(const char *s, size_t n);
#define MB_CUR_MAX 4
/* Microsoft extensions */
_CRTIMP void  *_aligned_malloc(size_t n, size_t align);
_CRTIMP void   _aligned_free(void *p);
_CRTIMP void  *_aligned_realloc(void *p, size_t n, size_t align);
_CRTIMP void  *_recalloc(void *p, size_t n, size_t size);
_CRTIMP char  *_i64toa(long long v, char *buf, int radix);
_CRTIMP char  *_ui64toa(unsigned long long v, char *buf, int radix);
_CRTIMP long long _strtoi64(const char *s, char **end, int base);
_CRTIMP unsigned long long _strtoui64(const char *s, char **end, int base);
_CRTIMP long long _atoi64(const char *s);
_CRTIMP int    rand_s(unsigned int *v);
_CRTIMP __declspec(noreturn) void quick_exit(int code);
_CRTIMP int    at_quick_exit(void (*fn)(void));
_CRTIMP int    _set_error_mode(int m);
extern _CRTIMP int __argc;
extern _CRTIMP char **__argv;
extern _CRTIMP wchar_t **__wargv;
extern _CRTIMP char **_environ;
_NOVA_END
#ifdef __cplusplus
/* C++: the overloads the Windows SDK's stdlib.h adds */
extern "C++" {
inline long abs(long x) { return labs(x); }
inline long long abs(long long x) { return llabs(x); }
}
#endif
