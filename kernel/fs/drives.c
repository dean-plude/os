/*
 * drives.c — the disks' NTFS volumes as drives D:, E:, ... (see drives.h)
 *
 * A disk holds a volume of its own (no partition table) or partitions in
 * an MBR or a GPT; each partition that is an NTFS volume is mounted,
 * read-only, into drive C:'s tree of nodes (ramfs.h).
 */

#include "drives.h"
#include "ntfs.h"
#include "ramfs.h"
#include "block.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"

static bool src_list(void *vol, UINT64 ref, bool (*add)(const RamfsExtEntry *e, void *ctx), void *ctx);
static bool src_size(void *vol, UINT64 ref, UINT64 *size) { return NtfsSize(vol, ref, size); }
static bool src_read(void *vol, UINT64 ref, UINT64 off, void *buf, UINT64 len) { return NtfsRead(vol, ref, off, buf, len); }

static const RamfsSource g_ntfs_source = { src_list, src_size, src_read };

typedef struct { bool (*add)(const RamfsExtEntry *e, void *ctx); void *ctx; } ListCtx;

static bool list_one(const NtfsEntry *e, void *ctx)
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

static bool src_list(void *vol, UINT64 ref, bool (*add)(const RamfsExtEntry *e, void *ctx), void *ctx)
{
    ListCtx l = { add, ctx };
    return NtfsList(vol, ref, list_one, &l);
}

static char g_next = 'D';

static void try_mount(BlockDev *d, UINT64 lba)
{
    if (g_next > 'Z') return;
    NtfsVol *v = NtfsMount(d, lba);
    if (!v) return;
    if (!RamfsMountDrive(g_next, &g_ntfs_source, v, NTFS_ROOT, NtfsLabel(v), NtfsTotalBytes(v))) { NtfsUnmount(v); return; }
    kprintf("[DRIVES] %c: is NTFS volume \"%s\" on %s (read-only)\n", g_next, NtfsLabel(v), d->name);
    g_next++;
}

static void scan(BlockDev *d)
{
    UINT8 *s = kmalloc(2 * BLOCK_SECTOR);
    if (!s || !d->read(d, 0, 2, s)) { kfree(s); return; }
    if (!memcmp(s + 3, "NTFS    ", 8)) {                       /* the whole disk is one volume */
        kfree(s);
        try_mount(d, 0);
        return;
    }
    if (s[510] != 0x55 || s[511] != 0xAA) { kfree(s); return; }
    bool gpt = false;
    for (int i = 0; i < 4; i++) {
        const UINT8 *e = s + 446 + 16 * i;
        UINT32 lba;
        memcpy(&lba, e + 8, 4);
        if (e[4] == 0xEE) gpt = true;
        else if (e[4] == 0x07 && lba) try_mount(d, lba);         /* IFS: NTFS (or exFAT, which NtfsMount refuses) */
    }
    if (gpt && !memcmp(s + BLOCK_SECTOR, "EFI PART", 8)) {
        UINT64 table;
        UINT32 count, esize;
        memcpy(&table, s + BLOCK_SECTOR + 72, 8);
        memcpy(&count, s + BLOCK_SECTOR + 80, 4);
        memcpy(&esize, s + BLOCK_SECTOR + 84, 4);
        UINT8 *ent = kmalloc(BLOCK_SECTOR);
        if (ent && esize >= 128 && esize <= BLOCK_SECTOR && count <= 128) {
            UINT32 per = BLOCK_SECTOR / esize;
            for (UINT32 i = 0; i < count; i++) {
                if (i % per == 0 && !d->read(d, table + i / per, 1, ent)) break;
                const UINT8 *e = ent + (i % per) * esize;
                static const UINT8 zero[16];
                if (!memcmp(e, zero, 16)) continue;
                UINT64 first;
                memcpy(&first, e + 32, 8);
                try_mount(d, first);                             /* NtfsMount checks the boot sector */
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
