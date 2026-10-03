/* msvcrt: <time.h> (UTC — NovaOS has no time zones yet) */
#define NOVA_BUILD_MSVCRT
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "ptd.h"

time_t time(time_t *t)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG v = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    time_t r = (time_t)(v / 10000000ULL) - 11644473600LL;
    if (t) *t = r;
    return r;
}

static ULONGLONG g_clock0;
clock_t clock(void)
{
    ULONGLONG now = GetTickCount64();
    if (!g_clock0) g_clock0 = now ? now : 1;
    return (clock_t)(now - g_clock0);
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

static long long days_from_civil(long long y, int m, int d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

struct tm *gmtime(const time_t *t)
{
    struct tm *r = &__nova_ptd()->tm;
    long long s = *t, days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    r->tm_hour = (int)(rem / 3600);
    r->tm_min = (int)(rem % 3600 / 60);
    r->tm_sec = (int)(rem % 60);
    r->tm_wday = (int)(((days % 7) + 11) % 7);
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    long long y = yoe + era * 400 + (m <= 2);
    r->tm_year = (int)(y - 1900);
    r->tm_mon = m - 1;
    r->tm_mday = d;
    r->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    r->tm_isdst = 0;
    return r;
}

struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *tm)
{
    long long y = tm->tm_year + 1900LL + tm->tm_mon / 12;
    int m = tm->tm_mon % 12;
    if (m < 0) { m += 12; y--; }
    time_t t = (time_t)(days_from_civil(y, m + 1, 1) + tm->tm_mday - 1) * 86400
             + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
    *tm = *gmtime(&t);
    return t;
}

static const char *wday[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *mon[] = { "January", "February", "March", "April", "May", "June", "July",
                             "August", "September", "October", "November", "December" };

size_t strftime(char *s, size_t n, const char *f, const struct tm *tm)
{
    size_t o = 0;
    char tmp[64];
    for (; *f; f++) {
        const char *add = tmp;
        if (*f != '%') { tmp[0] = *f; tmp[1] = 0; }
        else switch (*++f) {
        case 'a': snprintf(tmp, sizeof(tmp), "%.3s", wday[tm->tm_wday % 7]); break;
        case 'A': add = wday[tm->tm_wday % 7]; break;
        case 'b': case 'h': snprintf(tmp, sizeof(tmp), "%.3s", mon[tm->tm_mon % 12]); break;
        case 'B': add = mon[tm->tm_mon % 12]; break;
        case 'c': snprintf(tmp, sizeof(tmp), "%.3s %.3s %2d %02d:%02d:%02d %d", wday[tm->tm_wday % 7],
                           mon[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); break;
        case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
        case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
        case 'F': snprintf(tmp, sizeof(tmp), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
        case 'I': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
        case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
        case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
        case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
        case 'p': add = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
        case 'T': case 'X': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case 'x': case 'D': snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
        case 'y': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_year % 100); break;
        case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
        case 'w': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday); break;
        case 'Z': add = "UTC"; break;
        case 'z': add = "+0000"; break;
        case '%': add = "%"; break;
        default:  tmp[0] = 0; break;
        }
        size_t k = strlen(add);
        if (o + k >= n) return 0;
        memcpy(s + o, add, k);
        o += k;
    }
    if (o >= n) return 0;
    s[o] = 0;
    return o;
}

char *asctime(const struct tm *tm)
{
    char *buf = __nova_ptd()->asc;
    snprintf(buf, sizeof(__nova_ptd()->asc), "%.3s %.3s %2d %02d:%02d:%02d %d\n", wday[tm->tm_wday % 7], mon[tm->tm_mon % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *t) { return asctime(gmtime(t)); }
