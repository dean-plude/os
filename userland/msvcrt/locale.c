/* setlocale and _wsetlocale: every locale behaves as "C" (UTF-8 text,
 * '.' decimal point), but the names a program sets are remembered per
 * category and reported back, as the C++ runtime (msvcp140's _Locinfo)
 * and others expect: they set a name, then read it back to build their
 * own locale objects.
 *
 * A thread that calls _configthreadlocale(_ENABLE_PER_THREAD_LOCALE)
 * gets its own copy of the names (in its per-thread data, ptd.c): its
 * setlocale calls change only its copy, and other threads' calls no
 * longer reach it, until it disables the per-thread locale again.
 */
#define NOVA_BUILD_MSVCRT
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include "ptd.h"

#define NCAT 6                          /* LC_ALL .. LC_TIME */
#define NAMELEN 64
#define ALLLEN (NCAT * (NAMELEN + 16))
static const char *const g_catname[NCAT] = { "LC_ALL", "LC_COLLATE", "LC_CTYPE", "LC_MONETARY", "LC_NUMERIC", "LC_TIME" };

/* One set of names: the process's, or a thread's own */
typedef struct NovaLocale {
    char name[NCAT][NAMELEN];
    char all[ALLLEN];                   /* setlocale(LC_ALL)'s result */
    wchar_t wname[NCAT][NAMELEN];       /* _wsetlocale's results */
    wchar_t wall[ALLLEN];
} NovaLocale;
static NovaLocale g_global = { { "C", "C", "C", "C", "C", "C" } };
static SRWLOCK g_lock = SRWLOCK_INIT;

/* The LC_ALL name: one name when every category has it, else the
 * composite "LC_COLLATE=..;LC_CTYPE=..;..." form Windows uses */
static const char *all_name(NovaLocale *l)
{
    int same = 1;
    for (int c = 2; c < NCAT; c++) if (strcmp(l->name[c], l->name[1])) same = 0;
    if (same) return l->name[1];
    size_t k = 0;
    for (int c = 1; c < NCAT; c++) {
        size_t a = strlen(g_catname[c]), b = strlen(l->name[c]);
        memcpy(l->all + k, g_catname[c], a); k += a;
        l->all[k++] = '=';
        memcpy(l->all + k, l->name[c], b); k += b;
        if (c < NCAT - 1) l->all[k++] = ';';
    }
    l->all[k] = 0;
    return l->all;
}

/* "" is the user's default locale; NovaOS's is "C" */
static int valid(const char *n, size_t len) { return len < NAMELEN && !memchr(n, ';', len) && !memchr(n, '=', len); }
static void set_one(NovaLocale *l, int c, const char *n, size_t len)
{
    if (!len) n = "C", len = 1;
    memcpy(l->name[c], n, len);
    l->name[c][len] = 0;
}

static const char *set_locked(NovaLocale *l, int category, const char *locale)
{
    if (!locale) return category == LC_ALL ? all_name(l) : l->name[category];
    size_t len = strlen(locale);
    if (category != LC_ALL) {
        if (!valid(locale, len)) return 0;
        set_one(l, category, locale, len);
        return l->name[category];
    }
    if (!strchr(locale, '=')) {
        if (!valid(locale, len)) return 0;
        for (int c = 1; c < NCAT; c++) set_one(l, c, locale, len);
        return all_name(l);
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
    for (int c = 1; c < NCAT; c++) if (val[c]) set_one(l, c, val[c], vlen[c]);
    return all_name(l);
}

/* The calling thread's own names, or NULL when it uses the process's */
static NovaLocale *own(void)
{
    NovaPtd *p = __nova_ptd();
    return p->ownloc ? p->loc : 0;
}
static NovaLocale *lock(void)
{
    NovaLocale *l = own();
    if (l) return l;
    AcquireSRWLockExclusive(&g_lock);
    return &g_global;
}
static void unlock(NovaLocale *l) { if (l == &g_global) ReleaseSRWLockExclusive(&g_lock); }

_CRTIMP char *setlocale(int category, const char *locale)
{
    if (category < LC_ALL || category > LC_TIME) return 0;
    NovaLocale *l = lock();
    const char *r = set_locked(l, category, locale);
    unlock(l);
    return (char *)r;
}

__declspec(dllexport) wchar_t *_wsetlocale(int category, const wchar_t *locale)
{
    if (category < LC_ALL || category > LC_TIME) return 0;
    char n[ALLLEN];
    if (locale && !WideCharToMultiByte(CP_UTF8, 0, locale, -1, n, sizeof(n), 0, 0)) return 0;
    NovaLocale *l = lock();
    const char *r = set_locked(l, category, locale ? n : 0);
    wchar_t *w = category == LC_ALL ? l->wall : l->wname[category];
    int cap = category == LC_ALL ? ALLLEN : NAMELEN;
    if (!r || !MultiByteToWideChar(CP_UTF8, 0, r, -1, w, cap)) w = 0;
    unlock(l);
    return w;
}

/* _configthreadlocale: 1 (_ENABLE_PER_THREAD_LOCALE) gives the thread its
 * own copy of the process's locale names, 2 (_DISABLE_PER_THREAD_LOCALE)
 * goes back to the process's, 0 only asks; each returns the old setting.
 * -1 (the old "every thread" switch) is accepted and changes nothing. */
#define ENABLE_PER_THREAD  1
#define DISABLE_PER_THREAD 2
__declspec(dllexport) int _configthreadlocale(int type)
{
    NovaPtd *p = __nova_ptd();
    int old = p->ownloc ? ENABLE_PER_THREAD : DISABLE_PER_THREAD;
    switch (type) {
    case 0: case -1: break;
    case DISABLE_PER_THREAD: p->ownloc = 0; break;
    case ENABLE_PER_THREAD:
        if (p->ownloc) break;
        if (!p->loc && !(p->loc = HeapAlloc(GetProcessHeap(), 0, sizeof(NovaLocale)))) { errno = ENOMEM; return -1; }
        AcquireSRWLockShared(&g_lock);
        memcpy(((NovaLocale *)p->loc)->name, g_global.name, sizeof(g_global.name));
        ReleaseSRWLockShared(&g_lock);
        p->ownloc = 1;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    return old;
}
