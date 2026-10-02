/*
 * persist.c — keeps drive C: on disk (see persist.h)
 *
 * Saving walks the RAM disk along the RAMFS_F_SUB marks to the nodes that
 * changed: a changed file is written whole, a directory whose entries
 * changed has the disk entries it no longer holds deleted, and a moved
 * directory is written out in full.  Starter files the user deletes are
 * listed in a file (\NOVA\DELETED.TXT on FAT, \$NovaOS\Deleted.txt on
 * NTFS) so they stay deleted after a restart.
 *
 * On FAT, C: is the folder \NOVA\C and keeps names, contents, write times
 * and the read-only, hidden and system attributes.  On NTFS, C: is the
 * whole volume and also keeps creation times and security descriptors: a
 * node with its own descriptor gets it in $Secure, one that inherits gets
 * the root directory's (and reads back as inheriting).
 */

#include "persist.h"
#include "fat.h"
#include "ntfs.h"
#include "ramfs.h"
#include "block.h"
#include "../drivers/ahci.h"
#include "../drivers/nvme.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../um/um.h"

#define DATA_ROOT     "\\NOVA\\C"
#define DELETED_FILE  "DELETED.TXT"          /* in \NOVA */
#define NTFS_META     "$NovaOS"              /* NTFS: the root folder that holds it */
#define NTFS_DELETED  "Deleted.txt"
#define DATA_LABEL    "NOVADATA"

/* The volume: FAT or NTFS */
static FatVol  *g_vol;
static NtfsVol *g_ntfs;
static BlockDev *g_ntfs_dev;
static UINT64  g_lba;                         /* where the volume starts */
static UINT32  g_root_secid;                  /* NTFS: the root's descriptor (what inheriting nodes get) */
static UINT64  g_root_dir;                    /* \NOVA\C's cluster (FAT), the root (NTFS) */
static bool    g_root_known;
static UINT64  g_nova_dir;                    /* where the deleted-files list is */
static bool    g_loaded;
static UINT32  g_saved_changes, g_seen_changes;
static UINT64  g_seen_tick, g_retry_tick;
static bool    g_failed;

static bool have_vol(void) { return g_vol || g_ntfs; }

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
typedef struct { FatVol *vol; NtfsVol *ntfs; BlockDev *dev; UINT64 lba; } Cand;

static bool is_zero(const UINT8 *p, int n)
{
    for (int i = 0; i < n; i++) if (p[i]) return false;
    return true;
}

static bool mount_at(BlockDev *d, UINT64 lba, Cand *c)
{
    FatVol *v = FatMount(d, lba);
    if (v) { *c = (Cand){ v, NULL, d, lba }; return true; }
    NtfsVol *n = NtfsMount(d, lba);
    if (n) { *c = (Cand){ NULL, n, d, lba }; return true; }
    return false;
}

static void cand_free(Cand *c)
{
    if (c->vol) FatUnmount(c->vol);
    if (c->ntfs) NtfsUnmount(c->ntfs);
}

static const char *cand_label(const Cand *c) { return c->vol ? FatLabel(c->vol) : NtfsLabel(c->ntfs); }

/* The FAT and NTFS volumes on @d (a whole-disk volume, or MBR/GPT partitions) */
static int find_volumes(BlockDev *d, Cand *out, int max, bool *blank)
{
    UINT8 *s = kmalloc(2 * BLOCK_SECTOR);
    int n = 0;
    *blank = false;
    if (!s || !d->read(d, 0, 2, s)) { kfree(s); return 0; }
    if (mount_at(d, 0, &out[n])) {                                /* a whole-disk volume */
        n++;
    } else if (is_zero(s, 2 * BLOCK_SECTOR)) {
        *blank = true;
    } else if (s[510] == 0x55 && s[511] == 0xAA) {
        bool gpt = false;
        for (int i = 0; i < 4 && n < max; i++) {
            const UINT8 *e = s + 446 + 16 * i;
            UINT32 lba = *(const UINT32 *)(e + 8);
            if (e[4] == 0xEE) gpt = true;
            else if (e[4] && lba && mount_at(d, lba, &out[n])) n++;
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
                    if (mount_at(d, first, &out[n])) n++;
                }
            }
            kfree(ent);
        }
    }
    kfree(s);
    return n;
}

