/*
 * um_registry.c — the registry for Windows programs
 *
 * One tree under \Registry:
 *   \Registry\Machine\{SOFTWARE, SYSTEM, HARDWARE}      (HKEY_LOCAL_MACHINE)
 *   \Registry\User\<user SID>, \Registry\User\.DEFAULT  (HKEY_USERS)
 * advapi32/kernel32 map HKEY_CURRENT_USER to the user's SID and
 * HKEY_CLASSES_ROOT to Machine\SOFTWARE\Classes.  Names are UTF-16 and
 * compared case-insensitively; values hold any type and up to 1 MiB.
 *
 * Programs reach it through the NT key services (NtCreateKey, NtOpenKey,
 * NtQueryValueKey, ...); a key handle is a kernel object referencing the
 * key, so NtClose and NtDuplicateObject work as for any object.
 *
 * Persistence: the tree (except volatile keys, e.g. HARDWARE) is saved to
 * C:\Windows\System32\config\REGISTRY.DAT a moment after it changes and
 * loaded at boot; the defaults are created when there is no hive yet.
 */

#include "um_internal.h"
#include "../ke/kpcr.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../ke/timezone.h"
#include "../wm/kbdlayout.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../fs/ramfs.h"

#define ST_SUCCESS               0x00000000u
#define ST_BUFFER_OVERFLOW       0x80000005u
#define ST_NO_MORE_ENTRIES       0x8000001Au
#define ST_INVALID_HANDLE        0xC0000008u
#define ST_INVALID_PARAMETER     0xC000000Du
#define ST_NO_MEMORY             0xC0000017u
#define ST_BUFFER_TOO_SMALL      0xC0000023u
#define ST_OBJECT_NAME_INVALID   0xC0000033u
#define ST_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define ST_OBJECT_NAME_COLLISION 0xC0000035u
#define ST_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define ST_KEY_DELETED           0xC000017Cu
#define ST_CANNOT_DELETE         0xC0000121u
#define ST_INVALID_INFO_CLASS    0xC0000003u
#define ST_TOO_MANY_HANDLES      0xC000011Fu

#define NAME_MAX_CHARS 255                  /* key names (values: 16383 on Windows) */
#define VALUE_NAME_MAX 16383
#define DATA_MAX       (1024u * 1024u)
#define HIVE_PATH      "\\Windows\\System32\\config\\REGISTRY.DAT"
#define USER_SID       UM_USER_SID

typedef struct RegValue {
    struct RegValue *next;
    UINT16 *name;
    UINT32 nlen;                            /* characters */
    UINT32 type;
    UINT8 *data;
    UINT32 len;
} RegValue;

typedef struct RegKey {
    struct RegKey *parent, *child, *next;
    RegValue *values;
    UINT16 *name;
    UINT32 nlen;
    UINT64 wtime;                           /* last write (100 ns since 1601) */
    int refs;                               /* handles + the tree's own */
    bool deleted, vol;
} RegKey;

static RegKey *g_root;
/* g_reg: the tree (keys, their names and children), exclusive to change
 * it.  A key's values are changed and read under its own lock as well, so
 * setting and reading values, opening keys and enumerating take g_reg
 * shared.  Watches have a lock of their own.  Order: g_reg, a key's lock,
 * g_wlock. */
static UmRwLock g_reg;
static UmLock g_wlock;
#define KEY_LOCKS 64
static UmLock g_key_lock[KEY_LOCKS];
static UmLock *key_lock(const RegKey *k) { return &g_key_lock[((uintptr_t)k / 64) % KEY_LOCKS]; }
static volatile bool g_dirty;
static UINT64 g_dirty_ticks;
static volatile UINT32 g_generation;      /* counts changes (um_registry_generation) */

/* -----------------------------------------------------------------------
 * The tree
 * ----------------------------------------------------------------------- */
static UINT16 fold(UINT16 c)
{
    if (c >= 'a' && c <= 'z') return (UINT16)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (UINT16)(c - 32);
    return c;
}

static bool name_eq(const UINT16 *a, UINT32 an, const UINT16 *b, UINT32 bn)
{
    if (an != bn) return false;
    for (UINT32 i = 0; i < an; i++) if (fold(a[i]) != fold(b[i])) return false;
    return true;
}

static UINT16 *dup16(const UINT16 *s, UINT32 n)
{
    UINT16 *d = kmalloc(2 * (size_t)n + 2);
    if (!d) return NULL;
    if (n) memcpy(d, s, 2 * (size_t)n);
    d[n] = 0;
    return d;
}

static void touch(RegKey *k)
{
    k->wtime = um_now_100ns();
    g_generation++;
    if (!k->vol) { g_dirty = true; g_dirty_ticks = sched_ticks(); }
}

/* Change notification: signal the watchers of @k for @what (below) */
#define CHANGE_NAME     0x1u                /* REG_NOTIFY_CHANGE_NAME: subkeys added, deleted, renamed */
#define CHANGE_LAST_SET 0x4u                /* REG_NOTIFY_CHANGE_LAST_SET: values set or deleted */
static void notify(RegKey *k, UINT32 what);

static RegKey *find_child(RegKey *k, const UINT16 *name, UINT32 n)
{
    for (RegKey *c = k->child; c; c = c->next)
        if (name_eq(c->name, c->nlen, name, n)) return c;
    return NULL;
}

static RegKey *add_child(RegKey *k, const UINT16 *name, UINT32 n, bool vol)
{
    RegKey *c = kzalloc(sizeof(*c));
    if (!c) return NULL;
    c->name = dup16(name, n);
    if (!c->name) { kfree(c); return NULL; }
    c->nlen = n;
    c->parent = k;
    c->refs = 1;
    c->vol = vol || k->vol;
    /* keep children sorted (enumeration order, as on Windows) */
    RegKey **pp = &k->child;
    while (*pp) {
        const RegKey *o = *pp;
        UINT32 m = o->nlen < n ? o->nlen : n, i = 0;
        while (i < m && fold(o->name[i]) == fold(name[i])) i++;
        bool before = i < m ? fold(o->name[i]) < fold(name[i]) : o->nlen < n;
        if (!before) break;
        pp = &(*pp)->next;
    }
    c->next = *pp;
    *pp = c;
    touch(c);
    notify(k, CHANGE_NAME);
    return c;
}

static void free_value(RegValue *v) { kfree(v->name); kfree(v->data); kfree(v); }

