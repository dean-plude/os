/*
 * nlstest.exe — GetDateFormat, GetTimeFormat, GetNumberFormat and
 * GetCurrencyFormat in German, Japanese and English, and the user locale.
 *
 *   nlstest                   formatting (A, W, Ex; pictures, NUMBERFMT and
 *                             CURRENCYFMT, flags and errors); the expected
 *                             strings are what Windows 10/11 gives
 *   nlstest user              intl.exe de-DE: a new process formats the
 *                             German way for LOCALE_USER_DEFAULT; then en-US
 *   nlstest set NAME          intl.exe NAME (before a restart)
 *   nlstest after-restart NAME  the user locale is still NAME; back to en-US
 *   nlstest calendars         GetCalendarInfo, EnumCalendarInfo, the
 *                             locales' calendars (th-TH Buddhist, ar-SA
 *                             Um Al Qura, fa-IR Persian) and
 *                             DATE_USE_ALT_CALENDAR (ja-JP eras, zh-TW,
 *                             ko-KR, he-IL); EnumDateFormats/TimeFormats;
 *                             GetDurationFormat
 *   nlstest override          SetLocaleInfo: the user's overrides, here and
 *                             in a new process, then back to en-US's own
 *   (nlstest child NAME       the new process of "user")
 *
 * Built for x64 and x86.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

static void show(const WCHAR *w)
{
    char s[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, sizeof(s), NULL, NULL);
    printf("%s", s);
}

static void same(const WCHAR *got, int ret, const WCHAR *want, const char *what)
{
    int ok = ret == (int)wcslen(want) + 1 && !wcscmp(got, want);
    check(ok, what);
    if (!ok) { printf("  got \""); show(ret ? got : L""); printf("\" (%d), want \"", ret); show(want); printf("\"\n"); }
}

static void same_a(const char *got, int ret, const char *want, const char *what)
{
    int ok = ret == (int)strlen(want) + 1 && !strcmp(got, want);
    check(ok, what);
    if (!ok) printf("  got \"%s\" (%d), want \"%s\"\n", ret ? got : "", ret, want);
}

/* Friday 2 October 2026, 14:05:09 (wDayOfWeek wrong on purpose: Windows works it out) */
static const SYSTEMTIME g_st = { 2026, 10, 0, 2, 14, 5, 9, 0 };
static const SYSTEMTIME g_morning = { 2026, 10, 5, 2, 9, 5, 9, 0 };

static void date(LPCWSTR loc, DWORD flags, LPCWSTR pic, const WCHAR *want, const char *what)
{
    WCHAR w[128];
    same(w, GetDateFormatEx(loc, flags, &g_st, pic, w, 128, NULL), want, what);
}
static void time_(LPCWSTR loc, DWORD flags, const SYSTEMTIME *st, LPCWSTR pic, const WCHAR *want, const char *what)
{
    WCHAR w[128];
    same(w, GetTimeFormatEx(loc, flags, st, pic, w, 128), want, what);
}
static void number(LPCWSTR loc, LPCWSTR v, const NUMBERFMTW *f, const WCHAR *want, const char *what)
{
    WCHAR w[128];
    same(w, GetNumberFormatEx(loc, 0, v, f, w, 128), want, what);
}
static void money(LPCWSTR loc, LPCWSTR v, const CURRENCYFMTW *f, const WCHAR *want, const char *what)
{
    WCHAR w[128];
    same(w, GetCurrencyFormatEx(loc, 0, v, f, w, 128), want, what);
}

static void german(void)
{
    date(L"de-DE", DATE_SHORTDATE, NULL, L"02.10.2026", "de-DE short date");
    date(L"de-DE", DATE_LONGDATE, NULL, L"Freitag, 2. Oktober 2026", "de-DE long date");
    date(L"de-DE", DATE_YEARMONTH, NULL, L"Oktober 2026", "de-DE year and month");
    time_(L"de-DE", 0, &g_st, NULL, L"14:05:09", "de-DE time");
    time_(L"de-DE", TIME_NOSECONDS, &g_morning, NULL, L"09:05", "de-DE time without seconds");
    number(L"de-DE", L"1234567.891", NULL, L"1.234.567,89", "de-DE number");
    number(L"de-DE", L"-1234.5", NULL, L"-1.234,50", "de-DE negative number");
    money(L"de-DE", L"1234567.891", NULL, L"1.234.567,89 \x20ac", "de-DE currency");
    money(L"de-DE", L"-1234.5", NULL, L"-1.234,50 \x20ac", "de-DE negative currency");

    WCHAR w[128];
    same(w, GetDateFormatW(0x0407, DATE_LONGDATE, &g_st, NULL, w, 128), L"Freitag, 2. Oktober 2026", "GetDateFormatW(0x407)");
    same(w, GetCurrencyFormatW(0x0407, 0, L"0.5", NULL, w, 128), L"0,50 \x20ac", "GetCurrencyFormatW(0x407)");
    char a[128];
    same_a(a, GetDateFormatA(0x0407, DATE_LONGDATE, &g_st, NULL, a, 128), "Freitag, 2. Oktober 2026", "GetDateFormatA(0x407)");
    same_a(a, GetCurrencyFormatA(0x0407, 0, "1234567.891", NULL, a, 128), "1.234.567,89 \xe2\x82\xac", "GetCurrencyFormatA(0x407) (UTF-8)");
    same_a(a, GetTimeFormatA(0x0407, 0, &g_st, "HH 'Uhr' mm", a, 128), "14 Uhr 05", "GetTimeFormatA(0x407) with a picture");
}

