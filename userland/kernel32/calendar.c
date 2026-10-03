/*
 * calendar.c — kernel32's calendars: GetCalendarInfo, SetCalendarInfo,
 * EnumCalendarInfo, EnumDateFormats and EnumTimeFormats, and the
 * calendar-aware half of GetDateFormat (nlsformat.c).
 *
 * The calendars are Windows' CAL_* ids.  Which ones a locale has, and
 * which comes first (its LOCALE_ICALENDARTYPE), is ICU's list of the
 * calendars commonly used where the locale is (ja-JP: Gregorian and the
 * Japanese era calendar; th-TH: Thai Buddhist first; ar-SA: Um Al Qura
 * first; fa-IR: Persian first), mapped to CAL_* ids the way .NET's
 * pal_calendarData.c (MIT) maps them.  Dates are converted, and month and
 * era names and date patterns taken, from ICU's calendars (icu.dll);
 * Gregorian, its English, French and Arabic variants and the Korean
 * Tangun era (the Gregorian year plus 2333) are worked out here.
 */
#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include <winreg.h>
#include "k32.h"
#include "nls.h"

void *memcpy(void *d, const void *s, size_t n);

#define K32 __declspec(dllexport)

int _fltused = 0x9875;                   /* (ICU's dates are doubles) */

typedef DWORD CALID_;
typedef DWORD CALTYPE_;

#define CAL_NOUSEROVERRIDE_       0x80000000u
#define CAL_USE_CP_ACP_           0x40000000u
#define CAL_RETURN_NUMBER_        0x20000000u
#define CAL_RETURN_GENITIVE_NAMES_ 0x10000000u
#define LOCALE_NOUSEROVERRIDE_    0x80000000u
#define LOCALE_RETURN_GENITIVE_NAMES_ 0x10000000u
#define ENUM_ALL_CALENDARS_       0xFFFFFFFFu
#define ERROR_INVALID_FLAGS_      1004

/* ICU's UDateFormatSymbolType and styles */
enum { UDAT_ERAS = 0, UDAT_MONTHS = 1, UDAT_SHORT_MONTHS = 2, UDAT_WEEKDAYS = 3, UDAT_SHORT_WEEKDAYS = 4,
       UDAT_ERA_NAMES = 7, UDAT_STANDALONE_MONTHS = 10, UDAT_STANDALONE_SHORT_MONTHS = 11,
       UDAT_SHORTER_WEEKDAYS = 20 };
enum { UDAT_FULL = 0, UDAT_SHORT = 3 };

#define CAP 256

static const struct Cal {
    DWORD id;
    const char *icu;              /* ICU's calendar keyword */
    const char *names;            /* the ICU locale its names come from (NULL: the locale's own) */
    WORD twodigit;                /* CAL_ITWODIGITYEARMAX */
    char family;                  /* calendars with the same month names */
    const char *english;          /* CAL_SCALNAME in English */
} g_cals[] = {
    { 1,  "gregorian",        NULL,    2049, 'G', "Gregorian Calendar" },
    { 2,  "gregorian",        "en_US", 2049, 'E', "Gregorian Calendar (English)" },
    { 3,  "japanese",         NULL,    99,   'G', "Japanese Emperor Era" },
    { 4,  "roc",              NULL,    99,   'G', "Taiwan Calendar" },
    { 5,  "gregorian",        NULL,    4362, 'G', "Korean Tangun Era" },
    { 6,  "islamic-tbla",     NULL,    1451, 'I', "Hijri Calendar" },
    { 7,  "buddhist",         NULL,    2592, 'G', "Thai Buddhist Calendar" },
    { 8,  "hebrew",           NULL,    5790, 'H', "Hebrew Calendar" },
    { 9,  "gregorian",        "fr_FR", 2049, 'F', "Gregorian Calendar (Middle East French)" },
    { 10, "gregorian",        "ar_SA", 2049, 'A', "Gregorian Calendar (Arabic)" },
    { 22, "persian",          NULL,    1429, 'P', "Persian Calendar" },
    { 23, "islamic-umalqura", NULL,    1451, 'I', "Um Al Qura Calendar" },
};
#define NCALS ((int)(sizeof(g_cals) / sizeof(g_cals[0])))

static const struct Cal *cal_of(DWORD id)
{
    for (int i = 0; i < NCALS; i++) if (g_cals[i].id == id) return &g_cals[i];
    return 0;
}

/* ICU's keyword values -> CAL_* (.NET's GetCalendarId; every Islamic
 * variant but Um Al Qura is Windows' one Hijri calendar) */