static void key_unref(RegKey *k)
{
    if (!k || __atomic_sub_fetch(&k->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    for (RegValue *v = k->values, *n; v; v = n) { n = v->next; free_value(v); }
    kfree(k->name);
    kfree(k);
}

/* Unlink a key (and its subtree) from the tree; handles keep them alive */
static void detach(RegKey *k)
{
    for (RegKey *c = k->child, *n; c; c = n) { n = c->next; detach(c); }
    k->child = NULL;
    notify(k, ~0u);                                         /* its own watchers: the key is gone */
    if (k->parent) notify(k->parent, CHANGE_NAME);
    if (k->parent) {
        RegKey **pp = &k->parent->child;
        while (*pp && *pp != k) pp = &(*pp)->next;
        if (*pp) *pp = k->next;
        touch(k->parent);
    }
    k->deleted = true;
    k->parent = NULL;
    key_unref(k);                                           /* the tree's reference */
}

static RegValue *find_value(RegKey *k, const UINT16 *name, UINT32 n)
{
    for (RegValue *v = k->values; v; v = v->next)
        if (name_eq(v->name, v->nlen, name, n)) return v;
    return NULL;
}

static bool set_value(RegKey *k, const UINT16 *name, UINT32 n, UINT32 type, const void *data, UINT32 len)
{
    RegValue *same = find_value(k, name, n);
    if (same && same->len == len && len) {                  /* the same size: in place */
        memcpy(same->data, data, len);
        same->type = type;
        touch(k);
        notify(k, CHANGE_LAST_SET);
        return true;
    }
    UINT8 *copy = len ? kmalloc(len) : NULL;
    if (len && !copy) return false;
    if (len) memcpy(copy, data, len);
    RegValue *v = find_value(k, name, n);
    if (!v) {
        v = kzalloc(sizeof(*v));
        if (!v || !(v->name = dup16(name, n))) { kfree(v); kfree(copy); return false; }
        v->nlen = n;
        RegValue **pp = &k->values;                         /* in creation order */
        while (*pp) pp = &(*pp)->next;
        *pp = v;
    } else {
        kfree(v->data);
    }
    v->type = type;
    v->data = copy;
    v->len = len;
    touch(k);
    notify(k, CHANGE_LAST_SET);
    return true;
}

/* Walk @path ("A\B\C", UTF-16) below @k; with @create, make missing keys */
static UINT32 walk(RegKey *k, const UINT16 *path, UINT32 n, bool create, bool vol, RegKey **out, bool *created)
{
    UINT32 i = 0;
    if (created) *created = false;
    while (i < n) {
        while (i < n && path[i] == '\\') i++;
        if (i >= n) break;
        UINT32 s = i;
        while (i < n && path[i] != '\\') i++;
        UINT32 len = i - s;
        if (len > NAME_MAX_CHARS) return ST_OBJECT_NAME_INVALID;
        RegKey *c = find_child(k, path + s, len);
        if (!c) {
            /* Any component missing, the last or one on the way:
             * NAME_NOT_FOUND, as Windows' registry answers (RegOpenKeyEx's
             * ERROR_FILE_NOT_FOUND; Roblox's installer stops on anything else) */
            if (!create) return ST_OBJECT_NAME_NOT_FOUND;
            c = add_child(k, path + s, len, vol);
            if (!c) return ST_NO_MEMORY;
            if (created) *created = true;
        } else if (created) {
            *created = false;
        }
        k = c;
    }
    *out = k;
    return ST_SUCCESS;
}

/* Kernel-side helpers (defaults): ASCII paths and names */
static RegKey *kpath(const char *path, bool vol)
{
    UINT16 w[256];
    UINT32 n = 0;
    for (; path[n] && n < 255; n++) w[n] = (UINT8)path[n];
    RegKey *k = NULL;
    return walk(g_root, w, n, true, vol, &k, NULL) == ST_SUCCESS ? k : NULL;
}

static void kset_sz(RegKey *k, const char *name, const char *val, UINT32 type)
{
    if (!k) return;
    UINT16 nm[128], v[512];
    UINT32 n = 0, m = 0;
    for (; name[n] && n < 127; n++) nm[n] = (UINT8)name[n];
    for (; val[m] && m < 511; m++) v[m] = (UINT8)val[m];
    v[m++] = 0;
    set_value(k, nm, n, type, v, 2 * m);
}

static void kset_dword(RegKey *k, const char *name, UINT32 val)
{
    if (!k) return;
    UINT16 nm[128];
    UINT32 n = 0;
    for (; name[n] && n < 127; n++) nm[n] = (UINT8)name[n];
    set_value(k, nm, n, 4 /* REG_DWORD */, &val, 4);
}

static bool has_value(RegKey *k, const char *name)
{
    UINT16 nm[128];
    UINT32 n = 0;
    for (; name[n] && n < 127; n++) nm[n] = (UINT8)name[n];
    return k && find_value(k, nm, n);
}

/* -----------------------------------------------------------------------
 * Defaults
 * ----------------------------------------------------------------------- */
static void cpu_brand(char *out)
{
    UINT32 r[12];
    UINT32 a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a < 0x80000004u) { strcpy(out, "x86-64 processor"); return; }
    for (UINT32 i = 0; i < 3; i++)
        __asm__ volatile ("cpuid" : "=a"(r[4 * i]), "=b"(r[4 * i + 1]), "=c"(r[4 * i + 2]), "=d"(r[4 * i + 3]) : "a"(0x80000002u + i), "c"(0));
    memcpy(out, r, 48);
    out[48] = 0;
    char *s = out;
    while (*s == ' ') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
}

/* HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\N: one key per running processor */
static void add_cpus(UINT32 n)
{
    char brand[64];
    cpu_brand(brand);
    for (UINT32 i = 0; i < (n ? n : 1) && i < 64; i++) {
        char path[80];
        ksnprintf(path, sizeof(path), "Machine\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%u", (unsigned)i);
        RegKey *cpu = kpath(path, true);
        kset_sz(cpu, "ProcessorNameString", brand, 1);
        kset_sz(cpu, "Identifier", "Intel64 Family 6", 1);
        kset_dword(cpu, "~MHz", 2000);
    }
}

/* Once the other processors are running */
void um_registry_add_cpus(UINT32 n)
{
    um_lock_excl(&g_reg);
    add_cpus(n);
    um_unlock_excl(&g_reg);
}

/* Set a REG_DWORD from the kernel (an installer's registration): @path
 * from the root, e.g. "Machine\\SOFTWARE\\...", the key created if need be */
UINT32 um_registry_generation(void) { return g_generation; }

void um_registry_set_dword(const char *path, const char *name, UINT32 val)
{
    um_lock_excl(&g_reg);
    kset_dword(kpath(path, false), name, val);
    um_unlock_excl(&g_reg);
}

/* Set a REG_SZ from the kernel (ASCII), the key created if need be */
void um_registry_set_sz(const char *path, const char *name, const char *val)
{
    um_lock_excl(&g_reg);
    kset_sz(kpath(path, false), name, val, 1);
    um_unlock_excl(&g_reg);
}

/* Read a REG_DWORD from the kernel: false when the key or value is
 * missing or not a DWORD */
bool um_registry_get_dword(const char *path, const char *name, UINT32 *out)
{
    UINT16 w[256], nm[128];
    UINT32 n = 0, m = 0;
    for (; path[n] && n < 255; n++) w[n] = (UINT8)path[n];
    for (; name[m] && m < 127; m++) nm[m] = (UINT8)name[m];
    bool ok = false;
    um_lock_shared(&g_reg);
    RegKey *k = NULL;
    if (walk(g_root, w, n, false, false, &k, NULL) == ST_SUCCESS && k) {
        RegValue *v = find_value(k, nm, m);
        if (v && v->type == 4 /* REG_DWORD */ && v->len == 4) { memcpy(out, v->data, 4); ok = true; }
    }
    um_unlock_shared(&g_reg);
    return ok;
}

/* Read a REG_SZ from the kernel as ASCII (other characters as '?'):
 * false when the key or value is missing or not a string */
bool um_registry_get_sz(const char *path, const char *name, char *out, int cap)
{
    UINT16 w[256], nm[128];
    UINT32 n = 0, m = 0;
    for (; path[n] && n < 255; n++) w[n] = (UINT8)path[n];
    for (; name[m] && m < 127; m++) nm[m] = (UINT8)name[m];
    bool ok = false;
    um_lock_shared(&g_reg);
    RegKey *k = NULL;
    if (cap > 0 && walk(g_root, w, n, false, false, &k, NULL) == ST_SUCCESS && k) {
        RegValue *v = find_value(k, nm, m);
        if (v && (v->type == 1 /* REG_SZ */ || v->type == 2 /* REG_EXPAND_SZ */)) {
            const UINT16 *d = (const UINT16 *)v->data;
            int i = 0;
            for (; i < cap - 1 && i < (int)(v->len / 2) && d[i]; i++) out[i] = d[i] < 0x80 ? (char)d[i] : '?';
            out[i] = 0;
            ok = true;
        }
    }
    um_unlock_shared(&g_reg);
    return ok;
}

/* Set a REG_BINARY from the kernel, the key created if need be */
void um_registry_set_bin(const char *path, const char *name, const void *data, UINT32 len)
{
    UINT16 nm[128];
    UINT32 n = 0;
    for (; name[n] && n < 127; n++) nm[n] = (UINT8)name[n];
    um_lock_excl(&g_reg);
    RegKey *k = kpath(path, false);
    if (k) set_value(k, nm, n, 3 /* REG_BINARY */, data, len);
    um_unlock_excl(&g_reg);
}

/* Read a REG_BINARY from the kernel: its length (at most @cap bytes
 * copied), or -1 when the key or value is missing or not binary */
int um_registry_get_bin(const char *path, const char *name, void *out, int cap)
{
    UINT16 w[256], nm[128];
    UINT32 n = 0, m = 0;
    for (; path[n] && n < 255; n++) w[n] = (UINT8)path[n];
    for (; name[m] && m < 127; m++) nm[m] = (UINT8)name[m];
    int len = -1;
    um_lock_shared(&g_reg);
    RegKey *k = NULL;
    if (walk(g_root, w, n, false, false, &k, NULL) == ST_SUCCESS && k) {
        RegValue *v = find_value(k, nm, m);
        if (v && v->type == 3 /* REG_BINARY */) {
            len = (int)v->len;
            memcpy(out, v->data, (size_t)(len < cap ? len : cap));
        }
    }
    um_unlock_shared(&g_reg);
    return len;
}

/* HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Time Zones: one key
 * per zone NovaOS knows (ke/timezone.c), as Windows has them; rebuilt
 * every boot (volatile), so the hive on drive C: does not carry them */
static void time_zones(void)
{
    for (int i = 0; i < TzCount(); i++) {
        const TzZone *z = TzAt(i);
        char path[160];
        ksnprintf(path, sizeof(path), "Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\%s", z->key);
        RegKey *k = kpath(path, true);
        if (!k) continue;
        kset_sz(k, "Display", z->display, 1);
        kset_sz(k, "Std", z->std, 1);
        kset_sz(k, "Dlt", z->dlt, 1);
        TzTzi tzi;
        TzToTzi(z, &tzi);
        UINT16 nm[3] = { 'T', 'Z', 'I' };
        set_value(k, nm, 3, 3 /* REG_BINARY */, &tzi, sizeof(tzi));
    }
}

/* HKLM\SYSTEM\CurrentControlSet\Control\Keyboard Layouts: one key per
 * layout NovaOS has (wm/kbdlayout.c), volatile like the time zones */
static void keyboard_layouts(void)
{
    for (int i = 0; i < KbdCount(); i++) {
        char path[128];
        ksnprintf(path, sizeof(path), "Machine\\SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\%s", KbdKlid(i));
        RegKey *k = kpath(path, true);
        if (!k) continue;
        kset_sz(k, "Layout Text", KbdName(i), 1);
        if (KbdHkl(i) >> 28 == 0xF) {                     /* a variant (Dvorak): its Layout Id */
            char id[8];
            ksnprintf(id, sizeof(id), "%04x", (unsigned)((KbdHkl(i) >> 16) & 0x0FFF));
            kset_sz(k, "Layout Id", id, 1);
        }
    }
}

/* MSXML (msxml6.dll): DOMDocument, XMLHTTP and SAXXMLReader, version 6.0
 * and the 3.0 / version-independent classes Windows' msxml3.dll answers
 * (one DLL serves both on NovaOS), with their ProgIDs */
static void msxml_classes(void)
{
    static const char *const cls[][4] = {       /* CLSID, name, ProgID, version-independent ProgID */
        { "{2933BF90-7B36-11D2-B20E-00C04F983E60}", "XML DOM Document",                   "Microsoft.XMLDOM",                    0 },
        { "{2933BF91-7B36-11D2-B20E-00C04F983E60}", "Free Threaded XML DOM Document",     "Microsoft.FreeThreadedXMLDOM",        0 },
        { "{F6D90F11-9C73-11D3-B32E-00C04F990BB4}", "XML DOM Document",                   "Msxml2.DOMDocument",                  0 },
        { "{F6D90F12-9C73-11D3-B32E-00C04F990BB4}", "Free Threaded XML DOM Document",     "Msxml2.FreeThreadedDOMDocument",      0 },
        { "{F5078F1B-C551-11D3-89B9-0000F81FE221}", "XML DOM Document 2.6",               "Msxml2.DOMDocument.2.6",              "Msxml2.DOMDocument" },
        { "{F5078F1C-C551-11D3-89B9-0000F81FE221}", "Free Threaded XML DOM Document 2.6", "Msxml2.FreeThreadedDOMDocument.2.6",  "Msxml2.FreeThreadedDOMDocument" },
        { "{F5078F32-C551-11D3-89B9-0000F81FE221}", "XML DOM Document 3.0",               "Msxml2.DOMDocument.3.0",              "Msxml2.DOMDocument" },
        { "{F5078F33-C551-11D3-89B9-0000F81FE221}", "Free Threaded XML DOM Document 3.0", "Msxml2.FreeThreadedDOMDocument.3.0",  "Msxml2.FreeThreadedDOMDocument" },
        { "{88D96A05-F192-11D4-A65F-0040963251E5}", "XML DOM Document 6.0",               "Msxml2.DOMDocument.6.0",              0 },
        { "{88D96A06-F192-11D4-A65F-0040963251E5}", "Free Threaded XML DOM Document 6.0", "Msxml2.FreeThreadedDOMDocument.6.0",  0 },
        { "{ED8C108E-4349-11D2-91A4-00C04F7969E8}", "XML HTTP Request",                   "Microsoft.XMLHTTP",                   0 },
        { "{F6D90F16-9C73-11D3-B32E-00C04F990BB4}", "XML HTTP",                           "Msxml2.XMLHTTP",                      0 },
        { "{F5078F1E-C551-11D3-89B9-0000F81FE221}", "XML HTTP 2.6",                       "Msxml2.XMLHTTP.2.6",                  "Msxml2.XMLHTTP" },
        { "{F5078F35-C551-11D3-89B9-0000F81FE221}", "XML HTTP 3.0",                       "Msxml2.XMLHTTP.3.0",                  "Msxml2.XMLHTTP" },
        { "{88D96A0A-F192-11D4-A65F-0040963251E5}", "XML HTTP 6.0",                       "Msxml2.XMLHTTP.6.0",                  0 },
        { "{88D96A09-F192-11D4-A65F-0040963251E5}", "Free Threaded XML HTTP 6.0",         0,                                     0 },
        { "{AFBA6B42-5692-48EA-8141-DC517DCF0EF1}", "Server XML HTTP",                    "Msxml2.ServerXMLHTTP",                0 },
        { "{AFB40FFD-B609-40A3-9828-F88BBE11E4E3}", "Server XML HTTP 3.0",                "Msxml2.ServerXMLHTTP.3.0",            "Msxml2.ServerXMLHTTP" },
        { "{88D96A0B-F192-11D4-A65F-0040963251E5}", "Server XML HTTP 6.0",                "Msxml2.ServerXMLHTTP.6.0",            0 },
        { "{079AA557-4A18-424A-8EEE-E39F0A8D41B9}", "SAX XML Reader",                     "Msxml2.SAXXMLReader",                 0 },
        { "{3124C396-FB13-4836-A6AD-1317F1713688}", "SAX XML Reader 3.0",                 "Msxml2.SAXXMLReader.3.0",             "Msxml2.SAXXMLReader" },
        { "{88D96A0C-F192-11D4-A65F-0040963251E5}", "SAX XML Reader 6.0",                 "Msxml2.SAXXMLReader.6.0",             0 },
    };
    for (unsigned i = 0; i < sizeof cls / sizeof cls[0]; i++) {
        char path[160];
        ksnprintf(path, sizeof path, "Machine\\SOFTWARE\\Classes\\CLSID\\%s", cls[i][0]);
        RegKey *c = kpath(path, false);
        if (!has_value(c, "")) kset_sz(c, "", cls[i][1], 1);
        ksnprintf(path, sizeof path, "Machine\\SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", cls[i][0]);
        RegKey *ip = kpath(path, false);
        if (!has_value(ip, "")) { kset_sz(ip, "", "msxml6.dll", 1); kset_sz(ip, "ThreadingModel", "Both", 1); }
        if (!cls[i][2]) continue;
        ksnprintf(path, sizeof path, "Machine\\SOFTWARE\\Classes\\CLSID\\%s\\ProgID", cls[i][0]);
        RegKey *pk = kpath(path, false);
        if (!has_value(pk, "")) kset_sz(pk, "", cls[i][2], 1);
        if (cls[i][3]) {
            ksnprintf(path, sizeof path, "Machine\\SOFTWARE\\Classes\\CLSID\\%s\\VersionIndependentProgID", cls[i][0]);
            RegKey *vk = kpath(path, false);
            if (!has_value(vk, "")) kset_sz(vk, "", cls[i][3], 1);
            ksnprintf(path, sizeof path, "Machine\\SOFTWARE\\Classes\\%s\\CurVer", cls[i][3]);
            RegKey *cv = kpath(path, false);
            if (!has_value(cv, "")) kset_sz(cv, "", cls[i][2], 1);
        }
        ksnprintf(path, sizeof path, "Machine\\SOFTWARE\\Classes\\%s", cls[i][2]);
        RegKey *p = kpath(path, false);
        if (!has_value(p, "")) kset_sz(p, "", cls[i][1], 1);
        ksnprintf(path, sizeof path, "Machine\\SOFTWARE\\Classes\\%s\\CLSID", cls[i][2]);
        RegKey *pc = kpath(path, false);
        if (!has_value(pc, "")) kset_sz(pc, "", cls[i][0], 1);
    }
}

static void defaults(void)
{
    /* HKLM\SOFTWARE */
    RegKey *nt = kpath("Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", false);
    if (!has_value(nt, "ProductName")) {
        kset_sz(nt, "ProductName", "NovaOS", 1);
        kset_sz(nt, "CurrentVersion", "6.3", 1);
        kset_sz(nt, "CurrentBuild", "18362", 1);
        kset_sz(nt, "CurrentBuildNumber", "18362", 1);
        kset_dword(nt, "CurrentMajorVersionNumber", 10);
        kset_dword(nt, "CurrentMinorVersionNumber", 0);
        kset_sz(nt, "DisplayVersion", "1.0", 1);
        kset_sz(nt, "EditionID", "Core", 1);
        kset_sz(nt, "InstallationType", "Client", 1);
        kset_sz(nt, "RegisteredOwner", "dean", 1);
        kset_sz(nt, "SystemRoot", "C:\\Windows", 1);
        kset_sz(nt, "PathName", "C:\\Windows", 1);
    }
    RegKey *cv = kpath("Machine\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion", false);
    if (!has_value(cv, "ProgramFilesDir")) {
        kset_sz(cv, "ProgramFilesDir", "C:\\Programs", 1);
        kset_sz(cv, "ProgramFilesDir (x86)", "C:\\Programs", 1);
        kset_sz(cv, "CommonFilesDir", "C:\\Programs\\Common Files", 1);
        kset_sz(cv, "ProgramFilesPath", "%ProgramFiles%", 2);
    }
    kpath("Machine\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", false);
    kpath("Machine\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", false);
    kpath("Machine\\SOFTWARE\\Classes\\CLSID", false);
    kpath("Machine\\SOFTWARE\\Classes\\Interface", false);
    /* shortcuts: shell32's ShellLink class (a bare DLL name: the 32-bit
     * or 64-bit shell32, whichever the program is) */
    RegKey *sl = kpath("Machine\\SOFTWARE\\Classes\\CLSID\\{00021401-0000-0000-C000-000000000046}\\InprocServer32", false);
    if (!has_value(sl, "")) { kset_sz(sl, "", "shell32.dll", 1); kset_sz(sl, "ThreadingModel", "Both", 1); }
    RegKey *slc = kpath("Machine\\SOFTWARE\\Classes\\CLSID\\{00021401-0000-0000-C000-000000000046}", false);
    if (!has_value(slc, "")) kset_sz(slc, "", "Shortcut", 1);
    /* the file dialogs (comdlg32's FileOpenDialog and FileSaveDialog) */
    static const char *const fdlg[][2] = { { "{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}", "File Open Dialog" },
                                           { "{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}", "File Save Dialog" } };
    for (int i = 0; i < 2; i++) {
        char k[96];
        int n = 0;
        for (const char *s = "Machine\\SOFTWARE\\Classes\\CLSID\\"; *s; s++) k[n++] = *s;
        for (const char *s = fdlg[i][0]; *s; s++) k[n++] = *s;
        k[n] = 0;
        RegKey *c = kpath(k, false);
        if (!has_value(c, "")) kset_sz(c, "", fdlg[i][1], 1);
        for (const char *s = "\\InprocServer32"; *s; s++) k[n++] = *s;
        k[n] = 0;
        RegKey *ip = kpath(k, false);
        if (!has_value(ip, "")) { kset_sz(ip, "", "comdlg32.dll", 1); kset_sz(ip, "ThreadingModel", "Apartment", 1); }
    }

    /* internet shortcuts (.url files): shell32's InternetShortcut class */
    RegKey *is = kpath("Machine\\SOFTWARE\\Classes\\CLSID\\{FBF23B40-E3F0-101B-8488-00AA003E56F8}\\InprocServer32", false);
    if (!has_value(is, "")) { kset_sz(is, "", "shell32.dll", 1); kset_sz(is, "ThreadingModel", "Apartment", 1); }
    RegKey *isc = kpath("Machine\\SOFTWARE\\Classes\\CLSID\\{FBF23B40-E3F0-101B-8488-00AA003E56F8}", false);
    if (!has_value(isc, "")) kset_sz(isc, "", "Internet Shortcut", 1);
    /* the audio endpoints (mmdevapi's MMDeviceEnumerator) */
    RegKey *mmd = kpath("Machine\\SOFTWARE\\Classes\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\InprocServer32", false);
    if (!has_value(mmd, "")) { kset_sz(mmd, "", "mmdevapi.dll", 1); kset_sz(mmd, "ThreadingModel", "Both", 1); }
    /* DirectSound and DirectSoundCapture (dsound.dll), so CoCreateInstance finds them */
    static const char *const ds_clsids[] = {
        "{47D4D946-62E8-11CF-93BC-444553540000}", "{3901CC3F-84B5-4FA4-BA35-AA8172B8A09B}",
        "{B0210780-89CD-11D0-AF08-00A0C925CD16}", "{E4BCAC13-7F99-4908-9A8E-74E3BF24B6E1}",
    };
    for (unsigned i = 0; i < sizeof ds_clsids / sizeof ds_clsids[0]; i++) {
        char path[96];
        ksnprintf(path, sizeof path, "Machine\\SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", ds_clsids[i]);
        RegKey *k = kpath(path, false);
        if (!has_value(k, "")) { kset_sz(k, "", "dsound.dll", 1); kset_sz(k, "ThreadingModel", "Both", 1); }
    }
    /* XAudio2 2.7 (the DirectX SDK's): the engine, its debug twin and its two effects */
    static const char *const xa27_clsids[] = {
        "{5A508685-A254-4FBA-9B82-9A24B00306AF}", "{DB05EA35-0329-4D4B-A53A-6DEAD03D3852}",
        "{CAC1105F-619B-4D04-831A-44E1CBF12D57}", "{6A93130E-1D53-41D1-A9CF-E758800BB179}",
    };
    for (unsigned i = 0; i < sizeof xa27_clsids / sizeof xa27_clsids[0]; i++) {
        char path[96];
        ksnprintf(path, sizeof path, "Machine\\SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", xa27_clsids[i]);
        RegKey *k = kpath(path, false);
        if (!has_value(k, "")) { kset_sz(k, "", "xaudio2_7.dll", 1); kset_sz(k, "ThreadingModel", "Both", 1); }
    }
    msxml_classes();
    RegKey *lnk = kpath("Machine\\SOFTWARE\\Classes\\.lnk", false);
    if (!has_value(lnk, "")) kset_sz(lnk, "", "lnkfile", 1);
    RegKey *txt = kpath("Machine\\SOFTWARE\\Classes\\.txt", false);
    if (!has_value(txt, "")) { kset_sz(txt, "", "txtfile", 1); kset_sz(txt, "Content Type", "text/plain", 1); }

    /* HKLM\SYSTEM */
    RegKey *env = kpath("Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", false);
    if (!has_value(env, "Path")) {
        kset_sz(env, "Path", "C:\\Programs;C:\\Windows\\System32", 2);
        kset_sz(env, "PATHEXT", ".COM;.EXE;.BAT;.CMD", 1);
        kset_sz(env, "OS", "NovaOS", 1);
        kset_sz(env, "PROCESSOR_ARCHITECTURE", "AMD64", 1);
        kset_sz(env, "NUMBER_OF_PROCESSORS", "1", 1);
        kset_sz(env, "TEMP", "C:\\Temp", 2);
        kset_sz(env, "TMP", "C:\\Temp", 2);
        kset_sz(env, "windir", "C:\\Windows", 2);
    }
    RegKey *cn = kpath("Machine\\SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName", false);
    if (!has_value(cn, "ComputerName")) kset_sz(cn, "ComputerName", "NOVA-PC", 1);
    RegKey *tz = kpath("Machine\\SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation", false);
    if (!has_value(tz, "StandardStart")) {                /* (older hives held only the first two) */
        const TzZone *utc = TzAt(TzFind("UTC"));
        TzTzi tzi;
        TzToTzi(utc, &tzi);
        kset_sz(tz, "TimeZoneKeyName", utc->key, 1);
        kset_dword(tz, "Bias", (UINT32)tzi.bias);
        kset_sz(tz, "StandardName", utc->std, 1);
        kset_dword(tz, "StandardBias", 0);
        kset_sz(tz, "DaylightName", utc->dlt, 1);
        kset_dword(tz, "DaylightBias", 0);
        UINT16 ss[13] = { 'S','t','a','n','d','a','r','d','S','t','a','r','t' };
        UINT16 ds[13] = { 'D','a','y','l','i','g','h','t','S','t','a','r','t' };
        set_value(tz, ss, 13, 3 /* REG_BINARY */, &tzi.std_date, sizeof(tzi.std_date));
        set_value(tz, ds, 13, 3 /* REG_BINARY */, &tzi.dst_date, sizeof(tzi.dst_date));
        kset_dword(tz, "DynamicDaylightTimeDisabled", 0);
    }
    time_zones();
    keyboard_layouts();
    RegKey *nls = kpath("Machine\\SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage", false);
    if (!has_value(nls, "ACP")) { kset_sz(nls, "ACP", "65001", 1); kset_sz(nls, "OEMCP", "65001", 1); }
    kpath("Machine\\SYSTEM\\CurrentControlSet\\Services", false);

    /* HKLM\HARDWARE: rebuilt every boot */
    add_cpus(1);                           /* the others once they start */
    RegKey *bios = kpath("Machine\\HARDWARE\\DESCRIPTION\\System\\BIOS", true);
    kset_sz(bios, "SystemManufacturer", "QEMU", 1);
    kset_sz(bios, "SystemProductName", "NovaOS PC", 1);

    /* HKEY_USERS */
    const char *users[2] = { "User\\" USER_SID, "User\\.DEFAULT" };
    for (int i = 0; i < 2; i++) {
        char p[160];
        ksnprintf(p, sizeof(p), "%s\\Environment", users[i]);
        RegKey *uenv = kpath(p, false);
        if (i == 0 && !has_value(uenv, "TEMP")) { kset_sz(uenv, "TEMP", "C:\\Temp", 2); kset_sz(uenv, "TMP", "C:\\Temp", 2); }
        ksnprintf(p, sizeof(p), "%s\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", users[i]);
        kpath(p, false);
        ksnprintf(p, sizeof(p), "%s\\Software\\Classes", users[i]);
        kpath(p, false);
        ksnprintf(p, sizeof(p), "%s\\Control Panel\\International", users[i]);
        RegKey *intl = kpath(p, false);
        if (!has_value(intl, "LocaleName")) {
            kset_sz(intl, "LocaleName", "en-US", 1);
            kset_sz(intl, "sDecimal", ".", 1);
            kset_sz(intl, "sThousand", ",", 1);
            kset_sz(intl, "sShortDate", "M/d/yyyy", 1);
            kset_sz(intl, "sTimeFormat", "h:mm:ss tt", 1);
            kset_sz(intl, "sCountry", "United States", 1);
        }
        ksnprintf(p, sizeof(p), "%s\\Keyboard Layout\\Preload", users[i]);
        RegKey *pre = kpath(p, false);
        if (!has_value(pre, "1")) kset_sz(pre, "1", "00000409", 1);
        ksnprintf(p, sizeof(p), "%s\\Control Panel\\Desktop", users[i]);
        RegKey *desk = kpath(p, false);
        if (!has_value(desk, "WheelScrollLines")) { kset_sz(desk, "WheelScrollLines", "3", 1); kset_dword(desk, "LogPixels", 96); }
        ksnprintf(p, sizeof(p), "%s\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", users[i]);
        RegKey *sf = kpath(p, false);
        if (!has_value(sf, "Personal")) {
            kset_sz(sf, "Personal", "C:\\Documents", 1);
            kset_sz(sf, "My Pictures", "C:\\Pictures", 1);
            kset_sz(sf, "Desktop", "C:\\Desktop", 1);
            kset_sz(sf, "AppData", "C:\\AppData\\Roaming", 1);
            kset_sz(sf, "Local AppData", "C:\\AppData\\Local", 1);
        }
    }
}

/* -----------------------------------------------------------------------
 * Saving and loading
 * ----------------------------------------------------------------------- */
typedef struct { UINT8 *p; UINT32 n, cap; bool bad; } Buf;

static void put(Buf *b, const void *d, UINT32 n)
{
    if (b->bad) return;
    if (b->n + n > b->cap) {
        UINT32 cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->n + n) cap *= 2;
        if (cap > RAMFS_FILE_MAX) { b->bad = true; return; }
        UINT8 *q = kmalloc(cap);
        if (!q) { b->bad = true; return; }
        if (b->n) memcpy(q, b->p, b->n);
        kfree(b->p);
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put32(Buf *b, UINT32 v) { put(b, &v, 4); }

/* 'K' depth name, then its values 'V' type name data, then children */
static void save_key(Buf *b, RegKey *k, UINT32 depth)
{
    if (k->vol) return;
    put(b, "K", 1);
    put32(b, depth);
    put32(b, k->nlen);
    put(b, k->name, 2 * k->nlen);
    for (RegValue *v = k->values; v; v = v->next) {
        put(b, "V", 1);
        put32(b, v->type);
        put32(b, v->nlen);
        put(b, v->name, 2 * v->nlen);
        put32(b, v->len);
        put(b, v->data, v->len);
    }
    for (RegKey *c = k->child; c; c = c->next) save_key(b, c, depth + 1);
}

static bool load_hive(const UINT8 *d, UINT32 n)
{
    if (n < 8 || memcmp(d, "NOVAREG1", 8)) return false;
    RegKey *stack[64];
    UINT32 o = 8;
    #define NEED(k) do { if (o + (k) > n) return false; } while (0)
    int top = -1;
    while (o < n) {
        char tag = (char)d[o++];
        if (tag == 'E') return true;
        if (tag == 'K') {
            NEED(8);
            UINT32 depth, nlen;
            memcpy(&depth, d + o, 4); memcpy(&nlen, d + o + 4, 4); o += 8;
            NEED(2 * nlen);
            if (depth >= 64 || nlen > NAME_MAX_CHARS || (int)depth > top + 1) return false;
            RegKey *k;
            if (depth == 0) k = g_root;
            else {
                UINT16 nm[NAME_MAX_CHARS + 1];
                memcpy(nm, d + o, 2 * nlen);
                RegKey *parent = stack[depth - 1];
                k = find_child(parent, nm, nlen);
                if (!k) k = add_child(parent, nm, nlen, false);
                if (!k) return false;
            }
            o += 2 * nlen;
            stack[depth] = k;
            top = (int)depth;
        } else if (tag == 'V') {
            NEED(8);
            UINT32 type, nlen, len;
            memcpy(&type, d + o, 4); memcpy(&nlen, d + o + 4, 4); o += 8;
            if (nlen > VALUE_NAME_MAX || top < 0) return false;
            NEED(2 * nlen + 4);
            const UINT16 *nm = (const UINT16 *)(d + o);
            o += 2 * nlen;
            memcpy(&len, d + o, 4); o += 4;
            if (len > DATA_MAX) return false;
            NEED(len);
            UINT16 *nmc = dup16(nm, nlen);
            if (!nmc || !set_value(stack[top], nmc, nlen, type, d + o, len)) { kfree(nmc); return false; }
            kfree(nmc);
            o += len;
        } else {
            return false;
        }
    }
    #undef NEED
    return false;
}

/* Write the hive when it has been quiet for a second (desktop thread) */
static void release_spent(void);

void um_registry_poll(void)
{
    release_spent();
    if (!g_dirty || sched_ticks() - g_dirty_ticks < 100) return;
    Buf b = { 0 };
    um_lock_excl(&g_reg);
    g_dirty = false;
    put(&b, "NOVAREG1", 8);
    save_key(&b, g_root, 0);
    put(&b, "E", 1);
    um_unlock_excl(&g_reg);
    if (b.bad) { kprintf("[REG] The registry is too large to save\n"); kfree(b.p); return; }
    FsLock();
    RamNode *dir = RamfsResolve(NULL, "\\Windows\\System32");
    RamNode *cfg = dir ? RamfsCreate(dir, "config", true) : NULL;
    RamNode *f = cfg ? RamfsCreate(cfg, "REGISTRY.DAT", false) : NULL;
    bool ok = f && RamfsWrite(f, (const char *)b.p, b.n);
    FsUnlock();
    if (!ok) kprintf("[REG] Saving the registry failed\n");
    kfree(b.p);
}

/* Write the hive now if it has unsaved changes (before a restart) */
void um_registry_flush(void)
{
    if (!g_dirty) return;
    g_dirty_ticks = 0;
    um_registry_poll();
}

void um_registry_init(void)
{
    g_root = kzalloc(sizeof(*g_root));
    static const UINT16 name[] = { 'R', 'E', 'G', 'I', 'S', 'T', 'R', 'Y' };
    g_root->name = dup16(name, 8);
    g_root->nlen = 8;
    g_root->refs = 1;
    RamNode *f = RamfsResolve(NULL, HIVE_PATH);
    bool loaded = f && !f->dir && load_hive((const UINT8 *)f->data, f->size);
    if (f && !loaded) kprintf("[REG] %s is damaged; starting from the defaults\n", HIVE_PATH);
    defaults();
    g_dirty = !loaded;
    g_dirty_ticks = 0;
    kprintf("[REG] Registry ready (%s)\n", loaded ? "loaded from C:\\Windows\\System32\\config" : "defaults");
}

/* -----------------------------------------------------------------------
 * Key handles: kernel objects referencing a key
 * ----------------------------------------------------------------------- */
static void key_ob_destroy(UmObject *o)
{
    um_lock_shared(&g_reg);                                 /* (the last reference: a deleted key's) */
    key_unref((RegKey *)o->ptr);
    um_unlock_shared(&g_reg);
}

/* Lock order: a process's handle lock, then g_reg (closing a key handle
 * takes g_reg under it) — so handles are looked up and made without g_reg. */
static UINT64 new_key_handle(UmProcess *p, RegKey *k)
{
    UmObject *o = kzalloc(sizeof(*o));
    if (!o) { um_lock_shared(&g_reg); key_unref(k); um_unlock_shared(&g_reg); return 0; }
    o->type = UO_KEY;
    o->refs = 1;
    o->signaled = true;
    o->ptr = k;
    o->destroy = key_ob_destroy;                            /* the caller referenced @k for us */
    o->free_unlocked = true;                                /* (g_reg) */
    UINT64 h = um_handle_new_object(p, o);
    um_ob_unref(o);                                         /* the handle holds it (or it goes now) */
    return h;
}

/* The key behind a handle, referenced; release with key_release */
static RegKey *key_of(UINT64 h, UmObject **ob)
{
    UmObject *o = um_handle_object(UmCurrent(), h, UO_KEY);
    if (!o) return NULL;
    *ob = o;
    return (RegKey *)o->ptr;
}

/* -----------------------------------------------------------------------
 * Arguments
 * ----------------------------------------------------------------------- */
/* A UNICODE_STRING from user memory into @w (up to @cap characters) */
static UINT32 get_ustr(UINT64 us_ptr, UINT16 *w, UINT32 cap, UINT32 *n)
{
    UINT64 us[2];
    *n = 0;
    if (!us_ptr) return ST_SUCCESS;
    if (!NT_SUCCESS(CopyFromUser(us, (const void *)(uintptr_t)us_ptr, sizeof(us)))) return UM_STATUS_ACCESS_VIOLATION;
    UINT32 len = (UINT32)(us[0] & 0xFFFF) / 2;
    if (len > cap) return ST_OBJECT_NAME_INVALID;
    if (len && !NT_SUCCESS(CopyFromUser(w, (const void *)(uintptr_t)us[1], 2 * (size_t)len))) return UM_STATUS_ACCESS_VIOLATION;
    *n = len;
    return ST_SUCCESS;
}

/* OBJECT_ATTRIBUTES → the starting key and the path below it */
static UINT32 get_oa(UINT64 oa_ptr, RegKey **start, UmObject **root_ob, UINT16 *path, UINT32 cap, UINT32 *n)
{
    UINT64 oa[6];
    *root_ob = NULL;
    if (!oa_ptr || !NT_SUCCESS(CopyFromUser(oa, (const void *)(uintptr_t)oa_ptr, sizeof(oa)))) return UM_STATUS_ACCESS_VIOLATION;
    UINT32 st = get_ustr(oa[2], path, cap, n);
    if (st) return st;
    if (oa[1]) {
        RegKey *k = key_of(oa[1], root_ob);
        if (!k) return ST_INVALID_HANDLE;
        *start = k;
        return ST_SUCCESS;
    }
    /* absolute: "\Registry\..." */
    static const UINT16 pre[] = { '\\', 'R', 'E', 'G', 'I', 'S', 'T', 'R', 'Y' };
    if (*n < 9 || !name_eq(path, 9, pre, 9) || (*n > 9 && path[9] != '\\')) return ST_OBJECT_PATH_NOT_FOUND;
    memmove(path, path + 9, 2 * (size_t)(*n - 9));
    *n -= 9;
    *start = g_root;
    return ST_SUCCESS;
}

/* The length (characters) of OBJECT_ATTRIBUTES' name; 0 if unreadable */
static UINT32 oa_name_chars(UINT64 oa_ptr)
{
    UINT64 oa[3], us[1];
    if (!oa_ptr || !NT_SUCCESS(CopyFromUser(oa, (const void *)(uintptr_t)oa_ptr, sizeof(oa))) || !oa[2] ||
        !NT_SUCCESS(CopyFromUser(us, (const void *)(uintptr_t)oa[2], sizeof(us)))) return 0;
    return (UINT32)(us[0] & 0xFFFF) / 2;
}

/* A value name's buffer: @small (SMALL_NAME characters and the NUL) when
 * the name fits, else from the heap; release with name_done, and read the
 * name with name_cap characters at most (it may change meanwhile) */
#define SMALL_NAME 255
static UINT16 *name_buf(UINT64 us_ptr, UINT16 *small)
{
    UINT64 us;
    UINT32 len = us_ptr && NT_SUCCESS(CopyFromUser(&us, (const void *)(uintptr_t)us_ptr, 8)) ? (UINT32)(us & 0xFFFF) / 2 : 0;
    return len <= SMALL_NAME ? small : kmalloc(2 * (VALUE_NAME_MAX + 1));
}
static UINT32 name_cap(const UINT16 *name, const UINT16 *small) { return name == small ? SMALL_NAME : VALUE_NAME_MAX; }
static void name_done(UINT16 *name, UINT16 *small) { if (name != small) kfree(name); }

static bool put_ret(UINT64 ptr, UINT32 v) { return !ptr || NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, &v, 4)); }

