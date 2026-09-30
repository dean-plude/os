/*
 * persist.h — keeps drive C: on disk
 *
 * Drive C: is a RAM disk (ramfs).  This layer finds a FAT volume, saves
 * the files that change on C: under \NOVA\C on it, and restores them at
 * the next boot, on top of the starter files and the system files from
 * the OS image.  Files installed from the image (RAMFS_F_SEALED) are
 * never saved, so a new image always brings its own DLLs and programs.
 *
 * The volume used is the first that applies:
 *   1. a FAT volume labelled NOVADATA;
 *   2. an empty disk (its first sectors are all zero), formatted as FAT32 NOVADATA;
 *   3. the FAT volume NovaOS booted from (the one holding \EFI\NOVA\kernel.elf).
 */

#pragma once

#include "../include/types.h"

/* Probe the disks and choose the volume (after PCI enumeration). */
void PersistInit(void);
/* Restore the saved files onto C: (after the system files are installed),
 * then start recording changes. */
void PersistLoad(void);
/* Save changes once C: has been quiet for a second (desktop thread). */
void PersistPoll(void);
/* Save every change now (e.g. before a restart).  False on a disk error. */
bool PersistSync(void);

/* One line describing where C: is saved, e.g. "sata0 FAT32 NOVADATA, 12 MiB free" */
void PersistDescribe(char *buf, int cap);
bool PersistActive(void);
/* Short form for tight spaces, e.g. "sata0 (NOVADATA)" */
void PersistWhere(char *buf, int cap);
/* The volume's size and free space in bytes; false without one. */
bool PersistSpace(UINT64 *free, UINT64 *total);

/* For the installer (fs/setup.c) */
#include "fat.h"
/* The FAT volumes on @d (a whole-disk volume, or MBR/GPT partitions);
 * mounted, the caller unmounts them.  *blank: the disk's first sectors are
 * all zero. */
int  PersistFindVolumes(BlockDev *d, FatVol **out, int max, bool *blank);
/* The disk C: is saved to, or NULL */
BlockDev *PersistDevice(void);
/* Stop saving to the current volume (its disk is about to be erased) */
void PersistDetach(void);
/* Save C: to @vol from now on, starting with everything on it now (the
 * files of this session move with it).  False if that first save failed. */
bool PersistAdopt(FatVol *vol);
