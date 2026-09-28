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
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../lib/string.h"
#include "../mm/vmm.h"

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
#define USER_SID       "S-1-5-21-1000-2000-3000-1001"

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
static UmLock g_reg;
static volatile bool g_dirty;
static UINT64 g_dirty_ticks;

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
    if (!k->vol) { g_dirty = true; g_dirty_ticks = sched_ticks(); }
}

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
    return c;
}

static void free_value(RegValue *v) { kfree(v->name); kfree(v->data); kfree(v); }

static void key_unref(RegKey *k)
{
    if (!k || --k->refs > 0) return;
    for (RegValue *v = k->values, *n; v; v = n) { n = v->next; free_value(v); }
    kfree(k->name);
    kfree(k);
}

/* Unlink a key (and its subtree) from the tree; handles keep them alive */
static void detach(RegKey *k)
{
    for (RegKey *c = k->child, *n; c; c = n) { n = c->next; detach(c); }
    k->child = NULL;
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
            if (!create) {
                /* the last component missing: NAME_NOT_FOUND, else PATH_NOT_FOUND */
                while (i < n && path[i] == '\\') i++;
                return i >= n ? ST_OBJECT_NAME_NOT_FOUND : ST_OBJECT_PATH_NOT_FOUND;
            }
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
    RegKey *txt = kpath("Machine\\SOFTWARE\\Classes\\.txt", false);
    if (!has_value(txt, "")) { kset_sz(txt, "", "txtfile", 1); kset_sz(txt, "Content Type", "text/plain", 1); }

    /* HKLM\SYSTEM */
    RegKey *env = kpath("Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", false);
    if (!has_value(env, "Path")) {
        kset_sz(env, "Path", "C:\\Programs;C:\\Windows\\System32", 2);
        kset_sz(env, "PATHEXT", ".EXE", 1);
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
    if (!has_value(tz, "TimeZoneKeyName")) { kset_sz(tz, "TimeZoneKeyName", "UTC", 1); kset_dword(tz, "Bias", 0); }
    RegKey *nls = kpath("Machine\\SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage", false);
    if (!has_value(nls, "ACP")) { kset_sz(nls, "ACP", "65001", 1); kset_sz(nls, "OEMCP", "65001", 1); }
    kpath("Machine\\SYSTEM\\CurrentControlSet\\Services", false);

    /* HKLM\HARDWARE: rebuilt every boot */
    RegKey *cpu = kpath("Machine\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", true);
    char brand[64];
    cpu_brand(brand);
    kset_sz(cpu, "ProcessorNameString", brand, 1);
    kset_sz(cpu, "Identifier", "Intel64 Family 6", 1);
    kset_dword(cpu, "~MHz", 2000);
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
void um_registry_poll(void)
{
    if (!g_dirty || sched_ticks() - g_dirty_ticks < 100) return;
    Buf b = { 0 };
    um_lock(&g_reg);
    g_dirty = false;
    put(&b, "NOVAREG1", 8);
    save_key(&b, g_root, 0);
    put(&b, "E", 1);
    um_unlock(&g_reg);
    if (b.bad) { kprintf("[REG] The registry is too large to save\n"); kfree(b.p); return; }
    RamNode *dir = RamfsResolve(NULL, "\\Windows\\System32");
    RamNode *cfg = dir ? RamfsCreate(dir, "config", true) : NULL;
    RamNode *f = cfg ? RamfsCreate(cfg, "REGISTRY.DAT", false) : NULL;
    if (!f || !RamfsWrite(f, (const char *)b.p, b.n)) kprintf("[REG] Saving the registry failed\n");
    kfree(b.p);
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
    um_lock(&g_reg);
    key_unref((RegKey *)o->ptr);
    um_unlock(&g_reg);
}

/* Lock order: a process's handle lock, then g_reg (closing a key handle
 * takes g_reg under it) — so handles are looked up and made without g_reg. */
static UINT64 new_key_handle(UmProcess *p, RegKey *k)
{
    UmObject *o = kzalloc(sizeof(*o));
    if (!o) { um_lock(&g_reg); key_unref(k); um_unlock(&g_reg); return 0; }
    o->type = UO_KEY;
    o->refs = 1;
    o->signaled = true;
    o->ptr = k;
    o->destroy = key_ob_destroy;                            /* the caller referenced @k for us */
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
    static UINT16 path[4096];
    static UmLock plk;                                      /* guards @path */
    um_lock(&plk);
    UINT32 st = get_oa(oa_ptr, &start, &rob, path, 4095, &n);
    bool created = false;
    um_lock(&g_reg);
    if (!st && start->deleted) st = ST_KEY_DELETED;
    if (!st) st = walk(start, path, n, create, (options & 1) != 0 /* REG_OPTION_VOLATILE */, &k, &created);
    if (!st) k->refs++;                                     /* for the new handle */
    um_unlock(&g_reg);
    um_unlock(&plk);
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
    um_lock(&g_reg);
    UINT32 st = !k ? ST_INVALID_HANDLE : k->deleted ? ST_KEY_DELETED : (k->child || k == g_root || k->parent == g_root) ? ST_CANNOT_DELETE : ST_SUCCESS;
    if (!st) detach(k);
    um_unlock(&g_reg);
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
    UINT16 *name = kmalloc(2 * (VALUE_NAME_MAX + 1));
    if (!name) return ST_NO_MEMORY;
    UINT8 *data = size ? kmalloc(size) : NULL;
    if (size && !data) { kfree(name); return ST_NO_MEMORY; }
    if (size && !NT_SUCCESS(CopyFromUser(data, (const void *)(uintptr_t)data_ptr, size))) { kfree(data); kfree(name); return UM_STATUS_ACCESS_VIOLATION; }
    UmObject *o;
    UINT32 n, st = get_ustr(a2, name, VALUE_NAME_MAX, &n);
    RegKey *k = st ? NULL : key_of(a1, &o);
    um_lock(&g_reg);
    if (!st) st = !k ? ST_INVALID_HANDLE : k->deleted ? ST_KEY_DELETED : set_value(k, name, n, (UINT32)a4, data, size) ? ST_SUCCESS : ST_NO_MEMORY;
    um_unlock(&g_reg);
    if (k) um_ob_unref(o);
    kfree(data);
    kfree(name);
    return st;
}

/* KEY_VALUE_{BASIC 0, FULL 1, PARTIAL 2}_INFORMATION for @v */
static UINT32 value_info(RegValue *v, UINT32 cls, UINT64 out, UINT32 cap, UINT64 ret_ptr)
{
    UINT32 need, fixed;
    UINT8 *b;
    switch (cls) {
    case 0:
        fixed = 12; need = fixed + 2 * v->nlen;
        b = kmalloc(need);
        if (!b) return ST_NO_MEMORY;
        memset(b, 0, 4); memcpy(b + 4, &v->type, 4);
        { UINT32 nl = 2 * v->nlen; memcpy(b + 8, &nl, 4); }
        memcpy(b + 12, v->name, 2 * v->nlen);
        break;
    case 1: {
        fixed = 20;
        UINT32 doff = (fixed + 2 * v->nlen + 7) & ~7u;
        need = doff + v->len;
        b = kzalloc(need);
        if (!b) return ST_NO_MEMORY;
        UINT32 nl = 2 * v->nlen;
        memcpy(b + 4, &v->type, 4); memcpy(b + 8, &doff, 4); memcpy(b + 12, &v->len, 4); memcpy(b + 16, &nl, 4);
        memcpy(b + 20, v->name, nl);
        if (v->len) memcpy(b + doff, v->data, v->len);
        break;
    }
    case 2:
        fixed = 12; need = fixed + v->len;
        b = kmalloc(need);
        if (!b) return ST_NO_MEMORY;
        memset(b, 0, 4); memcpy(b + 4, &v->type, 4); memcpy(b + 8, &v->len, 4);
        if (v->len) memcpy(b + 12, v->data, v->len);
        break;
    default:
        return ST_INVALID_INFO_CLASS;
    }
    UINT32 st = give(out, cap, b, need, fixed, ret_ptr);
    kfree(b);
    return st;
}

/* NtQueryValueKey(HANDLE, PUNICODE_STRING, CLASS, PVOID, ULONG Length, PULONG ResultLength) */
static UINT64 sys_query_value_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT16 *name = kmalloc(2 * (VALUE_NAME_MAX + 1));
    if (!name) return ST_NO_MEMORY;
    UmObject *o;
    UINT32 n, st = get_ustr(a2, name, VALUE_NAME_MAX, &n);
    RegKey *k = st ? NULL : key_of(a1, &o);
    um_lock(&g_reg);
    if (!st) {
        RegValue *v = !k ? NULL : k->deleted ? NULL : find_value(k, name, n);
        st = !k ? ST_INVALID_HANDLE : k->deleted ? ST_KEY_DELETED : !v ? ST_OBJECT_NAME_NOT_FOUND
             : value_info(v, (UINT32)a3, a4, (UINT32)um_stack_arg(5), um_stack_arg(6));
    }
    um_unlock(&g_reg);
    if (k) um_ob_unref(o);
    kfree(name);
    return st;
}

/* NtEnumerateValueKey(HANDLE, ULONG Index, CLASS, PVOID, ULONG Length, PULONG ResultLength) */
static UINT64 sys_enum_value_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *o;
    RegKey *k = key_of(a1, &o);
    um_lock(&g_reg);
    UINT32 st;
    if (!k) st = ST_INVALID_HANDLE;
    else if (k->deleted) st = ST_KEY_DELETED;
    else {
        RegValue *v = k->values;
        for (UINT64 i = 0; v && i < a2; i++) v = v->next;
        st = v ? value_info(v, (UINT32)a3, a4, (UINT32)um_stack_arg(5), um_stack_arg(6)) : ST_NO_MORE_ENTRIES;
    }
    um_unlock(&g_reg);
    if (k) um_ob_unref(o);
    return st;
}

