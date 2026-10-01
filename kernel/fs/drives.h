/*
 * drives.h — the disks' other volumes as drives D:, E:, ... (read-only)
 */

#pragma once

/* Find the NTFS volumes on the SATA disks and mount each as the next free
 * drive letter from D:.  After PersistInit (which brings up the disks). */
void DrivesInit(void);