static DWORD cal_from_icu(const char *k)
{
    static const struct { const char *k; DWORD id; } map[] = {
        { "gregorian", 1 }, { "japanese", 3 }, { "roc", 4 }, { "dangi", 5 }, { "islamic", 6 }, { "islamic-civil", 6 },
        { "islamic-tbla", 6 }, { "islamic-rgsa", 6 }, { "buddhist", 7 }, { "hebrew", 8 }, { "persian", 22 },
        { "islamic-umalqura", 23 },
    };
    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        const char *a = map[i].k, *b = k;
        while (*a && *a == *b) a++, b++;
        if (!*a && !*b) return map[i].id;
    }
    return 0;
}

int nls_calendars(int idx, DWORD *out, int max)
{
    int n = 0;
    if (!nls_is_english(idx) && idx >= 0) {
        char k[8][24];
        int m = nls_icu_calendars(idx, k, 8);
        for (int i = 0; i < m && n < max; i++) {
            DWORD id = cal_from_icu(k[i]);
            BOOL dup = !id;
            for (int j = 0; j < n; j++) if (out[j] == id) dup = TRUE;
            if (!dup) out[n++] = id;
        }
    }
    if (!n) out[n++] = 1;
    return n;
}

static DWORD default_cal(int idx)
{
    DWORD c[8];
    nls_calendars(idx, c, 8);
    return c[0];
}

static BOOL has_cal(int idx, DWORD id)
{
    DWORD c[8];
    int n = nls_calendars(idx, c, 8);
    for (int i = 0; i < n; i++) if (c[i] == id) return TRUE;
    return FALSE;
}

/* the ICU locale @cal's names and patterns come from, for locale @idx */
static BOOL names_loc(int idx, const struct Cal *c, char *loc)
{
    if (!c->names) return nls_icu_loc(nls_is_english(idx) ? -1 : idx, c->icu, loc);
    if (!nls_icu_loc(-1, c->icu, loc)) return FALSE;
    for (int i = 0; c->names[i]; i++) loc[i] = c->names[i];   /* "en_US" -> "fr_FR"... (same length) */
    return TRUE;
}

/* the locale's own LOCALE_* names serve @c (same months as its default calendar) */
static BOOL own_names(int idx, const struct Cal *c)
{
    const struct Cal *d = cal_of(default_cal(idx));
    return !c->names && d && d->family == c->family;
}

/* -----------------------------------------------------------------------
 * Dates
 * ----------------------------------------------------------------------- */
/* days since 1 January 1970 (Howard Hinnant's days_from_civil, public domain) */
static int days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static BOOL hebrew_leap(int y) { return (7 * y + 1) % 19 < 7; }

BOOL nls_cal_date(DWORD id, const SYSTEMTIME *st, CalDate *cd)
{
    const struct Cal *c = cal_of(id);
    cd->leap = FALSE;
    if (!c) return FALSE;
    if (!c->icu[0] || (c->icu[0] == 'g' && c->icu[1] == 'r')) {          /* the Gregorian family */
        cd->era = 1;
        cd->year = st->wYear + (id == 5 ? 2333 : 0);
        cd->month = st->wMonth;
        cd->day = st->wDay;
        return TRUE;
    }
    char loc[64];
    int f[4];
    if (!nls_icu_loc(-1, c->icu, loc)) return FALSE;
    double ms = (double)days_from_civil(st->wYear, st->wMonth, st->wDay) * 86400000.0 + 43200000.0;
    if (!nls_icu_date(loc, ms, f)) return FALSE;
    cd->era = f[0];
    cd->year = f[1];
    cd->day = f[3];
    cd->month = f[2] + 1;
    if (id == 8) {                                       /* ICU: Adar I is month 5, skipped in common years */
        cd->leap = hebrew_leap(f[1]);
        if (!cd->leap && f[2] >= 6) cd->month = f[2];
    }
    return TRUE;
}

/* Windows' Hebrew month (1-13) -> ICU's month index */
static int hebrew_icu_month(int m, BOOL leap) { return leap || m <= 5 ? m - 1 : m; }

/* Hebrew numerals (days and years: 21 -> כ"א, 5787 -> תשפ"ז), with ASCII
 * geresh and gershayim as Windows and .NET write them */
