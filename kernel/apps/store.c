/*
 * store.c — App Store: a catalog of open-source Windows programs that
 * NovaOS downloads to C:\Downloads and runs unmodified.
 *
 * The catalog is read from JSON (name, publisher, download URL, and where the
 * installed program ends up).  "Get" fetches the installer over HTTP(S)
 * with the same asynchronous network operations the Terminal's wget
 * uses; "Install" runs the downloaded installer; "Open" starts the
 * program once its executable exists under C:\Programs.  The list
 * scrolls with the built-in apps' scroll bar (UiScroll, as File Explorer's).
 * The Updates page updates NovaOS itself from its update channel
 * (fs/update.c): check, download and stage, then restart.
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../net/net.h"
#include "../um/um.h"
#include "../fs/update.h"
#include "../wm/desktop.h"

/* -----------------------------------------------------------------------
 * Catalog
 * ----------------------------------------------------------------------- */
enum { CAT_ALL, CAT_UTILITIES, CAT_INTERNET, CAT_MEDIA, CAT_GRAPHICS, CAT_OFFICE,
       CAT_DEVELOPER, CAT_RUNTIMES, CAT_INSTALLED, CAT_UPDATES, CAT_COUNT };

static const char *g_cat_name[CAT_COUNT] = {
    "All apps", "Utilities", "Internet", "Media", "Graphics", "Office", "Developer",
    "Runtimes", "Installed", "Updates",
};

/* What the downloaded file is, and how it is installed */
enum {
    KIND_SETUP,        /* a 64-bit installer (.exe) or a Windows Installer package (.msi) */
    KIND_PORTABLE,     /* the 64-bit program itself: Run it from Downloads */
    KIND_ARCHIVE,      /* a .zip, .7z or 7-Zip self-extractor: 7-Zip unpacks it into C:\Programs\<dest> */
};

typedef struct {
    const char *name;
    const char *publisher;
    const char *summary;       /* one line */
    int         category;
    const char *url;           /* the download (the official 64-bit package where there is one) */
    const char *file;          /* saved as C:\Downloads\<file> */
    const char *dest;          /* KIND_ARCHIVE: unpacked into C:\Programs\<dest> */
    const char *exe;           /* installed: C:\Programs\<exe>, or an absolute path
                                * starting with '\'; a "**" component stands for
                                * any folders (archives with a versioned top folder) */
    int         kind;          /* KIND_* */
    unsigned    size_mb;       /* approximate download size */
    const char *note;          /* NovaOS compatibility note */
    const char *label;         /* 1-3 letter tile label */
    GdiColor    color;         /* tile colour */
    const char *system;        /* KIND_ARCHIVE: only these files (space-separated) are
                                * unpacked, and they go into the system folders:
                                * from an x86 or x32 folder to SysWOW64, others to
                                * System32; "path>name" renames.  Vulkan driver
                                * manifests (*_icd.*.json) among them are registered */
} StoreApp;

#include "store_catalog.h"

static StoreCatalog *g_store_catalog;
#define g_catalog (g_store_catalog->apps)
#define N_APPS (g_store_catalog ? g_store_catalog->count : 0)

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
enum { DL_NONE, DL_RESOLVE, DL_FETCH, DL_FAILED };

typedef struct {
    int     cat;                   /* selected category */
    UiScroll bar;                  /* the list's scroll bar: pixels down */
    int     logged[8];             /* the view last logged (log_view) */
    int     pressed;               /* catalog index whose button is held, or -1 */
    /* the one download in flight */
    int     dl;                    /* catalog index, or -1 */
    int     phase;
    NetOp  *op;
    UINT32  shown;                 /* bytes shown in the progress text */
    char    host[128], path[512];
    UINT16  port;
    bool    https;
    int     redirects;
    char    msg[STORE_MAX_APPS][72];       /* per-app status line */
    bool    bad[STORE_MAX_APPS];           /* the status line reports a failure */
    /* 7-Zip unpacking an archive */
    UmProcess *unpack;
    int     unpack_i;              /* catalog index, or -1 */
    char    tar[RAMFS_PATH_MAX];   /* a .tar.gz's .tar: unpacked next, then deleted */
    bool    tar_layer;             /* 7-Zip is taking the .gz layer off */
    /* the Updates page */
    UpdateStatus upd;              /* as last painted */
    bool    upd_pressed;
    NetOp  *catalog_op;
    int     catalog_phase, catalog_redirects;
    char    catalog_host[128], catalog_path[512], catalog_msg[96];
    UINT16  catalog_port;
    bool    catalog_pressed;
} Store;

#define SIDE_W   180
#define HEAD_H   56
#define ROW_H    84
#define BTN_W    92
#define BTN_H    30
#define TILE     48

static WND *g_store;               /* the one Store window */

/* Read the independently persisted catalog; the image's copy is an offline
 * fallback, not a C table. UI callbacks hold the file-system lock. */
static bool catalog_load(Store *s)
{
    const char *paths[] = { STORE_CATALOG_PATH, STORE_DEFAULT_PATH };
    for (int i = 0; i < 2; i++) {
        RamNode *f = RamfsResolve(NULL, paths[i]);
        if (!f || f->dir || f->size > STORE_JSON_MAX || !RamfsLoad(f)) continue;
        StoreCatalog *c = store_catalog_parse(f->data, f->size);
        if (!c) continue;
        kfree(g_store_catalog); g_store_catalog = c;
        strcpy(s->catalog_msg, i ? "Using offline catalog" : "Using saved catalog");
        kprintf("[STORE] Catalog: loaded %d apps from %s\n", N_APPS, paths[i]);
        return true;
    }
    strcpy(s->catalog_msg, "No valid catalog; refresh");
    return false;
}

