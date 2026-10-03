/*
 * setup.h — installing NovaOS on a disk
 *
 * The installation files are the bootloader (\EFI\BOOT\BOOTX64.EFI) and
 * the kernel (\EFI\NOVA\kernel.elf, which carries the whole userland).
 * Booted from the installation disc, the bootloader hands them over in
 * memory; on an installed system they are read from the disk NovaOS
 * booted from.  Installing erases the chosen disk and gives it a GPT with
 * two partitions:
 *
 *   1. EFI System Partition, FAT32 "NOVA_EFI", 128 MiB: the boot files
 *   2. Basic data, NTFS (or FAT32) "NOVADATA", the rest: drive C: is saved
 *      here (on NTFS with its files' security descriptors)
 *
 * The firmware finds \EFI\BOOT\BOOTX64.EFI on the ESP by itself, so no
 * boot entry needs to be written.
 */

#pragma once

#include "../include/types.h"
#include "../include/boot_protocol.h"
#include "block.h"

/* Record the installation media the bootloader passed (early boot) */
void SetupBootInfo(const BootInfo *info);
/* NovaOS is running from its installation disc */
bool SetupIsLive(void);

#define SETUP_MIN_BYTES  (256ull << 20)   /* smallest disk to install on */
#define SETUP_ESP_SECTORS 262144ull       /* 128 MiB */

typedef struct {
    BlockDev *dev;
    UINT64    bytes;
    bool      too_small;
    bool      boot;            /* NovaOS booted from it */
    bool      holds_c;         /* drive C: is saved on it */
    char      contents[64];    /* "Empty", "NovaOS (NOVA_EFI, NOVADATA)", ... */
} SetupDisk;

/* The disks NovaOS could be installed on; returns how many (<= max) */
int SetupListDisks(SetupDisk *out, int max);

/* Start installing on @dev in the background, with drive C: on NTFS (or
 * FAT32); false if one is running */
bool SetupStart(BlockDev *dev, bool ntfs);

typedef enum { SETUP_IDLE, SETUP_RUNNING, SETUP_DONE, SETUP_FAILED } SetupState;
typedef struct {
    SetupState state;
    int        percent;        /* 0-100 */
    char       step[96];       /* what it is doing now */
    char       error[128];     /* when SETUP_FAILED */
    bool       moved_c;        /* this session's files were copied to the new disk */
} SetupStatus;

void SetupGetStatus(SetupStatus *out);
