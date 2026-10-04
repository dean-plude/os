/*
 * firmware.c — the firmware's tables for GetSystemFirmwareTable (see firmware.h)
 */

#include "firmware.h"
#include "acpi.h"
#include "../lib/string.h"
#include "../ke/printf.h"

#define PROVIDER_RSMB 0x52534D42u          /* 'RSMB' */
#define PROVIDER_ACPI 0x41435049u          /* 'ACPI' */

static const UINT8 *g_smbios;              /* RawSMBIOSData, in the bootloader's pages */
static UINT32       g_smbios_size;

void FirmwareBootInfo(const BootInfo *info)
{
    if (!info || info->version < 4 || !info->smbios_base || info->smbios_size < 8) return;
    g_smbios = PHYS_TO_VIRT(info->smbios_base);
    g_smbios_size = (UINT32)info->smbios_size;
    kprintf("[FW] SMBIOS %u.%u, %u bytes of structures\n", g_smbios[1], g_smbios[2], g_smbios_size - 8);
}

static UINT32 give(const void *src, UINT32 size, void *out, UINT32 cap)
{
    if (out && cap >= size) memcpy(out, src, size);
    return size;
}

UINT32 FirmwareTable(UINT32 provider, UINT32 action, UINT32 id, void *out, UINT32 cap)
{
    if (provider == PROVIDER_RSMB) {
        if (!g_smbios) return 0;
        if (action == 0) { UINT32 zero = 0; return give(&zero, 4, out, cap); }
        return action == 1 ? give(g_smbios, g_smbios_size, out, cap) : 0;
    }
    if (provider == PROVIDER_ACPI) {
        if (action == 0) {                 /* every table's signature, as its ID */
            UINT32 n = 0;
            while (AcpiTableAt(n)) n++;
            if (out && cap >= 4 * n)
                for (UINT32 i = 0; i < n; i++) memcpy((UINT8 *)out + 4 * i, AcpiTableAt(i), 4);
            return 4 * n;
        }
        if (action != 1) return 0;
        for (UINT32 i = 0; ; i++) {        /* the ID is the signature's four bytes */
            const UINT8 *t = AcpiTableAt(i);
            if (!t) return 0;
            if (!memcmp(t, &id, 4)) return give(t, *(const UINT32 *)(t + 4), out, cap);
        }
    }
    return 0;
}
