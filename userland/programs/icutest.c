/*
 * icutest.exe — the system ICU (C:\Windows\System32\icu.dll)
 *
 * Loads icu.dll the way .NET does (LoadLibraryEx with
 * LOAD_LIBRARY_SEARCH_SYSTEM32, then GetProcAddress of unversioned names)
 * and checks what .NET's globalization relies on: locale display names,
 * number, currency and date formats, collation, case mapping, time-zone
 * ids, IDNA and normalization, in German and Japanese, from several
 * threads at once.  Then kernel32's locales, which answer from the same
 * data: names and LCIDs, GetLocaleInfoEx fields, enumeration.  Built for
 * x64 and x86 (SysWOW64\icu.dll).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* (not in the SDK headers) */
DWORD WINAPI LocaleNameToLCID(LPCWSTR name, DWORD flags);
int   WINAPI LCIDToLocaleName(DWORD lcid, LPWSTR name, int cap, DWORD flags);
BOOL  WINAPI IsValidLocaleName(LPCWSTR name);
BOOL  WINAPI EnumSystemLocalesEx(BOOL (CALLBACK *fn)(LPWSTR, DWORD, LPARAM), DWORD flags, LPARAM p, LPVOID r);

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

typedef unsigned short UChar;   /* wchar_t is 16-bit here, so L"" literals are UTF-16 */
typedef int UErrorCode;
#define U_FAILURE(e) ((e) > 0)

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

/* the ICU functions used, looked up by their plain names */
#define ICU_FUNCS(X) \
    X(void,  u_getVersion, (unsigned char v[4])) \
    X(const char *, u_errorName, (UErrorCode)) \
    X(int,   uloc_getDisplayName, (const char *, const char *, UChar *, int, UErrorCode *)) \
    X(void *, unum_open, (int, const UChar *, int, const char *, void *, UErrorCode *)) \
    X(int,   unum_formatDouble, (const void *, double, UChar *, int, void *, UErrorCode *)) \
    X(void,  unum_close, (void *)) \
    X(void *, udat_open, (int, int, const char *, const UChar *, int, const UChar *, int, UErrorCode *)) \
    X(int,   udat_format, (const void *, double, UChar *, int, void *, UErrorCode *)) \
    X(void,  udat_close, (void *)) \
    X(void *, ucol_open, (const char *, UErrorCode *)) \
    X(int,   ucol_strcoll, (const void *, const UChar *, int, const UChar *, int)) \
    X(void,  ucol_setStrength, (void *, int)) \
    X(void,  ucol_close, (void *)) \
    X(int,   u_strToUpper, (UChar *, int, const UChar *, int, const char *, UErrorCode *)) \
    X(int,   ucal_getTimeZoneIDForWindowsID, (const UChar *, int, const char *, UChar *, int, UErrorCode *)) \
    X(void *, uidna_openUTS46, (unsigned, UErrorCode *)) \
    X(int,   uidna_nameToASCII, (const void *, const UChar *, int, UChar *, int, void *, UErrorCode *)) \
    X(void,  uidna_close, (void *)) \
    X(const void *, unorm2_getNFCInstance, (UErrorCode *)) \
    X(int,   unorm2_normalize, (const void *, const UChar *, int, UChar *, int, UErrorCode *))

#define DECLARE(ret, name, args) static ret (*p_##name) args;
ICU_FUNCS(DECLARE)

static HMODULE g_icu;

