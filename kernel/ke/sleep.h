/*
 * sleep.h — S3 (suspend to RAM) for every CPU, or low-power S0 idle
 */

#pragma once

#include "../include/types.h"

/* Can this machine sleep (ACPI \_S3, a FACS, the start-up page free; or,
 * without \_S3, low-power S0 idle: the FADT's flag or an LPS0 device)? */
bool SleepSupported(void);

/* Put the machine to sleep; returns true once it has woken up and the
 * devices are back, false if it couldn't sleep.  The desktop thread calls
 * it, with drive C: saved. */
bool SleepEnter(void);

/* Is a CPU putting the machine to sleep?  Then an NMI is its request to
 * park: SleepFreezeCpu (interrupt context) saves this CPU's state and
 * waits until the machine wakes. */
bool SleepFreezing(void);
void SleepFreezeCpu(void);

/* Interrupt time (100 ns units since boot) when the machine last went to
 * sleep and last woke up; 0 if it hasn't */
UINT64 SleepLastSleepTime(void);
UINT64 SleepLastWakeTime(void);
