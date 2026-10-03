/*
 * bootlog.h — the boot log on the USB stick NovaOS started from
 *
 * A PC without a serial port (most laptops) shows nothing of the kernel's
 * log once the desktop is up.  Started from the ISO written to a USB
 * stick, NovaOS writes its log into \EFI\NOVA\bootlog.txt on that stick
 * as it goes, so the stick can be read on another computer after a hang.
 * The file is set aside on the ISO (scripts/create-iso.sh, 1 MiB) and
 * written in place, sector by sector: no FAT metadata changes, so the
 * stick's read-only drive letter stays valid.
 */

#pragma once

#include "block.h"

/* A USB stick arrived: if NovaOS started from it, start writing the log
 * there (the log so far, then what follows) */
void      BootLogAttach(BlockDev *d);
/* Write what was logged since the last time, at most once a second
 * (called often, from the desktop thread) */
void      BootLogPoll(void);
/* Write it now (before restarting or shutting down) */
void      BootLogSync(void);
/* A kernel fault is about to halt this CPU: write the log one last time
 * if the stick is free (best effort; the USB stack may be what faulted) */
void      BootLogPanic(void);
/* The stick the log goes to, or NULL */
BlockDev *BootLogDevice(void);
