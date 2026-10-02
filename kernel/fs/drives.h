/*
 * drives.h — the disks' other volumes as drives D:, E:, ... (read-only)
 */

#pragma once

#include "block.h"

/* Find the NTFS volumes on the fixed disks and mount each as the next
 * free drive letter from D:.  After PersistInit (which brings up the disks). */
void DrivesInit(void);
/* A disk arrived (a USB stick): mount its NTFS and FAT volumes */
void DrivesAttach(BlockDev *d);
/* A disk went away: its drive letters go with it */
void DrivesDetach(BlockDev *d);
