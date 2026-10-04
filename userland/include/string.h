#pragma once
#include <_nova.h>
#include <stddef.h>
_NOVA_BEGIN
_CRTIMP void  *memcpy(void *d, const void *s, size_t n);
_CRTIMP void  *memmove(void *d, const void *s, size_t n);
_CRTIMP void  *memset(void *d, int c, size_t n);
_CRTIMP int    memcmp(const void *a, const void *b, size_t n);
_CRTIMP void  *memchr(const void *s, int c, size_t n);
_CRTIMP size_t strlen(const char *s);
_CRTIMP size_t strnlen(const char *s, size_t n);
_CRTIMP char  *strcpy(char *d, const char *s);
_CRTIMP char  *strncpy(char *d, const char *s, size_t n);
_CRTIMP char  *strcat(char *d, const char *s);
_CRTIMP char  *strncat(char *d, const char *s, size_t n);
_CRTIMP int    strcmp(const char *a, const char *b);
_CRTIMP int    strncmp(const char *a, const char *b, size_t n);
_CRTIMP int    strcoll(const char *a, const char *b);
_CRTIMP char  *strchr(const char *s, int c);
_CRTIMP char  *strrchr(const char *s, int c);
_CRTIMP char  *strstr(const char *h, const char *n);
_CRTIMP size_t strspn(const char *s, const char *accept);
_CRTIMP size_t strcspn(const char *s, const char *reject);
_CRTIMP char  *strpbrk(const char *s, const char *accept);
_CRTIMP char  *strtok(char *s, const char *delim);
_CRTIMP char  *strtok_s(char *s, const char *delim, char **ctx);
_CRTIMP char  *_strdup(const char *s);
_CRTIMP int    _stricmp(const char *a, const char *b);
_CRTIMP int    _strnicmp(const char *a, const char *b, size_t n);
_CRTIMP int    _strcmpi(const char *a, const char *b);
_CRTIMP char  *_strlwr(char *s);
_CRTIMP char  *_strupr(char *s);
_CRTIMP char  *strerror(int errnum);
#define strdup      _strdup
#define stricmp     _stricmp
#define strnicmp    _strnicmp
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
_NOVA_END
