/*
 * update.h — updating an installed NovaOS to a newer build
 *
 * An installed NovaOS is two files on its EFI System Partition: the boot
 * loader (\EFI\BOOT\BOOTX64.EFI) and the kernel (\EFI\NOVA\kernel.elf),
 * which carries every system DLL and program in it (they are unpacked
 * onto drive C: at each start and never saved to disk).  So an update
 * replaces those two files, and only while the old ones are not in use:
 * a pending rename, carried out by the boot loader and the new kernel
 * rather than by the session that downloaded it.
 *
 * 1. Check: the update channel (a URL, HKLM\SOFTWARE\NovaOS\Update
 *    "Channel"; the GitHub release's novaos-update.txt by default) is a
 *    small text file naming the newest version and its files:
 *
 *        NovaOS update 1
 *        version 0.1.1
 *        kernel kernel.elf 41234567 <SHA-256 in hex>
 *        loader bootx64.efi 77455 <SHA-256 in hex>
 *        notes One line about the release
 *
 *    (file names are relative to the channel's URL, or full URLs;
 *    tools/mkupdate.py writes it).
 * 2. Install: the files are downloaded, their sizes and SHA-256 checked,
 *    and the kernel's stamped version must be the one the channel names.
 *    They are written to the ESP as \EFI\NOVA\kernel.new and bootx64.new,
 *    read back, and then \EFI\NOVA\update.pnd ("pending") marks the
 *    update as ready.  Nothing the running system uses has changed yet.
 * 3. Restart: the boot loader sees the mark, turns it into update.try
 *    ("trying") and starts kernel.new once.
 * 4. The new kernel reaches the desktop and finishes the update: kernel.elf
 *    becomes kernel.old, kernel.new becomes kernel.elf, the new loader
 *    replaces BOOTX64.EFI, and the mark is deleted.
 *
 * If the new kernel never gets that far (it stops, or the PC is reset),
 * the boot loader finds the "trying" mark at the next start, starts the
 * old kernel.elf again and tells it so, and the old NovaOS throws the
 * update away.  Live from the installation disc or stick there is
 * nothing to update.
 */

#pragma once

#include "../include/types.h"
#include "../include/boot_protocol.h"
#include "../ke/version.h"

#define UPDATE_KEY             "Machine\\SOFTWARE\\NovaOS\\Update"
#define UPDATE_DEFAULT_CHANNEL "https://github.com/dean-plude/os/releases/latest/download/novaos-update.txt"

typedef enum {
    UPDATE_IDLE,            /* not checked yet */
    UPDATE_CHECKING,
    UPDATE_CURRENT,         /* this is the newest version */
    UPDATE_AVAILABLE,       /* a newer one: version, size, notes */
    UPDATE_DOWNLOADING,     /* and checking and writing it (step, got) */
    UPDATE_READY,           /* staged: restart to finish */
    UPDATE_FAILED,          /* error */
} UpdateState;

typedef struct {
    UpdateState state;
    bool   busy;                          /* a check or an install is running */
    char   version[NOVA_STAMP_VER_MAX];   /* the channel's version */
    char   notes[160];
    UINT64 size;                          /* bytes to download */
    UINT64 got;                           /* downloaded so far */
    char   step[96];                      /* what UPDATE_DOWNLOADING is doing */
    char   error[192];
} UpdateStatus;

/* Early boot: the update flags the boot loader passed */
void UpdateBootInfo(const BootInfo *info);
/* The desktop is up: finish the update this start tried, or throw away
 * one that failed (on a thread of its own) */
void UpdateBootDone(void);
/* What happened to the last update at this start ("" if nothing):
 * "NovaOS was updated from 0.1.0 to 0.1.1." or why it was not */
const char *UpdateBootNotice(void);

/* The update channel's URL (the default when none is set) */
void UpdateGetChannel(char *out, int cap);
/* Set it (saved in the registry); NULL or "" goes back to the default */
void UpdateSetChannel(const char *url);

/* Ask the channel for the newest version; false if busy (or live) with
 * why in the status */
bool UpdateCheck(void);
/* Download and stage the newer version (checking first if need be);
 * false if busy, live or there is nothing newer */
bool UpdateInstall(void);
void UpdateGetStatus(UpdateStatus *out);