/* Copy an info block: whole, partial (BUFFER_OVERFLOW) or nothing (TOO_SMALL) */
static UINT32 give(UINT64 out, UINT32 cap, const UINT8 *info, UINT32 need, UINT32 fixed, UINT64 ret_ptr)
{
    if (!put_ret(ret_ptr, need)) return UM_STATUS_ACCESS_VIOLATION;
    if (cap < fixed) return ST_BUFFER_TOO_SMALL;
    UINT32 k = cap < need ? cap : need;
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)out, info, k))) return UM_STATUS_ACCESS_VIOLATION;
    return cap < need ? ST_BUFFER_OVERFLOW : ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * The services
 * ----------------------------------------------------------------------- */

static UINT32 open_or_create(UINT64 handle_ptr, UINT64 oa_ptr, bool create, UINT32 options, UINT64 disp_ptr)
{
    UmProcess *p = UmCurrent();
    RegKey *start, *k = NULL;
    UmObject *rob;
    UINT32 n;
    UINT16 small[256], *path = small;                       /* (longer: from the heap) */
    if (oa_name_chars(oa_ptr) > 255 && !(path = kmalloc(2 * 4096))) return ST_NO_MEMORY;
    UINT32 st = get_oa(oa_ptr, &start, &rob, path, path == small ? 255 : 4095, &n);
    bool created = false;
    /* A key that is there: found side by side with other readers */
    um_lock_shared(&g_reg);
    if (!st && start->deleted) st = ST_KEY_DELETED;
    UINT32 found = st ? st : walk(start, path, n, false, false, &k, NULL);
    if (!found) __atomic_add_fetch(&k->refs, 1, __ATOMIC_RELAXED);   /* for the new handle */
    um_unlock_shared(&g_reg);
    if (!st && found && create) {                           /* to be made: alone */
        um_lock_excl(&g_reg);
        st = start->deleted ? ST_KEY_DELETED : walk(start, path, n, true, (options & 1) != 0 /* REG_OPTION_VOLATILE */, &k, &created);
        if (!st) __atomic_add_fetch(&k->refs, 1, __ATOMIC_RELAXED);
        um_unlock_excl(&g_reg);
    } else if (!st) st = found;
    if (path != small) kfree(path);
    if (rob) um_ob_unref(rob);
    UINT64 h = 0;
    if (!st) { h = new_key_handle(p, k); if (!h) st = ST_TOO_MANY_HANDLES; }
    if (st) return st;
    UINT32 disp = created ? 1 : 2;                          /* REG_CREATED_NEW_KEY / REG_OPENED_EXISTING_KEY */
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)handle_ptr, &h, 8)) || !put_ret(disp_ptr, disp)) {
        um_close_handle(h);
        return UM_STATUS_ACCESS_VIOLATION;
    }
    return ST_SUCCESS;
}

/* NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG TitleIndex, PUNICODE_STRING Class, ULONG Options, PULONG Disposition) */
static UINT64 sys_create_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a4;
    return open_or_create(a1, a3, true, (UINT32)um_stack_arg(6), um_stack_arg(7));
}

/* NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES) and NtOpenKeyEx(+ ULONG OpenOptions) */
static UINT64 sys_open_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { (void)a2; (void)a4; return open_or_create(a1, a3, false, 0, 0); }

/* NtDeleteKey(HANDLE): only a key without subkeys */
static UINT64 sys_delete_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmObject *o;
    RegKey *k = key_of(a1, &o);
    um_lock_excl(&g_reg);
    UINT32 st = !k ? ST_INVALID_HANDLE : k->deleted ? ST_KEY_DELETED : (k->child || k == g_root || k->parent == g_root) ? ST_CANNOT_DELETE : ST_SUCCESS;
    if (!st) detach(k);
    um_unlock_excl(&g_reg);
    if (k) um_ob_unref(o);
    return st;
}

/* NtSetValueKey(HANDLE, PUNICODE_STRING Name, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG Size) */
static UINT64 sys_set_value_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3;
    UINT64 data_ptr = um_stack_arg(5);
    UINT32 size = (UINT32)um_stack_arg(6);
    if (size > DATA_MAX) return ST_INVALID_PARAMETER;
    UINT16 small[SMALL_NAME + 1], *name = name_buf(a2, small);
    if (!name) return ST_NO_MEMORY;
    UINT8 sdata[64], *data = size <= sizeof(sdata) ? sdata : kmalloc(size);   /* (small: on the stack) */
    if (!data) { name_done(name, small); return ST_NO_MEMORY; }
    if (size && !NT_SUCCESS(CopyFromUser(data, (const void *)(uintptr_t)data_ptr, size))) { if (data != sdata) kfree(data); name_done(name, small); return UM_STATUS_ACCESS_VIOLATION; }
    UmObject *o;
    UINT32 n, st = get_ustr(a2, name, name_cap(name, small), &n);
    RegKey *k = st ? NULL : key_of(a1, &o);
    um_lock_shared(&g_reg);
    if (k) um_lock(key_lock(k));
    if (!st) st = !k ? ST_INVALID_HANDLE : k->deleted ? ST_KEY_DELETED : set_value(k, name, n, (UINT32)a4, data, size) ? ST_SUCCESS : ST_NO_MEMORY;
    if (k) um_unlock(key_lock(k));
    um_unlock_shared(&g_reg);
    if (k) um_ob_unref(o);
    if (data != sdata) kfree(data);
    name_done(name, small);
    return st;
}

