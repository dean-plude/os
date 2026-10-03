/* msvcrt: <string.h> and <ctype.h> */
#define NOVA_BUILD_MSVCRT
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n)
{
    void *r = d;
    if ((char *)d <= (const char *)s || (char *)d >= (const char *)s + n) {
        __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        d = (char *)d + n - 1; s = (const char *)s + n - 1;
        __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return r;
}

void *memset(void *d, int c, size_t n)
{
    void *r = d;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

int memcmp(const void *x, const void *y, size_t n)
{
    const unsigned char *a = x, *b = y;
    for (; n >= 8; n -= 8, a += 8, b += 8) {          /* 8 bytes at a time up to a difference */
        unsigned long long p, q;
        __builtin_memcpy(&p, a, 8);
        __builtin_memcpy(&q, b, 8);
        if (p != q) break;
    }
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}

size_t strlen(const char *s)            { const char *p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
char *strcpy(char *d, const char *s)    { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strcat(char *d, const char *s)    { strcpy(d + strlen(d), s); return d; }

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *strncat(char *d, const char *s, size_t n)
{
    char *e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0;
    return d;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

char *strrchr(const char *s, int c)
{
    const char *r = 0;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strstr(const char *h, const char *n)
{
    size_t k = strlen(n);
    if (!k) return (char *)h;
    for (; *h; h++) if (*h == *n && !strncmp(h, n, k)) return (char *)h;
    return 0;
}

size_t strspn(const char *s, const char *a)  { size_t i = 0; while (s[i] && strchr(a, s[i])) i++; return i; }
size_t strcspn(const char *s, const char *r) { size_t i = 0; while (s[i] && !strchr(r, s[i])) i++; return i; }
char *strpbrk(const char *s, const char *a)  { s += strcspn(s, a); return *s ? (char *)s : 0; }

char *strtok_s(char *s, const char *delim, char **ctx)
{
    if (!s) s = *ctx;
    if (!s) return 0;
    s += strspn(s, delim);
    if (!*s) { *ctx = 0; return 0; }
    char *e = s + strcspn(s, delim);
    if (*e) { *e = 0; *ctx = e + 1; } else *ctx = 0;
    return s;
}

char *strtok(char *s, const char *delim)
{
    static char *ctx;
    return strtok_s(s, delim, &ctx);
}

char *_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

int _strnicmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}

int _stricmp(const char *a, const char *b) { return _strnicmp(a, b, (size_t)-1); }
char *_strlwr(char *s) { for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p); return s; }
char *_strupr(char *s) { for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p); return s; }

char *strerror(int e)
{
    switch (e) {
    case 0:       return "No error";
    case EPERM:   return "Operation not permitted";
    case ENOENT:  return "No such file or directory";
    case EIO:     return "Input/output error";
    case EBADF:   return "Bad file descriptor";
    case ENOMEM:  return "Not enough memory";
    case EACCES:  return "Permission denied";
    case EEXIST:  return "File exists";
    case ENOTDIR: return "Not a directory";
    case EISDIR:  return "Is a directory";
    case EINVAL:  return "Invalid argument";
    case EMFILE:  return "Too many open files";
    case ENOSPC:  return "No space left on device";
    case ESPIPE:  return "Illegal seek";
    case EDOM:    return "Domain error";
    case ERANGE:  return "Result too large";
    case ENOSYS:  return "Function not implemented";
    case ENOTEMPTY: return "Directory not empty";
    }
    return "Unknown error";
}

/* ctype (ASCII) */
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isblank(int c)  { return c == ' ' || c == '\t'; }
int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c)  { return c >= 32 && c < 127; }
int isgraph(int c)  { return c > 32 && c < 127; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int toupper(int c)  { return islower(c) ? c - 32 : c; }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }
