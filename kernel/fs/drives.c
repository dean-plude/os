/*
 * drives.c — the disks' volumes as drives D:, E:, ... (see drives.h)
 *
 * A disk holds a volume of its own (no partition table) or partitions in
 * an MBR or a GPT; each partition that is an NTFS volume is mounted into
 * drive C:'s tree of nodes (ramfs.h), writable when Windows left it clean
 * (ntfs.c).  On removable disks (USB sticks) FAT volumes are mounted too,
 * read-only; the fixed disks' FAT volumes are NovaOS's own (the EFI
 * partition, the one drive C: is saved to).
 */

#include "drives.h"
#include "ntfs.h"
#include "fat.h"
#include "ramfs.h"
#include "block.h"
#include "persist.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../um/um.h"
#include "../wm/wm.h"
#include "../ke/scheduler.h"

/* ---- NTFS ---- */

static bool ntfs_list(void *vol, UINT64 ref, bool (*add)(const RamfsExtEntry *e, void *ctx), void *ctx);
static bool ntfs_size(void *vol, UINT64 ref, UINT64 *size) { return NtfsSize(vol, ref, size); }
static bool ntfs_read(void *vol, UINT64 ref, UINT64 off, void *buf, UINT64 len) { return NtfsRead(vol, ref, off, buf, len); }

static bool ntfs_create(void *vol, UINT64 dir, const char *name, bool is_dir, UINT64 *ref)
{
    return NtfsCreate(vol, dir, name, is_dir, ref) && NtfsSync(vol);
}
static bool ntfs_remove(void *vol, UINT64 dir, UINT64 ref, const char *name)
{
    return NtfsDelete(vol, dir, ref, name) && NtfsSync(vol);
}
static bool ntfs_rename(void *vol, UINT64 dir, UINT64 ref, const char *old_name, UINT64 to, const char *name)
{
    return NtfsRename(vol, dir, ref, old_name, to, name) && NtfsSync(vol);
}
static bool ntfs_link(void *vol, UINT64 ref, UINT64 dir, const char *name)
{
    return NtfsLink(vol, ref, dir, name) && NtfsSync(vol);
}
static UINT32 ntfs_links(void *vol, UINT64 ref) { return NtfsLinks(vol, ref); }
static bool ntfs_write(void *vol, UINT64 ref, const void *data, UINT64 len)
{
    return NtfsWriteFile(vol, ref, data, len) && NtfsSync(vol);
}

static UINT64 ntfs_free(void *vol) { return NtfsFreeBytes(vol); }
static bool ntfs_can_write(void *vol, UINT64 ref) { return NtfsCanWrite(vol, ref); }

static const RamfsSource g_ntfs_source = { ntfs_list, ntfs_size, ntfs_read, NULL, NULL, NULL, NULL, NULL, NULL,
                                           NULL, ntfs_links };
static const RamfsSource g_ntfs_rw_source = { ntfs_list, ntfs_size, ntfs_read, ntfs_create, ntfs_remove,
                                              ntfs_rename, ntfs_write, ntfs_free, ntfs_can_write, ntfs_link, ntfs_links };

typedef struct { bool (*add)(const RamfsExtEntry *e, void *ctx); void *ctx; } ListCtx;

static bool ntfs_one(const NtfsEntry *e, void *ctx)
{
    ListCtx *l = ctx;
    RamfsExtEntry x;
    memcpy(x.name, e->name, sizeof(x.name));
    x.dir = e->dir;
    x.ref = e->mft;
    x.size = e->size;
    x.ctime = e->ctime;
    x.mtime = e->mtime;
    x.attrs = e->attrs;
    return l->add(&x, l->ctx);
}

static bool ntfs_list(void *vol, UINT64 ref, bool (*add)(const RamfsExtEntry *e, void *ctx), void *ctx)
{
    ListCtx l = { add, ctx };
    return NtfsList(vol, ref, ntfs_one, &l);
}

/* ---- FAT ----
 * FAT names files by their directory entry, not by a number, so the
 * entries listed so far are kept in a table and a node's ref is its index
 * there plus one (0 is the root directory). */

typedef struct {
    FatVol   *v;
    FatEntry *ents;
    UINT32    n, cap;
} FatSrc;

typedef struct { FatSrc *s; ListCtx l; } FatListCtx;

static bool fat_one(const FatEntry *e, void *ctx)
{
    FatListCtx *c = ctx;
    FatSrc *s = c->s;
    if (s->n == s->cap) {
        UINT32 cap = s->cap ? s->cap * 2 : 256;
        FatEntry *ne = kmalloc(sizeof(FatEntry) * cap);
        if (!ne) return false;
        if (s->n) memcpy(ne, s->ents, sizeof(FatEntry) * s->n);
        kfree(s->ents);
        s->ents = ne;
        s->cap = cap;
    }
    s->ents[s->n++] = *e;
    RamfsExtEntry x;
    memset(&x, 0, sizeof(x));
    strncpy(x.name, e->name, sizeof(x.name) - 1);
    x.dir = e->dir;
    x.ref = s->n;
    x.size = e->size;
    x.ctime = x.mtime = PersistDosToFiletime(e->wtime);
    x.attrs = e->attr & 0x27u;
    return c->l.add(&x, c->l.ctx);
}

static bool fat_list(void *vol, UINT64 ref, bool (*add)(const RamfsExtEntry *e, void *ctx), void *ctx)
{
    FatSrc *s = vol;
    if (ref > s->n) return false;
    UINT32 dir = ref ? s->ents[ref - 1].cluster : FAT_ROOT;
    FatListCtx c = { s, { add, ctx } };
    return FatList(s->v, dir, fat_one, &c);
}

static bool fat_size(void *vol, UINT64 ref, UINT64 *size)
{
    FatSrc *s = vol;
    if (!ref || ref > s->n) return false;
    *size = s->ents[ref - 1].size;
    return true;
}

static bool fat_read(void *vol, UINT64 ref, UINT64 off, void *buf, UINT64 len)
{
    FatSrc *s = vol;
    if (!ref || ref > s->n) return false;
    const FatEntry *e = &s->ents[ref - 1];
    if (off + len > e->size) return false;
    if (off == 0 && len == e->size) return FatRead(s->v, e, buf);
    UINT8 *all = kmalloc(e->size ? e->size : 1);
    if (!all) return false;
    bool ok = FatRead(s->v, e, all);
    if (ok) memcpy(buf, all + off, (size_t)len);
    kfree(all);
    return ok;
}