static void catalog_end(Store *s, const char *msg)
{
    if (s->catalog_op) NetRelease(s->catalog_op);
    s->catalog_op = NULL; s->catalog_phase = DL_NONE;
    strncpy(s->catalog_msg, msg, sizeof(s->catalog_msg) - 1);
    s->catalog_msg[sizeof(s->catalog_msg) - 1] = 0;
    kprintf("[STORE] Catalog: %s\n", s->catalog_msg);
}
static bool catalog_target(Store *s, const char *url)
{
    bool https;
    return sc_text(url, 500) && !strchr(url, ' ') && !strncmp(url, "https://", 8) &&
           NetParseUrl(url, s->catalog_host, sizeof(s->catalog_host), &s->catalog_port,
                       s->catalog_path, sizeof(s->catalog_path), &https) && https;
}
static void catalog_resolve(Store *s)
{
    s->catalog_phase = DL_RESOLVE;
    s->catalog_op = NetResolve(s->catalog_host);
    if (!s->catalog_op) catalog_end(s, "Catalog: network busy");
}
static void catalog_refresh(Store *s)
{
    if (s->catalog_op) return;
    if (s->dl >= 0 || s->unpack) { catalog_end(s, "Finish the app operation first"); return; }
    if (!NetAvailable()) { catalog_end(s, "Offline; keeping catalog"); return; }
    char url[512]; strcpy(url, STORE_CHANNEL);
    RamNode *f = RamfsResolve(NULL, STORE_CHANNEL_PATH);
    if (f) {
        if (f->dir || !f->size || f->size >= sizeof(url) || !RamfsLoad(f) || memchr(f->data, 0, f->size)) {
            catalog_end(s, "Invalid catalog URL file"); return;
        }
        memcpy(url, f->data, f->size); url[f->size] = 0;
        size_t n = f->size;
        while (n && (url[n-1] == '\n' || url[n-1] == '\r' || url[n-1] == ' ')) url[--n] = 0;
    }
    if (!catalog_target(s, url)) { catalog_end(s, "Catalog URL must use HTTPS"); return; }
    s->catalog_redirects = 0; s->pressed = -1;
    strcpy(s->catalog_msg, "Refreshing catalog...");
    catalog_resolve(s);
}
/* Cache a validated response with a same-directory rename. Failed writes or
 * malformed downloads cannot replace the last usable catalog. */
static bool catalog_save(const char *body, UINT32 len)
{
    RamNode *windows = RamfsResolve(NULL, "\\Windows");
    RamNode *dir = windows ? RamfsCreate(windows, "AppStore", true) : NULL;
    if (!dir) return false;
    RamNode *tmp = RamfsCreate(dir, "catalog.json.tmp", false);
    if (!tmp) return false;
    if (!RamfsWrite(tmp, body, len) || !RamfsRename(tmp, dir, "catalog.json", true)) {
        RamfsDelete(tmp); return false;
    }
    return true;
}
static bool catalog_tick(Store *s)
{
    NetOp *op = s->catalog_op;
    if (!op) return false;
    if (s->catalog_phase == DL_FETCH && op->len > STORE_JSON_MAX + 65536u) {
        catalog_end(s, "Catalog too large; kept old list"); return true;
    }
    if (op->state == NET_PENDING) return false;
    if (op->state == NET_FAILED) { catalog_end(s, "Refresh failed; keeping catalog"); return true; }
    if (s->catalog_phase == DL_RESOLVE) {
        NetIp ip = op->addr; NetRelease(op);
        s->catalog_op = NetHttpGetAddr(&ip, s->catalog_port, s->catalog_host, s->catalog_path, true);
        s->catalog_phase = DL_FETCH;
        if (!s->catalog_op) catalog_end(s, "Catalog: network busy");
        return true;
    }
    const char *body; UINT32 len; char location[512];
    int status = NetHttpParse(op, &body, &len, location, sizeof(location));
    if (status >= 300 && status < 400 && location[0] && s->catalog_redirects++ < 8) {
        bool valid;
        if (location[0] == '/' && location[1] != '/' && sc_text(location, 500) && !strchr(location, ' ')) {
            strcpy(s->catalog_path, location); valid = true;
        } else valid = catalog_target(s, location);
        if (!valid) { catalog_end(s, "Invalid catalog redirect"); return true; }
        NetRelease(op); s->catalog_op = NULL; catalog_resolve(s); return true;
    }
    if (status != 200) { catalog_end(s, "Refresh failed; keeping catalog"); return true; }
    StoreCatalog *c = store_catalog_parse(body, len);
    if (!c) { catalog_end(s, "Invalid JSON; keeping catalog"); return true; }
    if (!catalog_save(body, len)) { kfree(c); catalog_end(s, "Could not save; keeping catalog"); return true; }
    kfree(g_store_catalog); g_store_catalog = c;
    memset(s->msg, 0, sizeof(s->msg)); memset(s->bad, 0, sizeof(s->bad));
    s->pressed = -1; UiScrollTo(&s->bar, 0); memset(s->logged, 0xFF, sizeof(s->logged));
    catalog_end(s, "Catalog updated");
    kprintf("[STORE] Catalog: now %d apps\n", N_APPS);
    return true;
}

/* Walk the '\'-separated @path from @n; a component ending in '*' matches
 * the first folder with that prefix, and a "**" component any folders */
static RamNode *walk(RamNode *n, const char *p, int depth)
{
    while (*p == '\\') p++;
    if (!n) return NULL;
    if (!*p) return n;
    char comp[RAMFS_NAME_MAX];
    int len = 0;
    while (p[len] && p[len] != '\\' && len < (int)sizeof(comp) - 1) { comp[len] = p[len]; len++; }
    comp[len] = '\0';
    const char *rest = p + len;
    if (!strcmp(comp, "**")) {
        RamNode *r = walk(n, rest, depth);                /* no folder at all */
        for (RamNode *c = n->child; !r && c && depth < 6; c = c->next)
            if (c->dir) r = walk(c, p, depth + 1);        /* or one more, then again */
        return r;
    }
    if (len && comp[len - 1] == '*') {
        comp[--len] = '\0';
        for (RamNode *c = n->child; c; c = c->next)
            if (c->dir && !strncmp(c->name, comp, (size_t)len)) {
                RamNode *r = walk(c, rest, depth);
                if (r) return r;
            }
        return NULL;
    }
    return walk(RamfsFind(n, comp), rest, depth);
}

/* The installed program's file (see StoreApp.exe) */
static RamNode *installed_exe(const StoreApp *a)
{
    if (!a->exe) return NULL;
    RamNode *n = walk(a->exe[0] == '\\' ? RamfsRoot() : RamfsResolve(NULL, "\\Programs"), a->exe, 0);
    return n && !n->dir ? n : NULL;
}

static RamNode *downloaded_file(const StoreApp *a)
{
    char path[RAMFS_PATH_MAX];
    ksnprintf(path, sizeof(path), "\\Downloads\\%s", a->file);
    RamNode *n = RamfsResolve(NULL, path);
    return n && !n->dir ? n : NULL;
}