int nls_hebrew_number(int v, WCHAR *out, int cap)
{
    WCHAR t[16];
    int k = 0;
    v %= 1000;
    if (v <= 0) v += 1000;
    while (v >= 400 && k < 8) { t[k++] = 0x05EA; v -= 400; }          /* ת */
    if (v >= 100) { t[k++] = (WCHAR)(0x05E7 + v / 100 - 1); v %= 100; }   /* ק ר ש */
    if (v == 15 || v == 16) { t[k++] = 0x05D8; t[k++] = (WCHAR)(0x05D5 + v - 15); v = 0; }   /* ט"ו ט"ז */
    static const WCHAR tens[9] = { 0x05D9, 0x05DB, 0x05DC, 0x05DE, 0x05E0, 0x05E1, 0x05E2, 0x05E4, 0x05E6 };
    if (v >= 10) { t[k++] = tens[v / 10 - 1]; v %= 10; }
    if (v) t[k++] = (WCHAR)(0x05D0 + v - 1);
    int n = 0;
    for (int i = 0; i < k && n < cap - 2; i++) {
        if (i == k - 1 && k > 1) out[n++] = '"';
        out[n++] = t[i];
    }
    if (k == 1 && n < cap - 1) out[n++] = '\'';
    out[n] = 0;
    return n;
}

/* -----------------------------------------------------------------------
 * Names and patterns
 * ----------------------------------------------------------------------- */
static int put(const WCHAR *s, int n, WCHAR *out, int cap)
{
    if (n < 0 || n >= cap) return -1;
    memcpy(out, s, 2 * (SIZE_T)n);
    out[n] = 0;
    return n;
}
static int put_a(const char *s, WCHAR *out, int cap)
{
    int n = u2w(s, -1, out, cap - 1);
    if (n < 0) return -1;
    out[n] = 0;
    return n;
}
static int put_dec(int v, WCHAR *out, int cap)
{
    char s[16];
    int n = 0, k = 0;
    unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    do s[n++] = (char)('0' + u % 10); while (u /= 10);
    if (v < 0) s[n++] = '-';
    char r[16];
    while (n) r[k++] = s[--n];
    r[k] = 0;
    return put_a(r, out, cap);
}

int nls_cal_month(int idx, DWORD id, int m, BOOL leap, BOOL abbr, BOOL genitive, WCHAR *out, int cap)
{
    const struct Cal *c = cal_of(id);
    char loc[64];
    if (!c || m < 1 || m > 13) return -1;
    if (m == 13 && id != 8) { out[0] = 0; return 0; }
    if (id == 2) idx = nls_en_us();
    if (id == 2 || own_names(idx, c) || !names_loc(idx, c, loc)) {
        if (m == 13) return nls_info(idx, abbr ? 0x100F : 0x100E, out, cap);   /* LOCALE_SMONTHNAME13 */
        return nls_info(idx, ((abbr ? 0x44 : 0x38) + (DWORD)m - 1) | (genitive ? LOCALE_RETURN_GENITIVE_NAMES_ : 0),
                        out, cap);
    }
    int type = abbr ? (genitive ? UDAT_SHORT_MONTHS : UDAT_STANDALONE_SHORT_MONTHS)
                    : (genitive ? UDAT_MONTHS : UDAT_STANDALONE_MONTHS);
    return nls_icu_symbol(loc, type, id == 8 ? hebrew_icu_month(m, leap) : m - 1, out, cap);
}

/* @monday0: 0 Monday ... 6 Sunday; @abbr 0 full, 1 abbreviated, 2 shortest */
int nls_cal_day(int idx, DWORD id, int monday0, int abbr, WCHAR *out, int cap)
{
    const struct Cal *c = cal_of(id);
    char loc[64];
    if (!c) return -1;
    if (id == 2) idx = nls_en_us();
    if (!c->names || id == 2 || !names_loc(idx, c, loc)) {
        DWORD t = (abbr == 2 ? 0x60 : abbr ? 0x31 : 0x2A) + (DWORD)monday0;   /* LOCALE_SSHORTESTDAYNAME1... */
        return nls_info(idx, t, out, cap);
    }
    int type = abbr == 2 ? UDAT_SHORTER_WEEKDAYS : abbr ? UDAT_SHORT_WEEKDAYS : UDAT_WEEKDAYS;
    return nls_icu_symbol(loc, type, monday0 == 6 ? 1 : monday0 + 2, out, cap);
}

int nls_cal_era(int idx, DWORD id, const CalDate *cd, WCHAR *out, int cap)
{
    const struct Cal *c = cal_of(id);
    char loc[64];
    if (!c) return -1;
    if (id == 1) return nls_era(idx, out, cap);
    if (id == 2) return put_a("A.D.", out, cap);
    if (id == 5) return put(L"\xb2e8\xae30", 2, out, cap);      /* 단기 */
    if (!names_loc(idx, c, loc)) return -1;
    return nls_icu_symbol(loc, UDAT_ERAS, cd->era, out, cap);
}

