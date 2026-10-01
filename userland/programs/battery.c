/*
 * battery.exe — show the power source and battery charge
 *
 * GetSystemPowerStatus and powrprof's CallNtPowerInformation
 * (SystemBatteryState), which read the ACPI batteries and AC adapters.
 */
#include <windows.h>
#include <stdio.h>

typedef LONG (WINAPI *CallNtPowerInformationFn)(int, PVOID, ULONG, PVOID, ULONG);
typedef struct {
    BOOLEAN AcOnLine, BatteryPresent, Charging, Discharging, Spare1[3];
    BYTE    Tag;
    ULONG   MaxCapacity, RemainingCapacity;
    LONG    Rate;
    ULONG   EstimatedTime, DefaultAlert1, DefaultAlert2;
} BATTERY_STATE;

int main(void)
{
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s)) { printf("GetSystemPowerStatus failed: %lu\n", GetLastError()); return 1; }
    printf("Power source: %s\n", s.ACLineStatus == 1 ? "AC" : s.ACLineStatus == 0 ? "battery" : "unknown");
    if (s.BatteryFlag & 128) {
        printf("No battery\n");
    } else {
        if (s.BatteryLifePercent <= 100) printf("Battery: %u%%%s\n", s.BatteryLifePercent,
                                                s.BatteryFlag & 8 ? ", charging" : "");
        else printf("Battery: charge unknown\n");
        if (s.BatteryLifeTime != (DWORD)-1)
            printf("Time left: %lu h %02lu min\n", s.BatteryLifeTime / 3600, s.BatteryLifeTime / 60 % 60);
    }

    HMODULE pp = LoadLibraryA("powrprof.dll");
    CallNtPowerInformationFn cnpi = pp ? (CallNtPowerInformationFn)GetProcAddress(pp, "CallNtPowerInformation") : NULL;
    BATTERY_STATE b;
    if (cnpi && cnpi(5, NULL, 0, &b, sizeof(b)) == 0)
        printf("SystemBatteryState: present %d, AC %d, charging %d, discharging %d, %lu of %lu mWh, rate %ld mW\n",
               b.BatteryPresent, b.AcOnLine, b.Charging, b.Discharging,
               b.RemainingCapacity, b.MaxCapacity, b.Rate);
    return 0;
}