/* NtDeleteValueKey(HANDLE, PUNICODE_STRING) */
static UINT64 sys_delete_value_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UINT16 *name = kmalloc(2 * (VALUE_NAME_MAX + 1));
    if (!name) return ST_NO_MEMORY;
    UmObject *o;
    UINT32 n, st = get_ustr(a2, name, VALUE_NAME_MAX, &n);
    RegKey *k = st ? NULL : key_of(a1, &o);
    um_lock(&g_reg);
    if (!st) {
        if (!k) st = ST_INVALID_HANDLE;
        else if (k->deleted) st = ST_KEY_DELETED;
        else {
            RegValue **pp = &k->values;
            while (*pp && !name_eq((*pp)->name, (*pp)->nlen, name, n)) pp = &(*pp)->next;
            if (!*pp) st = ST_OBJECT_NAME_NOT_FOUND;
            else { RegValue *v = *pp; *pp = v->next; free_value(v); touch(k); }
        }
    }
    um_unlock(&g_reg);
    if (k) um_ob_unref(o);
    kfree(name);
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
    um_lock(&g_reg);
    UINT32 st;
    if (!k) st = ST_INVALID_HANDLE;
    else if (k->deleted) st = ST_KEY_DELETED;
    else {
        RegKey *c = k->child;
        for (UINT64 i = 0; c && i < a2; i++) c = c->next;
        st = c ? key_info(c, (UINT32)a3, a4, (UINT32)um_stack_arg(5), um_stack_arg(6)) : ST_NO_MORE_ENTRIES;
    }
    um_unlock(&g_reg);
    if (k) um_ob_unref(o);
    return st;
}

/* NtQueryKey(HANDLE, CLASS, PVOID, ULONG Length, PULONG ResultLength) */
static UINT64 sys_query_key(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *o;
    RegKey *k = key_of(a1, &o);
    um_lock(&g_reg);
    UINT32 st = !k ? ST_INVALID_HANDLE : k->deleted ? ST_KEY_DELETED : key_info(k, (UINT32)a2, a3, (UINT32)a4, um_stack_arg(5));
    um_unlock(&g_reg);
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
    um_lock(&g_reg);
    if (!st) {
        if (!k) st = ST_INVALID_HANDLE;
        else if (k->deleted || !k->parent) st = ST_KEY_DELETED;
        else if (!n) st = ST_OBJECT_NAME_INVALID;
        else if (find_child(k->parent, name, n)) st = ST_OBJECT_NAME_COLLISION;
        else {
            UINT16 *nm = dup16(name, n);
            if (!nm) st = ST_NO_MEMORY;
            else { kfree(k->name); k->name = nm; k->nlen = n; touch(k); touch(k->parent); }
        }
    }
    um_unlock(&g_reg);
    if (k) um_ob_unref(o);
    return st;
}

void um_registry_syscalls_init(void)
{
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
