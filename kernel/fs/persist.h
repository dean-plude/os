/*
 * persist.h — keeps drive C: on disk
 *
 * Drive C: is a RAM disk (ramfs).  This layer finds a FAT or NTFS volume,
 * saves the files that change on C: to it, and restores them at the next
 * boot, on top of the starter files and the system files from the OS
 * image.  Files installed from the image (RAMFS_F_SEALED) are never saved,
 * so a new image always brings its own DLLs and programs.  On FAT, C: is
 * kept under \NOVA\C; on NTFS, C: is the volume itself, with creation
 * times and security descriptors (file ACLs) kept too.
 *
 * The volume used is the first that applies:
 *   1. a FAT or (writable) NTFS volume labelled NOVADATA;
 *   2. an empty disk (its first sectors are all zero), formatted as FAT32 NOVADATA;
 *   3. the FAT volume NovaOS booted from (the one holding \EFI\NOVA\kernel.elf).
 */

#pragma once

#include "../include/types.h"
#include "block.h"

/* Probe the disks and choose the volume (after PCI enumeration). */
void PersistInit(void);
/* Restore the saved files onto C: (after the system files are installed),
 * then start recording changes. */
void PersistLoad(void);
/* Once C: has been quiet for a second, have the "persist" thread save the
 * changes (desktop thread; returns at once). */
void PersistPoll(void);
/* Save every change now, on this thread (e.g. before a restart).  False
 * on a disk error.  Holds the file-system lock only while it copies what
 * changed, not while the disk is written. */
bool PersistSync(void);

/* One line describing where C: is saved, e.g. "sata0 FAT32 NOVADATA, 12 MiB free" */
void PersistDescribe(char *buf, int cap);
bool PersistActive(void);
/* C: is saved to the volume at @lba on @d (other drives leave it alone) */
bool PersistOwns(BlockDev *d, UINT64 lba);
/* Short form for tight spaces, e.g. "sata0 (NOVADATA)" */
void PersistWhere(char *buf, int cap);
/* The volume's size and free space in bytes; false without one. */
bool PersistSpace(UINT64 *free, UINT64 *total);

/* A FAT (DOS) date and time as a FILETIME (100 ns since 1601) */
UINT64 PersistDosToFiletime(UINT32 dos);

/* For the installer (fs/setup.c) */
#include "fat.h"
/* The FAT volumes on @d (a whole-disk volume, or MBR/GPT partitions);
 * mounted, the caller unmounts them.  *blank: the disk's first sectors are
 * all zero. */
int  PersistFindVolumes(BlockDev *d, FatVol **out, int max, bool *blank);
/* The labels of the FAT and NTFS volumes on @d, as "NOVA_EFI, NOVADATA";
 * returns how many volumes there are */
int  PersistDiskLabels(BlockDev *d, char *out, int cap, bool *blank);
/* The disk C: is saved to, or NULL */
BlockDev *PersistDevice(void);
/* Stop saving to the current volume (its disk is about to be erased) */
void PersistDetach(void);
/* Save C: to the volume (FAT or NTFS) at @lba on @d from now on, starting
 * with everything on it now (the files of this session move with it).
 * False if it can't be mounted or that first save failed. */
bool PersistAdopt(BlockDev *d, UINT64 lba);
