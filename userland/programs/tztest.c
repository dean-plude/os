/* tztest.exe — time zones: the zone in effect and its offset now
 * (GetDynamicTimeZoneInformation, GetLocalTime against GetSystemTime,
 * the C runtime's localtime), then the daylight-saving rules of zones from
 * the list (GetTimeZoneInformationForYear by name) at known instants:
 * Berlin and Sydney in summer and winter, and the minute Los Angeles
 * springs forward.  Prints "[TZ] zone NAME bias B local-utc M crt M" and
 * "tztest: N passed, M failed". */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <windows.h>

__declspec(dllimport) time_t _mkgmtime64(struct tm *tm);

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static LONGLONG minutes(const SYSTEMTIME *st)
{
    FILETIME ft;
    SystemTimeToFileTime(st, &ft);
    return (LONGLONG)(((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 600000000ULL);
}

static BOOL zone(const WCHAR *name, TIME_ZONE_INFORMATION *tz)
{
    DYNAMIC_TIME_ZONE_INFORMATION d;
    memset(&d, 0, sizeof(d));
    wcscpy(d.TimeZoneKeyName, name);
    return GetTimeZoneInformationForYear(2026, &d, tz);
}

/* UTC y-m-d h:m in zone @name is local hour lh:lm */
static void at(const WCHAR *name, int y, int mo, int d, int h, int mi, int lh, int lm)
{
    TIME_ZONE_INFORMATION tz;
    SYSTEMTIME u = { 0 }, l, back;
    u.wYear = (WORD)y; u.wMonth = (WORD)mo; u.wDay = (WORD)d; u.wHour = (WORD)h; u.wMinute = (WORD)mi;
    char what[160];
    _snprintf(what, sizeof(what), "%ls %04d-%02d-%02d %02d:%02d UTC is %02d:%02d", name, y, mo, d, h, mi, lh, lm);
    if (!zone(name, &tz)) { CHECK(what, 0); return; }
    SystemTimeToTzSpecificLocalTime(&tz, &u, &l);
    if (l.wHour != lh || l.wMinute != lm) printf("  got %02u:%02u\n", l.wHour, l.wMinute);
    CHECK(what, l.wHour == lh && l.wMinute == lm);
    TzSpecificLocalTimeToSystemTime(&tz, &l, &back);
    CHECK("and back to UTC", minutes(&back) == minutes(&u));
}

int main(void)
{
    DYNAMIC_TIME_ZONE_INFORMATION d;
    SYSTEMTIME u, l;
    GetDynamicTimeZoneInformation(&d);
    GetSystemTime(&u);
    GetLocalTime(&l);
    LONGLONG diff = minutes(&l) - minutes(&u);
    time_t now = time(NULL);
    struct tm lt = *localtime(&now), gt = *gmtime(&now);
    long crt = (long)((_mkgmtime64(&lt) - _mkgmtime64(&gt)) / 60);
    printf("[TZ] zone %ls bias %ld local-utc %lld crt %ld\n", d.TimeZoneKeyName, d.Bias, diff, crt);
    CHECK("GetLocalTime and localtime agree", diff == crt);

    at(L"W. Europe Standard Time", 2026, 1, 15, 12, 0, 13, 0);
    at(L"W. Europe Standard Time", 2026, 7, 1, 12, 0, 14, 0);
    at(L"W. Europe Standard Time", 2026, 3, 29, 0, 59, 1, 59);       /* last Sunday of March, 01:00 UTC */
    at(L"W. Europe Standard Time", 2026, 3, 29, 1, 0, 3, 0);
    at(L"AUS Eastern Standard Time", 2026, 1, 15, 12, 0, 23, 0);     /* southern summer */
    at(L"AUS Eastern Standard Time", 2026, 7, 1, 12, 0, 22, 0);
    at(L"Pacific Standard Time", 2026, 3, 8, 9, 59, 1, 59);          /* second Sunday of March */
    at(L"Pacific Standard Time", 2026, 3, 8, 10, 0, 3, 0);
    at(L"Tokyo Standard Time", 2026, 7, 1, 12, 0, 21, 0);
    at(L"India Standard Time", 2026, 7, 1, 12, 0, 17, 30);

    printf("tztest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