static const RamfsSource g_fat_source = { fat_list, fat_size, fat_read, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* ---- mounting ---- */

#define MAX_MOUNTS 24

typedef struct {
    BlockDev *dev;
    char      letter;
    bool      ntfs;
    void     *vol;                         /* NtfsVol, or FatSrc */
} Mount;

static Mount g_mounts[MAX_MOUNTS];

static char free_letter(void)
{
    for (char l = 'D'; l <= 'Z'; l++)
        if (!RamfsDriveRoot(l)) return l;
    return 0;
}

static void record(BlockDev *d, char letter, bool ntfs, void *vol)
{
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (!g_mounts[i].dev) {
            g_mounts[i] = (Mount){ d, letter, ntfs, vol };
            return;
        }
}

static void try_mount(BlockDev *d, UINT64 lba)
{
    char letter = free_letter();
    if (!letter || PersistOwns(d, lba)) return;                  /* (drive C:'s volume is not D: too) */
    NtfsVol *v = NtfsMount(d, lba);
    if (v) {
        NtfsSetClock(RamfsNow);
        bool rw = NtfsEnableWrite(v);
        if (!RamfsMountDrive(letter, rw ? &g_ntfs_rw_source : &g_ntfs_source, v, NTFS_ROOT, NtfsLabel(v), "NTFS",
                             NtfsTotalBytes(v))) {
            NtfsUnmount(v);
            return;
        }
        record(d, letter, true, v);
        kprintf("[DRIVES] %c: is NTFS volume \"%s\" on %s (%s)\n", letter, NtfsLabel(v), d->name,
                rw ? "read-write" : "read-only");
        return;
    }
    if (!d->removable) return;
    FatVol *f = FatMount(d, lba);
    if (!f) return;
    FatSrc *s = kzalloc(sizeof(FatSrc));
    const char *fs = FatType(f) == 32 ? "FAT32" : "FAT16";
    if (!s || !RamfsMountDrive(letter, &g_fat_source, s, 0, FatLabel(f), fs, FatTotalBytes(f))) {
        kfree(s);
        FatUnmount(f);
        return;
    }
    s->v = f;
    record(d, letter, false, s);
    kprintf("[DRIVES] %c: is %s volume \"%s\" on %s (read-only)\n", letter, fs, FatLabel(f), d->name);
}

static void scan(BlockDev *d)
{
    UINT8 *s = kmalloc(2 * BLOCK_SECTOR);
    if (!s || !d->read(d, 0, 2, s)) { kfree(s); return; }
    bool mbr = s[510] == 0x55 && s[511] == 0xAA;
    /* A volume of its own: NTFS, or a FAT boot sector (no partition entries
     * with a sane type, but a BPB with 512-byte sectors) */
    bool whole_fat = mbr && (s[0] == 0xEB || s[0] == 0xE9) && s[11] == 0x00 && s[12] == 0x02 && s[13] &&
                     (!memcmp(s + 54, "FAT", 3) || !memcmp(s + 82, "FAT", 3));
    if (!memcmp(s + 3, "NTFS    ", 8) || whole_fat) {
        kfree(s);
        try_mount(d, 0);
        return;
    }
    if (!mbr) { kfree(s); return; }
    bool gpt = false;
    for (int i = 0; i < 4; i++) {
        const UINT8 *e = s + 446 + 16 * i;
        UINT32 lba;
        memcpy(&lba, e + 8, 4);
        UINT8 type = e[4];
        if (type == 0xEE) gpt = true;
        else if (type == 0x07 && lba) try_mount(d, lba);         /* IFS: NTFS (or exFAT, which neither mounts) */
        else if (lba && (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B ||
                         type == 0x0C || type == 0x0E)) try_mount(d, lba);   /* FAT12/16/32 */
    }
    if (gpt && !memcmp(s + BLOCK_SECTOR, "EFI PART", 8)) {
        UINT64 table;
        UINT32 count, esize;
        memcpy(&table, s + BLOCK_SECTOR + 72, 8);
        memcpy(&count, s + BLOCK_SECTOR + 80, 4);
        memcpy(&esize, s + BLOCK_SECTOR + 84, 4);
        UINT8 *ent = kmalloc(BLOCK_SECTOR);
        if (ent && esize >= 128 && esize <= BLOCK_SECTOR && count <= 1024) {   /* (128 entries is usual; xorriso writes 248) */
            UINT32 per = BLOCK_SECTOR / esize;
            for (UINT32 i = 0; i < count; i++) {
                if (i % per == 0 && !d->read(d, table + i / per, 1, ent)) break;
                const UINT8 *e = ent + (i % per) * esize;
                static const UINT8 zero[16];
                if (!memcmp(e, zero, 16)) continue;
                UINT64 first;
                memcpy(&first, e + 32, 8);
                try_mount(d, first);                             /* the mounts check the boot sector */
            }
        }
        kfree(ent);
    }
    kfree(s);
}

void DrivesInit(void)
{
    for (int i = 0; i < BlockCount(); i++) scan(BlockGet(i));
}

void DrivesAttach(BlockDev *d)
{
    DesktopLock();
    scan(d);
    WmInvalidate();                          /* (File Explorer lists the drives) */
    DesktopUnlock();
}

void DrivesPoll(void)
{
    static UINT32 seen;
    static UINT64 since;
    UINT32 c = RamfsExtChanges();
    UINT64 t = sched_ticks();
    if (c != seen) { seen = c; since = t; }
    if (!RamfsExtDirty() || t - since < 100) return;         /* wait for a quiet second */
    DrivesSync();
}

bool DrivesSync(void)
{
    DesktopLock();
    bool ok = RamfsFlush();
    DesktopUnlock();
    return ok;
}

void DrivesDetach(BlockDev *d)
{
    DesktopLock();
    for (int i = 0; i < MAX_MOUNTS; i++) {
        Mount *m = &g_mounts[i];
        if (m->dev != d) continue;
        RamfsUnmountDrive(m->letter);
        kprintf("[DRIVES] %c: removed (%s went away)\n", m->letter, d->name);
        if (m->ntfs) NtfsUnmount(m->vol);
        else {
            FatSrc *s = m->vol;
            FatUnmount(s->v);
            kfree(s->ents);
            kfree(s);
        }
        memset(m, 0, sizeof(*m));
    }
    WmInvalidate();
    DesktopUnlock();
}
