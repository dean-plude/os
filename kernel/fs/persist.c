/*
 * persist.c — keeps drive C: on disk (see persist.h)
 *
 * Saving walks the RAM disk along the RAMFS_F_SUB marks to the nodes that
 * changed: a changed file is written whole, a directory whose entries
 * changed has the disk entries it no longer holds deleted, and a moved
 * directory is written out in full.  Starter files the user deletes are
 * listed in \NOVA\DELETED.TXT so they stay deleted after a restart.
 */

#include "persist.h"
#include "fat.h"
#include "ramfs.h"
#include "block.h"
#include "../drivers/ahci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../um/um.h"

#define DATA_ROOT     "\\NOVA\\C"
#define DELETED_FILE  "DELETED.TXT"          /* in \NOVA */
#define DATA_LABEL    "NOVADATA"

static FatVol *g_vol;
static UINT32  g_root_dir;                    /* the cluster of \NOVA\C (created on first save) */
static bool    g_root_known;
static UINT32  g_nova_dir;
static bool    g_loaded;
static UINT32  g_saved_changes, g_seen_changes;
static UINT64  g_seen_tick, g_retry_tick;
static bool    g_failed;

/* ---------------------------------------------------------------------------
 * Deleted starter files
 * ------------------------------------------------------------------------- */
typedef struct Removed { struct Removed *next; char path[RAMFS_PATH_MAX]; } Removed;
static Removed *g_removed;
static bool g_removed_dirty;

static bool path_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

static void add_removed(const char *path)
{
    for (Removed *r = g_removed; r; r = r->next)
        if (path_eq(r->path, path)) return;
    Removed *r = kzalloc(sizeof(Removed));
    if (!r) return;
    strncpy(r->path, path, sizeof(r->path) - 1);
    Removed **pp = &g_removed;                /* keep the order: parents are removed after children */
    while (*pp) pp = &(*pp)->next;
    *pp = r;
    g_removed_dirty = true;
}

static void removed_hook(const char *path) { add_removed(path); }

/* ---------------------------------------------------------------------------
 * Choosing the volume
 * ------------------------------------------------------------------------- */
typedef struct { FatVol *vol; BlockDev *dev; } Cand;

static bool is_zero(const UINT8 *p, int n)
{
    for (int i = 0; i < n; i++) if (p[i]) return false;
    return true;
}

static int find_volumes(BlockDev *d, Cand *out, int max, bool *blank)
{
    UINT8 *s = kmalloc(2 * BLOCK_SECTOR);
    int n = 0;
    *blank = false;
    if (!s || !d->read(d, 0, 2, s)) { kfree(s); return 0; }
    FatVol *v = FatMount(d, 0);                                   /* a whole-disk volume */
    if (v) {
        out[n++] = (Cand){ v, d };
    } else if (is_zero(s, 2 * BLOCK_SECTOR)) {
        *blank = true;
    } else if (s[510] == 0x55 && s[511] == 0xAA) {
        bool gpt = false;
        for (int i = 0; i < 4 && n < max; i++) {
            const UINT8 *e = s + 446 + 16 * i;
            UINT32 lba = *(const UINT32 *)(e + 8);
            if (e[4] == 0xEE) gpt = true;
            else if (e[4] && lba && (v = FatMount(d, lba))) out[n++] = (Cand){ v, d };
        }
        if (gpt && !memcmp(s + BLOCK_SECTOR, "EFI PART", 8)) {
            UINT64 table = *(UINT64 *)(s + BLOCK_SECTOR + 72);
            UINT32 count = *(UINT32 *)(s + BLOCK_SECTOR + 80), esize = *(UINT32 *)(s + BLOCK_SECTOR + 84);
            UINT8 *ent = kmalloc(BLOCK_SECTOR);
            if (esize >= 128 && esize <= BLOCK_SECTOR && count <= 128 && ent) {
                UINT32 per = BLOCK_SECTOR / esize;
                for (UINT32 i = 0; i < count && n < max; i++) {
                    if (i % per == 0 && !d->read(d, table + i / per, 1, ent)) break;
                    const UINT8 *e = ent + (i % per) * esize;
                    if (is_zero(e, 16)) continue;
                    UINT64 first = *(const UINT64 *)(e + 32);
                    if ((v = FatMount(d, first))) out[n++] = (Cand){ v, d };
                }
            }
            kfree(ent);
        }
    }
    kfree(s);
    return n;
}

