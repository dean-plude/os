/*
 * string.c — freestanding string/memory implementations
 *
 * These MUST be present even if the compiler inlines them, because the
 * compiler-generated code for initializers and struct copies may emit
 * direct calls to memset/memcpy as library calls.
 *
 * We mark them __attribute__((used)) to prevent the linker from dead-
 * stripping them, and avoid __builtin_ wrappers here to prevent
 * infinite recursion when the compiler substitutes builtins.
 */

#include "string.h"
#include "../mm/vmm.h"   /* for strdup → kmalloc */

/* -----------------------------------------------------------------------
 * Memory functions
 * ----------------------------------------------------------------------- */

__attribute__((used))
void *memset(void *dst, int val, size_t n)
{
    unsigned char *p = dst;
    unsigned char  v = (unsigned char)val;
    while (n--) *p++ = v;
    return dst;
}

__attribute__((used))
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;
    /* Forward copy — no overlap handling (use memmove for overlaps) */
    while (n >= 8) {
        *(uint64_t *)d = *(const uint64_t *)s;
        d += 8; s += 8; n -= 8;
    }
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((used))
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;
    if (d < s || d >= s + n) {
        /* No overlap or dst before src — forward copy */
        while (n--) *d++ = *s++;
    } else {
        /* Overlap with dst after src — copy backwards */
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    unsigned char v = (unsigned char)c;
    while (n--) {
        if (*p == v) return (void *)p;
        p++;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * String functions
 * ----------------------------------------------------------------------- */

size_t strlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *a == *b) { a++; b++; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    char       ch   = (char)c;
    const char *last = NULL;
    while (*s) {
        if (*s == ch) last = s;
        s++;
    }
    return (char *)last;
}

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char  *p   = kmalloc(len);
    if (p) memcpy(p, s, len);
    return p;
}
