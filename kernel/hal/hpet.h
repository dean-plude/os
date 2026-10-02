/*
 * hpet.h — the High Precision Event Timer, as a reference clock
 *
 * Its main counter runs at a fixed rate the hardware states, so it
 * calibrates the TSC and the local APIC timer (apic.c) more precisely
 * than the PIT, and without needing a PIT at all.  NovaOS doesn't use
 * its comparators: each CPU's local APIC timer interrupts it.
 */

#pragma once

#include "../include/types.h"

/* Find the HPET (the ACPI "HPET" table, read straight from the RSDP:
 * this runs before AcpiInitialize) and start its main counter.  False if
 * there is none. */
bool HpetInit(UINT64 rsdp_physical);
bool HpetPresent(void);

/* The main counter, and its rate */
UINT64 HpetCounter(void);
UINT64 HpetFrequency(void);

/* Busy-wait @us microseconds on the main counter */
void HpetDelay(UINT64 us);