static bool in_category(const StoreApp *a, int cat)
{
    if (cat == CAT_ALL) return true;
    if (cat == CAT_INSTALLED) return installed_exe(a) || (a->kind == KIND_PORTABLE && downloaded_file(a));
    if (cat == CAT_UPDATES) return false;                /* (NovaOS itself: paint_updates) */
    return a->category == cat;
}

/* The catalog indices shown for the current category; returns the count */
static int visible(const Store *s, int *out)
{
    int n = 0;
    for (int i = 0; i < N_APPS; i++)
        if (in_category(&g_catalog[i], s->cat)) out[n++] = i;
    return n;
}

static void set_msg(Store *s, int i, const char *m)
{
    s->bad[i] = !strncmp(m, "Failed", 6) || !strncmp(m, "Could not", 9);
    if (s->bad[i] || !strncmp(m, "Installed", 9))       /* the outcome, for the serial log */
        kprintf("[STORE] %s: %s\n", g_catalog[i].name, m);
    strncpy(s->msg[i], m, sizeof(s->msg[i]) - 1);
    s->msg[i][sizeof(s->msg[i]) - 1] = '\0';
}

/* -----------------------------------------------------------------------
 * Downloading
 * ----------------------------------------------------------------------- */
static void dl_fail(Store *s, const char *why)
{
    char m[64];
    ksnprintf(m, sizeof(m), "Failed: %s", why);
    set_msg(s, s->dl, m);
    if (s->op) NetRelease(s->op);
    s->op = NULL;
    s->phase = DL_NONE;
    s->dl = -1;
}

static bool dl_target(Store *s, const char *url)
{
    return NetParseUrl(url, s->host, sizeof(s->host), &s->port, s->path, sizeof(s->path), &s->https);
}

static void dl_resolve(Store *s)
{
    s->op = NetResolve(s->host);
    s->phase = DL_RESOLVE;
    if (!s->op) dl_fail(s, "the network is busy");
}

static void dl_start(Store *s, int i)
{
    if (s->catalog_op) { set_msg(s, i, "Wait for catalog refresh"); return; }
    if (s->dl >= 0) { set_msg(s, i, "Another download is in progress"); return; }
    if (!NetAvailable()) { set_msg(s, i, "Failed: no network connection"); return; }
    if ((UINT64)g_catalog[i].size_mb * 1024 * 1024 > NET_HTTP_MAX) {
        char m[64];
        ksnprintf(m, sizeof(m), "Too large for this build (limit %u MB)", (unsigned)(NET_HTTP_MAX >> 20));
        set_msg(s, i, m);
        return;
    }
    s->dl = i;
    s->redirects = 0;
    s->shown = 0;
    set_msg(s, i, "Connecting...");
    if (!dl_target(s, g_catalog[i].url)) { dl_fail(s, "bad download address"); return; }
    dl_resolve(s);
}

static void dl_cancel(Store *s)
{
    if (s->dl < 0) return;
    set_msg(s, s->dl, "");
    if (s->op) NetRelease(s->op);
    s->op = NULL;
    s->phase = DL_NONE;
    s->dl = -1;
}

static void dl_done(Store *s)
{
    const StoreApp *a = &g_catalog[s->dl];
    const char *body;
    UINT32 blen;
    char loc[512];
    int status = NetHttpParse(s->op, &body, &blen, loc, sizeof(loc));
    if (!status) { dl_fail(s, "unreadable response"); return; }

    if (status >= 300 && status < 400 && loc[0] && s->redirects < 8) {
        NetRelease(s->op);
        s->op = NULL;
        s->redirects++;
        if (loc[0] == '/') {                          /* same host */
            strncpy(s->path, loc, sizeof(s->path) - 1);
            s->path[sizeof(s->path) - 1] = '\0';
        } else if (!dl_target(s, loc)) {
            dl_fail(s, "bad redirect");
            return;
        }
        dl_resolve(s);
        return;
    }
    if (status != 200) {
        char m[48];
        ksnprintf(m, sizeof(m), "server replied %d", status);
        dl_fail(s, m);
        return;
    }
    RamNode *dir = RamfsResolve(NULL, "\\Downloads");
    if (!dir) dir = RamfsCreate(RamfsResolve(NULL, "\\"), "Downloads", true);
    RamNode *f = dir ? RamfsFind(dir, a->file) : NULL;
    if (!f && dir) f = RamfsCreate(dir, a->file, false);
    if (!f || f->dir || !RamfsWrite(f, body, blen)) { dl_fail(s, "could not save the file"); return; }
    char sz[24], m[64];
    AppFormatSize(blen, sz, sizeof(sz));
    ksnprintf(m, sizeof(m), "Downloaded %s to C:\\Downloads", sz);
    set_msg(s, s->dl, m);
    AppNoteRecentFile(f);
    NetRelease(s->op);
    s->op = NULL;
    s->phase = DL_NONE;
    s->dl = -1;
}

static bool unpack_tick(Store *s);
static bool upd_changed(const Store *s);

/* The download in flight and 7-Zip's unpacking: true if a row changed */
static bool dl_tick(Store *s)
{
    bool changed = unpack_tick(s);
    if (s->dl < 0 || !s->op) return changed;
    NetOp *op = s->op;
    if (s->phase == DL_RESOLVE) {
        if (op->state == NET_PENDING) return false;
        if (op->state == NET_FAILED) { dl_fail(s, op->error[0] ? op->error : "name not found"); return true; }
        NetIp ip = op->addr;
        NetRelease(op);
        s->op = NetHttpGetAddr(&ip, s->port, s->host, s->path, s->https);
        s->phase = DL_FETCH;
        if (!s->op) { dl_fail(s, "the network is busy"); return true; }
        set_msg(s, s->dl, "Downloading...");
        return true;
    }
    if (s->phase == DL_FETCH) {
        if (op->state == NET_PENDING) {
            /* progress: the bytes received so far, refreshed every 64 KB */
            if (op->len - s->shown < 64 * 1024) return false;
            s->shown = op->len;
            char sz[24], m[64];
            AppFormatSize(op->len, sz, sizeof(sz));
            ksnprintf(m, sizeof(m), "Downloading... %s", sz);
            set_msg(s, s->dl, m);
            return true;
        }
        if (op->state == NET_FAILED) dl_fail(s, op->error[0] ? op->error : "connection lost");
        else dl_done(s);
        return true;
    }
    return false;
}

