/*
 * firmware.h — the firmware's tables for GetSystemFirmwareTable
 *
 * Programs read the SMBIOS tables ('RSMB': the machine's maker, model,
 * serial numbers, BIOS version) and the ACPI tables ('ACPI') through
 * NtQuerySystemInformation(SystemFirmwareTableInformation), which ntdll
 * passes to NtNovaFirmwareTable.  The bootloader copies the SMBIOS
 * tables (boot protocol 4); ACPI tables are read where the firmware left
 * them (acpi.h).
 */

#pragma once

#include "../include/types.h"
#include "../../include/boot_protocol.h"

/* Remember the bootloader's SMBIOS copy.  Call once, at boot. */
void FirmwareBootInfo(const BootInfo *info);

/* SYSTEM_FIRMWARE_TABLE_INFORMATION's work: for @provider ('RSMB',
 * 'ACPI', as the multi-character constants Windows uses), @action 0 lists
 * the table IDs (4 bytes each), 1 copies table @id.  Copies up to @cap
 * bytes to @out and returns the full size; 0 = no such provider or table. */
UINT32 FirmwareTable(UINT32 provider, UINT32 action, UINT32 id, void *out, UINT32 cap);