static BOOL has_text(const WCHAR *p, const char *s)
{
    for (int i = 0; p[i]; i++) {
        int k = 0;
        while (s[k] && p[i + k] == (WCHAR)s[k]) k++;
        if (!s[k]) return TRUE;
    }
    return FALSE;
}

int nls_cal_pattern(int idx, DWORD id, int kind, DWORD uo, WCHAR *out, int cap)
{
    static const DWORD lt[4] = { 0x1F, 0x20, 0x1006, 0x78 };   /* LOCALE_SSHORTDATE, SLONGDATE, SYEARMONTH, SMONTHDAY */
    const struct Cal *c = cal_of(id);
    char loc[64];
    if (!c) return -1;
    if (id == 2) {
        static const char *const en[4] = { "M/d/yyyy", "dddd, MMMM d, yyyy", "MMMM yyyy", "MMMM d" };
        return put_a(en[kind], out, cap);
    }
    if (id == default_cal(idx) || !names_loc(idx, c, loc))
        return nls_info(idx, lt[kind] | uo, out, cap);
    int n = kind == CALPAT_SHORT ? nls_icu_pattern(loc, UDAT_SHORT, out, cap - 4)
          : kind == CALPAT_LONG ? nls_icu_pattern(loc, UDAT_FULL, out, cap - 4)
          : nls_icu_skeleton(loc, kind == CALPAT_YEARMONTH ? L"yMMMM" : L"MMMMd", out, cap - 4);
    if (n < 0) return nls_info(idx, lt[kind] | uo, out, cap);
    out[n] = 0;
    /* no weekday glued to the day ("...d日dddd"): the long date then, as locale.c does */
    if (kind == CALPAT_LONG && n > 4 && out[n - 1] == 'd' && out[n - 2] == 'd' && out[n - 3] == 'd' && out[n - 4] == 'd' &&
        out[n - 5] != ' ' && out[n - 5] != ',' && out[n - 5] != '\'') {
        int m = nls_icu_pattern(loc, 1 /* UDAT_LONG */, out, cap - 4);
        if (m > 0) { n = m; out[n] = 0; }
    }
    if (kind == CALPAT_SHORT && c->family != 'H' && id != 3 && id != 4 && !has_text(out, "yyy") && has_text(out, "yy")) {
        int y = 0;                                                 /* yy -> yyyy, as Windows */
        while (!(out[y] == 'y' && out[y + 1] == 'y')) y++;
        for (int j = n; j >= y; j--) out[j + 2] = out[j];
        n += 2;
    }
    if (id == 5 && kind != CALPAT_MONTHDAY && !has_text(out, "g")) {   /* the Tangun era's name first */
        for (int j = n; j >= 0; j--) out[j + 3] = out[j];
        out[0] = 'g'; out[1] = 'g'; out[2] = ' ';
        n += 3;
    }
    return n;
}

/* -----------------------------------------------------------------------
 * GetCalendarInfo
 * ----------------------------------------------------------------------- */
/* the Japanese eras, newest first: ICU's era numbers and their first years */
static const struct { int era; int year; } g_japan[] = { { 236, 2019 }, { 235, 1989 }, { 234, 1926 }, { 233, 1912 }, { 232, 1868 } };
#define NJAPAN 5

static int two_digit_max(const struct Cal *c, DWORD flags)
{
    if (!(flags & CAL_NOUSEROVERRIDE_)) {
        HKEY k;
        if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\International\\Calendars\\TwoDigitYearMax", 0, KEY_READ, &k)) {
            WCHAR name[8], v[16];
            put_dec((int)c->id, name, 8);
            DWORD type, n = sizeof(v) - sizeof(WCHAR);
            LONG e = RegQueryValueExW(k, name, NULL, &type, (BYTE *)v, &n);
            RegCloseKey(k);
            if (!e && type == REG_SZ) {
                v[n / 2] = 0;
                int y = 0;
                for (const WCHAR *p = v; *p >= '0' && *p <= '9'; p++) y = y * 10 + (*p - '0');
                if (y >= 99) return y;
            }
        }
    }
    return c->twodigit;
}

/* the current era of @c (today's) */
static void today(DWORD id, CalDate *cd)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (!nls_cal_date(id, &st, cd)) { cd->era = 1; cd->year = st.wYear; cd->month = st.wMonth; cd->day = st.wDay; }
}

/* CAL_* @t (the low 16 bits) of calendar @c in locale @idx; @era: which
 * era for the era types (-1: the current one) */