static void japanese(void)
{
    date(L"ja-JP", DATE_SHORTDATE, NULL, L"2026/10/02", "ja-JP short date");
    date(L"ja-JP", DATE_LONGDATE, NULL, L"2026\x5e74" L"10\x6708" L"2\x65e5", "ja-JP long date");
    date(L"ja-JP", DATE_YEARMONTH, NULL, L"2026\x5e74" L"10\x6708", "ja-JP year and month");
    date(L"ja-JP", 0, L"dddd", L"\x91d1\x66dc\x65e5", "ja-JP day name");
    time_(L"ja-JP", 0, &g_st, NULL, L"14:05:09", "ja-JP time");
    time_(L"ja-JP", 0, &g_morning, NULL, L"9:05:09", "ja-JP morning time");
    number(L"ja-JP", L"1234567.891", NULL, L"1,234,567.89", "ja-JP number");
    money(L"ja-JP", L"1234567.891", NULL, L"\x00a5" L"1,234,568", "ja-JP currency");
    money(L"ja-JP", L"-1234.5", NULL, L"-\x00a5" L"1,235", "ja-JP negative currency");

    WCHAR w[128];
    same(w, GetLocaleInfoEx(L"ja-JP", LOCALE_SCURRENCY, w, 128), L"\x00a5", "ja-JP LOCALE_SCURRENCY is Windows' yen sign");
    same(w, GetLocaleInfoEx(L"ja-JP", LOCALE_SLONGDATE, w, 128), L"yyyy\x5e74M\x6708" L"d\x65e5", "ja-JP LOCALE_SLONGDATE");
    same(w, GetNumberFormatW(0x0411, 0, L"-0.001", NULL, w, 128), L"0.00", "a negative that rounds to zero has no sign");
}

static void english(void)
{
    date(L"en-US", DATE_SHORTDATE, NULL, L"10/2/2026", "en-US short date");
    date(L"en-US", DATE_LONGDATE, NULL, L"Friday, October 2, 2026", "en-US long date");
    date(L"en-US", DATE_MONTHDAY, NULL, L"October 2", "en-US month and day");
    date(L"en-US", 0, L"yyyy'-'MM'-'dd ''yy'' gg", L"2026-10-02 '26' A.D.", "quoted text, '' and the era");
    time_(L"en-US", 0, &g_st, NULL, L"2:05:09 PM", "en-US time");
    time_(L"en-US", TIME_NOSECONDS, &g_st, NULL, L"2:05 PM", "TIME_NOSECONDS");
    time_(L"en-US", TIME_NOMINUTESORSECONDS, &g_st, NULL, L"2 PM", "TIME_NOMINUTESORSECONDS");
    time_(L"en-US", TIME_NOTIMEMARKER, &g_morning, NULL, L"9:05:09", "TIME_NOTIMEMARKER");
    time_(L"en-US", 0, &g_morning, L"hh:mm t", L"09:05 A", "hh and t");
    number(L"en-US", L"1234567.891", NULL, L"1,234,567.89", "en-US number");
    money(L"en-US", L"1234567.891", NULL, L"$1,234,567.89", "en-US currency");
    money(L"en-US", L"-1234.5", NULL, L"-$1,234.50", "en-US negative currency");
    number(L"", L"1234.5", NULL, L"1,234.50", "the invariant locale");
    date(L"ru-RU", 0, L"d MMMM", L"2 \x043e\x043a\x0442\x044f\x0431\x0440\x044f", "the genitive month beside a day (ru-RU)");

    NUMBERFMTW nf = { 3, 0, 32, L".", L",", 0 };
    number(L"en-US", L"-1234567.8915", &nf, L"(12,34,567.892)", "NUMBERFMT: Indian groups, (n), rounding");
    number(L"en-US", L"0.5", &nf, L".500", "NUMBERFMT: no leading zero");
    nf.Grouping = 0; nf.NegativeOrder = 4;
    number(L"en-US", L"-1234", &nf, L"1234.000 -", "NUMBERFMT: no groups, n -");
    CURRENCYFMTW cf = { 2, 1, 3, L",", L".", 15, 3, L"CHF" };
    money(L"en-US", L"-1234.5", &cf, L"(1.234,50 CHF)", "CURRENCYFMT: (n C)");
    money(L"en-US", L"999.999", &cf, L"1.000,00 CHF", "CURRENCYFMT: rounding carries");
    NUMBERFMTA na = { 1, 1, 3, ",", "'", 1 };
    char a[64];
    same_a(a, GetNumberFormatA(0x0409, 0, "-9876543.21", &na, a, 64), "-9'876'543,2", "GetNumberFormatA with NUMBERFMTA");
}