static bool store_tick(WND *w)
{
    Store *s = w->user;
    if (!s) return false;
    bool moved = UiScrollTick(&s->bar);       /* a held arrow or trough repeats */
    bool upd = s->cat == CAT_UPDATES && upd_changed(s);
    bool catalog = catalog_tick(s);
    bool download = dl_tick(s);
    return catalog || download || moved || upd;
}

/* -----------------------------------------------------------------------
 * Running
 * ----------------------------------------------------------------------- */
static void failed_msg(Store *s, int i, const char *m)
{
    set_msg(s, i, m);
    s->bad[i] = true;
}

/* The machine a PE file is built for (0x8664 x64, 0x14C x86), 0 if not a PE */
static UINT16 pe_machine(RamNode *f)
{
    if (!RamfsLoad(f)) return 0;
    const UINT8 *d = (const UINT8 *)f->data;
    if (!d || f->size < 0x40 || d[0] != 'M' || d[1] != 'Z') return 0;
    UINT32 pe = (UINT32)(d[0x3C] | d[0x3D] << 8 | d[0x3E] << 16 | (UINT32)d[0x3F] << 24);
    if (pe > f->size - 6 || memcmp(d + pe, "PE\0\0", 4)) return 0;
    return (UINT16)(d[pe + 4] | d[pe + 5] << 8);
}

static void spawn(Store *s, int i, RamNode *exe, const char *cmdline, RamNode *cwd)
{
    char err[160];
    UmProcess *p = UmSpawn(exe, cmdline, cwd, NULL, err, sizeof(err));
    if (!p) {
        char m[96];
        ksnprintf(m, sizeof(m), "Could not start: %s", err);
        failed_msg(s, i, m);
        return;
    }
    UmDetach(p);
    set_msg(s, i, "");
}

static bool ends_with(const char *name, const char *ext)
{
    int n = (int)strlen(name), e = (int)strlen(ext);
    if (n < e) return false;
    for (int i = 0; i < e; i++)
        if ((name[n - e + i] | 0x20) != (ext[i] | 0x20)) return false;
    return true;
}

/* Unpack an archive (or a 7-Zip self-extracting installer) into
 * C:\Programs\<dest> with 7-Zip's command-line program */
static void unpack(Store *s, int i, RamNode *f)
{
    const StoreApp *a = &g_catalog[i];
    if (s->unpack) { set_msg(s, i, "Another download is being unpacked"); return; }
    RamNode *z = RamfsResolve(NULL, "\\Programs\\7-Zip\\7z.exe");
    if (!z) { failed_msg(s, i, "Could not unpack: get 7-Zip first (Utilities)"); return; }
    char path[RAMFS_PATH_MAX], cmd[2 * RAMFS_PATH_MAX + 640], err[160];
    RamfsPath(f, path, sizeof(path));
    s->tar_layer = ends_with(f->name, ".tar.gz") || ends_with(f->name, ".tgz");
    if (s->tar_layer) {
        /* 7-Zip takes one layer at a time: the .tar beside the download first */
        char dir[RAMFS_PATH_MAX];
        RamfsPath(f->parent, dir, sizeof(dir));
        ksnprintf(cmd, sizeof(cmd), "7z x \"%s\" \"-o%s\" -y", path, dir);
        ksnprintf(s->tar, sizeof(s->tar), "%s\\%s", dir + 2, f->name);
        int n = (int)strlen(s->tar);                        /* x.tar.gz -> x.tar, x.tgz -> x.tar */
        if (ends_with(s->tar, ".tgz")) strcpy(s->tar + n - 2, "ar");
        else s->tar[n - 3] = '\0';
    } else {
        int n = ksnprintf(cmd, sizeof(cmd), "7z x \"%s\" \"-oC:\\Programs\\%s\" -y", path, a->dest);
        for (const char *c = a->system; c && *c && n < (int)sizeof(cmd) - 2; ) {   /* the files, less any ">name" */
            if (*c == '>') { while (*c && *c != ' ') c++; continue; }
            if (c == a->system) cmd[n++] = ' ';
            cmd[n++] = *c++;
        }
        cmd[n] = '\0';
    }
    UmProcess *p = UmSpawn(z, cmd, f->parent, NULL, err, sizeof(err));
    if (!p) {
        char m[96];
        ksnprintf(m, sizeof(m), "Could not start 7-Zip: %s", err);
        failed_msg(s, i, m);
        return;
    }
    s->unpack = p;
    s->unpack_i = i;
    set_msg(s, i, "Unpacking with 7-Zip...");
}

/* An archive of system files unpacked: move them into System32 (x64\...)
 * and SysWOW64 (x86\...), replacing older copies, as an installer would */
static void move_system_files(const StoreApp *a)
{
    char list[512];
    strncpy(list, a->system, sizeof(list) - 1);
    list[sizeof(list) - 1] = '\0';
    for (char *f = list, *next; f && *f; f = next) {
        next = strchr(f, ' ');
        if (next) *next++ = '\0';
        char *as = strchr(f, '>');                          /* "path>name": installed as name */
        if (as) *as++ = '\0';
        char path[RAMFS_PATH_MAX];
        ksnprintf(path, sizeof(path), "\\Programs\\%s\\%s", a->dest, f);
        RamNode *n = RamfsResolve(NULL, path);
        const char *leaf = strrchr(f, '\\');
        bool x86 = !strncmp(f, "x86\\", 4) || !strncmp(f, "x32\\", 4) || strstr(f, "\\x86\\") || strstr(f, "\\x32\\");
        const char *sys = x86 ? "\\Windows\\SysWOW64" : "\\Windows\\System32";
        RamNode *dir = RamfsResolve(NULL, sys);
        if (!n || n->dir || !leaf || !dir) continue;
        const char *name = as ? as : leaf + 1;
        if (!RamfsRename(n, dir, name, true)) {
            kprintf("[STORE] Could not move %s into the system folder\n", path);
            continue;
        }
        if (strstr(name, "_icd.") && ends_with(name, ".json")) {   /* a Vulkan driver, as its installer registers it */
            char reg[RAMFS_PATH_MAX];
            ksnprintf(reg, sizeof(reg), "C:%s\\%s", sys, name);
            um_registry_set_dword("Machine\\SOFTWARE\\Khronos\\Vulkan\\Drivers", reg, 0);
        }
    }
    RamNode *top = RamfsResolve(NULL, "\\Programs");
    RamNode *d = top ? RamfsFind(top, a->dest) : NULL;     /* the emptied folders */
    for (RamNode *c = d ? d->child : NULL, *nx; c; c = nx) {
        nx = c->next;
        for (RamNode *g = c->dir ? c->child : NULL, *gn; g; g = gn) {
            gn = g->next;
            if (g->dir && !g->child) RamfsDelete(g);
        }
        if (c->dir && !c->child) RamfsDelete(c);
    }
    if (d && !d->child) RamfsDelete(d);
}