/* KEY_VALUE_{BASIC 0, FULL 1, PARTIAL 2}_INFORMATION for @v */
static UINT32 value_info(RegValue *v, UINT32 cls, UINT64 out, UINT32 cap, UINT64 ret_ptr)
{
    UINT32 need, fixed;
    UINT8 small[256], *b;                   /* (larger: from the heap) */
#define INFO_BUF(n) ((n) <= sizeof(small) ? small : kmalloc(n))
    switch (cls) {
    case 0:
        fixed = 12; need = fixed + 2 * v->nlen;
        b = INFO_BUF(need);
        if (!b) return ST_NO_MEMORY;
        memset(b, 0, 4); memcpy(b + 4, &v->type, 4);
        { UINT32 nl = 2 * v->nlen; memcpy(b + 8, &nl, 4); }
        memcpy(b + 12, v->name, 2 * v->nlen);
        break;
    case 1: {
        fixed = 20;
        UINT32 doff = (fixed + 2 * v->nlen + 7) & ~7u;
        need = doff + v->len;
        b = INFO_BUF(need);
        if (!b) return ST_NO_MEMORY;
        memset(b, 0, need);
        UINT32 nl = 2 * v->nlen;
        memcpy(b + 4, &v->type, 4); memcpy(b + 8, &doff, 4); memcpy(b + 12, &v->len, 4); memcpy(b + 16, &nl, 4);
        memcpy(b + 20, v->name, nl);
        if (v->len) memcpy(b + doff, v->data, v->len);
        break;
    }
    case 2:
        fixed = 12; need = fixed + v->len;
        b = INFO_BUF(need);
        if (!b) return ST_NO_MEMORY;
        memset(b, 0, 4); memcpy(b + 4, &v->type, 4); memcpy(b + 8, &v->len, 4);
        if (v->len) memcpy(b + 12, v->data, v->len);
        break;
    default:
        return ST_INVALID_INFO_CLASS;
    }
#undef INFO_BUF
    UINT32 st = give(out, cap, b, need, fixed, ret_ptr);
    if (b != small) kfree(b);
    return st;
}