static void errors(void)
{
    WCHAR w[128];
    SetLastError(0);
    check(!GetNumberFormatEx(L"en-US", 0, L"1,5", NULL, w, 128) && GetLastError() == ERROR_INVALID_PARAMETER,
          "a value that is not a number is refused");
    SetLastError(0);
    check(!GetNumberFormatEx(L"en-US", 0, L"", NULL, w, 128) && GetLastError() == ERROR_INVALID_PARAMETER, "an empty value is refused");
    NUMBERFMTW nf = { 2, 1, 3, L".", L",", 1 };
    SetLastError(0);
    check(!GetNumberFormatEx(L"en-US", LOCALE_NOUSEROVERRIDE, L"1", &nf, w, 128) && GetLastError() == ERROR_INVALID_FLAGS,
          "flags with a NUMBERFMT are refused");
    SetLastError(0);
    check(!GetDateFormatEx(L"en-US", DATE_SHORTDATE, &g_st, L"d", w, 128, NULL) && GetLastError() == ERROR_INVALID_FLAGS,
          "DATE_SHORTDATE with a picture is refused");
    SYSTEMTIME bad = g_st;
    bad.wMonth = 2; bad.wDay = 30;
    SetLastError(0);
    check(!GetDateFormatEx(L"en-US", 0, &bad, NULL, w, 128, NULL) && GetLastError() == ERROR_INVALID_PARAMETER,
          "30 February is refused");
    bad = g_st; bad.wHour = 24;
    SetLastError(0);
    check(!GetTimeFormatEx(L"en-US", 0, &bad, NULL, w, 128) && GetLastError() == ERROR_INVALID_PARAMETER, "hour 24 is refused");
    check(GetDateFormatEx(L"de-DE", DATE_LONGDATE, &g_st, NULL, NULL, 0, NULL) == 25, "the size needed (cap 0)");
    SetLastError(0);
    check(!GetDateFormatEx(L"de-DE", DATE_LONGDATE, &g_st, NULL, w, 10, NULL) && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
          "a short buffer is refused");
    SetLastError(0);
    check(!GetDateFormatEx(L"xx-YY", 0, &g_st, NULL, w, 128, NULL) && GetLastError() == ERROR_INVALID_PARAMETER,
          "an unknown locale is refused");
    check(GetDateFormatEx(L"de-DE", 0, NULL, NULL, w, 128, NULL) > 0, "no date: today");
}

/* ---- the user locale (several processes) ---- */
static char g_self[MAX_PATH];

static DWORD run(const char *cmd)
{
    char cl[512];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    snprintf(cl, sizeof(cl), "%s", cmd);
    if (!CreateProcessA(NULL, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0xDEAD;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0xDEAD;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static DWORD intl(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "C:\\Windows\\System32\\intl.exe %s", name);
    return run(cmd);
}

static DWORD child(const char *name)
{
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof(cmd), "\"%s\" child %s", g_self, name);
    return run(cmd);
}

static DWORD child_mode(const char *mode)
{
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", g_self, mode);
    return run(cmd);
}

/* (in a new process) the user locale is @name, and formatting follows it */
static void is_user(const char *name)
{
    WCHAR want[LOCALE_NAME_MAX_LENGTH], got[LOCALE_NAME_MAX_LENGTH], w[128];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, want, LOCALE_NAME_MAX_LENGTH);
    same(got, GetUserDefaultLocaleName(got, LOCALE_NAME_MAX_LENGTH), want, "GetUserDefaultLocaleName");
    check(GetUserDefaultLCID() == LocaleNameToLCID(want, 0), "GetUserDefaultLCID matches");
    check(GetUserDefaultLangID() == (LANGID)LocaleNameToLCID(want, 0), "GetUserDefaultLangID matches");
    check(GetSystemDefaultLCID() == 0x409, "the system locale stays en-US");
    WCHAR shortdate[128];
    GetLocaleInfoEx(want, LOCALE_SSHORTDATE, shortdate, 128);
    WCHAR expect[128];
    GetDateFormatEx(want, DATE_SHORTDATE, &g_st, NULL, expect, 128, NULL);
    same(w, GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &g_st, NULL, w, 128), expect, "GetDateFormatW(LOCALE_USER_DEFAULT)");
    same(w, GetDateFormatEx(NULL, DATE_SHORTDATE, &g_st, NULL, w, 128, NULL), expect, "GetDateFormatEx(NULL)");
    GetNumberFormatEx(want, 0, L"1234567.891", NULL, expect, 128);
    same(w, GetNumberFormatW(LOCALE_USER_DEFAULT, 0, L"1234567.891", NULL, w, 128), expect, "GetNumberFormatW(LOCALE_USER_DEFAULT)");
    GetLocaleInfoEx(want, LOCALE_SDECIMAL, expect, 128);
    same(w, GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, w, 128), expect, "GetLocaleInfoW(LOCALE_USER_DEFAULT)");
    HKEY k;
    DWORD n = sizeof(w), type;
    check(!RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\International", 0, KEY_READ, &k), "HKCU\\Control Panel\\International");
    LONG e = RegQueryValueExW(k, L"sShortDate", NULL, &type, (BYTE *)w, &n);
    RegCloseKey(k);
    check(!e && !wcscmp(w, shortdate), "intl.exe wrote sShortDate");
    printf("user locale: %s, ", name);
    GetDateFormatEx(NULL, DATE_LONGDATE, &g_st, NULL, w, 128, NULL);
    show(w);
    printf("\n");
}