int PersistFindVolumes(BlockDev *d, FatVol **out, int max, bool *blank)
{
    Cand c[16];
    int n = find_volumes(d, c, 16, blank), k = 0;
    for (int i = 0; i < n; i++) {
        if (c[i].vol && k < max) { out[k++] = c[i].vol; c[i].vol = NULL; }
        cand_free(&c[i]);
    }
    return k;
}

int PersistDiskLabels(BlockDev *d, char *out, int cap, bool *blank)
{
    Cand c[16];
    int n = find_volumes(d, c, 16, blank), len = 0;
    if (cap) out[0] = '\0';
    for (int i = 0; i < n; i++) {
        const char *l = cand_label(&c[i]);
        if (*l && len < cap - 16)
            len += ksnprintf(out + len, (size_t)(cap - len), "%s%s", len ? ", " : "", l);
        cand_free(&c[i]);
    }
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

/* Take NTFS volume @v for C: (it must be writable) */
static bool use_ntfs(NtfsVol *v, BlockDev *d)
{
    NtfsSetClock(RamfsNow);
    if (!NtfsEnableWrite(v)) return false;
    g_ntfs = v;
    g_ntfs_dev = d;
    return true;
}

void PersistInit(void)
{
    AhciInit();
    NvmeInit();
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
        if (path_eq(cand_label(&c[i]), DATA_LABEL) && (c[i].vol || use_ntfs(c[i].ntfs, c[i].dev))) pick = i;
    if (pick < 0 && blank) g_vol = format_blank(blank);
    for (int i = 0; i < n && pick < 0 && !g_vol; i++) {
        FatEntry e;
        if (c[i].vol && FatLookupPath(c[i].vol, "\\EFI\\NOVA\\kernel.elf", &e)) pick = i;
    }
    if (pick >= 0 && c[pick].vol) g_vol = c[pick].vol;
    if (pick >= 0) g_lba = c[pick].lba;
    else if (g_vol) g_lba = 2048;                                 /* (format_blank's partition) */
    for (int i = 0; i < n; i++)
        if (i != pick) cand_free(&c[i]);
    RamfsSetRemovedHook(removed_hook);
    if (have_vol()) {
        char d[96];
        PersistDescribe(d, sizeof(d));
        kprintf("[PERSIST] Drive C: is saved to %s\n", d);
    } else {
        kprintf("[PERSIST] No disk to save to: drive C: lasts until restart\n");
    }
}

bool PersistActive(void) { return have_vol(); }

bool PersistOwns(BlockDev *d, UINT64 lba) { return have_vol() && PersistDevice() == d && g_lba == lba; }

BlockDev *PersistDevice(void) { return g_vol ? FatDevice(g_vol) : g_ntfs ? g_ntfs_dev : NULL; }

static void drop_vol(void)
{
    if (g_vol) FatUnmount(g_vol);
    if (g_ntfs) NtfsUnmount(g_ntfs);
    g_vol = NULL;
    g_ntfs = NULL;
    g_ntfs_dev = NULL;
    g_root_known = false;
}

void PersistDetach(void)
{
    DesktopLock();
    if (have_vol()) kprintf("[PERSIST] Stopped saving to %s\n", PersistDevice()->name);
    drop_vol();
    DesktopUnlock();
}

/* Everything not from the OS image is to be saved */
static void mark_all(RamNode *n)
{
    if (!(n->pflags & RAMFS_F_SEALED) || n->dir)
        n->pflags |= n->dir ? (RAMFS_F_DIRTY | RAMFS_F_DIRTYDIR | RAMFS_F_SUB) : RAMFS_F_DIRTY;
    for (RamNode *c = n->child; c; c = c->next) {
        mark_all(c);
        n->pflags |= RAMFS_F_SUB;
    }
}

bool PersistAdopt(BlockDev *d, UINT64 lba)
{
    Cand c;
    if (!mount_at(d, lba, &c)) return false;
    DesktopLock();
    drop_vol();
    g_lba = lba;
    if (c.vol) g_vol = c.vol;
    else if (!use_ntfs(c.ntfs, d)) { NtfsUnmount(c.ntfs); DesktopUnlock(); return false; }
    if (g_ntfs) {                                                 /* the new volume's root descriptor goes with it */
        RamNode *root = RamfsRoot();
        g_root_secid = NtfsSecurityId(g_ntfs, NTFS_ROOT);
        UINT8 *sd = kmalloc(4096);
        UINT32 len;
        if (!root->sd && sd && g_root_secid && NtfsSecurityById(g_ntfs, g_root_secid, sd, 4096, &len)) {
            root->sd = sd; root->sdlen = len; sd = NULL;
        }
        kfree(sd);
    }
    g_loaded = true;
    mark_all(RamfsRoot());
    g_removed_dirty = g_removed != NULL;
    DesktopUnlock();
    bool ok = PersistSync();
    char desc[96];
    PersistDescribe(desc, sizeof(desc));
    kprintf("[PERSIST] Drive C: is now saved to %s%s\n", desc, ok ? "" : " (the first save failed)");
    return ok;
}

bool PersistSpace(UINT64 *free, UINT64 *total)
{
    if (g_vol) { *free = FatFreeBytes(g_vol); *total = FatTotalBytes(g_vol); return true; }
    if (g_ntfs) { *free = NtfsFreeBytes(g_ntfs); *total = NtfsTotalBytes(g_ntfs); return true; }
    return false;
}

void PersistWhere(char *buf, int cap)
{
    if (!have_vol()) { ksnprintf(buf, (size_t)cap, "memory only"); return; }
    ksnprintf(buf, (size_t)cap, "%s (%s)", PersistDevice()->name, g_vol ? FatLabel(g_vol) : NtfsLabel(g_ntfs));
}

void PersistDescribe(char *buf, int cap)
{
    if (!have_vol()) { ksnprintf(buf, (size_t)cap, "memory only (no disk found)"); return; }
    UINT64 free, total;
    PersistSpace(&free, &total);
    char fs[8];
    if (g_vol) ksnprintf(fs, sizeof(fs), "FAT%d", FatType(g_vol));
    else ksnprintf(fs, sizeof(fs), "NTFS");
    ksnprintf(buf, (size_t)cap, "%s %s \"%s\", %u MiB free of %u MiB", PersistDevice()->name, fs,
              g_vol ? FatLabel(g_vol) : NtfsLabel(g_ntfs), (unsigned)(free >> 20), (unsigned)(total >> 20));
}

static UINT32 filetime_to_dos(UINT64 ft);

/* ---------------------------------------------------------------------------
 * The volume's files, FAT or NTFS
 * ------------------------------------------------------------------------- */
typedef struct {
    char     name[RAMFS_NAME_MAX];
    bool     dir;
    UINT64   ref;                    /* FAT: a directory's first cluster; NTFS: the record */
    UINT64   size, ctime, mtime;
    UINT32   attrs;
    FatEntry fe;                     /* FAT: the entry itself */
} PEnt;

typedef struct { PEnt *e; int n, cap; } EntList;

static PEnt *list_add(EntList *l)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        PEnt *ne = kmalloc(sizeof(PEnt) * (size_t)cap);
        if (!ne) return NULL;
        if (l->n) memcpy(ne, l->e, sizeof(PEnt) * (size_t)l->n);
        kfree(l->e);
        l->e = ne;
        l->cap = cap;
    }
    PEnt *e = &l->e[l->n++];
    memset(e, 0, sizeof(*e));
    return e;
}

