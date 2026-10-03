/*
 * intl.exe — the user's regional format (the user locale).
 *
 *   intl            the current one, with a date, a time, a number and an
 *                   amount of money in it
 *   intl NAME       make NAME (de-DE, ja-JP, fr...) the user's format
 *   intl /list      the locales there are
 *
 * The choice is HKCU\Control Panel\International: LocaleName and Locale
 * (the LCID, 8 hex digits), which kernel32 reads for
 * GetUserDefaultLocaleName/LCID, plus the classic values Windows keeps
 * beside them (sShortDate, sDecimal, iCurrency...), which are also the
 * user's overrides that GetLocaleInfo and the formatting functions answer
 * (SetLocaleInfo changes one): choosing a locale resets them to its own.
 * The registry is saved to drive C:, so the choice lasts across restarts;
 * programs started afterwards use it.  The Settings app's "Time &
 * language" page runs this.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

BOOL WINAPI EnumSystemLocalesEx(BOOL (CALLBACK *fn)(LPWSTR, DWORD, LPARAM), DWORD flags, LPARAM p, LPVOID r);

static void put(const WCHAR *w)
{
    char s[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, sizeof(s), NULL, NULL);
    fputs(s, stdout);
}

static void show(void)
{
    WCHAR name[LOCALE_NAME_MAX_LENGTH], disp[128], w[128];
    SYSTEMTIME st;
    GetLocalTime(&st);
    GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
    GetLocaleInfoEx(name, LOCALE_SENGLISHDISPLAYNAME, disp, 128);
    printf("User locale: "); put(name); printf(" (LCID 0x%04lX), ", (unsigned long)GetUserDefaultLCID()); put(disp);
    printf("\nShort date:  "); GetDateFormatEx(NULL, DATE_SHORTDATE, &st, NULL, w, 128, NULL); put(w);
    printf("\nLong date:   "); GetDateFormatEx(NULL, DATE_LONGDATE, &st, NULL, w, 128, NULL); put(w);
    printf("\nTime:        "); GetTimeFormatEx(NULL, 0, &st, NULL, w, 128); put(w);
    printf("\nNumber:      "); GetNumberFormatEx(NULL, 0, L"1234567.891", NULL, w, 128); put(w);
    printf("\nCurrency:    "); GetCurrencyFormatEx(NULL, 0, L"-1234.5", NULL, w, 128); put(w);
    printf("\n");
}

static BOOL CALLBACK list_one(LPWSTR name, DWORD flags, LPARAM p)
{
    (void)flags; (void)p;
    WCHAR disp[128];
    GetLocaleInfoEx(name, LOCALE_SENGLISHDISPLAYNAME, disp, 128);
    put(name); printf("  "); put(disp); printf("\n");
    return TRUE;
}

/* the registry values Windows keeps for the user's format */
static const struct { const WCHAR *value; DWORD type; } g_values[] = {
    { L"sLanguage", LOCALE_SABBREVLANGNAME },   { L"sCountry", LOCALE_SENGCOUNTRY },
    { L"iCountry", LOCALE_ICOUNTRY },            { L"sList", LOCALE_SLIST },
    { L"iMeasure", LOCALE_IMEASURE },            { L"iPaperSize", LOCALE_IPAPERSIZE },
    { L"sDecimal", LOCALE_SDECIMAL },            { L"sThousand", LOCALE_STHOUSAND },
    { L"sGrouping", LOCALE_SGROUPING },          { L"iDigits", LOCALE_IDIGITS },
    { L"iLZero", LOCALE_ILZERO },                { L"iNegNumber", LOCALE_INEGNUMBER },
    { L"sNativeDigits", LOCALE_SNATIVEDIGITS },  { L"NumShape", LOCALE_IDIGITSUBSTITUTION },
    { L"sCurrency", LOCALE_SCURRENCY },          { L"sMonDecimalSep", LOCALE_SMONDECIMALSEP },
    { L"sMonThousandSep", LOCALE_SMONTHOUSANDSEP }, { L"sMonGrouping", LOCALE_SMONGROUPING },
    { L"iCurrDigits", LOCALE_ICURRDIGITS },      { L"iCurrency", LOCALE_ICURRENCY },
    { L"iNegCurr", LOCALE_INEGCURR },            { L"sPositiveSign", LOCALE_SPOSITIVESIGN },
    { L"sNegativeSign", LOCALE_SNEGATIVESIGN },  { L"sTimeFormat", LOCALE_STIMEFORMAT },
    { L"sShortTime", LOCALE_SSHORTTIME },        { L"sTime", LOCALE_STIME },
    { L"iTime", LOCALE_ITIME },                  { L"iTLZero", LOCALE_ITLZERO },
    { L"s1159", LOCALE_S1159 },                  { L"s2359", LOCALE_S2359 },
    { L"sShortDate", LOCALE_SSHORTDATE },        { L"sLongDate", LOCALE_SLONGDATE },
    { L"sYearMonth", LOCALE_SYEARMONTH },        { L"sDate", LOCALE_SDATE },
    { L"iDate", LOCALE_IDATE },                  { L"iFirstDayOfWeek", LOCALE_IFIRSTDAYOFWEEK },
    { L"iFirstWeekOfYear", LOCALE_IFIRSTWEEKOFYEAR }, { L"iCalendarType", LOCALE_ICALENDARTYPE },
    { L"iTimePrefix", LOCALE_ITIMEMARKPOSN },
};