/* ---- calendars and durations ---- */
typedef struct { WCHAR s[512]; int n; DWORD cal; } Collect;

static void add(Collect *c, const WCHAR *w, DWORD cal)
{
    if (c->n) wcscat(c->s, L"|");
    wcscat(c->s, w);
    c->n++;
    c->cal = cal;
}
static BOOL CALLBACK cal_cb(LPWSTR w, CALID cal, LPWSTR r, LPARAM p) { (void)r; add((Collect *)p, w, cal); return TRUE; }
static BOOL CALLBACK date_cb(LPWSTR w, CALID cal, LPARAM p) { add((Collect *)p, w, cal); return TRUE; }
static BOOL CALLBACK time_cb(LPWSTR w, LPARAM p) { add((Collect *)p, w, 0); return TRUE; }
static Collect g_ex;
static BOOL CALLBACK cal_cb_exw(LPWSTR w, CALID cal) { add(&g_ex, w, cal); return TRUE; }

static void enum_cal(LPCWSTR loc, CALID cal, CALTYPE type, const WCHAR *want, const char *what)
{
    Collect c = { { 0 } };
    check(EnumCalendarInfoExEx(cal_cb, loc, cal, NULL, type, (LPARAM)&c), what);
    same(c.s, c.n ? (int)wcslen(c.s) + 1 : 0, want, what);
}

static void cal_info(LPCWSTR loc, CALID cal, CALTYPE type, const WCHAR *want, const char *what)
{
    WCHAR w[128];
    same(w, GetCalendarInfoEx(loc, cal, NULL, type, w, 128, NULL), want, what);
}

static void locale_info(LPCWSTR loc, DWORD type, const WCHAR *want, const char *what)
{
    WCHAR w[128];
    same(w, GetLocaleInfoEx(loc, type, w, 128), want, what);
}

