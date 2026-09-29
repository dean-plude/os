/* wchar.h — wide (UTF-16) strings and conversions */
#pragma once
#include <_nova.h>
#include <stddef.h>
#include <stdarg.h>
_NOVA_BEGIN
#ifndef _WINT_T_DEFINED
#define _WINT_T_DEFINED
typedef unsigned short wint_t;
#endif
#ifndef WEOF
#define WEOF ((wint_t)0xFFFF)
#endif
typedef struct { unsigned int _state; } mbstate_t;
struct tm;
struct _iobuf;

_CRTIMP size_t   wcslen(const wchar_t *s);
_CRTIMP size_t   wcsnlen(const wchar_t *s, size_t n);
_CRTIMP wchar_t *wcscpy(wchar_t *d, const wchar_t *s);
_CRTIMP wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n);
_CRTIMP wchar_t *wcscat(wchar_t *d, const wchar_t *s);
_CRTIMP wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n);
_CRTIMP int      wcscmp(const wchar_t *a, const wchar_t *b);
_CRTIMP int      wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
_CRTIMP int      _wcsicmp(const wchar_t *a, const wchar_t *b);
_CRTIMP int      _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n);
_CRTIMP int      wcscoll(const wchar_t *a, const wchar_t *b);
_CRTIMP size_t   wcsxfrm(wchar_t *d, const wchar_t *s, size_t n);
_CRTIMP wchar_t *wcschr(const wchar_t *s, wchar_t c);
_CRTIMP wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
_CRTIMP wchar_t *wcsstr(const wchar_t *h, const wchar_t *n);
_CRTIMP size_t   wcsspn(const wchar_t *s, const wchar_t *accept);
_CRTIMP size_t   wcscspn(const wchar_t *s, const wchar_t *reject);
_CRTIMP wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
_CRTIMP wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **ctx);
_CRTIMP wchar_t *_wcsdup(const wchar_t *s);
_CRTIMP wchar_t *_wcslwr(wchar_t *s);
_CRTIMP wchar_t *_wcsupr(wchar_t *s);
_CRTIMP wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n);
_CRTIMP wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);
_CRTIMP wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n);
_CRTIMP int      wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
_CRTIMP wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

_CRTIMP long               wcstol(const wchar_t *s, wchar_t **end, int base);
_CRTIMP unsigned long      wcstoul(const wchar_t *s, wchar_t **end, int base);
_CRTIMP long long          wcstoll(const wchar_t *s, wchar_t **end, int base);
_CRTIMP unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);
_CRTIMP double             wcstod(const wchar_t *s, wchar_t **end);
_CRTIMP float              wcstof(const wchar_t *s, wchar_t **end);
_CRTIMP int                _wtoi(const wchar_t *s);
_CRTIMP long               _wtol(const wchar_t *s);
_CRTIMP long long          _wtoi64(const wchar_t *s);
_CRTIMP double             _wtof(const wchar_t *s);
_CRTIMP wchar_t *_itow(int v, wchar_t *buf, int radix);
_CRTIMP wchar_t *_ltow(long v, wchar_t *buf, int radix);
_CRTIMP wchar_t *_ultow(unsigned long v, wchar_t *buf, int radix);
_CRTIMP wchar_t *_i64tow(long long v, wchar_t *buf, int radix);
_CRTIMP wchar_t *_ui64tow(unsigned long long v, wchar_t *buf, int radix);

/* multibyte: UTF-8 <-> UTF-16 (the C locale here is UTF-8) */
_CRTIMP wint_t  btowc(int c);
_CRTIMP int     wctob(wint_t c);
_CRTIMP int     mbsinit(const mbstate_t *ps);
_CRTIMP size_t  mbrlen(const char *s, size_t n, mbstate_t *ps);
_CRTIMP size_t  mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
_CRTIMP size_t  wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
_CRTIMP size_t  mbsrtowcs(wchar_t *d, const char **src, size_t n, mbstate_t *ps);
_CRTIMP size_t  wcsrtombs(char *d, const wchar_t **src, size_t n, mbstate_t *ps);

/* wide stdio (%s is a wide string, %S a narrow one, as in Microsoft's) */
_CRTIMP int swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...);
_CRTIMP int vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);
_CRTIMP int _snwprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...);
_CRTIMP int _vsnwprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);
_CRTIMP int wprintf(const wchar_t *fmt, ...);
_CRTIMP int vwprintf(const wchar_t *fmt, va_list ap);
_CRTIMP int fwprintf(struct _iobuf *f, const wchar_t *fmt, ...);
_CRTIMP int vfwprintf(struct _iobuf *f, const wchar_t *fmt, va_list ap);
_CRTIMP wint_t fgetwc(struct _iobuf *f);
_CRTIMP wint_t getwc(struct _iobuf *f);
_CRTIMP wint_t fputwc(wchar_t c, struct _iobuf *f);
_CRTIMP wint_t putwc(wchar_t c, struct _iobuf *f);
_CRTIMP wint_t ungetwc(wint_t c, struct _iobuf *f);
_CRTIMP int    fputws(const wchar_t *s, struct _iobuf *f);
_CRTIMP wchar_t *fgetws(wchar_t *s, int n, struct _iobuf *f);
_CRTIMP int    _putws(const wchar_t *s);
_CRTIMP struct _iobuf *_wfopen(const wchar_t *name, const wchar_t *mode);
_CRTIMP size_t wcsftime(wchar_t *s, size_t n, const wchar_t *fmt, const struct tm *t);
_CRTIMP wchar_t *_wgetenv(const wchar_t *name);
_CRTIMP int    _wremove(const wchar_t *name);
_CRTIMP int    _wrename(const wchar_t *a, const wchar_t *b);
_CRTIMP int    _wunlink(const wchar_t *name);
_NOVA_END