/* NtQueryValueKey(HANDLE, PUNICODE_STRING, CLASS, PVOID, ULONG Length, PULONG ResultLength) */
static UINT64 sys_query_value_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT16 small[SMALL_NAME + 1], *name = name_buf(a2, small);
    if (!name) return ST_NO_MEMORY;
    UmObject *o;
    UINT32 n, st = get_ustr(a2, name, name_cap(name, small), &n);
    RegKey *k = st ? NULL : key_of(a1, &o);
    um_lock_shared(&g_reg);
    if (k) um_lock(key_lock(k));
    if (!st) {
        RegValue *v = !k ? NULL : k->deleted ? NULL : find_value(k, name, n);
        st = !k ? ST_INVALID_HANDLE : k->deleted ? ST_KEY_DELETED : !v ? ST_OBJECT_NAME_NOT_FOUND
             : value_info(v, (UINT32)a3, a4, (UINT32)um_stack_arg(5), um_stack_arg(6));
    }
    if (k) um_unlock(key_lock(k));
    um_unlock_shared(&g_reg);
    if (k) um_ob_unref(o);
    name_done(name, small);
    return st;
}

/* NtEnumerateValueKey(HANDLE, ULONG Index, CLASS, PVOID, ULONG Length, PULONG ResultLength) */
static UINT64 sys_enum_value_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *o;
    RegKey *k = key_of(a1, &o);
    um_lock_shared(&g_reg);
    UINT32 st;
    if (!k) st = ST_INVALID_HANDLE;
    else if (k->deleted) st = ST_KEY_DELETED;
    else {
        um_lock(key_lock(k));
        RegValue *v = k->values;
        for (UINT64 i = 0; v && i < a2; i++) v = v->next;
        st = v ? value_info(v, (UINT32)a3, a4, (UINT32)um_stack_arg(5), um_stack_arg(6)) : ST_NO_MORE_ENTRIES;
        um_unlock(key_lock(k));
    }
    um_unlock_shared(&g_reg);
    if (k) um_ob_unref(o);
    return st;
}