/* A running unpack finished: did it produce the program? */
static bool unpack_tick(Store *s)
{
    if (!s->unpack) return false;
    UINT32 status = 0;
    char why[96] = "";
    if (!UmHasExited(s->unpack, &status, why, sizeof(why))) return false;
    UmRelease(s->unpack);
    s->unpack = NULL;
    int i = s->unpack_i;
    s->unpack_i = -1;
    const StoreApp *a = &g_catalog[i];
    if (s->tar_layer) {                                     /* the .gz layer is off: now the .tar */
        RamNode *t = status == 0 ? RamfsResolve(NULL, s->tar) : NULL;
        if (t && !t->dir) {
            unpack(s, i, t);
            if (s->unpack) return true;
            RamfsDelete(t);
            s->tar[0] = '\0';
            return true;                                    /* unpack() said why */
        }
    } else if (s->tar[0]) {
        RamNode *t = RamfsResolve(NULL, s->tar);
        if (t && !t->dir) RamfsDelete(t);
    }
    s->tar_layer = false;
    s->tar[0] = '\0';
    if (a->system && status == 0) move_system_files(a);
    /* 7-Zip's exit codes: 0 done, 1 warnings, 2 and up a file it could not
     * write (Firefox's xul.dll with memory short: the program is there but
     * cut off) */
    if (status <= 1 && (installed_exe(a) || (!a->exe && status == 0))) {
        char m[96];
        if (a->system) ksnprintf(m, sizeof(m), "Installed in C:\\Windows\\System32");
        else           ksnprintf(m, sizeof(m), "Installed in C:\\Programs\\%s", a->dest);
        set_msg(s, i, m);
    } else {
        char m[96];
        if (why[0]) ksnprintf(m, sizeof(m), "Could not unpack: 7-Zip %s", why);
        else        ksnprintf(m, sizeof(m), "Could not unpack: 7-Zip stopped with code %u", (unsigned)status);
        failed_msg(s, i, m);
    }
    return true;
}

/* Install a downloaded file: 64-bit installers run, .msi packages go to
 * Windows Installer, archives are unpacked */
static void run_file(Store *s, int i, RamNode *f)
{
    if (!f) return;
    const StoreApp *a = &g_catalog[i];
    if (a->kind == KIND_ARCHIVE) { unpack(s, i, f); return; }
    if (ends_with(f->name, ".msi")) {
        if (!AppRunMsi(f)) failed_msg(s, i, "Could not start: Windows Installer is missing");
        else set_msg(s, i, "");
        return;
    }
    UINT16 m = pe_machine(f);                   /* 64-bit, or 32-bit (WoW64) */
    if (m != 0x8664 && m != 0x14C) { failed_msg(s, i, "Could not install: this download is not a Windows program"); return; }
    spawn(s, i, f, f->name, f->parent);
}

/* The row's button: what it says and what it does */
typedef enum { BTN_GET, BTN_CANCEL, BTN_INSTALL, BTN_RUN, BTN_OPEN, BTN_NONE } BtnKind;

static bool is_runtime(const StoreApp *a) { return a->category == CAT_RUNTIMES; }

static BtnKind row_button(const Store *s, int i)
{
    const StoreApp *a = &g_catalog[i];
    if (s->unpack_i == i) return BTN_NONE;
    if (s->dl == i) return BTN_CANCEL;
    if (installed_exe(a)) return is_runtime(a) ? BTN_NONE : BTN_OPEN;
    if (downloaded_file(a)) return a->kind == KIND_PORTABLE ? BTN_RUN : BTN_INSTALL;
    return BTN_GET;
}

/* What stands in the button's place when there is none */
static const char *no_button_text(const Store *s, int i)
{
    if (s->unpack_i == i) return "Unpacking";
    return "Installed";
}

static void press(Store *s, int i)
{
    if (s->catalog_op) { set_msg(s, i, "Wait for catalog refresh"); return; }
    const StoreApp *a = &g_catalog[i];
    switch (row_button(s, i)) {
    case BTN_GET:     dl_start(s, i); break;
    case BTN_CANCEL:  dl_cancel(s); break;
    case BTN_INSTALL: run_file(s, i, downloaded_file(a)); break;
    case BTN_RUN: {
        RamNode *f = downloaded_file(a);
        UINT16 m = f ? pe_machine(f) : 0;
        if (m == 0x8664 || m == 0x14C) { AppRunProgram(f, f->name); set_msg(s, i, ""); }
        else if (f) failed_msg(s, i, "Could not run: this download is not a Windows program");
        break; }
    case BTN_OPEN: {                         /* console programs get a Terminal */
        RamNode *exe = installed_exe(a);
        if (exe) { AppRunProgram(exe, exe->name); set_msg(s, i, ""); }
        break; }
    case BTN_NONE:    break;
    }
}

/* -----------------------------------------------------------------------
 * Layout and painting (client-relative rectangles)
 * ----------------------------------------------------------------------- */
/* The list, less the scroll bar's room when its rows don't fit */
static GdiRect r_list(const Store *s, GdiRect c)
{
    return RECT(SIDE_W, HEAD_H, c.w - SIDE_W - (UiScrollNeeded(&s->bar) ? UI_SB_W : 0), c.h - HEAD_H);
}
static GdiRect r_catalog(GdiRect c) { return RECT(c.w - BTN_W - 16, 12, BTN_W, BTN_H); }
static GdiRect r_bar(GdiRect c)  { return RECT(c.w - UI_SB_W, HEAD_H, UI_SB_W, c.h - HEAD_H); }
static GdiRect r_cat(int i)      { return RECT(8, 56 + i * 36, SIDE_W - 16, 32); }
static GdiRect r_btn(GdiRect lr, int row_y) { return RECT(lr.x + lr.w - BTN_W - 20, row_y + (ROW_H - BTN_H) / 2, BTN_W, BTN_H); }

static int list_height(const Store *s)
{
    int idx[STORE_MAX_APPS];
    return visible(s, idx) * ROW_H + 12;
}

