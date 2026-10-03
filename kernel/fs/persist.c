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
 *
 * A file with several names (hard links) is one NTFS record with a name in
 * each directory.  FAT has no links: each name is saved as a copy, and
 * \NOVA\LINKS.TXT lists the names of each such file so they are joined
 * again at the next boot.
 *
 * A save holds no lock while the disk works (see "Saving" below): it
 * copies what changed under the file-system lock, then writes the copy on
 * the "persist" thread without it, and without the big kernel lock.
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
#include "../ke/smp.h"
#include "../ke/waitq.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"
#include "../um/um.h"

#define DATA_ROOT     "\\NOVA\\C"
#define DELETED_FILE  "DELETED.TXT"          /* in \NOVA */
#define LINKS_FILE    "LINKS.TXT"            /* in \NOVA (FAT): the names of each hard-linked file */
#define NTFS_META     "$NovaOS"              /* NTFS: the root folder that holds it */
#define NTFS_DELETED  "Deleted.txt"
#define DATA_LABEL    "NOVADATA"
#define PANIC_FILE    "PANIC.TXT"            /* in \NOVA (FAT): set aside for a kernel crash report (um/um_crash.c) */
#define PANIC_BYTES   (64 * 1024)

static UINT64 tsc_us(UINT64 tsc) { return g_tsc_per_tick ? tsc * 10000 / g_tsc_per_tick : 0; }

/* The volume: FAT or NTFS (changed and used under the save lock, see
 * save_lock; have_vol and g_dev may be looked at without it) */
static FatVol  *g_vol;
static NtfsVol *g_ntfs;
static BlockDev *g_ntfs_dev;
static BlockDev *g_dev;                       /* the volume's disk */
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
static void load_links(void);
static char *g_links_text;

/* The save lock: one save (or change of volume) at a time.  Taken after
 * the desktop lock and the file-system lock, if at all (so a save that
 * waits for it holds them meanwhile), and waited for asleep (the big
 * kernel lock goes to others meanwhile). */
static volatile UINT32 g_saving;
static WaitQueue g_saving_q = WAITQ_INIT;

static bool save_trylock(void) { return !__atomic_exchange_n(&g_saving, 1, __ATOMIC_ACQUIRE); }

static void save_lock(void)
{
    for (;;) {
        UINT32 gen = waitq_gen(&g_saving_q);
        if (save_trylock()) return;
        waitq_wait(&g_saving_q, gen, 100);
    }
}

