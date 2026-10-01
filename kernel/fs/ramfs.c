/*
 * ramfs.c — in-memory filesystem backing drive C: (see ramfs.h)
 */

#include "ramfs.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"

static RamNode g_root;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static int name_cmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

static bool is_sep(char c) { return c == '\\' || c == '/'; }

RamNode *RamfsRoot(void) { return &g_root; }

/* -----------------------------------------------------------------------
 * Mounted drives (see ramfs.h)
 * ----------------------------------------------------------------------- */
typedef struct {
    RamNode           *root;
    const RamfsSource *src;
    void              *vol;
    char               label[64];
    UINT64             total;
} Drive;

static Drive g_drives[26];

static RamNode *top_of(const RamNode *n)
{
    while (n && n->parent) n = n->parent;
    return (RamNode *)n;
}

static Drive *drive_of(const RamNode *n)
{
    const RamNode *t = top_of(n);
    if (!t || !(t->xflags & RAMFS_X_EXTERN) || t->drive < 'A' || t->drive > 'Z') return NULL;
    return &g_drives[t->drive - 'A'];
}

RamNode *RamfsDriveRoot(char letter)
{
    letter = (char)(letter & ~0x20);
    if (letter == 'C') return &g_root;
    return letter >= 'A' && letter <= 'Z' ? g_drives[letter - 'A'].root : NULL;
}

UINT32 RamfsDriveMask(void)
{
    UINT32 m = 1u << 2;
    for (int i = 0; i < 26; i++) if (g_drives[i].root) m |= 1u << i;
    return m;
}

char RamfsDriveLetter(const RamNode *n)
{
    const RamNode *t = top_of(n);
    return t && t->drive ? t->drive : 'C';
}

bool RamfsReadOnly(const RamNode *n) { return n && (n->xflags & RAMFS_X_EXTERN); }

bool RamfsDriveInfo(const RamNode *n, const char **label, const char **fs, UINT64 *total)
{
    Drive *d = drive_of(n);
    if (!d) return false;
    if (label) *label = d->label;
    if (fs) *fs = "NTFS";
    if (total) *total = d->total;
    return true;
}

RamNode *RamfsMountDrive(char letter, const RamfsSource *src, void *vol, UINT64 root_ref, const char *label,
                         UINT64 total_bytes)
{
    letter = (char)(letter & ~0x20);
    if (letter < 'D' || letter > 'Z' || g_drives[letter - 'A'].root) return NULL;
    RamNode *r = kzalloc(sizeof(RamNode));
    if (!r) return NULL;
    r->dir = true;
    r->drive = letter;
    r->xflags = RAMFS_X_EXTERN;
    r->xref = root_ref;
    Drive *d = &g_drives[letter - 'A'];
    d->src = src;
    d->vol = vol;
    strncpy(d->label, label ? label : "", sizeof(d->label) - 1);
    d->total = total_bytes;
    d->root = r;
    return r;
}

/* -----------------------------------------------------------------------
 * Change tracking (see ramfs.h)
 * ----------------------------------------------------------------------- */
static RamfsMode g_mode = RAMFS_SEEDING;
static UINT64 (*g_clock)(void);
void RamfsSetClock(UINT64 (*now)(void)) { g_clock = now; }
static void touch(RamNode *n) { if (g_clock) n->mtime = g_clock(); }
static UINT32 g_changes;
static void (*g_removed_hook)(const char *path);

void RamfsSetMode(RamfsMode mode) { g_mode = mode; }
UINT32 RamfsChanges(void) { return g_changes; }
void RamfsSetRemovedHook(void (*fn)(const char *path)) { g_removed_hook = fn; }
static void (*g_change_hook)(RamNode *dir);
void RamfsSetChangeHook(void (*fn)(RamNode *dir)) { g_change_hook = fn; }

