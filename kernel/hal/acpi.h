/*
 * acpi.h — ACPI tables and fixed-hardware power control
 *
 * NovaOS has no AML interpreter.  It reads what power-off, reset and the
 * power button need straight from the tables: the FADT's PM1 blocks and
 * reset register, and the \_S5 package's sleep-type values, which the
 * DSDT (or an SSDT) declares as plain data that can be found without
 * running any AML.
 */

#pragma once

#include "../include/types.h"

/* Remember where the RSDP is (boot information), then read the FADT and
 * \_S5 and switch the chipset into ACPI mode.  Call once, after paging. */
void AcpiInitialize(UINT64 rsdp_physical);

/* The first table with this 4-character signature (XSDT, else RSDT), or
 * NULL.  The table starts with the standard 36-byte header. */
const void *AcpiFindTable(const char *sig);

/* Enter S5 (soft off).  Returns only if the hardware didn't power off. */
void AcpiPowerOff(void);

/* Is S3 (suspend to RAM) possible: an \_S3 package and a FACS? */
bool AcpiSleepSupported(void);

/* Enter S3 with the firmware waking vectors: @real_vector (real mode,
 * below 1 MiB) and @pm32_vector (32-bit protected mode, which ACPI 2.0+
 * firmware prefers).  Returns false if the machine didn't go to sleep; on
 * wake the firmware jumps to a vector instead of returning. */
bool AcpiEnterS3(UINT32 real_vector, UINT32 pm32_vector);

/* After waking: ACPI mode and the power button again */
void AcpiResume(void);

/* Reset the machine: the FADT's reset register, then port 0xCF9, then
 * the 8042, then a triple fault.  Never returns. */
void AcpiReset(void) __attribute__((noreturn));

/* Was the (fixed-feature) power button pressed since the last call? */
bool AcpiPowerButtonPressed(void);