/* The scroll bar's range: the rows' height, a page of the list's in view */
static void set_bar(Store *s, GdiRect c)
{
    s->bar.vert = true;
    s->bar.line = ROW_H;
    UiScrollSet(&s->bar, list_height(s), c.h - HEAD_H);
}

/* One line in the serial log when the view changes (what the self-test
 * reads): the category, how far the list is scrolled, the bar and where
 * the list is on the screen */
static void log_view(Store *s, GdiRect c)
{
    if (s->bar.held != UI_SB_NONE) return;
    GdiRect lr = r_list(s, c);
    int maxpos = s->bar.max > s->bar.page ? s->bar.max - s->bar.page : 0;
    int now[8] = { s->cat, s->bar.pos, maxpos, s->bar.page, c.x + lr.x, c.y + lr.y, lr.w, lr.h };
    if (!memcmp(now, s->logged, sizeof(now))) return;
    memcpy(s->logged, now, sizeof(now));
    kprintf("[STORE] view: %s: scrolled %d of %d px, page %d; bar:%s; list %d,%d %dx%d\n", g_cat_name[s->cat],
            s->bar.pos, maxpos, s->bar.page, UiScrollNeeded(&s->bar) ? " vertical" : "",
            c.x + lr.x, c.y + lr.y, lr.w, lr.h);
}

static void app_tile(const StoreApp *a, int x, int y, int sz)
{
    GdiRoundGradV(RECT(x, y, sz, sz), sz * 22 / 100, GdiLerp(a->color, GDI_WHITE, 40), a->color);
    GdiTextCenter(x, y + (sz - GDI_FONT_H) / 2, sz, a->label, GDI_WHITE);
}

/* -----------------------------------------------------------------------
 * The Updates page: NovaOS itself (fs/update.c)
 * ----------------------------------------------------------------------- */
typedef enum { UB_NONE, UB_CHECK, UB_UPDATE, UB_RESTART } UpdBtn;

#define UPD_H 112

static UpdBtn upd_button(const UpdateStatus *u)
{
    if (u->busy) return UB_NONE;
    if (u->state == UPDATE_AVAILABLE) return UB_UPDATE;
    if (u->state == UPDATE_READY) return UB_RESTART;
    return UB_CHECK;
}

static GdiRect r_upd_card(GdiRect lr) { return RECT(lr.x + 20, lr.y + 6, lr.w - 40, UPD_H); }
static GdiRect r_upd_btn(GdiRect lr)
{
    GdiRect c = r_upd_card(lr);
    return RECT(c.x + c.w - BTN_W - 14, c.y + (UPD_H - BTN_H) / 2, BTN_W, BTN_H);
}

/* What the page says about the update, and whether that is a failure */
static bool upd_line(const UpdateStatus *u, char *line, int cap)
{
    char a[24], b[24];
    if (u->busy && u->state == UPDATE_DOWNLOADING) {
        AppFormatSize(u->got, a, sizeof(a));
        AppFormatSize(u->size, b, sizeof(b));
        ksnprintf(line, (size_t)cap, "%s  -  %s of %s", u->step, a, b);
    } else if (u->busy) {
        ksnprintf(line, (size_t)cap, "Checking for a newer NovaOS...");
    } else switch (u->state) {
    case UPDATE_CURRENT:   ksnprintf(line, (size_t)cap, "NovaOS %s is up to date.", NovaVersion()); break;
    case UPDATE_AVAILABLE:
        AppFormatSize(u->size, a, sizeof(a));
        ksnprintf(line, (size_t)cap, "NovaOS %s is available (%s)%s%s", u->version, a, u->notes[0] ? ": " : ".", u->notes);
        break;
    case UPDATE_READY:     ksnprintf(line, (size_t)cap, "NovaOS %s is ready: restart to finish the update.", u->version); break;
    case UPDATE_FAILED:    ksnprintf(line, (size_t)cap, "%s", u->error); return true;
    default:               ksnprintf(line, (size_t)cap, "Check whether a newer NovaOS is out."); break;
    }
    return false;
}

static void paint_updates(Store *s, GdiRect lr)
{
    UpdateGetStatus(&s->upd);
    const UpdateStatus *u = &s->upd;
    GdiRect card = r_upd_card(lr);
    GdiRoundRect(card, 6, UI_CARD, GDI_TRANSPARENT);
    GdiRoundGradV(RECT(card.x + 14, card.y + 14, TILE, TILE), TILE * 22 / 100,
                  GDI_C(0x3A, 0x8A, 0xF0), GDI_C(0x2A, 0xC8, 0xC8));
    GdiTextCenter(card.x + 14, card.y + 14 + (TILE - GDI_FONT_H) / 2, TILE, "N", GDI_WHITE);
    int tx = card.x + 14 + TILE + 16;
    char line[256];
    GdiTextBold(tx, card.y + 12, "NovaOS", UI_TEXT);
    ksnprintf(line, sizeof(line), "This PC has NovaOS %s", NovaVersion());
    GdiTextT(tx + GdiTextBoldW("NovaOS") + 10, card.y + 12, line, UI_TEXT3);
    GdiSetClip(RECT(lr.x, lr.y, card.x + card.w - BTN_W - 28 - lr.x, lr.h));
    bool bad = upd_line(u, line, sizeof(line));
    GdiTextT(tx, card.y + 36, line, bad ? GDI_C(0xFF, 0x8A, 0x80) : UI_TEXT2);
    GdiTextT(tx, card.y + 62, "An update replaces the system at the next restart; if the new", UI_TEXT3);
    GdiTextT(tx, card.y + 82, "version does not start, NovaOS goes back to this one.", UI_TEXT3);
    GdiSetClip(lr);
    static const char *labels[] = { "", "Check", "Update", "Restart" };
    UpdBtn b = upd_button(u);
    GdiRect br = r_upd_btn(lr);
    if (s->upd_pressed) br.y += 1;
    if (b == UB_NONE) GdiTextCenter(br.x, br.y + (br.h - GDI_FONT_H) / 2, br.w,
                                    u->state == UPDATE_DOWNLOADING ? "Updating" : "Checking", UI_TEXT3);
    else UiButton(br, labels[b], b != UB_CHECK);
    int y = card.y + UPD_H + 18;
    if (UpdateBootNotice()[0]) {
        GdiTextT(card.x + 4, y, UpdateBootNotice(), UI_TEXT2);
        y += 24;
    }
    char ch[512];
    UpdateGetChannel(ch, sizeof(ch));
    ksnprintf(line, sizeof(line), "Updates come from %s", ch);
    GdiTextT(card.x + 4, y, line, UI_TEXT3);
}

