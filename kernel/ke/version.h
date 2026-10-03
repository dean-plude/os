/* The one place NovaOS's version lives: the boot banner, Settings > About, the
 * Terminal's `ver` and `sysinfo` and the App Store's updates all show it.
 * Roadmap phases are tracked in docs/roadmap/, never in the OS itself. */
#ifndef KE_VERSION_H
#define KE_VERSION_H

#define NOVA_VERSION "0.1.0"

/* The version this kernel image carries.  It is NOVA_VERSION in a field of
 * the image (version.c) that tools/mkupdate.py can stamp with another
 * version on a copy of a built kernel, so a test update needs no second
 * build. */
const char *NovaVersion(void);

/* The field's marker (NOVA_STAMP_MARK_LEN bytes), followed by the version
 * (NUL-terminated, at most NOVA_STAMP_VER_MAX bytes with the NUL): the
 * updater reads a downloaded kernel's version from it */
#define NOVA_STAMP_MARK_LEN 21
#define NOVA_STAMP_VER_MAX  27
const char *NovaVersionMark(void);

/* -1, 0 or 1 as version @a is older than, the same as or newer than @b
 * ("0.1.10" > "0.1.9"; "0.1.1-test" < "0.1.1" < "0.1.1.1"; "0.1" = "0.1.0") */
int NovaVersionCompare(const char *a, const char *b);

#endif
