/*
 * fat.h — FAT16/FAT32 filesystem with long file names (read and write)
 *
 * A volume is a region of a block device.  Directories are named by
 * their first cluster; FAT_ROOT stands for the root directory on both
 * FAT16 (fixed root region) and FAT32 (a cluster chain).  Names are
 * UTF-8 and matched case-insensitively (ASCII).  Callers serialize
 * access to a volume.
 */

#pragma once

#include "../include/types.h"
#include "block.h"

#define FAT_ROOT      0xFFFFFFFFu
#define FAT_NAME_MAX  256

typedef struct FatVol FatVol;

typedef struct {
    char   name[FAT_NAME_MAX];      /* long name (or the 8.3 name) */
    bool   dir;
    UINT32 cluster;                 /* first cluster (the directory to pass for dir entries) */
    UINT32 size;
    UINT32 dir_cluster;             /* where the entry lives */
    UINT32 index;                   /* entry index of its 8.3 entry */
    UINT32 first_index;             /* first entry (long-name entries included) */
    UINT32 wtime;                   /* last written: DOS date << 16 | DOS time */
    UINT8  attr;                    /* FAT attribute byte */
} FatEntry;

/* Find a FAT volume at @lba on @dev.  NULL if there is none. */
FatVol *FatMount(BlockDev *dev, UINT64 lba);
/* Create a FAT32 volume of @sectors sectors at @lba; then mount it. */
FatVol *FatFormat(BlockDev *dev, UINT64 lba, UINT64 sectors, const char *label);
void    FatUnmount(FatVol *v);

const char *FatLabel(const FatVol *v);        /* "NOVADATA", trimmed */
int         FatType(const FatVol *v);         /* 16 or 32 */
BlockDev   *FatDevice(const FatVol *v);
UINT64      FatFreeBytes(FatVol *v);
UINT64      FatTotalBytes(const FatVol *v);

/* Calls @fn for each entry of @dir (not "." or ".."); stops when it returns false. */
bool FatList(FatVol *v, UINT32 dir, bool (*fn)(const FatEntry *e, void *ctx), void *ctx);
bool FatLookup(FatVol *v, UINT32 dir, const char *name, FatEntry *out);
/* Resolve a '\'-separated path from the root; "" is the root itself. */
bool FatLookupPath(FatVol *v, const char *path, FatEntry *out);

/* Read a whole file into @buf (at least e->size bytes). */
bool FatRead(FatVol *v, const FatEntry *e, void *buf);
/* The time the next entries written on @v get (DOS date << 16 | time; 0: now) */
void FatSetStamp(FatVol *v, UINT32 dos_time);
/* Create or replace the file @name in @dir with @len bytes of @data. */
bool FatWriteFile(FatVol *v, UINT32 dir, const char *name, const void *data, UINT32 len);
/* Create the directory @name in @dir (or find it); its cluster in *out. */
bool FatMkdir(FatVol *v, UINT32 dir, const char *name, UINT32 *out);
/* Create each directory along @path from the root; the last one's cluster in *out. */
bool FatMkdirPath(FatVol *v, const char *path, UINT32 *out);
/* Delete @name from @dir; directories are deleted with their contents. */
bool FatDelete(FatVol *v, UINT32 dir, const char *name);

/* What FatReclaim did */
typedef struct {
    bool   scanned;                 /* false: the volume was closed cleanly, nothing to look for */
    UINT32 reclaimed;               /* clusters marked as used that nothing reached, now free */
    UINT32 crossed;                 /* chains that ran into clusters already reached (left alone) */
    UINT32 free;                    /* free clusters afterwards */
} FatReclaimInfo;
/* If @v was not closed cleanly (its clean-shutdown bit was clear at mount,
 * as a crash in the middle of a save leaves it) or @force: free every
 * cluster marked as used that no file or directory reaches, then mark the
 * volume clean.  Shared or looping chains stay as they are.  False if it
 * could not finish (a read error, out of memory); nothing is freed then. */
bool FatReclaim(FatVol *v, bool force, FatReclaimInfo *info);
UINT32 FatClusterBytes(const FatVol *v);
/* Whether file @e's clusters follow one another on the disk; if so its
 * first sector (on the device, not the volume) in *lba */
bool FatContiguous(FatVol *v, const FatEntry *e, UINT64 *lba);

/* Write cached metadata to the disk and flush the disk's write cache. */
bool FatSync(FatVol *v);
