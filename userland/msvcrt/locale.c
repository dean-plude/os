/* setlocale and _wsetlocale: every locale behaves as "C" (UTF-8 text,
 * '.' decimal point), but the names a program sets are remembered per
 * category and reported back, as the C++ runtime (msvcp140's _Locinfo)
 * and others expect: they set a name, then read it back to build their
 * own locale objects.
 */
#define NOVA_BUILD_MSVCRT
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define NCAT 6                          /* LC_ALL .. LC_TIME */
#define NAMELEN 64
static const char *const g_catname[NCAT] = { "LC_ALL", "LC_COLLATE", "LC_CTYPE", "LC_MONETARY", "LC_NUMERIC", "LC_TIME" };
static char g_name[NCAT][NAMELEN] = { "C", "C", "C", "C", "C", "C" };
static char g_all[NCAT * (NAMELEN + 16)];
static wchar_t g_wname[NCAT][sizeof(g_all)];
static SRWLOCK g_lock = SRWLOCK_INIT;

/* The LC_ALL name: one name when every category has it, else the
 * composite "LC_COLLATE=..;LC_CTYPE=..;..." form Windows uses */
static const char *all_name(void)
{
    int same = 1;
    for (int c = 2; c < NCAT; c++) if (strcmp(g_name[c], g_name[1])) same = 0;
    if (same) return g_name[1];
    size_t k = 0;
    for (int c = 1; c < NCAT; c++) {
        size_t a = strlen(g_catname[c]), b = strlen(g_name[c]);
        memcpy(g_all + k, g_catname[c], a); k += a;
        g_all[k++] = '=';
        memcpy(g_all + k, g_name[c], b); k += b;
        if (c < NCAT - 1) g_all[k++] = ';';
    }
    g_all[k] = 0;
    return g_all;
}

/* "" is the user's default locale; NovaOS's is "C" */
static int valid(const char *n, size_t len) { return len < NAMELEN && !memchr(n, ';', len) && !memchr(n, '=', len); }
static void set_one(int c, const char *n, size_t len)
{
    if (!len) n = "C", len = 1;
    memcpy(g_name[c], n, len);
    g_name[c][len] = 0;
}

static const char *set_locked(int category, const char *locale)
{
    if (!locale) return category == LC_ALL ? all_name() : g_name[category];
    size_t len = strlen(locale);
    if (category != LC_ALL) {
        if (!valid(locale, len)) return 0;
        set_one(category, locale, len);
        return g_name[category];
    }
    if (!strchr(locale, '=')) {
        if (!valid(locale, len)) return 0;
        for (int c = 1; c < NCAT; c++) set_one(c, locale, len);
        return all_name();
    }
    /* composite: check every part, then apply */
    const char *val[NCAT] = { 0 };
    size_t vlen[NCAT] = { 0 };
    for (const char *p = locale; *p; ) {
        const char *eq = strchr(p, '='), *end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        if (!eq || eq > end) return 0;
        int c = 1;
        while (c < NCAT && (strlen(g_catname[c]) != (size_t)(eq - p) || strncmp(g_catname[c], p, (size_t)(eq - p)))) c++;
        if (c == NCAT || !valid(eq + 1, (size_t)(end - eq - 1))) return 0;
        val[c] = eq + 1;
        vlen[c] = (size_t)(end - eq - 1);
        p = *end ? end + 1 : end;
    }
    for (int c = 1; c < NCAT; c++) if (val[c]) set_one(c, val[c], vlen[c]);
    return all_name();
}

_CRTIMP char *setlocale(int category, const char *locale)
{
    if (category < LC_ALL || category > LC_TIME) return 0;
    AcquireSRWLockExclusive(&g_lock);
    const char *r = set_locked(category, locale);
    ReleaseSRWLockExclusive(&g_lock);
    return (char *)r;
}

__declspec(dllexport) wchar_t *_wsetlocale(int category, const wchar_t *locale)
{
    if (category < LC_ALL || category > LC_TIME) return 0;
    char n[NAMELEN * NCAT];
    if (locale && !WideCharToMultiByte(CP_UTF8, 0, locale, -1, n, sizeof(n), 0, 0)) return 0;
    AcquireSRWLockExclusive(&g_lock);
    const char *r = set_locked(category, locale ? n : 0);
    wchar_t *w = 0;
    if (r && MultiByteToWideChar(CP_UTF8, 0, r, -1, g_wname[category], sizeof(g_wname[0]) / sizeof(wchar_t)))
        w = g_wname[category];
    ReleaseSRWLockExclusive(&g_lock);
    return w;
}