static bool collect_fat(const FatEntry *f, void *ctx)
{
    if (strlen(f->name) >= RAMFS_NAME_MAX) return true;           /* never loaded: left be */
    PEnt *e = list_add(ctx);
    if (!e) return false;
    strcpy(e->name, f->name);
    e->dir = f->dir;
    e->ref = f->cluster;
    e->size = f->size;
    e->ctime = e->mtime = PersistDosToFiletime(f->wtime);
    e->attrs = f->attr & 0x07;
    e->fe = *f;
    return true;
}

static bool collect_ntfs(const NtfsEntry *f, void *ctx)
{
    if (strlen(f->name) >= RAMFS_NAME_MAX) return true;
    PEnt *e = list_add(ctx);
    if (!e) return false;
    strcpy(e->name, f->name);
    e->dir = f->dir;
    e->ref = f->mft;
    e->size = f->size;
    e->ctime = f->ctime;
    e->mtime = f->mtime;
    e->attrs = f->attrs & 0x07;
    return true;
}

/* Folders at the root of an NTFS C: that are not the user's files: ours,
 * and what Windows makes on every volume it sees */
static bool reserved(UINT64 dir, const char *name)
{
    return g_ntfs && dir == NTFS_ROOT &&
           (path_eq(name, NTFS_META) || path_eq(name, "System Volume Information") || path_eq(name, "$RECYCLE.BIN"));
}

