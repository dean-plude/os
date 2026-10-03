/*
 * acpi.h — ACPI tables and fixed-hardware power control
 *
 * Power-off, reset and the power button work without the AML interpreter
 * (aml.h), which loads later: they read straight from the tables: the FADT's PM1 blocks and
 * reset register, and the \_S5 package's sleep-type values, which the
 * DSDT (or an SSDT) declares as plain data that can be found without
 * running any AML.
 */

#pragma once

#include "../include/types.h"

/* Remember where the RSDP is (boot information), then read the FADT and
 * \_S5 and switch the chipset into ACPI mode.  Call once, after paging. */
void AcpiInitialize(UINT64 rsdp_physical);

/* The RSDP's physical address (0 if the firmware gave none) */
UINT64 AcpiRsdpAddress(void);

/* The first table with this 4-character signature (XSDT, else RSDT), or
 * NULL.  The table starts with the standard 36-byte header. */
const void *AcpiFindTable(const char *sig);

/* Enter S5 (soft off).  Returns only if the hardware didn't power off. */
void AcpiPowerOff(void);

/* Is S3 (suspend to RAM) possible: an \_S3 package and a FACS? */
bool AcpiSleepSupported(void);

/* The FADT says the platform idles at low power in S0 (Modern Standby):
 * firmware that often has no \_S3 at all */
bool AcpiLowPowerS0(void);

/* Enter S3 with the firmware waking vectors: @real_vector (real mode,
 * below 1 MiB) and @pm32_vector (32-bit protected mode, which ACPI 2.0+
 * firmware prefers).  Returns false if the machine didn't go to sleep; on
 * wake the firmware jumps to a vector instead of returning. */
bool AcpiEnterS3(UINT32 real_vector, UINT32 pm32_vector);

/* After waking: ACPI mode and the power button again */
void AcpiResume(void);
/* PM1 status as the machine woke (bit 8: the power button, 10: the RTC alarm) */
UINT16 AcpiWakeStatus(void);

/* Reset the machine: the FADT's reset register, then port 0xCF9, then
 * the 8042, then a triple fault.  Never returns. */
void AcpiReset(void) __attribute__((noreturn));

/* Was the power button pressed since the last call?  The fixed-feature
 * one, read here until the AML interpreter (aml.h) takes over the events;
 * then also control-method buttons. */
bool AcpiPowerButtonPressed(void);