static void mark(RamNode *n, UINT8 flags)
{
    if (n && g_change_hook && g_mode != RAMFS_LOADING) g_change_hook((flags & RAMFS_F_DIRTYDIR) ? n : n->parent ? n->parent : n);
    if (g_mode != RAMFS_TRACK || !n) return;
    n->pflags |= flags;
    for (RamNode *a = n->parent; a && !(a->pflags & RAMFS_F_SUB); a = a->parent) a->pflags |= RAMFS_F_SUB;
    if (n->parent) n->parent->pflags |= RAMFS_F_SUB;
    g_changes++;
}

/* Before @n moves or goes away: report its starter files, which then stop being ones. */
static void report_seeds(RamNode *n)
{
    if (g_mode != RAMFS_TRACK) return;
    if ((n->pflags & RAMFS_F_SEED) && g_removed_hook) {
        char path[RAMFS_PATH_MAX];
        RamfsPath(n, path, sizeof(path));
        g_removed_hook(path);
    }
    n->pflags &= (UINT8)~RAMFS_F_SEED;
    for (RamNode *c = n->child; c; c = c->next) report_seeds(c);
}

/* A moved subtree is new at its destination: save all of it. */
static void mark_moved(RamNode *n)
{
    n->pflags &= (UINT8)~RAMFS_F_SEALED;
    mark(n, n->dir ? (RAMFS_F_DIRTY | RAMFS_F_DIRTYDIR) : RAMFS_F_DIRTY);
    for (RamNode *c = n->child; c; c = c->next) mark_moved(c);
}

void RamfsMarkChanged(RamNode *n) { if (n && !n->dir) mark(n, RAMFS_F_DIRTY); }

/* Directories first, then case-insensitive by name */
static bool sorts_before(const RamNode *a, const RamNode *b)
{
    if (a->dir != b->dir) return a->dir;
    return name_cmp(a->name, b->name) < 0;
}

static RamNode *find_in(RamNode *dir, const char *name)
{
    for (RamNode *c = dir->child; c; c = c->next)
        if (!name_cmp(c->name, name)) return c;
    return NULL;
}

/* Reading a directory of a mounted volume: entries arrive about in order,
 * so each goes after the last one put in when it can (no list walk) */
typedef struct { RamNode *dir, *last_dir, *last_file; } Fill;

static bool fill_add(const RamfsExtEntry *e, void *ctx)
{
    Fill *f = ctx;
    size_t len = strnlen(e->name, RAMFS_NAME_MAX);
    if (!len || len >= RAMFS_NAME_MAX || strchr(e->name, '\\') || strchr(e->name, '/')) return true;
    RamNode *n = kzalloc(sizeof(RamNode));
    if (!n) return false;
    memcpy(n->name, e->name, len + 1);
    n->dir = e->dir;
    n->parent = f->dir;
    n->xflags = RAMFS_X_EXTERN;
    n->xref = e->ref;
    n->size = e->dir ? 0 : e->size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (UINT32)e->size;
    n->ctime = e->ctime;
    n->mtime = e->mtime;
    n->attrs = (e->attrs & 0x27u) | 0x1u;                       /* FILE_ATTRIBUTE_READONLY: it is */
    RamNode **last = e->dir ? &f->last_dir : &f->last_file;
    RamNode **pp;
    if (*last && sorts_before(*last, n)) pp = &(*last)->next;
    else if (!e->dir && f->last_dir && !*last) pp = &f->last_dir->next;
    else pp = &f->dir->child;
    while (*pp && sorts_before(*pp, n)) pp = &(*pp)->next;
    if (*pp && !name_cmp((*pp)->name, n->name)) { kfree(n); return true; }   /* another name for one entry */
    n->next = *pp;
    *pp = n;
    *last = n;
    return true;
}