/* The status changed in a way the page shows */
static bool upd_changed(const Store *s)
{
    UpdateStatus u;
    UpdateGetStatus(&u);
    return u.state != s->upd.state || u.busy != s->upd.busy || (u.got >> 20) != (s->upd.got >> 20) ||
           strcmp(u.step, s->upd.step);
}

static void upd_press(Store *s)
{
    switch (upd_button(&s->upd)) {
    case UB_CHECK:   UpdateCheck(); break;
    case UB_UPDATE:  UpdateInstall(); break;
    case UB_RESTART: DesktopRestart(); break;
    case UB_NONE:    break;
    }
}

static void store_paint(WND *w)
{
    Store *s = w->user;
    GdiRect c = WmClientRect(w);
    set_bar(s, c);

    /* Sidebar */
    GdiFillRect(RECT(c.x, c.y, SIDE_W, c.h), UI_PANEL);
    AppDrawIcon(APP_STORE, c.x + 14, c.y + 12, 28);
    GdiTextBold(c.x + 52, c.y + 18, "App Store", UI_TEXT);
    for (int i = 0; i < CAT_COUNT; i++) {
        GdiRect r = r_cat(i);
        r.x += c.x; r.y += c.y;
        if (i == s->cat) {
            GdiRoundRect(r, 4, UI_HOVER, GDI_TRANSPARENT);
            GdiFillRect(RECT(r.x, r.y + 8, 3, r.h - 16), UI_ACCENT);
        }
        GdiTextT(r.x + 14, r.y + (r.h - GDI_FONT_H) / 2, g_cat_name[i], i == s->cat ? UI_TEXT : UI_TEXT2);
    }
    GdiSetClip(RECT(c.x + 8, c.y + c.h - 48, SIDE_W - 16, 48));
    GdiTextT(c.x + 12, c.y + c.h - 42, "App catalog (F5 refresh)", UI_TEXT3);
    GdiTextT(c.x + 12, c.y + c.h - 24, s->catalog_msg, UI_TEXT3);
    GdiSetClip(c);

    /* Header */
    GdiTextLarge(c.x + SIDE_W + 24, c.y + 14, g_cat_name[s->cat], UI_TEXT);
    int idx[STORE_MAX_APPS];
    int n = visible(s, idx);
    char cnt[32];
    ksnprintf(cnt, sizeof(cnt), "%d app%s", n, n == 1 ? "" : "s");
    if (s->cat != CAT_UPDATES) {
        GdiTextT(c.x + c.w - BTN_W - 32 - GdiTextW(cnt), c.y + 22, cnt, UI_TEXT3);
        GdiRect refresh = r_catalog(c); refresh.x += c.x; refresh.y += c.y;
        UiButton(refresh, s->catalog_op ? "Refreshing" : "Refresh", false);
    }
    GdiFillRect(RECT(c.x + SIDE_W, c.y + HEAD_H - 1, c.w - SIDE_W, 1), UI_LINE);

    /* Rows */
    GdiRect lr = r_list(s, c);
    lr.x += c.x; lr.y += c.y;
    GdiSetClip(lr);
    if (s->cat == CAT_UPDATES) paint_updates(s, lr);
    else if (!n) {
        GdiTextT(lr.x + 24, lr.y + 20, s->cat == CAT_INSTALLED ? "Nothing from the store is installed yet."
                                                                : "No apps here.", UI_TEXT2);
    }
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        const StoreApp *a = &g_catalog[i];
        int y = lr.y + 6 + k * ROW_H - s->bar.pos;
        if (y + ROW_H < lr.y || y > lr.y + lr.h) continue;
        int x = lr.x + 20;
        GdiRoundRect(RECT(x, y, lr.w - 40, ROW_H - 8), 6, UI_CARD, GDI_TRANSPARENT);
        app_tile(a, x + 14, y + (ROW_H - 8 - TILE) / 2, TILE);
        int tx = x + 14 + TILE + 16;
        GdiTextBold(tx, y + 10, a->name, UI_TEXT);
        int nw = GdiTextBoldW(a->name);
        GdiTextT(tx + nw + 10, y + 10, a->publisher, UI_TEXT3);
        /* the description, cut to the room before the button */
        GdiSetClip(RECT(lr.x, lr.y, lr.w - BTN_W - 32, lr.h));
        GdiTextT(tx, y + 30, a->summary, UI_TEXT2);
        /* status: the download's progress or result, else size + note */
        char line[96];
        if (s->msg[i][0]) {
            ksnprintf(line, sizeof(line), "%s", s->msg[i]);
        } else if (installed_exe(a)) {
            ksnprintf(line, sizeof(line), "Installed  -  %s", a->note);
        } else if (a->size_mb) {
            ksnprintf(line, sizeof(line), "%u MB  -  %s", a->size_mb, a->note);
        } else {
            ksnprintf(line, sizeof(line), "%s", a->note);
        }
        bool failed = s->msg[i][0] && s->bad[i];
        GdiTextT(tx, y + 50, line, failed ? GDI_C(0xFF, 0x8A, 0x80) : UI_TEXT3);
        GdiSetClip(lr);
        static const char *labels[] = { "Get", "Cancel", "Install", "Run", "Open", "" };
        BtnKind b = row_button(s, i);
        GdiRect br = r_btn(lr, y);
        if (s->pressed == i) { br.y += 1; }
        if (b == BTN_NONE) GdiTextCenter(br.x, br.y + (br.h - GDI_FONT_H) / 2, br.w, no_button_text(s, i), UI_TEXT3);
        else UiButton(br, labels[b], b == BTN_GET || b == BTN_INSTALL || b == BTN_OPEN);
    }
    GdiSetClip(c);
    UiScrollDraw(&s->bar, r_bar(c), c);
    log_view(s, c);
}

/* -----------------------------------------------------------------------
 * Input
 * ----------------------------------------------------------------------- */