static void calendars(void)
{
    WCHAR w[128];
    DWORD v = 0;

    /* each locale's calendars, its own first */
    locale_info(L"en-US", LOCALE_ICALENDARTYPE, L"1", "en-US: Gregorian");
    locale_info(L"ja-JP", LOCALE_ICALENDARTYPE, L"1", "ja-JP: Gregorian first");
    locale_info(L"ja-JP", LOCALE_IOPTIONALCALENDAR, L"3", "ja-JP: the Japanese era calendar besides");
    locale_info(L"th-TH", LOCALE_ICALENDARTYPE, L"7", "th-TH: Thai Buddhist");
    enum_cal(L"ar-SA", ENUM_ALL_CALENDARS, CAL_ICALINTVALUE, L"1|23|6", "ar-SA: Gregorian, Um Al Qura and Hijri");
    locale_info(L"fa-IR", LOCALE_ICALENDARTYPE, L"22", "fa-IR: Persian");
    enum_cal(L"ja-JP", ENUM_ALL_CALENDARS, CAL_ICALINTVALUE, L"1|3", "EnumCalendarInfoExEx(ja-JP, ENUM_ALL_CALENDARS)");
    enum_cal(L"zh-TW", ENUM_ALL_CALENDARS, CAL_ICALINTVALUE, L"1|4", "zh-TW: Gregorian and Taiwan");
    enum_cal(L"ko-KR", ENUM_ALL_CALENDARS, CAL_ICALINTVALUE, L"1|5", "ko-KR: Gregorian and Tangun");

    /* GetCalendarInfo */
    check(GetCalendarInfoEx(L"ja-JP", CAL_JAPAN, NULL, CAL_ICALINTVALUE | CAL_RETURN_NUMBER, NULL, 0, &v) == 2 && v == 3,
          "CAL_RETURN_NUMBER");
    check(GetCalendarInfoW(0x409, CAL_GREGORIAN, CAL_ITWODIGITYEARMAX | CAL_RETURN_NUMBER | CAL_NOUSEROVERRIDE, NULL, 0, &v) == 2 &&
          v == 2049, "CAL_ITWODIGITYEARMAX is 2049");
    cal_info(L"en-US", CAL_GREGORIAN, CAL_SCALNAME, L"Gregorian Calendar", "CAL_SCALNAME");
    cal_info(L"en-US", CAL_GREGORIAN, CAL_SMONTHNAME1, L"January", "CAL_SMONTHNAME1");
    cal_info(L"en-US", CAL_GREGORIAN, CAL_SDAYNAME1, L"Monday", "CAL_SDAYNAME1 is Monday");
    cal_info(L"en-US", CAL_GREGORIAN, CAL_SSHORTDATE, L"M/d/yyyy", "CAL_SSHORTDATE");
    cal_info(L"de-DE", CAL_GREGORIAN, CAL_SLONGDATE, L"dddd, d. MMMM yyyy", "de-DE CAL_SLONGDATE");
    cal_info(L"ja-JP", CAL_JAPAN, CAL_SERASTRING, L"\x4ee4\x548c", "CAL_SERASTRING: Reiwa");
    cal_info(L"ja-JP", CAL_JAPAN, CAL_SABBREVERASTRING, L"\x4ee4", "CAL_SABBREVERASTRING");
    cal_info(L"ja-JP", CAL_JAPAN, CAL_SENGLISHERANAME, L"Reiwa", "CAL_SENGLISHERANAME");
    cal_info(L"ja-JP", CAL_JAPAN, CAL_SMONTHNAME1, L"1\x6708", "ja-JP Japanese months are the Gregorian ones");
    cal_info(L"zh-TW", CAL_TAIWAN, CAL_IYEAROFFSETRANGE, L"1912", "Taiwan's year offset");
    cal_info(L"th-TH", CAL_THAI, CAL_IYEAROFFSETRANGE, L"-543", "Thai year offset");
    cal_info(L"ar-SA", CAL_UMALQURA, CAL_SMONTHNAME1, L"\x0645\x062d\x0631\x0645", "Um Al Qura month 1 (Muharram)");
    cal_info(L"ar-SA", CAL_HIJRI, CAL_SMONTHNAME9, L"\x0631\x0645\x0636\x0627\x0646", "Hijri month 9 (Ramadan)");
    cal_info(L"fa-IR", CAL_PERSIAN, CAL_SMONTHNAME1, L"\x0641\x0631\x0648\x0631\x062f\x06cc\x0646", "Persian month 1 (Farvardin)");
    cal_info(L"fa-IR", CAL_GREGORIAN, CAL_SMONTHNAME1, L"\x0698\x0627\x0646\x0648\x06cc\x0647", "fa-IR Gregorian month 1 (Zhanviyeh)");
    check(GetCalendarInfoEx(L"he-IL", CAL_HEBREW, NULL, CAL_SMONTHNAME13, w, 128, NULL) > 1, "the Hebrew calendar's 13th month");
    cal_info(L"en-US", CAL_GREGORIAN, CAL_SMONTHNAME13, L"", "no 13th Gregorian month");
    enum_cal(L"ja-JP", CAL_JAPAN, CAL_SERASTRING, L"\x4ee4\x548c|\x5e73\x6210|\x662d\x548c|\x5927\x6b63|\x660e\x6cbb",
             "the Japanese eras, newest first");
    enum_cal(L"ja-JP", CAL_JAPAN, CAL_IYEAROFFSETRANGE, L"2019|1989|1926|1912|1868", "the Japanese eras' first years");
    char a[64];
    int n = GetCalendarInfoA(0x411, CAL_JAPAN, CAL_SERASTRING, a, 64, NULL);
    same_a(a, n, "\xe4\xbb\xa4\xe5\x92\x8c", "GetCalendarInfoA (UTF-8)");
    g_ex.n = 0; g_ex.s[0] = 0;
    check(EnumCalendarInfoExW(cal_cb_exw, 0x0411, ENUM_ALL_CALENDARS, CAL_SCALNAME) && g_ex.n == 2 && g_ex.cal == CAL_JAPAN,
          "EnumCalendarInfoExW");
    SetLastError(0);
    check(!GetCalendarInfoEx(L"en-US", 99, NULL, CAL_SCALNAME, w, 128, NULL) && GetLastError() == ERROR_INVALID_PARAMETER,
          "an unknown calendar is refused");
    SetLastError(0);
    check(!GetCalendarInfoEx(L"en-US", CAL_GREGORIAN, NULL, CAL_SCALNAME | CAL_RETURN_NUMBER, NULL, 0, &v) &&
          GetLastError() == ERROR_INVALID_FLAGS, "CAL_RETURN_NUMBER of a name is refused");
    SetLastError(0);
    check(!GetCalendarInfoEx(L"en-US", CAL_GREGORIAN, NULL, CAL_SCALNAME, w, 3, NULL) && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
          "a short buffer is refused (GetCalendarInfo)");

    /* dates in other calendars */
    date(L"th-TH", 0, L"d/M/yyyy", L"2/10/2569", "th-TH: the Buddhist year");
    date(L"ja-JP", DATE_USE_ALT_CALENDAR, L"ggy'\x5e74'M'\x6708'd'\x65e5'", L"\x4ee4\x548c" L"8\x5e74" L"10\x6708" L"2\x65e5",
         "ja-JP DATE_USE_ALT_CALENDAR: Reiwa 8");
    date(L"ja-JP", 0, L"yyyy/M/d", L"2026/10/2", "ja-JP without it: Gregorian");
    date(L"zh-TW", DATE_USE_ALT_CALENDAR, L"y/M/d", L"115/10/2", "zh-TW: the year of the Republic");
    date(L"ko-KR", DATE_USE_ALT_CALENDAR, L"yyyy-MM-dd", L"4359-10-02", "ko-KR: the Tangun year");
    date(L"fa-IR", 0, L"yyyy/MM/dd", L"1405/07/10", "fa-IR: 10 Mehr 1405");
    date(L"ar-SA", DATE_USE_ALT_CALENDAR, L"yyyy/MM/dd", L"1448/04/21", "ar-SA: 21 Rabi' II 1448 (Um Al Qura)");
    date(L"he-IL", DATE_USE_ALT_CALENDAR, L"d MMMM yyyy", L"\x05db\"\x05d0 \x05ea\x05e9\x05e8\x05d9 \x05ea\x05e9\x05e4\"\x05d6",
         "he-IL: 21 Tishri 5787 in Hebrew numerals");
    date(L"ja-JP", DATE_USE_ALT_CALENDAR | DATE_LONGDATE, NULL, L"\x4ee4\x548c" L"8\x5e74" L"10\x6708" L"2\x65e5",
         "ja-JP Japanese long date");
    GetDateFormatEx(L"th-TH", DATE_SHORTDATE, &g_st, NULL, w, 128, NULL);
    printf("th-TH short date: "); show(w); printf("\n");
    check(wcsstr(w, L"2569") != NULL, "th-TH's short date has the Buddhist year");

    /* EnumDateFormats, EnumTimeFormats */
    Collect c = { { 0 } };
    check(EnumDateFormatsExEx(date_cb, L"ja-JP", DATE_SHORTDATE, (LPARAM)&c) && c.cal == CAL_GREGORIAN, "EnumDateFormatsExEx");
    same(c.s, c.n ? (int)wcslen(c.s) + 1 : 0, L"yyyy/MM/dd", "EnumDateFormatsExEx(ja-JP, DATE_SHORTDATE)");
    memset(&c, 0, sizeof(c));
    check(EnumDateFormatsExEx(date_cb, L"ja-JP", DATE_SHORTDATE | DATE_USE_ALT_CALENDAR, (LPARAM)&c) && c.cal == CAL_JAPAN && c.n == 1,
          "EnumDateFormatsExEx(DATE_USE_ALT_CALENDAR): the Japanese calendar's");
    memset(&c, 0, sizeof(c));
    check(EnumTimeFormatsEx(time_cb, L"de-DE", 0, (LPARAM)&c), "EnumTimeFormatsEx");
    same(c.s, c.n ? (int)wcslen(c.s) + 1 : 0, L"HH:mm:ss", "EnumTimeFormatsEx(de-DE)");
}