bool RamfsLoad(RamNode *n)
{
    Drive *d;
    if (!n || !(n->xflags & RAMFS_X_EXTERN) || !(d = drive_of(n))) return true;
    if (n->dir) {
        if (n->xflags & RAMFS_X_LISTED) return true;
        n->xflags |= RAMFS_X_LISTED;
        Fill f = { n, NULL, NULL };
        if (d->src->list(d->vol, n->xref, fill_add, &f)) return true;
        kprintf("[RAMFS] %c: a directory could not be read\n", top_of(n)->drive);
        return false;                                            /* (what was read stays) */
    }
    if (n->xflags & RAMFS_X_LOADED) return true;
    UINT64 size;
    if (!d->src->size(d->vol, n->xref, &size) || size > RAMFS_FILE_MAX) return false;
    char *buf = kmalloc(size ? size : 1);
    if (!buf) return false;
    if (!d->src->read(d->vol, n->xref, 0, buf, size)) { kfree(buf); return false; }
    n->data = buf;
    n->size = n->cap = (UINT32)size;
    n->xflags |= RAMFS_X_LOADED;
    return true;
}

/* The contents of a mounted volume's file go when nothing holds it */
static void unload(RamNode *f)
{
    if (!(f->xflags & RAMFS_X_LOADED) || f->refs > 0 || f->pins > 0) return;
    kfree(f->data);
    f->data = NULL;
    f->cap = 0;
    f->xflags &= (UINT8)~RAMFS_X_LOADED;
}

RamNode *RamfsFind(RamNode *dir, const char *name)
{
    if (!dir || !dir->dir) return NULL;
    RamfsLoad(dir);
    return find_in(dir, name);
}

int RamfsCount(const RamNode *dir)
{
    int n = 0;
    if (dir && dir->dir) {
        RamfsLoad((RamNode *)dir);
        for (const RamNode *c = dir->child; c; c = c->next) n++;
    }
    return n;
}

RamNode *RamfsCreate(RamNode *dir, const char *name, bool is_dir)
{
    if (!dir || !dir->dir || !name || !*name) return NULL;
    if (RamfsReadOnly(dir)) {                                    /* only what is already there */
        RamNode *e = RamfsFind(dir, name);
        return e && e->dir == is_dir ? e : NULL;
    }
    size_t len = strlen(name);
    if (len >= RAMFS_NAME_MAX) return NULL;
    for (size_t i = 0; i < len; i++)
        if (is_sep(name[i]) || name[i] == ':') return NULL;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return NULL;

    RamNode *existing = RamfsFind(dir, name);
    if (existing) return existing->dir == is_dir ? existing : NULL;

    RamNode *n = kzalloc(sizeof(RamNode));
    if (!n) return NULL;
    memcpy(n->name, name, len + 1);
    n->dir    = is_dir;
    n->parent = dir;
    n->pflags = g_mode == RAMFS_SEEDING ? RAMFS_F_SEED : g_mode == RAMFS_INSTALLING ? RAMFS_F_SEALED : 0;
    touch(n);
    n->ctime = n->mtime;

    RamNode **pp = &dir->child;
    while (*pp && sorts_before(*pp, n)) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
    mark(n, RAMFS_F_DIRTY);
    mark(dir, RAMFS_F_DIRTYDIR);
    return n;
}

bool RamfsWrite(RamNode *f, const char *data, UINT32 len)
{
    if (!f || f->dir || f->pins || len > RAMFS_FILE_MAX || RamfsReadOnly(f)) return false;
    char *buf = NULL;
    if (len) {
        buf = kmalloc(len);
        if (!buf) return false;
        memcpy(buf, data, len);
    }
    kfree(f->data);
    f->data = buf;
    f->size = len;
    f->cap = len;
    mark(f, RAMFS_F_DIRTY);
    touch(f);
    return true;
}