/* NtDeleteValueKey(HANDLE, PUNICODE_STRING) */
static UINT64 sys_delete_value_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UINT16 small[SMALL_NAME + 1], *name = name_buf(a2, small);
    if (!name) return ST_NO_MEMORY;
    UmObject *o;
    UINT32 n, st = get_ustr(a2, name, name_cap(name, small), &n);
    RegKey *k = st ? NULL : key_of(a1, &o);
    um_lock_shared(&g_reg);
    if (!st) {
        if (!k) st = ST_INVALID_HANDLE;
        else if (k->deleted) st = ST_KEY_DELETED;
        else {
            um_lock(key_lock(k));
            RegValue **pp = &k->values;
            while (*pp && !name_eq((*pp)->name, (*pp)->nlen, name, n)) pp = &(*pp)->next;
            if (!*pp) st = ST_OBJECT_NAME_NOT_FOUND;
            else { RegValue *v = *pp; *pp = v->next; free_value(v); touch(k); notify(k, CHANGE_LAST_SET); }
            um_unlock(key_lock(k));
        }
    }
    um_unlock_shared(&g_reg);
    if (k) um_ob_unref(o);
    name_done(name, small);
    return st;
}

/* The full name "\REGISTRY\MACHINE\..." of @k into @w; returns characters */
static UINT32 full_name(RegKey *k, UINT16 *w, UINT32 cap)
{
    RegKey *chain[128];
    int d = 0;
    for (RegKey *c = k; c && d < 128; c = c->parent) chain[d++] = c;
    UINT32 o = 0;
    for (int i = d - 1; i >= 0; i--) {
        if (o + 1 + chain[i]->nlen > cap) break;
        w[o++] = '\\';
        memcpy(w + o, chain[i]->name, 2 * (size_t)chain[i]->nlen);
        o += chain[i]->nlen;
    }
    return o;
}

/* KEY_{BASIC 0, NODE 1, FULL 2, NAME 3}_INFORMATION for @k */
static UINT32 key_info(RegKey *k, UINT32 cls, UINT64 out, UINT32 cap, UINT64 ret_ptr)
{
    UINT32 need, fixed;
    UINT8 *b;
    switch (cls) {
    case 0: case 1: {
        fixed = cls == 0 ? 16 : 24;
        need = fixed + 2 * k->nlen;
        b = kzalloc(need);
        if (!b) return ST_NO_MEMORY;
        memcpy(b, &k->wtime, 8);
        UINT32 nl = 2 * k->nlen;
        if (cls == 0) { memcpy(b + 12, &nl, 4); memcpy(b + 16, k->name, nl); }
        else { UINT32 co = 0xFFFFFFFF; memcpy(b + 12, &co, 4); memcpy(b + 20, &nl, 4); memcpy(b + 24, k->name, nl); }
        break;
    }
    case 2: {
        fixed = 44; need = fixed;
        b = kzalloc(need);
        if (!b) return ST_NO_MEMORY;
        UINT32 subkeys = 0, maxname = 0, values = 0, maxvname = 0, maxdata = 0, co = 44;
        for (RegKey *c = k->child; c; c = c->next) { subkeys++; if (2 * c->nlen > maxname) maxname = 2 * c->nlen; }
        for (RegValue *v = k->values; v; v = v->next) {
            values++;
            if (2 * v->nlen > maxvname) maxvname = 2 * v->nlen;
            if (v->len > maxdata) maxdata = v->len;
        }
        memcpy(b, &k->wtime, 8);
        memcpy(b + 12, &co, 4);
        memcpy(b + 20, &subkeys, 4); memcpy(b + 24, &maxname, 4);
        memcpy(b + 32, &values, 4); memcpy(b + 36, &maxvname, 4); memcpy(b + 40, &maxdata, 4);
        break;
    }
    case 3: {
        UINT16 *w = kmalloc(2 * 2048);
        if (!w) return ST_NO_MEMORY;
        UINT32 n = full_name(k, w, 2048);
        fixed = 4; need = 4 + 2 * n;
        b = kmalloc(need);
        if (!b) { kfree(w); return ST_NO_MEMORY; }
        UINT32 nl = 2 * n;
        memcpy(b, &nl, 4);
        memcpy(b + 4, w, nl);
        kfree(w);
        break;
    }
    default:
        return ST_INVALID_INFO_CLASS;
    }
    UINT32 st = give(out, cap, b, need, fixed, ret_ptr);
    kfree(b);
    return st;
}

/* NtEnumerateKey(HANDLE, ULONG Index, CLASS, PVOID, ULONG Length, PULONG ResultLength) */
static UINT64 sys_enum_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *o;
    RegKey *k = key_of(a1, &o);
    um_lock_shared(&g_reg);
    UINT32 st;
    if (!k) st = ST_INVALID_HANDLE;
    else if (k->deleted) st = ST_KEY_DELETED;
    else {
        RegKey *c = k->child;
        for (UINT64 i = 0; c && i < a2; i++) c = c->next;
        if (c) um_lock(key_lock(c));
        st = c ? key_info(c, (UINT32)a3, a4, (UINT32)um_stack_arg(5), um_stack_arg(6)) : ST_NO_MORE_ENTRIES;
        if (c) um_unlock(key_lock(c));
    }
    um_unlock_shared(&g_reg);
    if (k) um_ob_unref(o);
    return st;
}

/* NtQueryKey(HANDLE, CLASS, PVOID, ULONG Length, PULONG ResultLength) */
static UINT64 sys_query_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *o;
    RegKey *k = key_of(a1, &o);
    um_lock_shared(&g_reg);
    if (k) um_lock(key_lock(k));
    UINT32 st = !k ? ST_INVALID_HANDLE : k->deleted ? ST_KEY_DELETED : key_info(k, (UINT32)a2, a3, (UINT32)a4, um_stack_arg(5));
    if (k) um_unlock(key_lock(k));
    um_unlock_shared(&g_reg);
    if (k) um_ob_unref(o);
    return st;
}

/* NtFlushKey(HANDLE): save now */
static UINT64 sys_flush_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmObject *o;
    RegKey *k = key_of(a1, &o);
    if (!k) return ST_INVALID_HANDLE;
    um_ob_unref(o);
    if (g_dirty) g_dirty_ticks = 0;                         /* the next poll writes it */
    return ST_SUCCESS;
}

/* NtRenameKey(HANDLE, PUNICODE_STRING NewName) */
static UINT64 sys_rename_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UINT16 name[NAME_MAX_CHARS + 1];
    UmObject *o;
    UINT32 n, st = get_ustr(a2, name, NAME_MAX_CHARS, &n);
    RegKey *k = st ? NULL : key_of(a1, &o);
    um_lock_excl(&g_reg);
    if (!st) {
        if (!k) st = ST_INVALID_HANDLE;
        else if (k->deleted || !k->parent) st = ST_KEY_DELETED;
        else if (!n) st = ST_OBJECT_NAME_INVALID;
        else if (find_child(k->parent, name, n)) st = ST_OBJECT_NAME_COLLISION;
        else {
            UINT16 *nm = dup16(name, n);
            if (!nm) st = ST_NO_MEMORY;
            else {
                kfree(k->name); k->name = nm; k->nlen = n; touch(k); touch(k->parent);
                notify(k->parent, CHANGE_NAME);
            }
        }
    }
    um_unlock_excl(&g_reg);
    if (k) um_ob_unref(o);
    return st;
}

/* -----------------------------------------------------------------------
 * Change notification (NtNotifyChangeKey, RegNotifyChangeKeyValue): a
 * watch on a key (and with @tree its subkeys) signals its event once,
 * when something its filter names changes, and is gone.  The event is
 * the caller's (asynchronous) or the kernel's own that the call waits on.
 * ----------------------------------------------------------------------- */
typedef struct Watch {
    struct Watch *next;
    RegKey *key;                            /* referenced */
    UINT32 filter;
    bool tree;
    UmObject *ev;                           /* referenced */
    UmProcess *proc;
} Watch;

static Watch *g_watch;                      /* under g_wlock (and g_reg, shared or not) */
static Watch *g_spent;                      /* fired: their events are released outside g_reg */

static bool below(RegKey *k, RegKey *top)
{
    for (; k; k = k->parent) if (k == top) return true;
    return false;
}

static void spend(Watch *w)
{
    key_unref(w->key);
    w->key = NULL;
    w->next = g_spent;
    g_spent = w;
}

static void notify(RegKey *k, UINT32 what)
{
    if (!__atomic_load_n(&g_watch, __ATOMIC_ACQUIRE)) return;
    um_lock(&g_wlock);
    for (Watch **pp = &g_watch; *pp;) {
        Watch *w = *pp;
        if (!(w->filter & what) || !(w->key == k || (w->tree && below(k, w->key)))) { pp = &w->next; continue; }
        *pp = w->next;
        IrqState s = ob_lock();
        w->ev->signaled = true;
        um_ob_wake(w->ev);
        ob_unlock(s);
        spend(w);
    }
    um_unlock(&g_wlock);
}

/* Release fired watches (not under g_reg: an event's last reference may
 * take the big kernel lock) */
static void release_spent(void)
{
    if (!__atomic_load_n(&g_spent, __ATOMIC_ACQUIRE)) return;
    um_lock(&g_wlock);
    Watch *w = g_spent;
    g_spent = NULL;
    um_unlock(&g_wlock);
    while (w) {
        Watch *n = w->next;
        um_ob_unref(w->ev);
        kfree(w);
        w = n;
    }
}

/* A process ended: its watches go (their events are not signalled) */
void um_registry_process_gone(UmProcess *p)
{
    um_lock(&g_wlock);
    for (Watch **pp = &g_watch; *pp;) {
        Watch *w = *pp;
        if (w->proc != p) { pp = &w->next; continue; }
        *pp = w->next;
        spend(w);
    }
    um_unlock(&g_wlock);
    release_spent();
}

/* NtNotifyChangeKey(HANDLE Key, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
 *                   PIO_STATUS_BLOCK, ULONG CompletionFilter, BOOLEAN WatchTree,
 *                   PVOID Buffer, ULONG BufferSize, BOOLEAN Asynchronous) */