static void duration(LPCWSTR loc, const SYSTEMTIME *d, ULONGLONG t, LPCWSTR pic, const WCHAR *want, const char *what)
{
    WCHAR w[128];
    same(w, GetDurationFormatEx(loc, 0, d, t, pic, w, 128), want, what);
}

static void durations(void)
{
    const ULONGLONG sec = 10000000ull;
    duration(L"en-US", NULL, 5445 * sec + sec / 2, NULL, L"1:30:45", "GetDurationFormatEx: LOCALE_SDURATION");
    duration(L"en-US", NULL, 5445 * sec + sec / 2, L"hh:mm:ss.fff", L"01:30:45.500", "hh, mm, ss and fff");
    duration(L"en-US", NULL, 129600 * sec, L"h:mm", L"36:00", "hours beyond a day without d");
    duration(L"en-US", NULL, 129600 * sec, L"d' days 'h:mm", L"1 days 12:00", "with d");
    duration(L"en-US", NULL, 90 * sec + sec / 2, L"m' min'", L"1 min", "minutes alone");
    duration(L"en-US", NULL, 90 * sec, L"s's'", L"90s", "seconds alone");
    duration(L"en-US", NULL, 12345678, L"fffffffff", L"234567800", "nine fraction digits");
    SYSTEMTIME d = { 0, 0, 0, 0, 2, 3, 4, 5 };
    duration(L"en-US", &d, 0, L"H:mm:ss.fff", L"2:03:04.005", "a SYSTEMTIME duration");
    WCHAR w[128];
    same(w, GetDurationFormat(0x0407, 0, NULL, 61 * sec, L"m:ss", w, 128), L"1:01", "GetDurationFormat (LCID)");
    check(GetDurationFormatEx(L"en-US", 0, NULL, 61 * sec, L"m:ss", NULL, 0) == 5, "GetDurationFormatEx: the size needed");
    SetLastError(0);
    check(!GetDurationFormatEx(L"en-US", LOCALE_NOUSEROVERRIDE, NULL, 1, L"s", w, 128) && GetLastError() == ERROR_INVALID_FLAGS,
          "GetDurationFormatEx: flags with a picture are refused");
    SetLastError(0);
    check(!GetDurationFormatEx(L"en-US", 1, NULL, 1, NULL, w, 128) && GetLastError() == ERROR_INVALID_FLAGS,
          "GetDurationFormatEx: unknown flags are refused");
    d.wHour = 24;
    SetLastError(0);
    check(!GetDurationFormatEx(L"en-US", 0, &d, 0, NULL, w, 128) && GetLastError() == ERROR_INVALID_PARAMETER,
          "GetDurationFormatEx: hour 24 is refused");
    SetLastError(0);
    check(!GetDurationFormatEx(L"en-US", 0, NULL, 61 * sec, L"m:ss", w, 4) && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
          "GetDurationFormatEx: a short buffer is refused");
}