/* An MBR with one FAT32 (LBA) partition from 1 MiB to the end, formatted */
static FatVol *format_blank(BlockDev *d)
{
    if (d->sectors < 2048 + 131072) return NULL;                  /* under 65 MiB */
    UINT8 *mbr = kzalloc(BLOCK_SECTOR);
    if (!mbr) return NULL;
    UINT64 size = d->sectors - 2048;
    if (size > 0xFFFFFFFFull) size = 0xFFFFFFFFull;
    UINT8 *e = mbr + 446;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;                        /* CHS: beyond 8 GiB */
    e[4] = 0x0C;                                                   /* FAT32 (LBA) */
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    *(UINT32 *)(e + 8) = 2048;
    *(UINT32 *)(e + 12) = (UINT32)size;
    mbr[510] = 0x55; mbr[511] = 0xAA;
    bool ok = d->write(d, 0, 1, mbr);
    kfree(mbr);
    if (!ok) return NULL;
    kprintf("[PERSIST] %s is empty: formatting it for your files\n", d->name);
    return FatFormat(d, 2048, size, DATA_LABEL);
}

void PersistInit(void)
{
    AhciInit();
    Cand c[16];
    int n = 0;
    BlockDev *blank = NULL;
    for (int i = 0; i < BlockCount(); i++) {
        bool is_blank;
        n += find_volumes(BlockGet(i), c + n, 16 - n, &is_blank);
        if (is_blank && !blank) blank = BlockGet(i);
    }
    int pick = -1;
    for (int i = 0; i < n && pick < 0; i++)
        if (path_eq(FatLabel(c[i].vol), DATA_LABEL)) pick = i;
    if (pick < 0 && blank) g_vol = format_blank(blank);
    for (int i = 0; i < n && pick < 0 && !g_vol; i++) {
        FatEntry e;
        if (FatLookupPath(c[i].vol, "\\EFI\\NOVA\\kernel.elf", &e)) pick = i;
    }
    if (pick >= 0) g_vol = c[pick].vol;
    for (int i = 0; i < n; i++)
        if (i != pick) FatUnmount(c[i].vol);
    RamfsSetRemovedHook(removed_hook);
    if (g_vol) {
        char d[96];
        PersistDescribe(d, sizeof(d));
        kprintf("[PERSIST] Drive C: is saved to %s\n", d);
    } else {
        kprintf("[PERSIST] No disk to save to: drive C: lasts until restart\n");
    }
}

bool PersistActive(void) { return g_vol != NULL; }

bool PersistSpace(UINT64 *free, UINT64 *total)
{
    if (!g_vol) return false;
    *free = FatFreeBytes(g_vol);
    *total = FatTotalBytes(g_vol);
    return true;
}

void PersistWhere(char *buf, int cap)
{
    if (!g_vol) { ksnprintf(buf, (size_t)cap, "memory only"); return; }
    ksnprintf(buf, (size_t)cap, "%s (%s)", FatDevice(g_vol)->name, FatLabel(g_vol));
}

void PersistDescribe(char *buf, int cap)
{
    if (!g_vol) { ksnprintf(buf, (size_t)cap, "memory only (no disk found)"); return; }
    ksnprintf(buf, (size_t)cap, "%s FAT%d \"%s\", %u MiB free of %u MiB", FatDevice(g_vol)->name, FatType(g_vol),
              FatLabel(g_vol), (unsigned)(FatFreeBytes(g_vol) >> 20), (unsigned)(FatTotalBytes(g_vol) >> 20));
}

