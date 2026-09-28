#pragma once
#include <_nova.h>
#include <stddef.h>
_NOVA_BEGIN
typedef long long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
struct timespec { time_t tv_sec; long tv_nsec; };
_CRTIMP time_t     time(time_t *t);
_CRTIMP clock_t    clock(void);
_CRTIMP double     difftime(time_t a, time_t b);
_CRTIMP struct tm *gmtime(const time_t *t);
_CRTIMP struct tm *localtime(const time_t *t);
_CRTIMP time_t     mktime(struct tm *tm);
_CRTIMP size_t     strftime(char *s, size_t n, const char *fmt, const struct tm *tm);
_CRTIMP char      *asctime(const struct tm *tm);
_CRTIMP char      *ctime(const time_t *t);
_NOVA_END