static void vol_list(UINT64 dir, EntList *l)
{
    if (g_vol) FatList(g_vol, (UINT32)dir, collect_fat, l);
    else NtfsList(g_ntfs, dir, collect_ntfs, l);
    int k = 0;
    for (int i = 0; i < l->n; i++)
        if (!reserved(dir, l->e[i].name)) l->e[k++] = l->e[i];
    l->n = k;
}

static bool vol_read(const PEnt *e, void *buf)
{
    if (g_vol) return FatRead(g_vol, &e->fe, buf);
    return !e->size || NtfsRead(g_ntfs, e->ref, 0, buf, e->size);
}

static bool vol_lookup(UINT64 dir, const char *name, UINT64 *ref, bool *is_dir)
{
    if (g_vol) {
        FatEntry e;
        if (!FatLookup(g_vol, (UINT32)dir, name, &e)) return false;
        *ref = e.cluster;
        *is_dir = e.dir;
        return true;
    }
    return NtfsLookup(g_ntfs, dir, name, ref, is_dir);
}

/* NTFS: give record @ref @n's times, attributes and descriptor */
static bool ntfs_info(UINT64 ref, const RamNode *n)
{
    UINT32 id = g_root_secid;
    if (n->sd && n != RamfsRoot()) id = NtfsAddSecurity(g_ntfs, n->sd, n->sdlen);
    bool ok = n == RamfsRoot() || NtfsSetInfo(g_ntfs, ref, n->ctime, n->mtime, n->attrs & 0x07);
    if (id) ok = NtfsSetSecurityId(g_ntfs, ref, id) && ok;
    else if (n->sd) ok = false;
    return ok;
}

static bool vol_mkdir(UINT64 dir, const RamNode *n, UINT64 *sub)
{
    if (g_vol) {
        UINT32 c;
        if (!FatMkdir(g_vol, (UINT32)dir, n->name, &c)) return false;
        *sub = c;
        return true;
    }
    return NtfsCreate(g_ntfs, dir, n->name, true, sub);
}

static bool vol_write(UINT64 dir, const RamNode *n)
{
    if (g_vol) {
        FatSetStamp(filetime_to_dos(n->mtime));
        bool ok = FatWriteFile(g_vol, (UINT32)dir, n->name, n->data, n->size);
        FatSetStamp(0);
        return ok;
    }
    UINT64 ref;
    bool is_dir;
    if (!NtfsLookup(g_ntfs, dir, n->name, &ref, &is_dir)) {
        if (!NtfsCreate(g_ntfs, dir, n->name, false, &ref)) return false;
    } else if (is_dir) return false;
    return NtfsWriteFile(g_ntfs, ref, n->data, n->size) && ntfs_info(ref, n);
}