static int load(void)
{
    g_icu = LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(g_icu != NULL, "icu.dll loads from the system folder");
    if (!g_icu) return 0;
    int all = 1;
#define LOOKUP(ret, name, args) \
    p_##name = (ret (*) args)GetProcAddress(g_icu, #name); \
    if (!p_##name) { printf("FAIL: icu.dll has no %s\n", #name); all = 0; }
    ICU_FUNCS(LOOKUP)
    check(all, "icu.dll exports the C API under unversioned names");
    check(GetProcAddress(g_icu, "u_strlen_77") == NULL, "no versioned names (as on Windows)");
    return all;
}

static int ulen(const UChar *s) { int n = 0; while (s[n]) n++; return n; }

/* @got (ICU output) equals @want; prints both as UTF-8 when not */
static void same(const UChar *got, UErrorCode e, const wchar_t *want, const char *what)
{
    int ok = !U_FAILURE(e) && ulen(got) == (int)wcslen(want) &&
             memcmp(got, want, wcslen(want) * sizeof(UChar)) == 0;
    if (!ok) {
        char g[256] = "", w[256] = "";
        WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)got, -1, g, sizeof(g), 0, 0);
        WideCharToMultiByte(CP_UTF8, 0, want, -1, w, sizeof(w), 0, 0);
        printf("  %s: got \"%s\" (%s), expected \"%s\"\n", what, g, p_u_errorName(e), w);
    }
    check(ok, what);
}

#define UNUM_DECIMAL 1
#define UNUM_CURRENCY 2
#define UDAT_FULL 0
#define UDAT_NONE (-1)
#define UDAT_PATTERN (-2)
#define UCOL_PRIMARY 0
static const double DAY = 1790949909000.0;      /* 2026-10-02 14:05:09 UTC */

static void number(const char *loc, int style, double v, const wchar_t *want, const char *what)
{
    UErrorCode e = 0;
    UChar buf[128] = { 0 };
    void *f = p_unum_open(style, NULL, 0, loc, NULL, &e);
    if (!U_FAILURE(e)) p_unum_formatDouble(f, v, buf, 128, NULL, &e);
    same(buf, e, want, what);
    if (f) p_unum_close(f);
}

static void date(const char *loc, int dstyle, int tstyle, const wchar_t *pattern, const wchar_t *want, const char *what)
{
    UErrorCode e = 0;
    UChar buf[128] = { 0 };
    void *f = p_udat_open(tstyle, dstyle, loc, (const UChar *)L"UTC", 3,
                          (const UChar *)pattern, pattern ? (int)wcslen(pattern) : 0, &e);
    if (!U_FAILURE(e)) p_udat_format(f, DAY, buf, 128, NULL, &e);
    same(buf, e, want, what);
    if (f) p_udat_close(f);
}

static int coll(const char *loc, const wchar_t *a, const wchar_t *b, int strength)
{
    UErrorCode e = 0;
    void *c = p_ucol_open(loc, &e);
    if (U_FAILURE(e)) return 99;
    if (strength >= 0) p_ucol_setStrength(c, strength);
    int r = p_ucol_strcoll(c, (const UChar *)a, -1, (const UChar *)b, -1);
    p_ucol_close(c);
    return r;
}

/* several threads opening collators and formatters at once (ICU's
 * one-time initialisation and caches under contention) */
static volatile LONG g_thread_ok;
static DWORD WINAPI worker(LPVOID arg)
{
    static const char *locs[] = { "de_DE", "ja_JP", "fr_FR", "zh_CN" };
    const char *loc = locs[(INT_PTR)arg % 4];
    int ok = 1;
    for (int i = 0; i < 20 && ok; i++) {
        ok = coll(loc, L"a", L"b", -1) < 0;
        UErrorCode e = 0;
        UChar buf[64];
        void *f = p_unum_open(UNUM_DECIMAL, NULL, 0, loc, NULL, &e);
        if (U_FAILURE(e) || p_unum_formatDouble(f, 1234.5, buf, 64, NULL, &e) <= 0 || U_FAILURE(e)) ok = 0;
        if (f) p_unum_close(f);
    }
    if (ok) InterlockedIncrement(&g_thread_ok);
    return 0;
}

/* kernel32: GetLocaleInfoEx and friends for locales other than English */
static void info(const wchar_t *loc, DWORD type, const wchar_t *want, const char *what)
{
    WCHAR buf[128] = { 0 };
    int n = GetLocaleInfoEx(loc, type, buf, 128);
    same((const UChar *)buf, n > 0 ? 0 : 1, want, what);
}

static BOOL CALLBACK count_locale(LPWSTR name, DWORD flags, LPARAM p) { (void)name; (void)flags; ++*(int *)p; return TRUE; }

