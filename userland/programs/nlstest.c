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

int main(int argc, char **argv)
{
    GetModuleFileNameA(NULL, g_self, MAX_PATH);
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "child") && argc > 2) {
        is_user(argv[2]);
        return g_fail ? 1 : 0;
    }
    if (!strcmp(mode, "user")) {
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
