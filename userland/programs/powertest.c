/*
 * powertest.exe — the lid, a thermal zone and sleep, with the test's help
 *
 * Run by tools/selftest.py on a boot with tests/acpi/lid-thermal.asl: a lid
 * and a thermal zone (40 C; passive 60 C, hot 90 C, critical 95 C) that the
 * test drives from the QEMU monitor when this program asks:
 *   "powertest: close the lid"   the lid closes; NovaOS should sleep, and
 *                                the test opens it, presses a USB key and
 *                                wakes the machine
 *   "powertest: heat to 70 C"    above the passive trip point
 *   "powertest: cool to 45 C"
 * Checks the capabilities, the thermal readings (CallNtPowerInformation)
 * and that closing the lid put the machine to sleep (LastSleepTime and
 * LastWakeTime moved on).
 */
#include <windows.h>
#include <stdio.h>

typedef LONG (WINAPI *CallNtPowerInformationFn)(int, PVOID, ULONG, PVOID, ULONG);
typedef BOOLEAN (WINAPI *GetPwrCapabilitiesFn)(PVOID);

typedef struct {
    ULONG     ThermalStamp, ThermalConstant1, ThermalConstant2;
    ULONG_PTR Processors;
    ULONG     SamplingPeriod, CurrentTemperature, PassiveTripPoint, CriticalTripPoint;
    UCHAR     ActiveTripPointCount;
    ULONG     ActiveTripPoint[10];
} THERMAL_INFO;

static CallNtPowerInformationFn cnpi;
static int passed, failed;

static void check(const char *what, BOOL ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (ok) passed++; else failed++;
}

static ULONG temperature(void)
{
    THERMAL_INFO t;
    return cnpi(12, NULL, 0, &t, sizeof(t)) == 0 ? t.CurrentTemperature : 0;
}

static ULONGLONG power_time(int level)
{
    ULONGLONG t = 0;
    cnpi(level, NULL, 0, &t, sizeof(t));
    return t;
}

/* Wait up to @secs for the zone to read @dk tenths of a kelvin */
static BOOL wait_temp(ULONG dk, int secs)
{
    for (int i = 0; i < secs * 4; i++) {
        if (temperature() == dk) return TRUE;
        Sleep(250);
    }
    return FALSE;
}

int main(void)
{
    HMODULE pp = LoadLibraryA("powrprof.dll");
    cnpi = pp ? (CallNtPowerInformationFn)GetProcAddress(pp, "CallNtPowerInformation") : NULL;
    GetPwrCapabilitiesFn gpc = pp ? (GetPwrCapabilitiesFn)GetProcAddress(pp, "GetPwrCapabilities") : NULL;
    if (!cnpi || !gpc) { printf("FAIL: powrprof\n"); return 1; }

    BOOLEAN caps[76];
    check("GetPwrCapabilities", gpc(caps));
    check("LidPresent", caps[2]);
    check("SystemS3", caps[5]);
    check("ThermalControl", caps[13]);

    THERMAL_INFO t;
    LONG st = cnpi(12, NULL, 0, &t, sizeof(t));
    check("ThermalInformation", st == 0);
    printf("Thermal zone: %lu.%lu C, passive %lu.%lu C, critical %lu.%lu C, sampled every %lu.%lu s\n",
           (t.CurrentTemperature - 2732) / 10, (t.CurrentTemperature - 2732) % 10,
           (t.PassiveTripPoint - 2732) / 10, (t.PassiveTripPoint - 2732) % 10,
           (t.CriticalTripPoint - 2732) / 10, (t.CriticalTripPoint - 2732) % 10,
           t.SamplingPeriod / 10, t.SamplingPeriod % 10);
    check("temperature 40 C", t.CurrentTemperature == 3132);
    check("passive trip point 60 C", t.PassiveTripPoint == 3332);
    check("critical trip point 95 C", t.CriticalTripPoint == 3682);
    check("sampled every second", t.SamplingPeriod == 10);

    /* The lid: closing it sleeps */
    ULONGLONG slept0 = power_time(15);
    printf("powertest: close the lid\n");
    fflush(stdout);
    BOOL slept = FALSE;
    for (int i = 0; i < 480 && !slept; i++) {
        Sleep(250);
        slept = power_time(15) != slept0;
    }
    check("closing the lid put the machine to sleep", slept);
    for (int i = 0; i < 40 && power_time(14) <= power_time(15); i++) Sleep(250);
    check("it woke up again", power_time(14) > power_time(15));

    /* Passive cooling, and back */
    printf("powertest: heat to 70 C\n");
    fflush(stdout);
    check("the zone reads 70 C", wait_temp(3432, 30));
    printf("powertest: cool to 45 C\n");
    fflush(stdout);
    check("the zone reads 45 C", wait_temp(3182, 30));

    printf("powertest: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
