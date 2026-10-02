/*
 * ntfs.h — NTFS
 *
 * A volume is a region of a block device.  Files and directories are
 * named by their MFT record number; NTFS_ROOT is the root directory.
 * Names come out as UTF-8.  File contents are the unnamed $DATA stream:
 * resident, non-resident, sparse and LZNT1-compressed streams are read;
 * encrypted ones are not.  Callers serialize access to a volume.
 *
 * A volume is read-only until NtfsEnableWrite.  Then files can be
 * rewritten, created, renamed and deleted.  There is no journal: the
 * $LogFile is marked clean when writing is enabled, and every change is
 * written through before its call returns, so the volume is consistent
 * between calls.  Files kept in an attribute list, and compressed, sparse
 * or encrypted files, can't be rewritten.
 */

#pragma once

#include "../include/types.h"
#include "block.h"

#define NTFS_ROOT      5ull
#define NTFS_NAME_MAX  256

typedef struct NtfsVol NtfsVol;

typedef struct {
    char   name[NTFS_NAME_MAX];     /* long (Win32) name, UTF-8 */
    bool   dir;
    UINT64 mft;                     /* MFT record number */
    UINT64 size;                    /* bytes, as the directory index records it */
    UINT64 ctime, mtime;            /* 100 ns units since 1601 (UTC) */
    UINT32 attrs;                   /* FILE_ATTRIBUTE_* */
} NtfsEntry;

/* Find an NTFS volume at @lba on @dev.  NULL if there is none. */
NtfsVol    *NtfsMount(BlockDev *dev, UINT64 lba);
void        NtfsUnmount(NtfsVol *v);
const char *NtfsLabel(const NtfsVol *v);      /* the volume label, UTF-8 ("" if none) */
UINT64      NtfsTotalBytes(const NtfsVol *v);

/* Calls @fn for each entry of directory @dir (not "." or the system
 * metafiles); stops when it returns false.  False on a disk error. */
bool NtfsList(NtfsVol *v, UINT64 dir, bool (*fn)(const NtfsEntry *e, void *ctx), void *ctx);
/* The size of file @mft's data (its real size, which a directory index
 * can lag behind).  False if it has none or can't be read. */
bool NtfsSize(NtfsVol *v, UINT64 mft, UINT64 *size);
/* Read file @mft's data from @off: @len bytes into @buf. */
bool NtfsRead(NtfsVol *v, UINT64 mft, UINT64 off, void *buf, UINT64 len);

/* ---- writing ---- */

/* Where new timestamps come from (100 ns units since 1601, UTC). */
void   NtfsSetClock(UINT64 (*now)(void));
/* Make the volume writable: loads the allocation bitmaps and $UpCase and
 * marks $LogFile clean.  False (and still read-only) if it can't. */
bool   NtfsEnableWrite(NtfsVol *v);
bool   NtfsWritable(const NtfsVol *v);
UINT64 NtfsFreeBytes(NtfsVol *v);
/* Write the cached bitmaps back (each call already does). */
bool   NtfsSync(NtfsVol *v);
/* Whether NtfsWriteFile can rewrite file @mft (not compressed, sparse,
 * encrypted or kept in an attribute list) */
bool   NtfsCanWrite(NtfsVol *v, UINT64 mft);
/* Replace file @mft's data with @len bytes of @data. */
bool   NtfsWriteFile(NtfsVol *v, UINT64 mft, const void *data, UINT64 len);
/* Create file or directory @name (UTF-8) in @dir; its record in @out. */
bool   NtfsCreate(NtfsVol *v, UINT64 dir, const char *name, bool is_dir, UINT64 *out);
/* Remove @mft's name in @dir (directories only when empty). */
bool   NtfsDelete(NtfsVol *v, UINT64 dir, UINT64 mft);
/* Move @mft from @old_dir to @new_dir under @name. */
bool   NtfsRename(NtfsVol *v, UINT64 old_dir, UINT64 mft, UINT64 new_dir, const char *name);
