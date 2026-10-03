/*
 * version.c — the version stamped in the kernel image (see version.h)
 *
 * The field is a marker followed by the version, in the image's data.
 * tools/mkupdate.py finds the marker in kernel.elf (it occurs exactly
 * once: nothing else spells it, and NovaVersionMark hands out this
 * field's own bytes rather than a second copy) and writes another
 * version after it.
 */

#include "version.h"
#include "../lib/string.h"

__attribute__((used, aligned(16)))
static volatile const char g_stamp[NOVA_STAMP_MARK_LEN + NOVA_STAMP_VER_MAX] =
    "NovaOS-version-stamp:" NOVA_VERSION;

_Static_assert(sizeof("NovaOS-version-stamp:") - 1 == NOVA_STAMP_MARK_LEN, "marker length");
_Static_assert(sizeof(NOVA_VERSION) <= NOVA_STAMP_VER_MAX, "version too long");

const char *NovaVersionMark(void) { return (const char *)g_stamp; }

const char *NovaVersion(void)
{
    static char ver[NOVA_STAMP_VER_MAX];
    if (!ver[0]) {                                  /* (read through volatile: the stamp, not the constant) */
        for (int i = 0; i < NOVA_STAMP_VER_MAX - 1; i++) ver[i] = g_stamp[NOVA_STAMP_MARK_LEN + i];
        ver[NOVA_STAMP_VER_MAX - 1] = '\0';
        if (!ver[0]) strcpy(ver, "?");
    }
    return ver;
}

/* The next dot-separated number of @p (*p moves past it and its dot);
 * -1 at the end or at a '-' suffix */
static long part(const char **p)
{
    if (!**p || **p == '-') return -1;
    long n = 0;
    while (**p >= '0' && **p <= '9') n = n * 10 + (*(*p)++ - '0');
    while (**p && **p != '.' && **p != '-') (*p)++;   /* (anything else in a part is ignored) */
    if (**p == '.') (*p)++;
    return n;
}

int NovaVersionCompare(const char *a, const char *b)
{
    for (;;) {
        long x = part(&a), y = part(&b);
        if (x < 0 && y < 0) break;
        if (x < 0) x = 0;                            /* (a missing part is 0: "0.1" is "0.1.0") */
        if (y < 0) y = 0;
        if (x != y) return x < y ? -1 : 1;
    }
    /* the numbers are equal: a pre-release ("-test", "-rc1") comes before the release */
    int sa = *a == '-', sb = *b == '-';
    if (sa != sb) return sa ? -1 : 1;
    if (!sa) return 0;
    int c = strcmp(a, b);
    return c < 0 ? -1 : c > 0;
}