static int cal_value(int idx, const struct Cal *c, DWORD t, DWORD flags, int era, WCHAR *out, int cap)
{
    DWORD uo = flags & CAL_NOUSEROVERRIDE_ ? LOCALE_NOUSEROVERRIDE_ : 0;
    BOOL gen = (flags & CAL_RETURN_GENITIVE_NAMES_) != 0;
    char loc[64];
    CalDate cd;
    switch (t) {
    case 0x01: return put_dec((int)c->id, out, cap);                          /* CAL_ICALINTVALUE */
    case 0x02:                                                                 /* CAL_SCALNAME */
        if (!nls_is_english(idx) && !c->names && c->id != 5 && names_loc(idx, c, loc)) {
            int n = nls_icu_calname(loc, out, cap);
            if (n > 0) return n;
        }
        return put_a(c->english, out, cap);
    case 0x03:                                                                 /* CAL_IYEAROFFSETRANGE */
        if (c->id == 3) return put_dec(g_japan[era < 0 ? 0 : era].year, out, cap);
        return put_dec(c->id == 4 ? 1912 : c->id == 5 ? -2333 : c->id == 7 ? -543 : 0, out, cap);
    case 0x04: case 0x39: case 0x3B: case 0x3C: {                              /* the era's names */
        if (c->id == 3 && era >= 0) cd.era = g_japan[era].era;
        else today(c->id, &cd);
        if (t == 0x3B || t == 0x3C) {                                          /* CAL_SENGLISHERANAME, ABBREV */
            if (c->family == 'G' && c->id != 3 && c->id != 4 && c->id != 7)
                return put_a(c->id == 5 ? "Tangun Era" : "A.D.", out, cap);
            if (!nls_icu_loc(-1, c->icu, loc)) return -1;
            return nls_icu_symbol(loc, t == 0x3B ? UDAT_ERA_NAMES : UDAT_ERAS, cd.era, out, cap);
        }
        int n = nls_cal_era(idx, c->id, &cd, out, cap);
        if (n > 1 && t == 0x39 && c->id == 3) { out[1] = 0; n = 1; }        /* CAL_SABBREVERASTRING: 令 */
        return n;
    }
    case 0x05: return nls_cal_pattern(idx, c->id, CALPAT_SHORT, uo, out, cap);
    case 0x06: return nls_cal_pattern(idx, c->id, CALPAT_LONG, uo, out, cap);
    case 0x2F: return nls_cal_pattern(idx, c->id, CALPAT_YEARMONTH, uo, out, cap);
    case 0x38: return nls_cal_pattern(idx, c->id, CALPAT_MONTHDAY, uo, out, cap);
    case 0x30: return put_dec(two_digit_max(c, flags), out, cap);              /* CAL_ITWODIGITYEARMAX */
    case 0x3D: return c->id == 3 ? put(L"\x5143", 1, out, cap) : put(L"", 0, out, cap);   /* 元 */
    }
    if (t >= 0x07 && t <= 0x0D) return nls_cal_day(idx, c->id, (int)(t - 0x07), 0, out, cap);
    if (t >= 0x0E && t <= 0x14) return nls_cal_day(idx, c->id, (int)(t - 0x0E), 1, out, cap);
    if (t >= 0x31 && t <= 0x37) return nls_cal_day(idx, c->id, (int)(t - 0x31), 2, out, cap);
    if (t >= 0x15 && t <= 0x21) return nls_cal_month(idx, c->id, (int)(t - 0x14), TRUE, FALSE, gen, out, cap);
    if (t >= 0x22 && t <= 0x2E) return nls_cal_month(idx, c->id, (int)(t - 0x21), TRUE, TRUE, gen, out, cap);
    return -2;
}

static BOOL numeric(DWORD t) { return t == 0x01 || t == 0x03 || t == 0x30; }

