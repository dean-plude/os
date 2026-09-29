/* advapi32.h — helpers shared by advapi32's sources (not exported) */
#pragma once
#include <windows.h>

#define ERROR_INVALID_SID 1337

static inline size_t strlen_(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static inline int memcmp_(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

const char *user_name(void);        /* %USERNAME%, else "User" */
