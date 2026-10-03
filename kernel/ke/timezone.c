/*
 * timezone.c — time zones and local time (see timezone.h)
 */

#include "timezone.h"
#include "printf.h"
#include "../lib/string.h"
#include "../um/um.h"

#define TZ_KEY "Machine\\SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation"

static const TzZone g_zones[] = {
#include "tzdata.inc"
};
#define N_ZONES ((int)(sizeof(g_zones) / sizeof(g_zones[0])))

int TzCount(void) { return N_ZONES; }
const TzZone *TzAt(int i) { return i >= 0 && i < N_ZONES ? &g_zones[i] : NULL; }

int TzFind(const char *key)
{
    for (int i = 0; i < N_ZONES; i++)
        if (!strcmp(g_zones[i].key, key)) return i;
    return -1;
}

static TzSystemTime rule_st(TzRule r)
{
    TzSystemTime st = { 0, r.month, r.dow, r.week, r.hour, r.minute, 0, 0 };
    return st;
}

void TzToTzi(const TzZone *z, TzTzi *out)
{
    memset(out, 0, sizeof(*out));
    out->bias     = z->bias;
    out->dst_bias = z->std_date.month ? z->dst_bias : 0;
    out->std_date = rule_st(z->std_date);
    out->dst_date = rule_st(z->dst_date);
}

int TzCurrent(void)
{
    char key[64];
    int i = um_registry_get_sz(TZ_KEY, "TimeZoneKeyName", key, sizeof(key)) ? TzFind(key) : -1;
    return i >= 0 ? i : TzFind("UTC");
}

void TzSet(int i)
{
    const TzZone *z = TzAt(i);
    if (!z) return;
    TzTzi tzi;
    TzToTzi(z, &tzi);
    um_registry_set_dword(TZ_KEY, "Bias", (UINT32)tzi.bias);
    um_registry_set_sz(TZ_KEY, "StandardName", z->std);
    um_registry_set_dword(TZ_KEY, "StandardBias", 0);
    um_registry_set_bin(TZ_KEY, "StandardStart", &tzi.std_date, sizeof(tzi.std_date));
    um_registry_set_sz(TZ_KEY, "DaylightName", z->dlt);
    um_registry_set_dword(TZ_KEY, "DaylightBias", (UINT32)tzi.dst_bias);
    um_registry_set_bin(TZ_KEY, "DaylightStart", &tzi.dst_date, sizeof(tzi.dst_date));
    um_registry_set_dword(TZ_KEY, "DynamicDaylightTimeDisabled", 0);
    um_registry_set_sz(TZ_KEY, "TimeZoneKeyName", z->key);
    um_registry_set_sz(UM_SETUP_KEY, "TimeZone", z->key);
    kprintf("[TZ] Time zone %s (bias %d)\n", z->key, (int)z->bias);
}

/* ---------------------------------------------------------------------------
 * Calendar arithmetic (days since 1970-01-01)
 * ------------------------------------------------------------------------- */
static INT64 days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    INT64 era = (y >= 0 ? y : y - 399) / 400;
    INT64 yoe = y - era * 400;
    INT64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    INT64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(INT64 z, int *y, int *m, int *d)
{
    z += 719468;
    INT64 era = (z >= 0 ? z : z - 146096) / 146097;
    INT64 doe = z - era * 146097;
    INT64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    INT64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    INT64 mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static int days_in_month(int y, int m)
{
    static const UINT8 n[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29 : n[m - 1];
}

/* The minute (since 1970, local time) a Windows rule names in @year */
static INT64 rule_minute(int year, const TzSystemTime *r)
{
    INT64 first = days_from_civil(year, r->month, 1);
    int first_dow = (int)((first % 7 + 11) % 7);                /* 1970-01-01 was a Thursday */
    int day = 1 + (r->dow - first_dow + 7) % 7 + (r->day - 1) * 7;
    while (day > days_in_month(year, r->month)) day -= 7;
    return (first + day - 1) * 1440 + r->hour * 60 + r->minute;
}

static INT64 to_minute(const RtcTime *t)
{
    return days_from_civil(t->year, t->month, t->day) * 1440 + t->hour * 60 + t->minute;
}

/* The bias at UTC time @utc under a zone's Windows values */
static INT32 bias_at(const TzTzi *z, const RtcTime *utc, bool *dst)
{
    if (dst) *dst = false;
    const TzSystemTime *sd = &z->std_date, *dd = &z->dst_date;
    if (!sd->month || !dd->month || sd->month > 12 || dd->month > 12 || sd->day < 1 || dd->day < 1)
        return z->bias + z->std_bias;
    /* (only rules for every year: wYear 0) */
    INT64 now = to_minute(utc);
    INT64 on  = rule_minute(utc->year, dd) + z->bias + z->std_bias;     /* in UTC */
    INT64 off = rule_minute(utc->year, sd) + z->bias + z->dst_bias;
    bool in = on < off ? (now >= on && now < off) : (now >= on || now < off);
    if (dst) *dst = in;
    return z->bias + (in ? z->dst_bias : z->std_bias);
}

INT32 TzBiasAt(const RtcTime *utc, bool *dst)
{
    TzTzi z;
    UINT32 v;
    memset(&z, 0, sizeof(z));
    if (um_registry_get_dword(TZ_KEY, "Bias", &v))         z.bias = (INT32)v;
    if (um_registry_get_dword(TZ_KEY, "StandardBias", &v)) z.std_bias = (INT32)v;
    if (um_registry_get_dword(TZ_KEY, "DaylightBias", &v)) z.dst_bias = (INT32)v;
    if (um_registry_get_bin(TZ_KEY, "StandardStart", &z.std_date, sizeof(z.std_date)) != (int)sizeof(z.std_date) ||
        um_registry_get_bin(TZ_KEY, "DaylightStart", &z.dst_date, sizeof(z.dst_date)) != (int)sizeof(z.dst_date))
        z.std_date.month = 0;
    return bias_at(&z, utc, dst);
}

static void shift(const RtcTime *utc, INT32 bias, RtcTime *out)
{
    INT64 m = to_minute(utc) - bias;
    int y, mo, d;
    civil_from_days(m >= 0 ? m / 1440 : (m - 1439) / 1440, &y, &mo, &d);
    INT64 mod = ((m % 1440) + 1440) % 1440;
    out->year   = (UINT16)y;
    out->month  = (UINT8)mo;
    out->day    = (UINT8)d;
    out->hour   = (UINT8)(mod / 60);
    out->minute = (UINT8)(mod % 60);
    out->second = utc->second;
}

void TzToLocal(const RtcTime *utc, RtcTime *out)
{
    RtcTime u = *utc;
    shift(&u, TzBiasAt(&u, NULL), out);
}

void TzLocalNow(RtcTime *out)
{
    RtcTime t;
    rtc_read(&t);
    TzToLocal(&t, out);
}

INT32 TzZoneLocalNow(int i, RtcTime *out, bool *dst)
{
    RtcTime t;
    TzTzi z;
    rtc_read(&t);
    TzToTzi(TzAt(i) ? TzAt(i) : TzAt(TzFind("UTC")), &z);
    INT32 b = bias_at(&z, &t, dst);
    shift(&t, b, out);
    return b;
}