/* Delete @e from @dir: a directory with everything in it */
static bool vol_delete(UINT64 dir, const PEnt *e, int depth)
{
    if (g_vol) return FatDelete(g_vol, (UINT32)dir, e->name);
    if (e->dir) {
        if (depth > 24) return false;
        EntList l = { 0 };
        NtfsList(g_ntfs, e->ref, collect_ntfs, &l);
        bool ok = true;
        for (int i = 0; i < l.n; i++) ok = vol_delete(e->ref, &l.e[i], depth + 1) && ok;
        kfree(l.e);
        if (!ok) return false;
    }
    return NtfsDelete(g_ntfs, dir, e->ref);
}

static bool vol_write_meta(UINT64 dir, const char *name, const void *data, UINT32 len)
{
    if (g_vol) return FatWriteFile(g_vol, (UINT32)dir, name, data, len);
    UINT64 ref;
    bool is_dir;
    if (!NtfsLookup(g_ntfs, dir, name, &ref, &is_dir) && !NtfsCreate(g_ntfs, dir, name, false, &ref)) return false;
    return NtfsWriteFile(g_ntfs, ref, data, len);
}

static bool vol_sync(void) { return g_vol ? FatSync(g_vol) : NtfsSync(g_ntfs); }

/* ---------------------------------------------------------------------------
 * Restoring
 * ------------------------------------------------------------------------- */
static int g_restored;

/* File times: RAM nodes keep FILETIMEs (100 ns since 1601), FAT keeps DOS
 * date and time (2-second steps, from 1980) */
static INT64 days_from_civil(INT64 y, unsigned m, unsigned d)
{
    y -= m <= 2;
    INT64 era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (INT64)doe - 719468;              /* days since 1970-01-01 */
}

static UINT64 dos_to_filetime(UINT32 dos)
{
    unsigned date = dos >> 16, time = dos & 0xFFFF;
    unsigned d = date & 31, m = (date >> 5) & 15, y = 1980 + (date >> 9);
    if (!d || !m || m > 12) return 0;
    INT64 secs = days_from_civil(y, m, d) * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
    return ((UINT64)secs + UINT64_C(11644473600)) * 10000000ULL;
}

UINT64 PersistDosToFiletime(UINT32 dos) { return dos_to_filetime(dos); }

static UINT32 filetime_to_dos(UINT64 ft)
{
    if (!ft) return 0;
    INT64 secs = (INT64)(ft / 10000000ULL) - INT64_C(11644473600);
    if (secs < 315532800) return 0;                          /* before 1980 */
    INT64 z = secs / 86400 + 719468, rem = secs % 86400;
    INT64 era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    INT64 y = (INT64)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    if (y > 2107) return 0;
    UINT32 date = (UINT32)((y - 1980) << 9 | m << 5 | d);
    UINT32 time = (UINT32)((rem / 3600) << 11 | ((rem / 60) % 60) << 5 | (rem % 60) / 2);
    return date << 16 | time;
}

/* NTFS: the descriptor of record @ref, unless it is the root's (inherited) */
static void load_sd(RamNode *n, UINT64 ref)
{
    UINT32 id = NtfsSecurityId(g_ntfs, ref), len;
    if (!id || id == g_root_secid) return;
    UINT8 *sd = kmalloc(4096);
    if (sd && NtfsSecurityById(g_ntfs, id, sd, 4096, &len)) {
        kfree(n->sd);
        n->sd = sd;
        n->sdlen = len;
        return;
    }
    kfree(sd);
}