/* ---- the user's overrides (SetLocaleInfo) ---- */
static void overridden(void)
{
    WCHAR w[128];
    same(w, GetDateFormatEx(NULL, DATE_SHORTDATE, &g_st, NULL, w, 128, NULL), L"2026/10/02", "the overridden short date");
    same(w, GetNumberFormatEx(NULL, 0, L"1234.5", NULL, w, 128), L"1.234,50", "the overridden separators");
    same(w, GetTimeFormatEx(NULL, 0, &g_st, NULL, w, 128), L"14.05.09", "the overridden time format");
    Collect c = { { 0 } };
    EnumDateFormatsExEx(date_cb, NULL, DATE_SHORTDATE, (LPARAM)&c);
    same(c.s, c.n ? (int)wcslen(c.s) + 1 : 0, L"yyyy/MM/dd|M/d/yyyy", "EnumDateFormatsExEx: the user's first, then en-US's");
}

static void overrides(void)
{
    WCHAR w[128];
    DWORD v;
    same(w, GetUserDefaultLocaleName(w, 128), L"en-US", "the user locale is en-US");
    check(SetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SSHORTDATE, L"yyyy-MM-dd"), "SetLocaleInfoW(LOCALE_SSHORTDATE)");
    same(w, GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SSHORTDATE, w, 128), L"yyyy-MM-dd", "GetLocaleInfoW: the override");
    same(w, GetLocaleInfoEx(L"en-US", LOCALE_SSHORTDATE, w, 128), L"yyyy-MM-dd", "GetLocaleInfoEx(en-US): the override");
    same(w, GetLocaleInfoEx(L"en-US", LOCALE_SSHORTDATE | LOCALE_NOUSEROVERRIDE, w, 128), L"M/d/yyyy", "LOCALE_NOUSEROVERRIDE");
    same(w, GetLocaleInfoEx(L"en-US", LOCALE_SDATE, w, 128), L"-", "the short date set LOCALE_SDATE");
    same(w, GetLocaleInfoEx(L"en-US", LOCALE_IDATE, w, 128), L"2", "and LOCALE_IDATE");
    same(w, GetLocaleInfoEx(L"de-DE", LOCALE_SSHORTDATE, w, 128), L"dd.MM.yyyy", "other locales keep their own");
    same(w, GetDateFormatEx(NULL, DATE_SHORTDATE, &g_st, NULL, w, 128, NULL), L"2026-10-02", "GetDateFormatEx follows the override");
    same(w, GetDateFormatEx(NULL, DATE_SHORTDATE | LOCALE_NOUSEROVERRIDE, &g_st, NULL, w, 128, NULL), L"10/2/2026",
         "GetDateFormatEx(LOCALE_NOUSEROVERRIDE)");
    check(SetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDATE, L"/"), "SetLocaleInfoW(LOCALE_SDATE)");
    same(w, GetLocaleInfoEx(NULL, LOCALE_SSHORTDATE, w, 128), L"yyyy/MM/dd", "a new LOCALE_SDATE goes into the short date");
    check(SetLocaleInfoW(0x0409, LOCALE_SDECIMAL, L",") && SetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, "."),
          "SetLocaleInfo(LOCALE_SDECIMAL, LOCALE_STHOUSAND)");
    same(w, GetNumberFormatEx(L"en-US", LOCALE_NOUSEROVERRIDE, L"1234.5", NULL, w, 128), L"1,234.50",
         "GetNumberFormatEx(LOCALE_NOUSEROVERRIDE)");
    check(SetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STIMEFORMAT, L"HH.mm.ss"), "SetLocaleInfoW(LOCALE_STIMEFORMAT)");
    same(w, GetLocaleInfoEx(NULL, LOCALE_STIME, w, 128), L".", "the time format set LOCALE_STIME");
    check(GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_ITIME | LOCALE_RETURN_NUMBER, (LPWSTR)&v, 2) == 2 && v == 1,
          "and LOCALE_ITIME (24-hour)");
    overridden();
    SetLastError(0);
    check(!SetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SNAME, L"de-DE") && GetLastError() == ERROR_INVALID_FLAGS,
          "LOCALE_SNAME cannot be set");
    SetLastError(0);
    check(!SetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, L"abcd") && GetLastError() == ERROR_INVALID_PARAMETER,
          "a decimal separator of four characters is refused");
    check(child_mode("override-child") == 0, "a new process has the overrides");
    check(SetCalendarInfoW(LOCALE_USER_DEFAULT, CAL_GREGORIAN, CAL_ITWODIGITYEARMAX, L"2060"), "SetCalendarInfoW");
    check(GetCalendarInfoW(LOCALE_USER_DEFAULT, CAL_GREGORIAN, CAL_ITWODIGITYEARMAX | CAL_RETURN_NUMBER, NULL, 0, &v) && v == 2060,
          "CAL_ITWODIGITYEARMAX follows it");
    check(GetCalendarInfoW(LOCALE_USER_DEFAULT, CAL_GREGORIAN, CAL_ITWODIGITYEARMAX | CAL_RETURN_NUMBER | CAL_NOUSEROVERRIDE, NULL, 0, &v) &&
          v == 2049, "CAL_NOUSEROVERRIDE: 2049");
    check(SetCalendarInfoW(LOCALE_USER_DEFAULT, CAL_GREGORIAN, CAL_ITWODIGITYEARMAX, L"2049"), "SetCalendarInfoW back");