/* ---------------------------------------------------------------------------
 * Restoring
 * ------------------------------------------------------------------------- */
typedef struct { FatEntry *e; int n, cap; } EntList;

static bool collect(const FatEntry *e, void *ctx)
{
    EntList *l = ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        FatEntry *ne = kmalloc(sizeof(FatEntry) * (size_t)cap);
        if (!ne) return false;
        if (l->n) memcpy(ne, l->e, sizeof(FatEntry) * (size_t)l->n);
        kfree(l->e);
        l->e = ne;
        l->cap = cap;
    }
    l->e[l->n++] = *e;
    return true;
}

static int g_restored;

static void load_dir(UINT32 fdir, RamNode *rdir, int depth)
{
    if (depth > 24) return;
    EntList l = { 0 };
    FatList(g_vol, fdir, collect, &l);
    for (int i = 0; i < l.n; i++) {
        const FatEntry *e = &l.e[i];
        if (strlen(e->name) >= RAMFS_NAME_MAX) continue;
        RamNode *have = RamfsFind(rdir, e->name);
        if (have && (have->dir != e->dir || ((have->pflags & RAMFS_F_SEALED) && !have->dir))) continue;
        if (e->dir) {
            RamNode *d = RamfsCreate(rdir, e->name, true);
            if (d) load_dir(e->cluster, d, depth + 1);
            continue;
        }
        if (e->size > RAMFS_FILE_MAX) continue;
        char *buf = e->size ? kmalloc(e->size) : NULL;
        if (e->size && !buf) continue;
        RamNode *f = RamfsCreate(rdir, e->name, false);
        if (f && FatRead(g_vol, e, buf) && RamfsWrite(f, buf, e->size)) g_restored++;
        kfree(buf);
    }
    kfree(l.e);
}

static void load_removed(void)
{
    FatEntry e;
    if (!FatLookup(g_vol, g_nova_dir, DELETED_FILE, &e) || e.dir || e.size > 256 * 1024) return;
    char *text = kmalloc(e.size + 1);
    if (!text) return;
    if (FatRead(g_vol, &e, text)) {
        text[e.size] = '\0';
        for (char *line = text, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = '\0';
            size_t n = strlen(line);
            if (n && line[n - 1] == '\r') line[--n] = '\0';
            if (!n) continue;
            add_removed(line);
            RamNode *node = RamfsResolve(NULL, line);
            if (node && !(node->pflags & RAMFS_F_SEALED)) {
                node->pflags &= (UINT8)~RAMFS_F_SEED;
                RamfsDelete(node);
            }
        }
    }
    kfree(text);
    g_removed_dirty = false;
}

void PersistLoad(void)
{
    if (g_vol) {
        RamfsSetMode(RAMFS_LOADING);
        FatEntry nova, root;
        if (FatLookupPath(g_vol, "\\NOVA", &nova) && nova.dir) {
            g_nova_dir = nova.cluster;
            if (FatLookup(g_vol, g_nova_dir, "C", &root) && root.dir) {
                g_root_dir = root.cluster;
                g_root_known = true;
                load_dir(g_root_dir, RamfsRoot(), 0);
            }
            load_removed();
        }
        kprintf("[PERSIST] Restored %d file(s) to drive C:\n", g_restored);
    }
    g_loaded = true;
    RamfsSetMode(RAMFS_TRACK);
    g_saved_changes = g_seen_changes = RamfsChanges();
}

/* ---------------------------------------------------------------------------
 * Saving
 * ------------------------------------------------------------------------- */
static bool g_error;

static void save_error(const char *what, const RamNode *n)
{
    char path[RAMFS_PATH_MAX];
    RamfsPath(n, path, sizeof(path));
    if (!g_error) kprintf("[PERSIST] Could not save %s (%s)\n", path, what);
    g_error = true;
}

static const UINT8 CLEAR = RAMFS_F_DIRTY | RAMFS_F_DIRTYDIR | RAMFS_F_SUB;