static void load_dir(UINT64 vdir, RamNode *rdir, int depth)
{
    if (depth > 24) return;
    EntList l = { 0 };
    vol_list(vdir, &l);
    for (int i = 0; i < l.n; i++) {
        const PEnt *e = &l.e[i];
        RamNode *have = RamfsFind(rdir, e->name);
        if (have && (have->dir != e->dir || ((have->pflags & RAMFS_F_SEALED) && !have->dir))) continue;
        if (e->dir) {
            RamNode *d = RamfsCreate(rdir, e->name, true);
            if (!d) continue;
            if (g_ntfs) {
                load_sd(d, e->ref);
                if (e->ctime) d->ctime = e->ctime;
                if (e->mtime) d->mtime = e->mtime;
                d->attrs = e->attrs;
            }
            load_dir(e->ref, d, depth + 1);
            continue;
        }
        UINT64 size = e->size;
        if (g_ntfs && !NtfsSize(g_ntfs, e->ref, &size)) continue;   /* (the index may lag behind) */
        if (size > RAMFS_FILE_MAX) continue;
        PEnt real = *e;
        real.size = size;
        char *buf = size ? kmalloc(size) : NULL;
        if (size && !buf) continue;
        RamNode *f = RamfsCreate(rdir, e->name, false);
        if (f && vol_read(&real, buf) && RamfsWrite(f, buf, (UINT32)size)) {
            g_restored++;
            if (e->mtime) f->mtime = e->mtime;
            f->ctime = e->ctime ? e->ctime : f->mtime;
            f->attrs = e->attrs;
            if (g_ntfs) load_sd(f, e->ref);
        }
        kfree(buf);
    }
    kfree(l.e);
}