static void save_unlock(void)
{
    __atomic_store_n(&g_saving, 0, __ATOMIC_RELEASE);
    waitq_wake(&g_saving_q);
}

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
            if (esize >= 128 && esize <= BLOCK_SECTOR && count <= 1024 && ent) {   /* (128 entries is usual; xorriso writes 248) */
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

/* A power cut in the middle of a save can leave clusters marked as used
 * that no file reaches (fat.c): free them before drive C: is loaded */
static void reclaim(void)
{
    UINT64 t0 = rdtsc();
    FatReclaimInfo ri;
    bool ok = FatReclaim(g_vol, false, &ri);
    UINT64 us = tsc_us(rdtsc() - t0);
    if (!ok) {
        kprintf("[PERSIST] Drive C: was not closed cleanly, and the space no file uses could not be reclaimed\n");
    } else if (ri.scanned) {
        kprintf("[PERSIST] Drive C: was not closed cleanly: reclaimed %u cluster(s), %llu KiB no file used, "
                "in %llu.%02llu ms; %u shared or looping chain(s) left alone; %u clusters free\n",
                ri.reclaimed, (unsigned long long)ri.reclaimed * FatClusterBytes(g_vol) >> 10,
                (unsigned long long)(us / 1000), (unsigned long long)(us % 1000 / 10), ri.crossed, ri.free);
    }
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
    g_dev = g_vol ? FatDevice(g_vol) : g_ntfs_dev;
    for (int i = 0; i < n; i++)
        if (i != pick) cand_free(&c[i]);
    RamfsSetRemovedHook(removed_hook);
    if (g_vol) reclaim();
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

BlockDev *PersistDevice(void) { return g_dev; }

/* ---------------------------------------------------------------------------
 * The kernel crash slot (FAT): \NOVA\PANIC.TXT, PANIC_BYTES of zeros whose
 * clusters follow one another.  A kernel fault writes its report into
 * those sectors with the disk driver alone (no FAT changes, nothing
 * allocated: PersistPanicWrite); the next start hands it to
 * UmCrashKernelFound and blanks the file again.
 * ------------------------------------------------------------------------- */
static BlockDev *volatile g_panic_dev;          /* NULL: no slot */
static UINT64  g_panic_lba;
static UINT8  *g_panic_buf;                     /* PANIC_BYTES, allocated with the slot */
static volatile int g_panic_busy;

/* (FAT, \NOVA known; at boot or under the save lock) @take: pass on what it holds first */
static void panic_slot(bool take)
{
    g_panic_dev = NULL;
    FatEntry e;
    bool have = FatLookup(g_vol, (UINT32)g_nova_dir, PANIC_FILE, &e) && !e.dir;
    bool blank = have && e.size == PANIC_BYTES;
    if (!g_panic_buf && !(g_panic_buf = kmalloc(PANIC_BYTES))) return;
    if (have && e.size && e.size <= PANIC_BYTES && FatRead(g_vol, &e, g_panic_buf)) {
        UINT32 len = 0;
        while (len < e.size && g_panic_buf[len]) len++;
        if (len) {
            if (take) UmCrashKernelFound((const char *)g_panic_buf, len);
            blank = false;
        }
    }
    UINT64 lba;
    if (!blank) {
        memset(g_panic_buf, 0, PANIC_BYTES);
        if (!FatWriteFile(g_vol, (UINT32)g_nova_dir, PANIC_FILE, g_panic_buf, PANIC_BYTES) ||
            !FatLookup(g_vol, (UINT32)g_nova_dir, PANIC_FILE, &e) || !FatSync(g_vol)) {
            kprintf("[PERSIST] Could not set aside \\NOVA\\%s for kernel crash reports\n", PANIC_FILE);
            return;
        }
    }
    if (!FatContiguous(g_vol, &e, &lba)) {
        kprintf("[PERSIST] \\NOVA\\%s is fragmented: kernel crash reports are not saved\n", PANIC_FILE);
        return;
    }
    g_panic_lba = lba;
    __atomic_store_n(&g_panic_dev, g_dev, __ATOMIC_RELEASE);
}

bool PersistPanicReady(void) { return __atomic_load_n(&g_panic_dev, __ATOMIC_ACQUIRE) != NULL; }

bool PersistPanicWrite(const char *text, UINT32 len)
{
    BlockDev *d = __atomic_load_n(&g_panic_dev, __ATOMIC_ACQUIRE);
    if (!d || d->gone || __atomic_exchange_n(&g_panic_busy, 1, __ATOMIC_ACQUIRE)) return false;
    if (len > PANIC_BYTES - 1) len = PANIC_BYTES - 1;
    memcpy(g_panic_buf, text, len);
    memset(g_panic_buf + len, 0, PANIC_BYTES - len);
    UINT32 sectors = (len + BLOCK_SECTOR) / BLOCK_SECTOR;     /* (the text and a zero after it) */
    return d->write(d, g_panic_lba, sectors, g_panic_buf) && (!d->flush || d->flush(d));
}

static void drop_vol(void)
{
    g_panic_dev = NULL;
    if (g_vol) FatUnmount(g_vol);
    if (g_ntfs) NtfsUnmount(g_ntfs);
    g_vol = NULL;
    g_ntfs = NULL;
    g_ntfs_dev = NULL;
    g_dev = NULL;
    g_root_known = false;
    kfree(g_links_text);
    g_links_text = NULL;
}

void PersistDetach(void)
{
    save_lock();                                                  /* (after a save in progress) */
    if (have_vol()) kprintf("[PERSIST] Stopped saving to %s\n", g_dev->name);
    drop_vol();
    save_unlock();
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

static bool save_locked(void);

bool PersistAdopt(BlockDev *d, UINT64 lba)
{
    Cand c;
    if (!mount_at(d, lba, &c)) return false;
    FsLock();
    save_lock();
    drop_vol();
    g_lba = lba;
    if (c.vol) g_vol = c.vol;
    else if (!use_ntfs(c.ntfs, d)) { NtfsUnmount(c.ntfs); save_unlock(); FsUnlock(); return false; }
    g_dev = d;
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
    bool ok = save_locked();                                      /* (lets go of both locks) */
    if (ok && g_vol) {                                            /* a kernel crash slot on the new volume */
        save_lock();
        if (g_vol) panic_slot(false);
        save_unlock();
    }
    char desc[96];
    PersistDescribe(desc, sizeof(desc));
    kprintf("[PERSIST] Drive C: is now saved to %s%s\n", desc, ok ? "" : " (the first save failed)");
    return ok;
}

/* What Settings and the Terminal show about the volume, as of the last
 * look that did not have to wait for a save (they never wait for one) */
static struct { bool have; UINT64 free, total; char where[64], desc[96]; } g_shown;

static void refresh_shown(void)
{
    if (!save_trylock()) return;
    g_shown.have = have_vol();
    if (g_shown.have) {
        if (g_vol) { g_shown.free = FatFreeBytes(g_vol); g_shown.total = FatTotalBytes(g_vol); }
        else { g_shown.free = NtfsFreeBytes(g_ntfs); g_shown.total = NtfsTotalBytes(g_ntfs); }
        const char *label = g_vol ? FatLabel(g_vol) : NtfsLabel(g_ntfs);
        char fs[8];
        if (g_vol) ksnprintf(fs, sizeof(fs), "FAT%d", FatType(g_vol));
        else ksnprintf(fs, sizeof(fs), "NTFS");
        ksnprintf(g_shown.where, sizeof(g_shown.where), "%s (%s)", g_dev->name, label);
        ksnprintf(g_shown.desc, sizeof(g_shown.desc), "%s %s \"%s\", %u MiB free of %u MiB", g_dev->name, fs, label,
                  (unsigned)(g_shown.free >> 20), (unsigned)(g_shown.total >> 20));
    }
    save_unlock();
}

bool PersistSpace(UINT64 *free, UINT64 *total)
{
    refresh_shown();
    if (!g_shown.have) return false;
    *free = g_shown.free;
    *total = g_shown.total;
    return true;
}

void PersistWhere(char *buf, int cap)
{
    refresh_shown();
    ksnprintf(buf, (size_t)cap, "%s", g_shown.have ? g_shown.where : "memory only");
}

void PersistDescribe(char *buf, int cap)
{
    refresh_shown();
    ksnprintf(buf, (size_t)cap, "%s", g_shown.have ? g_shown.desc : "memory only (no disk found)");
}

static UINT32 filetime_to_dos(UINT64 ft);

/* What a save writes: a copy of the changed part of C:, taken under the
 * file-system lock (see "Saving") */
typedef struct SNode {
    struct SNode *parent, *child, *next;
    char    name[RAMFS_NAME_MAX];
    bool    dir;
    UINT8   flags;                /* its RAMFS_F_DIRTY and RAMFS_F_DIRTYDIR, as they were */
    UINT32  attrs;
    UINT64  ctime, mtime;
    UINT8  *sd;                   /* NTFS: its own security descriptor, or NULL (inherits) */
    UINT32  sdlen;
    const char *data;             /* a changed file's contents (lent: RamfsLend) */
    UINT32  size;
    char   *keep;                 /* a directory whose entries changed: those to keep on the disk, */
    UINT32  keeplen;              /*   'D' (directory) or 'F' (file), the name and a NUL each */
    const void *link_id;          /* NTFS: a file with several names: which file (compared only) */
    char   *links;                /*   and its other names, paths from the root, a NUL after each */
    UINT32  linkslen;
} SNode;

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
static bool ntfs_info(UINT64 ref, const SNode *n)
{
    UINT32 id = g_root_secid;
    if (n->sd && n->parent) id = NtfsAddSecurity(g_ntfs, n->sd, n->sdlen);
    bool ok = !n->parent || NtfsSetInfo(g_ntfs, ref, n->ctime, n->mtime, n->attrs & 0x07);
    if (id) ok = NtfsSetSecurityId(g_ntfs, ref, id) && ok;
    else if (n->sd) ok = false;
    return ok;
}

static bool vol_mkdir(UINT64 dir, const SNode *n, UINT64 *sub)
{
    if (g_vol) {
        UINT32 c;
        if (!FatMkdir(g_vol, (UINT32)dir, n->name, &c)) return false;
        *sub = c;
        return true;
    }
    return NtfsCreate(g_ntfs, dir, n->name, true, sub);
}

static bool vol_write(UINT64 dir, const SNode *n)
{
    if (g_vol) {
        FatSetStamp(g_vol, filetime_to_dos(n->mtime));
        bool ok = FatWriteFile(g_vol, (UINT32)dir, n->name, n->data, n->size);
        FatSetStamp(g_vol, 0);
        return ok;
    }
    UINT64 ref;
    bool is_dir;
    if (!NtfsLookup(g_ntfs, dir, n->name, &ref, &is_dir)) {
        if (!NtfsCreate(g_ntfs, dir, n->name, false, &ref)) return false;
    } else if (is_dir) return false;
    return NtfsWriteFile(g_ntfs, ref, n->data, n->size) && ntfs_info(ref, n);
}

/* NTFS: the record of the file at @path (from the root, "\\a\\b.txt") on the disk */
static bool ntfs_ref_of(const char *path, UINT64 *ref)
{
    UINT64 at = NTFS_ROOT;
    bool is_dir = true;
    char part[RAMFS_NAME_MAX];
    for (const char *p = path; *p;) {
        while (*p == '\\') p++;
        int n = 0;
        while (*p && *p != '\\' && n < RAMFS_NAME_MAX - 1) part[n++] = *p++;
        part[n] = '\0';
        if (!n) break;
        if (!is_dir || !NtfsLookup(g_ntfs, at, part, &at, &is_dir)) return false;
    }
    if (is_dir) return false;
    *ref = at;
    return true;
}

/* The hard-linked files written so far in this save: their records */
typedef struct { const void *id; UINT64 ref; } SavedLink;
static SavedLink *g_slinks;
static int g_nslinks, g_slinks_cap;

static bool slinks_find(const void *id, UINT64 *ref)
{
    for (int i = 0; i < g_nslinks; i++) if (g_slinks[i].id == id) { *ref = g_slinks[i].ref; return true; }
    return false;
}

static void slinks_add(const void *id, UINT64 ref)
{
    if (g_nslinks == g_slinks_cap) {
        int cap = g_slinks_cap ? 2 * g_slinks_cap : 16;
        SavedLink *n = kmalloc(sizeof(SavedLink) * (size_t)cap);
        if (!n) return;
        if (g_nslinks) memcpy(n, g_slinks, sizeof(SavedLink) * (size_t)g_nslinks);
        kfree(g_slinks);
        g_slinks = n;
        g_slinks_cap = cap;
    }
    g_slinks[g_nslinks].id = id;
    g_slinks[g_nslinks++].ref = ref;
}

/* NTFS: save @n, a name of a file with several, into @dir: its names
 * share one record, so the first name written makes it and the others
 * link to it */
static bool save_link(UINT64 dir, SNode *n)
{
    const void *id = n->link_id;
    UINT64 ref = 0, here = 0;
    bool written = slinks_find(id, &ref), known = written, is_dir = false;
    for (UINT32 at = 0; at < n->linkslen && !known; at += (UINT32)strlen(n->links + at) + 1)
        known = ntfs_ref_of(n->links + at, &ref);
    bool have = NtfsLookup(g_ntfs, dir, n->name, &here, &is_dir);
    if (have && is_dir) return false;
    if (!known) {                                                /* its first name on the disk: a file as usual */
        if (!vol_write(dir, n) || !NtfsLookup(g_ntfs, dir, n->name, &ref, &is_dir)) return false;
        slinks_add(id, ref);
        return true;
    }
    if (have && here != ref && !NtfsDelete(g_ntfs, dir, here, n->name)) return false;   /* another file had the name */
    if ((!have || here != ref) && !NtfsLink(g_ntfs, ref, dir, n->name)) return false;
    bool ok = written || (NtfsWriteFile(g_ntfs, ref, n->data, n->size) && ntfs_info(ref, n));
    if (!written) slinks_add(id, ref);
    return ok;
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
    return NtfsDelete(g_ntfs, dir, e->ref, e->name);
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

/* NTFS: the hard-linked files loaded so far, by record (the first name makes the node) */
typedef struct { UINT64 ref; RamNode *n; } LoadedLink;
static LoadedLink *g_llinks;
static int g_nllinks, g_llinks_cap;

static RamNode *llinks_find(UINT64 ref)
{
    for (int i = 0; i < g_nllinks; i++) if (g_llinks[i].ref == ref) return g_llinks[i].n;
    return NULL;
}

static void llinks_add(UINT64 ref, RamNode *n)
{
    if (g_nllinks == g_llinks_cap) {
        int cap = g_llinks_cap ? 2 * g_llinks_cap : 16;
        LoadedLink *nl = kmalloc(sizeof(LoadedLink) * (size_t)cap);
        if (!nl) return;
        if (g_nllinks) memcpy(nl, g_llinks, sizeof(LoadedLink) * (size_t)g_nllinks);
        kfree(g_llinks);
        g_llinks = nl;
        g_llinks_cap = cap;
    }
    g_llinks[g_nllinks].ref = ref;
    g_llinks[g_nllinks++].n = n;
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
        bool linked = g_ntfs && NtfsLinks(g_ntfs, e->ref) > 1;    /* a hard link: one of its names may be in */
        if (linked && llinks_find(e->ref)) {
            if (!have) RamfsLink(llinks_find(e->ref), rdir, e->name);
            continue;
        }
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
            if (linked) llinks_add(e->ref, f);
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
            load_links();
        }
        kprintf("[PERSIST] Restored %d file(s) to drive C:\n", g_restored);
        if (g_nova_dir) panic_slot(true);
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
        kfree(g_llinks);
        g_llinks = NULL;
        g_nllinks = g_llinks_cap = 0;
        if (g_root_known) load_removed();
        kprintf("[PERSIST] Restored %d file(s) to drive C: (NTFS)\n", g_restored);
    }
    g_loaded = true;
    RamfsSetMode(RAMFS_TRACK);
    g_saved_changes = g_seen_changes = RamfsChanges();
}

/* FAT: \NOVA\LINKS.TXT, a line per hard-linked file with its names ("C:\a\x.txt|C:\b\y.txt") */
static char *g_links_text;                    /* what the file holds, as last written or read (this volume's) */

static void links_collect(const RamNode *dir, char **text, UINT32 *len, UINT32 *cap)
{
    for (const RamNode *c = dir->child; c; c = c->next) {
        if (c->dir) { links_collect(c, text, len, cap); continue; }
        if (!c->link || RamfsFileId(c) != c) continue;           /* (once per file: by its first node) */
        for (const RamNode *m = c;; ) {
            char path[RAMFS_PATH_MAX];
            RamfsPath(m, path, sizeof(path));
            UINT32 need = *len + (UINT32)strlen(path) + 3;
            if (need > *cap) {
                UINT32 ncap = *cap ? 2 * *cap : 1024;
                while (ncap < need) ncap *= 2;
                char *nt = kmalloc(ncap);
                if (!nt) return;
                if (*len) memcpy(nt, *text, *len);
                kfree(*text);
                *text = nt;
                *cap = ncap;
            }
            memcpy(*text + *len, path, strlen(path));
            *len += (UINT32)strlen(path);
            m = m->link ? m->link : m;
            if (m == c) break;
            (*text)[(*len)++] = '|';
        }
        (*text)[(*len)++] = '\r';
        (*text)[(*len)++] = '\n';
    }
}

/* FAT: join the names LINKS.TXT lists (each loaded as a copy) into one file again */
static void load_links(void)
{
    UINT64 ref;
    bool is_dir;
    if (!vol_lookup(g_nova_dir, LINKS_FILE, &ref, &is_dir) || is_dir) return;
    EntList l = { 0 };
    vol_list(g_nova_dir, &l);
    const PEnt *e = NULL;
    for (int i = 0; i < l.n && !e; i++) if (path_eq(l.e[i].name, LINKS_FILE)) e = &l.e[i];
    char *text = e && e->size <= 1024 * 1024 ? kmalloc(e->size + 1) : NULL;
    if (text && vol_read(e, text)) {
        text[e->size] = '\0';
        for (char *line = text, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = '\0';
            size_t n = strlen(line);
            if (n && line[n - 1] == '\r') line[--n] = '\0';
            RamNode *f = NULL;
            for (char *name = line, *bar; name && *name; name = bar) {
                bar = strchr(name, '|');
                if (bar) *bar++ = '\0';
                RamNode *o = RamfsResolve(NULL, name);
                if (o && o->dir) continue;
                if (!f) { f = o; continue; }
                if (o == f || (o && o->link && RamfsFileId(o) == RamfsFileId(f))) continue;
                char *slash = NULL;
                for (char *c = name; *c; c++) if (*c == '\\' || *c == '/') slash = c;
                if (!slash) continue;
                *slash = '\0';
                RamNode *dir = slash == name + 2 ? RamfsRoot() : RamfsResolve(NULL, name);
                if (dir && dir->dir && (!o || RamfsDelete(o))) RamfsLink(f, dir, slash + 1);
            }
        }
        kfree(g_links_text);
        g_links_text = text;
        text = NULL;
    }
    kfree(text);
    kfree(l.e);
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

/* ---------------------------------------------------------------------------
 * Saving
 *
 * A save holds no lock while the disk works.  It runs in two steps:
 *   1. the snapshot (take_snapshot), under the file-system lock and the
 *      save lock: the changed part of C: is copied into a tree of SNodes
 *      (a changed file's details and its contents, which are lent, not
 *      copied: RamfsLend; the entries of a directory whose entries
 *      changed) and its change marks are cleared;
 *   2. the write (write_snapshot), holding only the save lock, which
 *      nothing else waits for (Settings and the Terminal show what they saw
 *      last), and without the big kernel lock: the disk drivers serialize
 *      their transfers themselves.
 * What changes during the write is marked again and goes in the next save.
 * What could not be written is listed (g_unsaved) and marked again at the
 * next snapshot, so the next save tries it again.
 *
 * PersistPoll asks the "persist" thread to save once C: has been quiet for
 * a second; PersistSync saves on the calling thread (before a restart).
 * ------------------------------------------------------------------------- */
static bool g_error;
static const UINT8 CLEAR = RAMFS_F_DIRTY | RAMFS_F_DIRTYDIR | RAMFS_F_SUB;

/* What a save could not write: marked again at the next snapshot (@tree:
 * a directory that is not on the disk, with everything in it) */
typedef struct Unsaved { struct Unsaved *next; UINT8 flags; bool tree; char path[RAMFS_PATH_MAX]; } Unsaved;
static Unsaved *g_unsaved;                    /* (under the save lock) */

static void unsaved_add(const char *path, UINT8 flags, bool tree)
{
    Unsaved *u = kzalloc(sizeof(Unsaved));
    if (!u) return;
    u->flags = flags;
    u->tree = tree;
    strncpy(u->path, path, sizeof(u->path) - 1);
    u->next = g_unsaved;
    g_unsaved = u;
}

/* @n's path from the root of C:, "\a\b.txt" ("\" for the root) */
static void snode_path(const SNode *n, char *buf, int cap)
{
    const SNode *chain[32];
    int depth = 0, len = 0;
    for (const SNode *m = n; m && m->parent && depth < 32; m = m->parent) chain[depth++] = m;
    ksnprintf(buf, (size_t)cap, "\\");
    for (int i = depth - 1; i >= 0 && len < cap - 1; i--)
        len += ksnprintf(buf + len, (size_t)(cap - len), "\\%s", chain[i]->name);
}

/* @n could not be saved (@what is why); @again: its marks to set again */
static void save_error(const char *what, const SNode *n, UINT8 again, bool tree)
{
    char path[RAMFS_PATH_MAX];
    snode_path(n, path, sizeof(path));
    if (!g_error) kprintf("[PERSIST] Could not save C:%s (%s)\n", path, what);
    g_error = true;
    if (again) unsaved_add(path, again, tree);
}

/* ---- the snapshot (under the file-system lock and the save lock) ---- */
typedef struct {
    SNode  *root;                 /* the changed part of C: (NULL: nothing) */
    bool    partial;              /* something was left marked (out of memory): save again */
    UINT8  *root_sd;              /* NTFS: the root's own descriptor, changed */
    UINT32  root_sdlen;
    char   *removed;              /* the deleted starter files' list, changed */
    UINT32  removed_len;
    char   *links;                /* FAT: what LINKS.TXT should hold */
    UINT32  changes, files;
    UINT64  bytes;
} Snapshot;

static void *dup_bytes(const void *p, UINT32 n)
{
    void *d = kmalloc(n ? n : 1);
    if (d && n) memcpy(d, p, n);
    return d;
}

/* Append @tag (if not 0), @s and a NUL to a growing buffer */
static bool buf_add(char **buf, UINT32 *len, UINT32 *cap, char tag, const char *s)
{
    UINT32 need = *len + (UINT32)strlen(s) + 2;
    if (need > *cap) {
        UINT32 ncap = *cap ? 2 * *cap : 512;
        while (ncap < need) ncap *= 2;
        char *nb = kmalloc(ncap);
        if (!nb) return false;
        if (*len) memcpy(nb, *buf, *len);
        kfree(*buf);
        *buf = nb;
        *cap = ncap;
    }
    if (tag) (*buf)[(*len)++] = tag;
    strcpy(*buf + *len, s);
    *len += (UINT32)strlen(s) + 1;
    return true;
}

static void snode_free(SNode *n)
{
    while (n) {
        SNode *next = n->next;
        snode_free(n->child);
        kfree(n->sd);
        kfree(n->keep);
        kfree(n->links);
        kfree(n);
        n = next;
    }
}

/* Copy the changed part of @r (marked RAMFS_F_DIRTY, _DIRTYDIR or _SUB)
 * and clear its marks; NULL, with the marks left, when out of memory */
static SNode *snap(RamNode *r, SNode *parent, Snapshot *s, int depth)
{
    if (depth > 24) { r->pflags &= (UINT8)~CLEAR; return NULL; }   /* (too deep to save) */
    SNode *n = kzalloc(sizeof(SNode));
    if (!n) return NULL;
    strcpy(n->name, r->name);
    n->parent = parent;
    n->dir = r->dir;
    n->flags = r->pflags & (RAMFS_F_DIRTY | RAMFS_F_DIRTYDIR);
    n->attrs = r->attrs;
    n->ctime = r->ctime;
    n->mtime = r->mtime;
    bool ok = true;
    if (g_ntfs && r->sd && (r->dir || (n->flags & RAMFS_F_DIRTY))) {
        n->sd = dup_bytes(r->sd, r->sdlen);
        n->sdlen = r->sdlen;
        ok = n->sd != NULL;
    }
    if (!r->dir) {
        if (ok) {                                                /* (only changed files get here) */
            n->data = RamfsLend(r);                              /* (no copy: see ramfs.h) */
            n->size = r->size;
            ok = n->data || !r->size;
            s->files++;
            s->bytes += r->size;
        }
        if (ok && g_ntfs && r->link) {                           /* its other names, for save_link */
            UINT32 cap = 0;
            n->link_id = RamfsFileId(r);
            for (RamNode *m = RamfsNextLink(r); m != r && ok; m = RamfsNextLink(m)) {
                char path[RAMFS_PATH_MAX];
                RamfsPath(m, path, sizeof(path));                /* ("C:\a\b.txt") */
                ok = buf_add(&n->links, &n->linkslen, &cap, 0, path + 2);
            }
        }
    } else {
        UINT32 cap = 0;
        if (n->flags & RAMFS_F_DIRTYDIR)                         /* the entries the disk may keep */
            for (RamNode *c = r->child; c && ok; c = c->next)
                if (c->dir || !(c->pflags & RAMFS_F_SEALED)) ok = buf_add(&n->keep, &n->keeplen, &cap, c->dir ? 'D' : 'F', c->name);
        SNode **tail = &n->child;
        for (RamNode *c = r->child; c && ok; c = c->next) {
            if (!(c->pflags & CLEAR)) continue;
            if (!c->dir && (!(c->pflags & RAMFS_F_DIRTY) || (c->pflags & RAMFS_F_SEALED))) {   /* nothing to write */
                c->pflags &= (UINT8)~CLEAR;
                continue;
            }
            SNode *sc = snap(c, n, s, depth + 1);
            if (sc) { *tail = sc; tail = &sc->next; }
        }
    }
    if (!ok) { snode_free(n); return NULL; }
    r->pflags &= (UINT8)~CLEAR;
    for (RamNode *c = r->child; c; c = c->next)
        if (c->pflags & CLEAR) r->pflags |= RAMFS_F_SUB;         /* (left for the next save) */
    return n;
}

/* The caller holds the file-system lock and the save lock */
static void take_snapshot(Snapshot *s)
{
    memset(s, 0, sizeof(*s));
    while (g_unsaved) {                                          /* what the last save could not write */
        Unsaved *u = g_unsaved;
        g_unsaved = u->next;
        RamNode *n = RamfsResolve(NULL, u->path);
        if (n) {
            if (u->tree) mark_all(n);
            RamfsMarkUnsaved(n, u->flags);
        }
        kfree(u);
    }
    s->changes = RamfsChanges();
    RamNode *root = RamfsRoot();
    if (g_ntfs && (root->pflags & RAMFS_F_DIRTY) && root->sd) {  /* the root's own descriptor changed */
        s->root_sd = dup_bytes(root->sd, root->sdlen);
        s->root_sdlen = root->sdlen;
    }
    if (root->pflags & CLEAR) s->root = snap(root, NULL, s, 0);
    s->partial = (root->pflags & CLEAR) != 0;

    /* starter files the user has since recreated are no longer deleted */
    for (Removed **pp = &g_removed; *pp;) {
        if (RamfsResolve(NULL, (*pp)->path)) {
            Removed *r = *pp;
            *pp = r->next;
            kfree(r);
            g_removed_dirty = true;
        } else pp = &(*pp)->next;
    }
    if (g_removed_dirty) {
        UINT32 len = 0;
        for (Removed *r = g_removed; r; r = r->next) len += (UINT32)strlen(r->path) + 2;
        char *text = kmalloc(len + 1), *p = text;
        if (text) {
            for (Removed *r = g_removed; r; r = r->next) p += ksnprintf(p, (size_t)(len + 1 - (UINT32)(p - text)), "%s\r\n", r->path);
            s->removed = text;
            s->removed_len = (UINT32)(p - text);
            g_removed_dirty = false;
        }
    }

    if (g_vol) {
        char *text = NULL;
        UINT32 len = 0, cap = 0;
        links_collect(RamfsRoot(), &text, &len, &cap);
        if (len == cap) {                                        /* (room for the NUL) */
            char *nt = kmalloc(len + 1);
            if (nt && len) memcpy(nt, text, len);
            kfree(text);
            text = nt;
        }
        if (text) text[len] = '\0';
        s->links = text;
    }
}

static void snapshot_free(Snapshot *s)
{
    snode_free(s->root);
    kfree(s->root_sd);
    kfree(s->removed);
    kfree(s->links);
}

/* ---- the write (under the save lock only) ---- */

/* Directory @d (whose entries changed) keeps the disk entry @name */
static bool keeps(const SNode *d, const char *name, bool dir)
{
    for (UINT32 at = 0; at < d->keeplen; at += (UINT32)strlen(d->keep + at + 1) + 2)
        if ((d->keep[at] == 'D') == dir && path_eq(d->keep + at + 1, name)) return true;
    return false;
}

/* Brings the disk directory @vdir in line with the copy of directory @r. */
static void save_dir(SNode *r, UINT64 vdir, bool fresh, int depth)
{
    if ((r->flags & RAMFS_F_DIRTYDIR) && !fresh) {
        EntList l = { 0 };
        vol_list(vdir, &l);
        for (int i = 0; i < l.n; i++)
            if (!keeps(r, l.e[i].name, l.e[i].dir) && !vol_delete(vdir, &l.e[i], 0))
                save_error("delete failed", r, RAMFS_F_DIRTYDIR, false);
        kfree(l.e);
    }
    for (SNode *c = r->child; c; c = c->next) {
        if (c->dir) {
            UINT64 sub;
            bool is_dir = false;
            bool existed = !fresh && vol_lookup(vdir, c->name, &sub, &is_dir) && is_dir;
            if (!existed && !vol_mkdir(vdir, c, &sub)) { save_error("disk full?", c, RAMFS_F_DIRTY | RAMFS_F_DIRTYDIR, true); continue; }
            if (g_ntfs && (!existed || (c->flags & RAMFS_F_DIRTY)) && !ntfs_info(sub, c)) save_error("its details", c, 0, false);
            save_dir(c, sub, !existed, depth + 1);
        } else if (!(c->link_id ? save_link(vdir, c) : vol_write(vdir, c))) {
            save_error("disk full?", c, RAMFS_F_DIRTY, false);
        }
    }
}

static void write_snapshot(Snapshot *s)
{
    if (!g_root_known) {
        if (find_root()) g_root_known = true;
        else {
            kprintf("[PERSIST] Could not make the folder for drive C: on the disk\n");
            g_error = true;
            if (s->root) unsaved_add("\\", RAMFS_F_DIRTY | RAMFS_F_DIRTYDIR, true);
            if (s->removed) g_removed_dirty = true;
            return;
        }
    }
    if (s->root_sd) {
        UINT32 id = NtfsAddSecurity(g_ntfs, s->root_sd, s->root_sdlen);
        if (!id || !NtfsSetSecurityId(g_ntfs, NTFS_ROOT, id)) {
            if (!g_error) kprintf("[PERSIST] Could not save C:\\ (its descriptor)\n");
            g_error = true;
            unsaved_add("\\", RAMFS_F_DIRTY, false);
        }
    }
    g_nslinks = 0;
    if (s->root) save_dir(s->root, g_root_dir, false, 0);
    if (s->removed && !vol_write_meta(g_nova_dir, g_vol ? DELETED_FILE : NTFS_DELETED, s->removed, s->removed_len))
        g_removed_dirty = true;
    if (s->links && !(g_links_text && !strcmp(g_links_text, s->links)) &&
        vol_write_meta(g_nova_dir, LINKS_FILE, s->links, (UINT32)strlen(s->links))) {
        kfree(g_links_text);
        g_links_text = s->links;
        s->links = NULL;
    }
}


/* Save C:.  The caller holds the file-system lock (one level of it) and
 * the save lock; both are let go of. */
static bool save_locked(void)
{
    UINT64 t0 = rdtsc();
    Snapshot s;
    take_snapshot(&s);
    UINT64 held = rdtsc() - t0;
    FsUnlock();
    UINT32 bkl = bkl_drop();                     /* (the disk drivers take what they need) */
    g_error = false;
    write_snapshot(&s);
    bool ok = vol_sync() && !g_error;
    RamfsGiveBackAll();
    if (!s.partial) g_saved_changes = s.changes;
    if (s.root) {
        UINT64 us = tsc_us(held);
        kprintf("[PERSIST] %s %u file(s), %llu KiB in %llu ms; the file-system lock was held %llu.%02llu ms\n",
                ok ? "Saved" : "Failed to save all of", s.files, (unsigned long long)(s.bytes >> 10), (unsigned long long)(tsc_us(rdtsc() - t0) / 1000),
                (unsigned long long)(us / 1000), (unsigned long long)(us % 1000 / 10));
    }
    snapshot_free(&s);
    save_unlock();
    bkl_restore(bkl);
    return ok;
}

static bool save(void)
{
    if (!have_vol() || !g_loaded) return !have_vol();
    FsLock();
    save_lock();
    if (!have_vol()) { save_unlock(); FsUnlock(); return true; }   /* (stopped saving meanwhile) */
    return save_locked();
}

bool PersistSync(void) { return save(); }

/* The "persist" thread: saves when PersistPoll asks */
static Thread *g_saver;
static WaitQueue g_saver_q = WAITQ_INIT;
static volatile UINT32 g_save_asked;

static void saver_thread(void *arg)
{
    (void)arg;
    bkl_drop();                                  /* (kernel threads start with the big lock) */
    for (;;) {
        UINT32 gen = waitq_gen(&g_saver_q);
        if (__atomic_exchange_n(&g_save_asked, 0, __ATOMIC_ACQ_REL)) {
            bool ok = save();
            if (!ok) g_retry_tick = sched_ticks() + 3000;        /* try again in 30 s */
            g_failed = !ok;
            continue;
        }
        waitq_wait(&g_saver_q, gen, 360000);                     /* (an hour: PersistPoll wakes it) */
    }
}

void PersistPoll(void)
{
    if (!have_vol() || !g_loaded) return;
    UINT32 now = RamfsChanges();
    UINT64 t = sched_ticks();
    if (now != g_seen_changes) { g_seen_changes = now; g_seen_tick = t; }
    if (now == g_saved_changes && !g_removed_dirty && !g_unsaved) return;
    if (t - g_seen_tick < 100) return;                    /* wait for a quiet second */
    if (g_failed && t < g_retry_tick) return;
    if (g_save_asked || g_saving) return;                 /* (asked already, or saving) */
    if (!g_saver) g_saver = sched_create_thread("persist", saver_thread, NULL, PRIO_SERVICE);   /* (writeback: above programs, below the rest) */
    if (!g_saver) {                                       /* (no thread: save here) */
        g_failed = !save();
        if (g_failed) g_retry_tick = t + 3000;
        return;
    }
    __atomic_store_n(&g_save_asked, 1, __ATOMIC_RELEASE);
    waitq_wake(&g_saver_q);
}