static void nls(void)
{
    WCHAR buf[128];
    info(L"de-DE", 0x5C, L"de-DE", "GetLocaleInfoEx knows de-DE (LOCALE_SNAME)");
    info(L"de_de", 0x5C, L"de-DE", "de_de is de-DE");
    info(L"ja-JP", 0x5C, L"ja-JP", "GetLocaleInfoEx knows ja-JP");
    info(L"de-DE", 0x73, L"Deutsch (Deutschland)", "de-DE native name");
    info(L"de-DE", 0x72, L"German (Germany)", "de-DE English name");
    info(L"ja-JP", 0x04, L"\x65e5\x672c\x8a9e", "ja-JP native language name");
    info(L"de-DE", 0x0E, L",", "de-DE decimal separator");
    info(L"de-DE", 0x0F, L".", "de-DE thousands separator");
    info(L"de-DE", 0x0C, L";", "de-DE list separator");
    info(L"de-DE", 0x14, L"\x20ac", "de-DE currency symbol");
    info(L"de-DE", 0x15, L"EUR", "de-DE currency code");
    info(L"de-DE", 0x1B, L"3", "de-DE positive currency pattern (n C)");
    info(L"de-DE", 0x1C, L"8", "de-DE negative currency pattern (-n C)");
    info(L"ja-JP", 0x19, L"0", "ja-JP currency digits");
    info(L"de-DE", 0x1F, L"dd.MM.yyyy", "de-DE short date");
    info(L"de-DE", 0x20, L"dddd, d. MMMM yyyy", "de-DE long date");
    info(L"de-DE", 0x1003, L"HH:mm:ss", "de-DE time format");
    info(L"ja-JP", 0x1F, L"yyyy/MM/dd", "ja-JP short date");
    info(L"ja-JP", 0x20, L"yyyy\x5e74M\x6708" L"d\x65e5" L"dddd", "ja-JP long date");
    info(L"de-DE", 0x3A, L"M\x00e4rz", "de-DE month name");
    info(L"ja-JP", 0x2E, L"\x91d1\x66dc\x65e5", "ja-JP day name (Friday)");
    info(L"de-DE", 0x100C, L"0", "de-DE week starts on Monday");
    info(L"ja-JP", 0x1004, L"932", "ja-JP ANSI code page");
    info(L"de-DE", 0x6D, L"de", "de-DE parent");
    info(L"en-US", 0x1F, L"M/d/yyyy", "en-US short date unchanged");
    info(L"en-US", 0x0E, L".", "en-US decimal separator unchanged");
    check(GetLocaleInfoEx(L"xx-YY", 0x5C, buf, 128) == 0, "an unknown locale is refused");
    check(LocaleNameToLCID(L"ja-JP", 0) == 0x411, "LocaleNameToLCID(ja-JP)");
    check(LCIDToLocaleName(0x407, buf, 128, 0) > 0 && !wcscmp(buf, L"de-DE"), "LCIDToLocaleName(0x407)");
    check(IsValidLocaleName(L"fr-CA") && !IsValidLocaleName(L"fr-ZZ"), "IsValidLocaleName");
    int count = 0;
    EnumSystemLocalesEx(count_locale, 0, (LPARAM)&count, NULL);
    check(count > 500, "EnumSystemLocalesEx lists the Windows locales");
}

