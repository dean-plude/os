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
 * rewritten, created, renamed, linked and deleted.  There is no journal: the
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
/* Remove @mft's name @name in @dir (NULL: every name it has there;
 * directories only when empty).  The file goes when its last name does. */
bool   NtfsDelete(NtfsVol *v, UINT64 dir, UINT64 mft, const char *name);
/* Move @mft's name @old_name in @old_dir (NULL: every name it has there)
 * to @new_dir under @name. */
bool   NtfsRename(NtfsVol *v, UINT64 old_dir, UINT64 mft, const char *old_name, UINT64 new_dir, const char *name);
/* Give file @mft another name, @name in @dir (a hard link). */
bool   NtfsLink(NtfsVol *v, UINT64 mft, UINT64 dir, const char *name);
/* How many names @mft has (DOS aliases aside); 0 if it can't be read. */
UINT32 NtfsLinks(NtfsVol *v, UINT64 mft);

/* Find @name (UTF-8, any case) in directory @dir */
bool   NtfsLookup(NtfsVol *v, UINT64 dir, const char *name, UINT64 *mft, bool *is_dir);
/* Set @mft's creation and write times (0: leave as is) and its read-only,
 * hidden, system and archive attributes */
bool   NtfsSetInfo(NtfsVol *v, UINT64 mft, UINT64 ctime, UINT64 mtime, UINT32 attrs);

/* Security descriptors (self-relative) are kept once each in $Secure and
 * named by an id: the one file @mft has (0 if none), the descriptor with
 * id @id (*len: its size, false if larger than @cap), the id of @sd
 * (stored if new; 0 on failure), and giving @mft descriptor @id. */
UINT32 NtfsSecurityId(NtfsVol *v, UINT64 mft);
bool   NtfsSecurityById(NtfsVol *v, UINT32 id, void *buf, UINT32 cap, UINT32 *len);
UINT32 NtfsAddSecurity(NtfsVol *v, const void *sd, UINT32 len);
bool   NtfsSetSecurityId(NtfsVol *v, UINT64 mft, UINT32 id);

/* Make a new, empty NTFS volume of @sectors at @lba on @dev, labelled
 * @label (UTF-8), with serial number @serial. */
bool   NtfsFormat(BlockDev *dev, UINT64 lba, UINT64 sectors, const char *label, UINT64 serial);
