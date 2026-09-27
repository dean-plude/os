/* stdlib.h shim for lwIP: the only function it uses is atoi (netif_find) */
#pragma once
#include <stddef.h>

static inline int atoi(const char *s)
{
    int n = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') neg = (*s++ == '-');
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}