/* Grow capacity to at least @need (geometrically, so appends are cheap). */
static bool reserve(RamNode *f, UINT32 need)
{
    if (need <= f->cap) return true;
    if (need > RAMFS_FILE_MAX) return false;
    UINT32 cap = f->cap ? f->cap : 256;
    while (cap < need) cap = cap > RAMFS_FILE_MAX / 2 ? RAMFS_FILE_MAX : cap * 2;
    char *nb = kmalloc(cap);
    if (!nb) return false;
    if (f->size) memcpy(nb, f->data, f->size);
    kfree(f->data);
    f->data = nb;
    f->cap = cap;
    return true;
}

bool RamfsWriteAt(RamNode *f, UINT32 off, const void *data, UINT32 len)
{
    if (!f || f->dir || f->pins || off > RAMFS_FILE_MAX || len > RAMFS_FILE_MAX - off || RamfsReadOnly(f)) return false;
    if (!reserve(f, off + len)) return false;
    if (off > f->size) memset(f->data + f->size, 0, off - f->size);
    memcpy(f->data + off, data, len);
    if (off + len > f->size) f->size = off + len;
    mark(f, RAMFS_F_DIRTY);
    touch(f);
    return true;
}

bool RamfsResize(RamNode *f, UINT32 len)
{
    if (!f || f->dir || f->pins || RamfsReadOnly(f) || !reserve(f, len)) return false;
    if (len > f->size) memset(f->data + f->size, 0, len - f->size);
    f->size = len;
    mark(f, RAMFS_F_DIRTY);
    touch(f);
    return true;
}

void RamfsRef(RamNode *n)   { if (n) n->refs++; }
void RamfsUnref(RamNode *n) { if (n && n->refs > 0 && !--n->refs && !n->dir) unload(n); }
void RamfsPin(RamNode *f)
{
    if (!f) return;
    f->refs++;
    f->pins++;
    RamfsLoad(f);
}
void RamfsUnpin(RamNode *f) { if (f && f->pins > 0) { f->pins--; RamfsUnref(f); } }

bool RamfsDelete(RamNode *n)
{
    if (!n || n == &g_root || n->refs > 0 || (n->dir && n->child) || RamfsReadOnly(n)) return false;
    RamNode **pp = &n->parent->child;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (!*pp) return false;
    report_seeds(n);
    mark(n->parent, RAMFS_F_DIRTYDIR);
    *pp = n->next;
    kfree(n->data);
    kfree(n);
    return true;
}