int main(void)
{
    if (!load()) {
        printf("icutest: %d passed, %d failed\n", g_pass, g_fail);
        return 1;
    }
    unsigned char v[4];
    p_u_getVersion(v);
    printf("ICU %u.%u (%d-bit)\n", v[0], v[1], (int)sizeof(void *) * 8);
    check(v[0] >= 60, "ICU version");

    UErrorCode e = 0;
    UChar buf[128] = { 0 };
    p_uloc_getDisplayName("de_DE", "de", buf, 128, &e);
    same(buf, e, L"Deutsch (Deutschland)", "de_DE display name (the data file loads)");
    e = 0; memset(buf, 0, sizeof(buf));
    p_uloc_getDisplayName("ja_JP", "ja", buf, 128, &e);
    same(buf, e, L"\x65e5\x672c\x8a9e (\x65e5\x672c)", "ja_JP display name");
    e = 0; memset(buf, 0, sizeof(buf));
    p_uloc_getDisplayName("ja_JP", "en", buf, 128, &e);
    same(buf, e, L"Japanese (Japan)", "ja_JP name in English");

    number("de_DE", UNUM_DECIMAL, 1234567.891, L"1.234.567,891", "de_DE number");
    number("de_DE", UNUM_CURRENCY, 1234.5, L"1.234,50\x00a0\x20ac", "de_DE currency");
    number("ja_JP", UNUM_DECIMAL, 1234567.891, L"1,234,567.891", "ja_JP number");
    number("ja_JP", UNUM_CURRENCY, 1234.6, L"\xffe5" L"1,235", "ja_JP currency");

    date("de_DE", UDAT_FULL, UDAT_NONE, NULL, L"Freitag, 2. Oktober 2026", "de_DE full date");
    date("ja_JP", UDAT_FULL, UDAT_NONE, NULL, L"2026\x5e74" L"10\x6708" L"2\x65e5\x91d1\x66dc\x65e5", "ja_JP full date");
    date("de_DE", UDAT_PATTERN, UDAT_PATTERN, L"dd.MM.yyyy HH:mm:ss", L"02.10.2026 14:05:09", "de_DE pattern");
    date("ja_JP@calendar=japanese", UDAT_PATTERN, UDAT_PATTERN, L"GGGGy\x5e74", L"\x4ee4\x548c" L"8\x5e74",
         "Japanese calendar era");

    check(coll("de_DE", L"\x00e4" L"b", L"az", -1) < 0, "de_DE collation: a-umlaut sorts with a");
    check(coll("de_DE", L"M\x00fcller", L"Muller", UCOL_PRIMARY) == 0, "de_DE primary strength ignores accents");
    check(coll("ja_JP", L"\x3042", L"\x30a2", UCOL_PRIMARY) == 0, "ja_JP: hiragana = katakana at primary strength");
    check(coll("ja_JP", L"\x3042", L"\x3044", -1) < 0, "ja_JP: a before i");

    e = 0; memset(buf, 0, sizeof(buf));
    p_u_strToUpper(buf, 128, (const UChar *)L"istanbul", -1, "tr_TR", &e);
    same(buf, e, L"\x0130STANBUL", "tr_TR upper case");
    e = 0; memset(buf, 0, sizeof(buf));
    p_u_strToUpper(buf, 128, (const UChar *)L"stra\x00df" L"e", -1, "de_DE", &e);
    same(buf, e, L"STRASSE", "de_DE full case mapping");

    e = 0; memset(buf, 0, sizeof(buf));
    p_ucal_getTimeZoneIDForWindowsID((const UChar *)L"Tokyo Standard Time", -1, NULL, buf, 128, &e);
    same(buf, e, L"Asia/Tokyo", "Windows time-zone id to IANA");

    e = 0;
    void *idna = p_uidna_openUTS46(0, &e);
    struct { short size; unsigned char a, b; unsigned errors; int r2, r3; } info = { sizeof(info) };
    memset(buf, 0, sizeof(buf));
    if (!U_FAILURE(e)) p_uidna_nameToASCII(idna, (const UChar *)L"b\x00fc" L"cher.de", -1, buf, 128, &info, &e);
    same(buf, e, L"xn--bcher-kva.de", "IDNA (UTS #46)");
    if (idna) p_uidna_close(idna);

    e = 0; memset(buf, 0, sizeof(buf));
    const void *nfc = p_unorm2_getNFCInstance(&e);
    if (!U_FAILURE(e)) p_unorm2_normalize(nfc, (const UChar *)L"e\x0301", -1, buf, 128, &e);
    same(buf, e, L"\x00e9", "NFC normalization");

    HANDLE th[8];
    for (int i = 0; i < 8; i++) th[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
    WaitForMultipleObjects(8, th, TRUE, 60000);
    for (int i = 0; i < 8; i++) CloseHandle(th[i]);
    check(g_thread_ok == 8, "8 threads collating and formatting at once");

    nls();

    printf("icutest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