/* The catalog index whose button is at client point (x, y), or -1 */
static int button_at(const Store *s, GdiRect c, int x, int y)
{
    GdiRect lr = r_list(s, c);
    if (!UiHit(lr, x, y)) return -1;
    int idx[STORE_MAX_APPS];
    int n = visible(s, idx);
    for (int k = 0; k < n; k++) {
        int ry = lr.y + 6 + k * ROW_H - s->bar.pos;
        if (UiHit(r_btn(lr, ry), x, y)) return row_button(s, idx[k]) == BTN_NONE ? -1 : idx[k];
    }
    return -1;
}

static void store_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Store *s = w->user;
    GdiRect c = WmClientRect(w);
    set_bar(s, c);
    /* the scroll bar first: a press on it, and its drag until release */
    if (UiScrollMouse(&s->bar, msg, x, y)) return;
    switch (msg) {
    case WM_MOUSE_DOWN:
    case WM_MOUSE_DBLCLK:
        if (s->cat != CAT_UPDATES && UiHit(r_catalog(c), x, y)) { s->catalog_pressed = true; s->pressed = -1; return; }
        for (int i = 0; i < CAT_COUNT; i++)
            if (UiHit(r_cat(i), x, y)) {
                s->cat = i;
                UiScrollTo(&s->bar, 0);
                UpdateGetStatus(&s->upd);
                if (i == CAT_UPDATES && s->upd.state == UPDATE_IDLE && !s->upd.busy) UpdateCheck();   /* (the first look checks) */
                return;
            }
        if (s->cat == CAT_UPDATES) {
            s->upd_pressed = upd_button(&s->upd) != UB_NONE && UiHit(r_upd_btn(r_list(s, c)), x, y);
            return;
        }
        s->pressed = button_at(s, c, x, y);
        break;
    case WM_MOUSE_UP: {
        if (s->catalog_pressed) {
            s->catalog_pressed = false;
            if (s->cat != CAT_UPDATES && UiHit(r_catalog(c), x, y)) catalog_refresh(s);
            break;
        }
        if (s->cat == CAT_UPDATES) {
            if (s->upd_pressed && UiHit(r_upd_btn(r_list(s, c)), x, y)) upd_press(s);
            s->upd_pressed = false;
            break;
        }
        int i = button_at(s, c, x, y);
        if (i >= 0 && i == s->pressed) press(s, i);
        s->pressed = -1;
        break; }
    case WM_MOUSE_WHEEL:
        UiScrollTo(&s->bar, s->bar.pos - WmWheelDelta() * ROW_H);
        break;
    default:
        break;
    }
}

static void store_key(WND *w, const KeyEvent *k)
{
    Store *s = w->user;
    GdiRect c = WmClientRect(w);
    if (k->scancode == KEY_ESC) { WmDestroyWindow(w); return; }
    if (!k->extended && k->scancode == 0x3F) { catalog_refresh(s); return; }
    if (!k->extended) return;
    set_bar(s, c);
    switch (k->scancode) {
    case KEY_UP:   UiScrollTo(&s->bar, s->bar.pos - ROW_H); break;
    case KEY_DOWN: UiScrollTo(&s->bar, s->bar.pos + ROW_H); break;
    case KEY_PGUP: UiScrollTo(&s->bar, s->bar.pos - s->bar.page); break;
    case KEY_PGDN: UiScrollTo(&s->bar, s->bar.pos + s->bar.page); break;
    case KEY_HOME: UiScrollTo(&s->bar, 0); break;
    case KEY_END:  UiScrollTo(&s->bar, s->bar.max); break;
    }
}

static void store_close(WND *w)
{
    Store *s = w->user;
    if (s->op) NetRelease(s->op);
    if (s->catalog_op) NetRelease(s->catalog_op);
    if (s->unpack) UmDetach(s->unpack);          /* let 7-Zip finish on its own */
    kfree(s);
    w->user = NULL;
    if (g_store == w) g_store = NULL;
}

/* "store install NAME" in the Terminal: press the row's button as a click
 * would (Get, or Install once the file is in C:\Downloads).  The Store
 * window opens in the background; the outcome lands in the serial log as
 * "[STORE] NAME: Installed ..." or "... Failed/Could not ...". */
const char *StoreInstall(const char *name)
{
    WND *was = WmActiveWindow();
    StoreOpen();
    if (!g_store) return "Could not open the App Store.";
    if (was && was != g_store) WmSetActive(was);
    Store *s = g_store->user;
    if (s->catalog_op) return "Wait for catalog refresh.";
    int i = 0;
    for (; i < N_APPS; i++) {
        const char *a = g_catalog[i].name, *b = name;
        while (*a && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*a && !*b) break;
    }
    if (i == N_APPS) return "There is no such program in the App Store.";
    if (installed_exe(&g_catalog[i])) {
        kprintf("[STORE] %s: Installed already\n", g_catalog[i].name);
        return "Installed already.";
    }
    press(s, i);
    return s->msg[i][0] ? s->msg[i] : "Started.";
}

/* "store close" in the Terminal: close the App Store's window (one
 * "store install" opened in the background), as its close button would */
const char *StoreClose(void)
{
    if (!g_store) return "The App Store is not open.";
    WmRequestClose(g_store);
    return "Closed the App Store.";
}

const char *StoreRefresh(void)
{
    StoreOpen();
    if (!g_store) return "Could not open the App Store.";
    Store *s = g_store->user;
    catalog_refresh(s);
    return s->catalog_msg;
}

void StoreShowUpdates(void)
{
    StoreOpen();
    if (!g_store) return;
    Store *s = g_store->user;
    s->cat = CAT_UPDATES;
    UiScrollTo(&s->bar, 0);
    UpdateGetStatus(&s->upd);
    if (s->upd.state == UPDATE_IDLE && !s->upd.busy) UpdateCheck();
}

void StoreOpen(void)
{
    if (g_store) {
        Store *s = g_store->user;
        memset(s->logged, 0xFF, sizeof(s->logged));   /* log the view again */
        WmSetActive(g_store);
        return;
    }
    Store *s = kzalloc(sizeof(Store));
    if (!s) return;
    s->dl = -1;
    s->pressed = -1;
    s->unpack_i = -1;
    catalog_load(s);
    WND *w = AppCreateWindow(APP_STORE, "App Store", 820, 540, UI_BG);
    if (!w) { kfree(s); return; }
    w->user     = s;
    w->on_paint = store_paint;
    w->on_mouse = store_mouse;
    w->on_key   = store_key;
    w->on_close = store_close;
    w->on_tick  = store_tick;
    w->rbutton  = true;                     /* the wheel scrolls the list */
    g_store = w;
}