static void load_removed(void)
{
    UINT64 ref;
    bool is_dir;
    const char *name = g_vol ? DELETED_FILE : NTFS_DELETED;
    if (!vol_lookup(g_nova_dir, name, &ref, &is_dir) || is_dir) return;
    EntList l = { 0 };
    vol_list(g_nova_dir, &l);
    const PEnt *e = NULL;
    for (int i = 0; i < l.n && !e; i++) if (path_eq(l.e[i].name, name)) e = &l.e[i];
    UINT64 size = e ? e->size : 0;
    if (e && g_ntfs) NtfsSize(g_ntfs, e->ref, &size);
    char *text = e && size <= 256 * 1024 ? kmalloc(size + 1) : NULL;
    PEnt real;
    if (e) { real = *e; real.size = size; }
    if (text && vol_read(&real, text)) {
        text[size] = '\0';
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
    kfree(l.e);
    g_removed_dirty = false;
}

void PersistLoad(void)
{
    if (g_vol) {
        RamfsSetMode(RAMFS_LOADING);
        FatEntry nova, root;
        if (FatLookupPath(g_vol, "\\NOVA", &nova) && nova.dir) {
            g_nova_dir = nova.cluster;
            if (FatLookup(g_vol, (UINT32)g_nova_dir, "C", &root) && root.dir) {
                g_root_dir = root.cluster;
                g_root_known = true;
                load_dir(g_root_dir, RamfsRoot(), 0);
            }
            load_removed();
        }
        kprintf("[PERSIST] Restored %d file(s) to drive C:\n", g_restored);
    } else if (g_ntfs) {
        RamfsSetMode(RAMFS_LOADING);
        RamNode *root = RamfsRoot();
        g_root_secid = NtfsSecurityId(g_ntfs, NTFS_ROOT);
        load_sd(root, NTFS_ROOT);
        if (!root->sd && g_root_secid) {                          /* (load_sd leaves the root's own out) */
            UINT8 *sd = kmalloc(4096);
            UINT32 len;
            if (sd && NtfsSecurityById(g_ntfs, g_root_secid, sd, 4096, &len)) { root->sd = sd; root->sdlen = len; }
            else kfree(sd);
        }
        g_root_dir = NTFS_ROOT;
        bool is_dir;
        UINT64 meta;
        if (vol_lookup(NTFS_ROOT, NTFS_META, &meta, &is_dir) && is_dir) { g_nova_dir = meta; g_root_known = true; }
        load_dir(NTFS_ROOT, root, 0);
        if (g_root_known) load_removed();
        kprintf("[PERSIST] Restored %d file(s) to drive C: (NTFS)\n", g_restored);
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

/* Brings the disk directory @vdir in line with the RAM directory @r. */
static void save_dir(RamNode *r, UINT64 vdir, bool fresh, int depth)
{
    if (depth > 24) return;
    if ((r->pflags & RAMFS_F_DIRTYDIR) && !fresh) {
        EntList l = { 0 };
        vol_list(vdir, &l);
        for (int i = 0; i < l.n; i++) {
            RamNode *c = RamfsFind(r, l.e[i].name);
            if (!c || c->dir != l.e[i].dir || (!c->dir && (c->pflags & RAMFS_F_SEALED)))
                if (!vol_delete(vdir, &l.e[i], 0)) save_error("delete failed", r);
        }
        kfree(l.e);
    }
    for (RamNode *c = r->child; c; c = c->next) {
        if (!(c->pflags & CLEAR)) continue;
        if (c->dir) {
            UINT64 sub;
            bool is_dir = false;
            bool existed = !fresh && vol_lookup(vdir, c->name, &sub, &is_dir) && is_dir;
            if (!existed && !vol_mkdir(vdir, c, &sub)) { save_error("disk full?", c); continue; }
            if (g_ntfs && (!existed || (c->pflags & RAMFS_F_DIRTY)) && !ntfs_info(sub, c)) save_error("its details", c);
            save_dir(c, sub, !existed, depth + 1);
        } else if (c->pflags & RAMFS_F_DIRTY) {
            if (!(c->pflags & RAMFS_F_SEALED) && !vol_write(vdir, c)) {
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
    if (vol_write_meta(g_nova_dir, g_vol ? DELETED_FILE : NTFS_DELETED, text, (UINT32)(p - text))) g_removed_dirty = false;
    kfree(text);
}

/* Where the files go: \NOVA\C on FAT, the root on NTFS (with \$NovaOS for the list) */
static bool find_root(void)
{
    if (g_vol) {
        UINT32 nova, root;
        if (!FatMkdirPath(g_vol, "\\NOVA", &nova) || !FatMkdirPath(g_vol, DATA_ROOT, &root)) return false;
        g_nova_dir = nova;
        g_root_dir = root;
        return true;
    }
    UINT64 meta;
    bool is_dir;
    if (!vol_lookup(NTFS_ROOT, NTFS_META, &meta, &is_dir)) {
        if (!NtfsCreate(g_ntfs, NTFS_ROOT, NTFS_META, true, &meta)) return false;
        NtfsSetInfo(g_ntfs, meta, 0, 0, 0x06);                     /* hidden, system */
    } else if (!is_dir) return false;
    g_nova_dir = meta;
    g_root_dir = NTFS_ROOT;
    return true;
}

bool PersistSync(void)
{
    if (!have_vol() || !g_loaded) return !have_vol();
    DesktopLock();
    UINT32 changes = RamfsChanges();
    g_error = false;
    if (!g_root_known) {
        if (find_root()) g_root_known = true;
        else g_error = true;
    }
    if (g_root_known) {
        RamNode *root = RamfsRoot();
        if (g_ntfs && (root->pflags & RAMFS_F_DIRTY) && root->sd) {    /* the root's own descriptor changed */
            UINT32 id = NtfsAddSecurity(g_ntfs, root->sd, root->sdlen);
            if (!id || !NtfsSetSecurityId(g_ntfs, NTFS_ROOT, id)) save_error("its descriptor", root);
        }
        if (root->pflags & CLEAR) save_dir(root, g_root_dir, false, 0);
        save_removed();
    }
    bool ok = vol_sync() && !g_error;
    g_saved_changes = changes;
    DesktopUnlock();
    return ok;
}

void PersistPoll(void)
{
    if (!have_vol() || !g_loaded) return;
    UINT32 now = RamfsChanges();
    UINT64 t = sched_ticks();
    if (now != g_seen_changes) { g_seen_changes = now; g_seen_tick = t; }
    if (now == g_saved_changes && !g_removed_dirty) return;
    if (t - g_seen_tick < 100) return;                    /* wait for a quiet second */
    if (g_failed && t < g_retry_tick) return;
    g_failed = !PersistSync();
    if (g_failed) g_retry_tick = t + 3000;                /* try again in 30 s */
}