bool RamfsRename(RamNode *n, RamNode *dir, const char *name, bool replace)
{
    if (!n || n == &g_root || !dir || !dir->dir || !name || !*name) return false;
    if (RamfsReadOnly(n) || RamfsReadOnly(dir)) return false;
    size_t len = strlen(name);
    if (len >= RAMFS_NAME_MAX || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (size_t i = 0; i < len; i++)
        if (is_sep(name[i]) || name[i] == ':') return false;
    for (RamNode *a = dir; a; a = a->parent)
        if (a == n) return false;                   /* into itself or a descendant */
    RamNode *old = RamfsFind(dir, name);
    if (old && old != n) {
        if (!replace || old->dir || n->dir) return false;
        if (!RamfsDelete(old)) return false;
    }
    RamNode **pp = &n->parent->child;               /* unlink */
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (!*pp) return false;
    report_seeds(n);
    mark(n->parent, RAMFS_F_DIRTYDIR);
    *pp = n->next;
    memcpy(n->name, name, len + 1);
    n->parent = dir;
    pp = &dir->child;                               /* insert in order */
    while (*pp && sorts_before(*pp, n)) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
    mark(dir, RAMFS_F_DIRTYDIR);
    mark_moved(n);
    return true;
}

RamNode *RamfsResolve(RamNode *cwd, const char *path)
{
    RamNode *cur = cwd ? cwd : &g_root;
    if (!path) return NULL;

    /* Drive prefix / leading separator → absolute (on the current drive) */
    if (((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z') && path[1] == ':') {
        if (!(cur = RamfsDriveRoot(path[0]))) return NULL;
        path += 2;
    }
    if (is_sep(*path)) cur = top_of(cur);

    char part[RAMFS_NAME_MAX];
    while (*path) {
        while (is_sep(*path)) path++;
        if (!*path) break;
        int n = 0;
        while (*path && !is_sep(*path)) {
            if (n >= RAMFS_NAME_MAX - 1) return NULL;
            part[n++] = *path++;
        }
        part[n] = '\0';
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { if (cur->parent) cur = cur->parent; continue; }
        cur = RamfsFind(cur, part);
        if (!cur) return NULL;
    }
    return cur;
}

void RamfsPath(const RamNode *node, char *buf, int cap)
{
    if (!buf || cap < 4) return;
    /* Collect ancestors, then emit root-first */
    const RamNode *chain[32];
    int depth = 0;
    for (const RamNode *n = node; n && n->parent && depth < 32; n = n->parent)
        chain[depth++] = n;
    int pos = 0;
    buf[pos++] = RamfsDriveLetter(node); buf[pos++] = ':'; buf[pos++] = '\\';
    for (int i = depth - 1; i >= 0; i--) {
        for (const char *s = chain[i]->name; *s && pos < cap - 1; s++) buf[pos++] = *s;
        if (i > 0 && pos < cap - 1) buf[pos++] = '\\';
    }
    buf[pos] = '\0';
}

/* -----------------------------------------------------------------------
 * Seed contents
 * ----------------------------------------------------------------------- */
static void seed_file(const char *dir_path, const char *name, const char *text)
{
    RamNode *d = RamfsResolve(NULL, dir_path);
    RamNode *f = d ? RamfsCreate(d, name, false) : NULL;
    if (f) RamfsWrite(f, text, (UINT32)strlen(text));
}

static void seed_dir(const char *dir_path, const char *name)
{
    RamNode *d = RamfsResolve(NULL, dir_path);
    if (d) RamfsCreate(d, name, true);
}

void RamfsInit(void)
{
    memset(&g_root, 0, sizeof(g_root));
    g_root.dir = true;
    g_root.drive = 'C';
    g_mode = RAMFS_SEEDING;

    seed_dir("\\", "Documents");
    seed_dir("\\", "Downloads");
    seed_dir("\\", "Pictures");
    seed_dir("\\", "Personal");
    seed_dir("\\", "Projects");
    seed_dir("\\", "Windows");
    seed_dir("\\Windows", "System32");
    seed_dir("\\Projects", "NovaOS");

    seed_file("\\Documents", "Welcome.txt",
        "Welcome to NovaOS!\n"
        "\n"
        "Things to try:\n"
        "  - Open Terminal from the dock and type 'help'.\n"
        "  - Drag windows by their title bar; double-click it to maximize.\n"
        "  - Browse files in File Explorer and double-click one to open it.\n"
        "  - Edit this file in Notepad and press Ctrl+S to save.\n"
        "\n"
        "Your files are kept on the computer's disk, so they are still\n"
        "here after a restart (when NovaOS finds a disk it can use).\n");
    seed_file("\\Documents", "Shopping list.txt",
        "Milk\nEggs\nCoffee\nBread\n");
    seed_file("\\Personal", "Ideas.txt",
        "- Learn how an OS kernel schedules threads\n"
        "- Build a tiny game for NovaOS\n");
    seed_file("\\Projects\\NovaOS", "Roadmap.txt",
        "Phase 8   Interactive desktop            done\n"
        "Phase 9   Run a real .exe in user mode   next\n"
        "Phase 10  Win32 GUI subsystem\n"
        "Phase 11  Disk drivers and filesystems\n");
    seed_file("\\Windows", "win.ini",
        "; for 16-bit app support\n[fonts]\n[extensions]\n[mci extensions]\n[files]\n");
    seed_file("\\Windows\\System32", "README.txt",
        "System files will live here once NovaOS can load real DLLs.\n");

    kprintf("[RAMFS] Drive C: ready (RAM disk)\n");
}