#ifdef _WIN64
    check(intl("en-US") == 0, "intl en-US resets the overrides");
#else
    static const DWORD changed[] = { LOCALE_SSHORTDATE, LOCALE_SDECIMAL, LOCALE_STHOUSAND, LOCALE_STIMEFORMAT };
    for (int i = 0; i < 4; i++) {                          /* (intl.exe is 64-bit only) */
        GetLocaleInfoEx(L"en-US", changed[i] | LOCALE_NOUSEROVERRIDE, w, 128);
        check(SetLocaleInfoW(LOCALE_USER_DEFAULT, changed[i], w), "SetLocaleInfoW back to en-US's own");
    }
#endif
    check(child_mode("plain-child") == 0, "a new process formats en-US's own way again");
}

int main(int argc, char **argv)
{
    GetModuleFileNameA(NULL, g_self, MAX_PATH);
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "child") && argc > 2) {
        is_user(argv[2]);
        return g_fail ? 1 : 0;
    }
    if (!strcmp(mode, "override-child")) {
        overridden();
        return g_fail ? 1 : 0;
    }
    if (!strcmp(mode, "plain-child")) {
        WCHAR w[128];
        same(w, GetDateFormatEx(NULL, DATE_SHORTDATE, &g_st, NULL, w, 128, NULL), L"10/2/2026", "en-US's short date");
        same(w, GetNumberFormatEx(NULL, 0, L"1234.5", NULL, w, 128), L"1,234.50", "en-US's separators");
        same(w, GetTimeFormatEx(NULL, 0, &g_st, NULL, w, 128), L"2:05:09 PM", "en-US's time");
        return g_fail ? 1 : 0;
    }
    if (!strcmp(mode, "calendars")) {
        calendars();
        durations();
    } else if (!strcmp(mode, "override")) {
        overrides();
    } else if (!strcmp(mode, "user")) {
        check(intl("de-DE") == 0, "intl de-DE");
        check(child("de-DE") == 0, "a new process uses de-DE");
        check(intl("ja") == 0, "intl ja (a neutral name: its default)");
        check(child("ja-JP") == 0, "a new process uses ja-JP");
        check(intl("en-US") == 0, "intl en-US");
        check(child("en-US") == 0, "back to en-US");
        check(intl("xx-YY") == 1, "intl refuses an unknown locale");
    } else if (!strcmp(mode, "set") && argc > 2) {
        check(intl(argv[2]) == 0, "intl NAME");
        check(child(argv[2]) == 0, "a new process uses it");
    } else if (!strcmp(mode, "after-restart") && argc > 2) {
        check(child(argv[2]) == 0, "the user locale lasted across the restart");
        check(intl("en-US") == 0, "intl en-US");
        check(child("en-US") == 0, "back to en-US");
    } else {
        german();
        japanese();
        english();
        errors();
    }
    printf("nlstest%s%s: %d passed, %d failed\n", *mode ? " " : "", mode, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