static int get_cal_info(int idx, CALID_ id, CALTYPE_ type, WCHAR *w, int *k, DWORD *num)
{
    const struct Cal *c = cal_of(id);
    DWORD t = type & 0xFFFF;
    if (idx == LOC_NONE || !c) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (type & ~(0xFFFFu | CAL_NOUSEROVERRIDE_ | CAL_USE_CP_ACP_ | CAL_RETURN_NUMBER_ | CAL_RETURN_GENITIVE_NAMES_) ||
        ((type & CAL_RETURN_NUMBER_) && !numeric(t))) {
        SetLastError(ERROR_INVALID_FLAGS_);
        return 0;
    }
    int n = cal_value(idx, c, t, type, -1, w, CAP);
    if (n == -2) { SetLastError(ERROR_INVALID_FLAGS_); return 0; }
    if (n < 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    w[n] = 0;
    *k = n;
    if (num) {
        int v = 0, neg = w[0] == '-';
        for (const WCHAR *p = w + neg; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        *num = (DWORD)(neg ? -v : v);
    }
    return 1;
}

static int cal_info_w(int idx, CALID_ id, CALTYPE_ type, LPWSTR out, int cap, LPDWORD value)
{
    WCHAR w[CAP];
    DWORD num;
    int k;
    if (cap < 0 || (cap && !out)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (!get_cal_info(idx, id, type, w, &k, &num)) return 0;
    if (type & CAL_RETURN_NUMBER_) {
        if (out || cap || !value) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
        *value = num;
        return sizeof(DWORD) / sizeof(WCHAR);
    }
    if (!cap) return k + 1;
    if (cap <= k) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, w, 2 * (SIZE_T)(k + 1));
    return k + 1;
}

K32 int WINAPI GetCalendarInfoEx(LPCWSTR loc, CALID_ id, LPCWSTR reserved, CALTYPE_ type, LPWSTR out, int cap, LPDWORD value)
{
    (void)reserved;
    return cal_info_w(nls_name(loc), id, type, out, cap, value);
}

K32 int WINAPI GetCalendarInfoW(LCID lcid, CALID_ id, CALTYPE_ type, LPWSTR out, int cap, LPDWORD value)
{
    return cal_info_w(nls_lcid(lcid), id, type, out, cap, value);
}

K32 int WINAPI GetCalendarInfoA(LCID lcid, CALID_ id, CALTYPE_ type, LPSTR out, int cap, LPDWORD value)
{
    WCHAR w[CAP];
    DWORD num;
    int k;
    if (cap < 0 || (cap && !out)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (!get_cal_info(nls_lcid(lcid), id, type, w, &k, &num)) return 0;
    if (type & CAL_RETURN_NUMBER_) {
        if (out || cap || !value) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
        *value = num;
        return sizeof(DWORD);
    }
    char a[CAP * 3];
    int n = w2u(w, k, a, sizeof(a) - 1);
    if (n < 0) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (!cap) return n + 1;
    if (cap <= n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, a, (SIZE_T)n);
    out[n] = 0;
    return n + 1;
}

/* SetCalendarInfo: only CAL_ITWODIGITYEARMAX can be set, as on Windows */
static BOOL set_cal_info(int idx, CALID_ id, CALTYPE_ type, const WCHAR *v)
{
    const struct Cal *c = cal_of(id);
    if (idx == LOC_NONE || !c || !v) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if ((type & ~CAL_USE_CP_ACP_) != 0x30) { SetLastError(ERROR_INVALID_FLAGS_); return FALSE; }
    int y = 0, n = 0;
    for (; v[n]; n++) {
        if (v[n] < '0' || v[n] > '9' || n > 4) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        y = y * 10 + (v[n] - '0');
    }
    if (y < 99 || y > 9999) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\International\\Calendars\\TwoDigitYearMax", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &k, NULL)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    WCHAR name[8];
    put_dec((int)id, name, 8);
    LONG e = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)(n + 1) * 2);
    RegCloseKey(k);
    if (e) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}

K32 BOOL WINAPI SetCalendarInfoW(LCID lcid, CALID_ id, CALTYPE_ type, LPCWSTR v)
{
    return set_cal_info(nls_lcid(lcid), id, type, v);
}

K32 BOOL WINAPI SetCalendarInfoA(LCID lcid, CALID_ id, CALTYPE_ type, LPCSTR v)
{
    WCHAR w[16];
    int n = v ? u2w(v, -1, w, 15) : -1;
    if (n < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    w[n] = 0;
    return set_cal_info(nls_lcid(lcid), id, type, w);
}

/* -----------------------------------------------------------------------
 * The enumerations: one callback shape for all their forms
 * ----------------------------------------------------------------------- */
enum { CB_W, CB_A, CB_EXW, CB_EXA, CB_EXEX_CAL, CB_EXEX_DATE, CB_EXEX_TIME };
typedef struct { int kind; void *fn; LPARAM p; } Callback;

static BOOL call(const Callback *cb, const WCHAR *w, DWORD cal)
{
    char a[CAP * 3];
    if (cb->kind == CB_A || cb->kind == CB_EXA) {
        int n = w2u(w, -1, a, sizeof(a));
        if (n < 0) return TRUE;
    }
    switch (cb->kind) {
    case CB_W:  return ((BOOL (WINAPI *)(LPWSTR))cb->fn)((LPWSTR)w);
    case CB_A:  return ((BOOL (WINAPI *)(LPSTR))cb->fn)(a);
    case CB_EXW: return ((BOOL (WINAPI *)(LPWSTR, DWORD))cb->fn)((LPWSTR)w, cal);
    case CB_EXA: return ((BOOL (WINAPI *)(LPSTR, DWORD))cb->fn)(a, cal);
    case CB_EXEX_CAL: return ((BOOL (WINAPI *)(LPWSTR, DWORD, LPWSTR, LPARAM))cb->fn)((LPWSTR)w, cal, NULL, cb->p);
    case CB_EXEX_DATE: return ((BOOL (WINAPI *)(LPWSTR, DWORD, LPARAM))cb->fn)((LPWSTR)w, cal, cb->p);
    case CB_EXEX_TIME: return ((BOOL (WINAPI *)(LPWSTR, LPARAM))cb->fn)((LPWSTR)w, cb->p);
    }
    return FALSE;
}

static BOOL enum_cal_info(int idx, CALID_ id, CALTYPE_ type, const Callback *cb)
{
    DWORD t = type & 0xFFFF, cals[8];
    int n;
    if (!cb->fn || idx == LOC_NONE) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (type & CAL_RETURN_NUMBER_) { SetLastError(ERROR_INVALID_FLAGS_); return FALSE; }
    if (id == ENUM_ALL_CALENDARS_) n = nls_calendars(idx, cals, 8);
    else if (cal_of(id)) { cals[0] = id; n = 1; }
    else { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    for (int i = 0; i < n; i++) {
        const struct Cal *c = cal_of(cals[i]);
        BOOL eras = t == 0x03 || t == 0x04 || t == 0x39 || t == 0x3B || t == 0x3C;
        int count = eras && c->id == 3 ? NJAPAN : 1;
        for (int e = 0; e < count; e++) {
            WCHAR w[CAP];
            int k = cal_value(idx, c, t, type, eras && c->id == 3 ? e : -1, w, CAP);
            if (k == -2) { SetLastError(ERROR_INVALID_FLAGS_); return FALSE; }
            if (k < 0) continue;
            w[k] = 0;
            if (!call(cb, w, c->id)) return TRUE;
        }
    }
    return TRUE;
}

K32 BOOL WINAPI EnumCalendarInfoW(CALINFO_ENUMPROCW fn, LCID lcid, CALID_ id, CALTYPE_ type)
{
    Callback cb = { CB_W, (void *)fn, 0 };
    return enum_cal_info(nls_lcid(lcid), id, type, &cb);
}
K32 BOOL WINAPI EnumCalendarInfoA(void *fn, LCID lcid, CALID_ id, CALTYPE_ type)
{
    Callback cb = { CB_A, (void *)fn, 0 };
    return enum_cal_info(nls_lcid(lcid), id, type, &cb);
}
K32 BOOL WINAPI EnumCalendarInfoExW(CALINFO_ENUMPROCEXW fn, LCID lcid, CALID_ id, CALTYPE_ type)
{
    Callback cb = { CB_EXW, (void *)fn, 0 };
    return enum_cal_info(nls_lcid(lcid), id, type, &cb);
}
K32 BOOL WINAPI EnumCalendarInfoExA(void *fn, LCID lcid, CALID_ id, CALTYPE_ type)
{
    Callback cb = { CB_EXA, (void *)fn, 0 };
    return enum_cal_info(nls_lcid(lcid), id, type, &cb);
}
K32 BOOL WINAPI EnumCalendarInfoExEx(CALINFO_ENUMPROCEXEX fn, LPCWSTR loc, CALID_ id, LPCWSTR reserved, CALTYPE_ type, LPARAM p)
{
    (void)reserved;
    Callback cb = { CB_EXEX_CAL, (void *)fn, p };
    return enum_cal_info(nls_name(loc), id, type, &cb);
}

/* the user's format (an override) first, then the locale's own, once each */
static BOOL enum_two(int idx, DWORD lctype, DWORD cal, BOOL alt, const Callback *cb)
{
    WCHAR a[CAP], b[CAP];
    int kind = lctype == 0x1F ? CALPAT_SHORT : lctype == 0x20 ? CALPAT_LONG : lctype == 0x1006 ? CALPAT_YEARMONTH :
               lctype == 0x78 ? CALPAT_MONTHDAY : -1;
    int na = kind < 0 ? nls_info(idx, lctype, a, CAP) : alt ? -1 : nls_cal_pattern(idx, cal, kind, 0, a, CAP);
    int nb = kind < 0 ? nls_info(idx, lctype | LOCALE_NOUSEROVERRIDE_, b, CAP)
                      : nls_cal_pattern(idx, cal, kind, LOCALE_NOUSEROVERRIDE_, b, CAP);
    BOOL go = TRUE;
    if (na >= 0) go = call(cb, a, cal);
    if (go && nb >= 0) {
        BOOL same = na == nb;
        for (int i = 0; same && i < na; i++) same = a[i] == b[i];
        if (!same) call(cb, b, cal);
    }
    return TRUE;
}

static BOOL enum_date_formats(int idx, DWORD flags, const Callback *cb)
{
    if (!cb->fn || idx == LOC_NONE) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD kind = flags & ~(4u /* DATE_USE_ALT_CALENDAR */ | 0x40000000u /* LOCALE_USE_CP_ACP */ | 0x30u /* LTR/RTL */);
    DWORD t = kind == 1 ? 0x1F : kind == 2 ? 0x20 : kind == 8 ? 0x1006 : kind == 0x80 ? 0x78 : 0;
    if (!t) { SetLastError(ERROR_INVALID_FLAGS_); return FALSE; }
    WCHAR w[CAP];
    DWORD cal = 0;
    BOOL alt = (flags & 4) != 0;
    if (alt && nls_info(idx, 0x100B | LOCALE_NOUSEROVERRIDE_, w, CAP) > 0)   /* LOCALE_IOPTIONALCALENDAR */
        for (const WCHAR *p = w; *p >= '0' && *p <= '9'; p++) cal = cal * 10 + (DWORD)(*p - '0');
    if (!cal_of(cal) && nls_info(idx, 0x1009, w, CAP) > 0)                     /* LOCALE_ICALENDARTYPE */
        for (const WCHAR *p = (cal = 0, w); *p >= '0' && *p <= '9'; p++) cal = cal * 10 + (DWORD)(*p - '0');
    if (!cal_of(cal)) cal = 1;
    return enum_two(idx, t, cal, alt, cb);
}

K32 BOOL WINAPI EnumDateFormatsW(DATEFMT_ENUMPROCW fn, LCID lcid, DWORD flags)
{
    Callback cb = { CB_W, (void *)fn, 0 };
    return enum_date_formats(nls_lcid(lcid), flags, &cb);
}
K32 BOOL WINAPI EnumDateFormatsA(void *fn, LCID lcid, DWORD flags)
{
    Callback cb = { CB_A, (void *)fn, 0 };
    return enum_date_formats(nls_lcid(lcid), flags, &cb);
}
K32 BOOL WINAPI EnumDateFormatsExW(DATEFMT_ENUMPROCEXW fn, LCID lcid, DWORD flags)
{
    Callback cb = { CB_EXW, (void *)fn, 0 };
    return enum_date_formats(nls_lcid(lcid), flags, &cb);
}
K32 BOOL WINAPI EnumDateFormatsExA(void *fn, LCID lcid, DWORD flags)
{
    Callback cb = { CB_EXA, (void *)fn, 0 };
    return enum_date_formats(nls_lcid(lcid), flags, &cb);
}
K32 BOOL WINAPI EnumDateFormatsExEx(DATEFMT_ENUMPROCEXEX fn, LPCWSTR loc, DWORD flags, LPARAM p)
{
    Callback cb = { CB_EXEX_DATE, (void *)fn, p };
    return enum_date_formats(nls_name(loc), flags, &cb);
}

static BOOL enum_time_formats(int idx, DWORD flags, const Callback *cb)
{
    if (!cb->fn || idx == LOC_NONE) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    flags &= ~0x40000000u;                                                   /* LOCALE_USE_CP_ACP */
    if (flags && flags != 2 /* TIME_NOSECONDS */) { SetLastError(ERROR_INVALID_FLAGS_); return FALSE; }
    return enum_two(idx, flags ? 0x79 /* LOCALE_SSHORTTIME */ : 0x1003 /* LOCALE_STIMEFORMAT */, 1, FALSE, cb);
}

K32 BOOL WINAPI EnumTimeFormatsW(TIMEFMT_ENUMPROCW fn, LCID lcid, DWORD flags)
{
    Callback cb = { CB_W, (void *)fn, 0 };
    return enum_time_formats(nls_lcid(lcid), flags, &cb);
}
K32 BOOL WINAPI EnumTimeFormatsA(void *fn, LCID lcid, DWORD flags)
{
    Callback cb = { CB_A, (void *)fn, 0 };
    return enum_time_formats(nls_lcid(lcid), flags, &cb);
}
K32 BOOL WINAPI EnumTimeFormatsEx(TIMEFMT_ENUMPROCEX fn, LPCWSTR loc, DWORD flags, LPARAM p)
{
    Callback cb = { CB_EXEX_TIME, (void *)fn, p };
    return enum_time_formats(nls_name(loc), flags, &cb);
}
