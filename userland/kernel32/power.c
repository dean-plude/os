/*
 * power.c — GetSystemPowerStatus: the ACPI batteries and AC adapters
 * (NtPowerInformation's SystemBatteryState, as on Windows)
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include "k32.h"

typedef struct {
    BOOLEAN AcOnLine, BatteryPresent, Charging, Discharging, Spare1[3];
    BYTE    Tag;
    ULONG   MaxCapacity, RemainingCapacity;
    LONG    Rate;
    ULONG   EstimatedTime, DefaultAlert1, DefaultAlert2;
} BATTERY_STATE;

WINBASEAPI BOOL WINAPI GetSystemPowerStatus(LPSYSTEM_POWER_STATUS s)
{
    BATTERY_STATE b;
    if (!s) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (NtPowerInformation(5, NULL, 0, &b, sizeof(b)) < 0) {
        s->ACLineStatus = 255;
        s->BatteryFlag = 255;
        s->BatteryLifePercent = 255;
        s->SystemStatusFlag = 0;
        s->BatteryLifeTime = s->BatteryFullLifeTime = (DWORD)-1;
        return TRUE;
    }
    s->ACLineStatus = b.AcOnLine ? 1 : 0;
    s->SystemStatusFlag = 0;
    s->BatteryLifeTime = s->BatteryFullLifeTime = (DWORD)-1;
    if (!b.BatteryPresent) {
        s->BatteryFlag = 128;
        s->BatteryLifePercent = 255;
        return TRUE;
    }
    DWORD pct = 255;
    if (b.MaxCapacity) {
        pct = (DWORD)((ULONGLONG)b.RemainingCapacity * 100 / b.MaxCapacity);
        if (pct > 100) pct = 100;
    }
    s->BatteryLifePercent = (BYTE)pct;
    s->BatteryFlag = pct == 255 ? 255 : pct > 66 ? 1 : pct < 5 ? 2 | 4 : pct < 33 ? 2 : 0;
    if (b.Charging && pct != 255) s->BatteryFlag |= 8;
    if (b.Discharging && b.EstimatedTime != 0xFFFFFFFF) {
        s->BatteryLifeTime = b.EstimatedTime;
        if (b.RemainingCapacity)
            s->BatteryFullLifeTime = (DWORD)((ULONGLONG)b.EstimatedTime * b.MaxCapacity / b.RemainingCapacity);
    }
    return TRUE;
}