static UINT64 sys_notify_change_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UINT64 iosb = um_stack_arg(5);
    UINT32 filter = (UINT32)um_stack_arg(6);
    bool tree = (um_stack_arg(7) & 0xFF) != 0, async = (um_stack_arg(10) & 0xFF) != 0;
    if (!(filter & 0xFu) || (filter & ~0x1000000Fu)) return ST_INVALID_PARAMETER;   /* (THREAD_AGNOSTIC: as it is) */
    if (async && !a2) return a3 ? 0xC00000BBu /* NOT_SUPPORTED: APC completion */ : ST_INVALID_PARAMETER;
    UmObject *ev;
    if (async) {
        ev = um_handle_object(UmCurrent(), a2, UO_EVENT);
        if (!ev) return ST_INVALID_HANDLE;
        IrqState s = ob_lock();
        ev->signaled = false;                               /* as Windows: reset when the watch starts */
        ob_unlock(s);
    } else {
        ev = kzalloc(sizeof(*ev));
        if (!ev) return ST_NO_MEMORY;
        ev->type = UO_EVENT;
        ev->refs = 1;
        ev->manual = true;
    }
    Watch *w = kzalloc(sizeof(*w));
    UmObject *o = NULL;
    RegKey *k = w ? key_of(a1, &o) : NULL;
    UINT32 st = !w ? ST_NO_MEMORY : !k ? ST_INVALID_HANDLE : ST_SUCCESS;
    if (!st) {
        um_lock_shared(&g_reg);                             /* (deleting a key is exclusive) */
        um_lock(&g_wlock);
        if (k->deleted) st = ST_KEY_DELETED;
        else {
            __atomic_add_fetch(&k->refs, 1, __ATOMIC_RELAXED);
            w->key = k;
            w->filter = filter;
            w->tree = tree;
            w->ev = async ? ev : um_ob_ref(ev);             /* sync: the watch's reference and ours */
            w->proc = UmCurrent();
            w->next = g_watch;
            __atomic_store_n(&g_watch, w, __ATOMIC_RELEASE);
        }
        um_unlock(&g_wlock);
        um_unlock_shared(&g_reg);
    }
    if (o) um_ob_unref(o);
    if (st) { kfree(w); um_ob_unref(ev); return st; }
    if (async) return 0x00000103u;                          /* STATUS_PENDING */

    st = um_wait_one(ev, -1);
    um_lock(&g_wlock);                                      /* not fired (the process is ending): drop it */
    for (Watch **pp = &g_watch; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; spend(w); break; }
    um_unlock(&g_wlock);
    release_spent();
    um_ob_unref(ev);
    if (st) return st;
    if (iosb) {
        UINT64 zero[2] = { 0, 0 };                          /* STATUS_SUCCESS, 0 bytes */
        bool wow = UmCurrent() && UmCurrent()->wow;
        if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)iosb, zero, wow ? 8 : 16))) return UM_STATUS_ACCESS_VIOLATION;
    }
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT): Session Manager's
 * PendingFileRenameOperations, carried out at boot before any program
 * runs, then deleted.  Pairs of "\??\C:\from" and "\??\C:\to" ("!"
 * first: replace an existing file), or "" to delete "from".
 * ----------------------------------------------------------------------- */
static void to_utf8(const UINT16 *w, UINT32 n, char *out, UINT32 cap)
{
    UINT32 o = 0;
    for (UINT32 i = 0; i < n && w[i] && o + 4 < cap; i++) {
        UINT32 c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
               out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
}

static const char *nt_path(const char *p)                   /* "\??\C:\x" -> "C:\x" */
{
    if (p[0] == '\\' && p[1] == '?' && p[2] == '?' && p[3] == '\\') return p + 4;
    return p;
}

static bool pending_op(const char *from, const char *to)
{
    RamNode *src = RamfsResolve(NULL, nt_path(from));
    if (!src) return false;
    if (!*to) return RamfsDelete(src);
    bool replace = *to == '!';
    if (replace) to++;
    to = nt_path(to);
    char dir[512];
    const char *slash = NULL;
    for (const char *c = to; *c; c++) if (*c == '\\' || *c == '/') slash = c;
    if (!slash) return false;
    UINT32 dl = (UINT32)(slash - to);
    if (dl >= sizeof(dir) - 1) return false;
    memcpy(dir, to, dl);
    dir[dl] = 0;
    if (dl == 2 && dir[1] == ':') { dir[2] = '\\'; dir[3] = 0; }  /* "C:" alone is the drive's root */
    RamNode *d = RamfsResolve(NULL, dir);
    return d && d->dir && RamfsRename(src, d, slash + 1, replace);
}

void um_registry_pending_renames(void)
{
    static const UINT16 val[] = { 'P','e','n','d','i','n','g','F','i','l','e','R','e','n','a','m','e','O','p','e','r','a','t','i','o','n','s' };
    const char *sm = "Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    UINT16 w[96];
    UINT32 n = 0;
    for (; sm[n]; n++) w[n] = (UINT8)sm[n];
    um_lock_excl(&g_reg);
    RegKey *k = NULL;
    RegValue *v = NULL;
    if (walk(g_root, w, n, false, false, &k, NULL) == ST_SUCCESS && k) v = find_value(k, val, 27);
    UINT16 *list = NULL;
    UINT32 len = 0;
    if (v && v->type == 7 /* REG_MULTI_SZ */ && v->len >= 2) {
        len = v->len / 2;
        list = kmalloc(2 * (size_t)len + 4);
        if (list) { memcpy(list, v->data, 2 * (size_t)len); list[len] = list[len + 1] = 0; }
    }
    if (v) {                                                /* done once, whatever happens */
        RegValue **pp = &k->values;
        while (*pp && *pp != v) pp = &(*pp)->next;
        if (*pp) { *pp = v->next; free_value(v); touch(k); }
    }
    um_unlock_excl(&g_reg);
    if (!list) return;
    char *from = kmalloc(1024), *to = kmalloc(1024);
    int done = 0, failed = 0;
    for (UINT32 i = 0; from && to && i < len && list[i];) {
        UINT32 a = i;
        while (i < len && list[i]) i++;
        to_utf8(list + a, i - a, from, 1024);
        i++;
        UINT32 b = i;
        while (i < len && list[i]) i++;
        to_utf8(list + b, i - b, to, 1024);
        i++;
        if (pending_op(from, to)) done++;
        else { failed++; kprintf("[REG] Pending %s of %s failed\n", *to ? "rename" : "delete", from); }
    }
    kprintf("[REG] Pending file operations at boot: %d done, %d failed\n", done, failed);
    kfree(from);
    kfree(to);
    kfree(list);
}

void um_registry_syscalls_init(void)
{
    um_install(SYSCALL_NtNotifyChangeKey,  sys_notify_change_key);
    um_install(SYSCALL_NtCreateKey,        sys_create_key);
    um_install(SYSCALL_NtOpenKey,          sys_open_key);
    um_install(SYSCALL_NtOpenKeyEx,        sys_open_key);
    um_install(SYSCALL_NtDeleteKey,        sys_delete_key);
    um_install(SYSCALL_NtSetValueKey,      sys_set_value_key);
    um_install(SYSCALL_NtQueryValueKey,    sys_query_value_key);
    um_install(SYSCALL_NtEnumerateValueKey, sys_enum_value_key);
    um_install(SYSCALL_NtDeleteValueKey,   sys_delete_value_key);
    um_install(SYSCALL_NtEnumerateKey,     sys_enum_key);
    um_install(SYSCALL_NtQueryKey,         sys_query_key);
    um_install(SYSCALL_NtFlushKey,         sys_flush_key);
    um_install(SYSCALL_NtRenameKey,        sys_rename_key);
}

/* -----------------------------------------------------------------------
 * The environment new processes start with (Windows' CreateEnvironmentBlock):
 * HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment,
 * then the user's HKCU\Environment.  @cb gets each value (UTF-8) with
 * whether it came from the user's key and is REG_EXPAND_SZ.
 * ----------------------------------------------------------------------- */
static void env_key(const char *path, bool user, void (*cb)(void *, const char *, const char *, bool, bool), void *ctx)
{
    UINT16 w[256];
    UINT32 n = 0;
    for (; path[n] && n < 255; n++) w[n] = (UINT8)path[n];
    RegKey *k = NULL;
    if (walk(g_root, w, n, false, false, &k, NULL) != ST_SUCCESS || !k) return;
    for (RegValue *v = k->values; v; v = v->next) {
        if ((v->type != 1 && v->type != 2) || !v->nlen) continue;
        char name[128], val[1024];
        UINT32 i = 0, o = 0;
        for (; i < v->nlen && i < 127; i++) name[i] = v->name[i] < 0x80 ? (char)v->name[i] : '?';
        name[i] = 0;
        const UINT16 *d = (const UINT16 *)v->data;
        for (i = 0; i < v->len / 2 && d[i] && o < sizeof(val) - 4; i++) {
            UINT32 c = d[i];
            if (c < 0x80) val[o++] = (char)c;
            else if (c < 0x800) { val[o++] = (char)(0xC0 | c >> 6); val[o++] = (char)(0x80 | (c & 0x3F)); }
            else { val[o++] = (char)(0xE0 | c >> 12); val[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); val[o++] = (char)(0x80 | (c & 0x3F)); }
        }
        val[o] = 0;
        cb(ctx, name, val, user, v->type == 2);
    }
}

void um_registry_environment(void (*cb)(void *, const char *, const char *, bool, bool), void *ctx)
{
    um_lock_excl(&g_reg);
    env_key("Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", false, cb, ctx);
    env_key("User\\" USER_SID "\\Environment", true, cb, ctx);
    um_unlock_excl(&g_reg);
}
