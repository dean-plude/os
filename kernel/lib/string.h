/*
 * string.h — freestanding string/memory utilities for the kernel
 *
 * No libc dependency.  These are the functions the kernel uses internally.
 * The compiler may also call __builtin_memcpy/__builtin_memset which
 * we satisfy through the actual implementations below.
 */

#pragma once

#include "../include/types.h"

/* Memory */
void  *memset(void *dst, int val, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

/* String */
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strdup(const char *s);   /* uses kmalloc */