/* Brings the disk directory @fdir in line with the RAM directory @r. */
static void save_dir(RamNode *r, UINT32 fdir, bool fresh, int depth)
{
    if (depth > 24) return;
    if ((r->pflags & RAMFS_F_DIRTYDIR) && !fresh) {
        EntList l = { 0 };
        FatList(g_vol, fdir, collect, &l);
        for (int i = 0; i < l.n; i++) {
            if (strlen(l.e[i].name) >= RAMFS_NAME_MAX) continue;       /* never loaded: leave it be */
            RamNode *c = RamfsFind(r, l.e[i].name);
            if (!c || c->dir != l.e[i].dir || (!c->dir && (c->pflags & RAMFS_F_SEALED)))
                if (!FatDelete(g_vol, fdir, l.e[i].name)) save_error("delete failed", r);
        }
        kfree(l.e);
    }
    for (RamNode *c = r->child; c; c = c->next) {
        if (!(c->pflags & CLEAR)) continue;
        if (c->dir) {
            FatEntry have;
            bool existed = !fresh && FatLookup(g_vol, fdir, c->name, &have) && have.dir;
            UINT32 sub;
            if (existed) sub = have.cluster;
            else if (!FatMkdir(g_vol, fdir, c->name, &sub)) { save_error("disk full?", c); continue; }
            save_dir(c, sub, !existed, depth + 1);
        } else if (c->pflags & RAMFS_F_DIRTY) {
            if (!(c->pflags & RAMFS_F_SEALED) && !FatWriteFile(g_vol, fdir, c->name, c->data, c->size)) {
                save_error("disk full?", c);
                continue;
            }
        }
        c->pflags &= (UINT8)~CLEAR;
    }
    r->pflags &= (UINT8)~CLEAR;
}

static void save_removed(void)
{
    /* starter files the user has since recreated are no longer deleted */
    for (Removed **pp = &g_removed; *pp;) {
        if (RamfsResolve(NULL, (*pp)->path)) {
            Removed *r = *pp;
            *pp = r->next;
            kfree(r);
            g_removed_dirty = true;
        } else pp = &(*pp)->next;
    }
    if (!g_removed_dirty) return;
    UINT32 len = 0;
    for (Removed *r = g_removed; r; r = r->next) len += (UINT32)strlen(r->path) + 2;
    char *text = kmalloc(len + 1), *p = text;
    if (!text) return;
    for (Removed *r = g_removed; r; r = r->next) p += ksnprintf(p, (size_t)(len + 1 - (UINT32)(p - text)), "%s\r\n", r->path);
    if (FatWriteFile(g_vol, g_nova_dir, DELETED_FILE, text, (UINT32)(p - text))) g_removed_dirty = false;
    kfree(text);
}

bool PersistSync(void)
{
    if (!g_vol || !g_loaded) return g_vol == NULL;
    DesktopLock();
    UINT32 changes = RamfsChanges();
    g_error = false;
    if (!g_root_known) {
        if (FatMkdirPath(g_vol, "\\NOVA", &g_nova_dir) && FatMkdirPath(g_vol, DATA_ROOT, &g_root_dir))
            g_root_known = true;
        else g_error = true;
    }
    if (g_root_known) {
        RamNode *root = RamfsRoot();
        if (root->pflags & CLEAR) save_dir(root, g_root_dir, false, 0);
        save_removed();
    }
    bool ok = FatSync(g_vol) && !g_error;
    g_saved_changes = changes;
    DesktopUnlock();
    return ok;
}

void PersistPoll(void)
{
    if (!g_vol || !g_loaded) return;
    UINT32 now = RamfsChanges();
    UINT64 t = sched_ticks();
    if (now != g_seen_changes) { g_seen_changes = now; g_seen_tick = t; }
    if (now == g_saved_changes && !g_removed_dirty) return;
    if (t - g_seen_tick < 100) return;                    /* wait for a quiet second */
    if (g_failed && t < g_retry_tick) return;
    g_failed = !PersistSync();
    if (g_failed) g_retry_tick = t + 3000;                /* try again in 30 s */
}