static int set(const char *arg)
{
    WCHAR want[LOCALE_NAME_MAX_LENGTH], name[LOCALE_NAME_MAX_LENGTH];
    MultiByteToWideChar(CP_UTF8, 0, arg, -1, want, LOCALE_NAME_MAX_LENGTH);
    if (!want[0] || !ResolveLocaleName(want, name, LOCALE_NAME_MAX_LENGTH) || !name[0]) {
        printf("intl: %s is not a locale (intl /list shows them)\n", arg);
        return 1;
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\International", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL)) {
        printf("intl: cannot open HKCU\\Control Panel\\International\n");
        return 1;
    }
    WCHAR v[256];
    DWORD lcid = LocaleNameToLCID(name, 0);
    for (int i = 7; i >= 0; i--, lcid >>= 4) v[i] = L"0123456789ABCDEF"[lcid & 15];
    v[8] = 0;
    LONG e = RegSetValueExW(k, L"Locale", 0, REG_SZ, (const BYTE *)v, (DWORD)(wcslen(v) + 1) * 2);
    if (!e) e = RegSetValueExW(k, L"LocaleName", 0, REG_SZ, (const BYTE *)name, (DWORD)(wcslen(name) + 1) * 2);
    for (size_t i = 0; !e && i < sizeof(g_values) / sizeof(g_values[0]); i++) {
        if (GetLocaleInfoEx(name, g_values[i].type | LOCALE_NOUSEROVERRIDE, v, 256))
            e = RegSetValueExW(k, g_values[i].value, 0, REG_SZ, (const BYTE *)v, (DWORD)(wcslen(v) + 1) * 2);
        else
            RegDeleteValueW(k, g_values[i].value);           /* (no override left from the last locale) */
    }
    RegCloseKey(k);
    if (e) { printf("intl: writing the registry failed (%ld)\n", (long)e); return 1; }
    printf("The user locale is now "); put(name);
    printf(" (programs started from now on use it)\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { show(); return 0; }
    if (!strcmp(argv[1], "/list") || !strcmp(argv[1], "-list")) {
        EnumSystemLocalesEx(list_one, 0x20 /* LOCALE_SPECIFICDATA */, 0, NULL);
        return 0;
    }
    if (!strcmp(argv[1], "/?") || !strcmp(argv[1], "-h") || argc > 2) {
        printf("intl            the user's regional format\n"
               "intl NAME       make NAME (de-DE, ja-JP...) the user's format\n"
               "intl /list      the locales there are\n");
        return argc > 2;
    }
    return set(argv[1]);
}
