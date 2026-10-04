/*
 * powrprof.dll — power management.  NovaOS runs every processor at one
 * fixed speed, with the "Balanced" scheme active; batteries, the lid,
 * thermal zones and the last sleep and wake come from the kernel
 * (NtPowerInformation).
 */
#include <windows.h>
#include <winternl.h>

#define POWRPROF __declspec(dllexport)
#define STATUS_SUCCESS_           0
#define STATUS_BUFFER_TOO_SMALL_  0xC0000023L
#define STATUS_INVALID_PARAMETER_ 0xC000000DL

typedef struct { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; } PPI;
typedef struct {                     /* SYSTEM_POWER_STATUS-like SYSTEM_BATTERY_STATE */
    BOOLEAN AcOnLine, BatteryPresent, Charging, Discharging, Spare1[3];
    BYTE Tag;
    ULONG MaxCapacity, RemainingCapacity, Rate, EstimatedTime, DefaultAlert1, DefaultAlert2;
} BATTERY_STATE_;

/* NTSTATUS CallNtPowerInformation(POWER_INFORMATION_LEVEL, in, inlen, out, outlen) */
POWRPROF LONG WINAPI CallNtPowerInformation(int level, PVOID in, ULONG inlen, PVOID out, ULONG outlen)
{
    (void)in; (void)inlen;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (level) {
    case 11: {                                       /* ProcessorInformation: one entry per CPU */
        ULONG need = si.dwNumberOfProcessors * (ULONG)sizeof(PPI);
        if (!out || outlen < need) return STATUS_BUFFER_TOO_SMALL_;
        PPI *p = out;
        for (DWORD i = 0; i < si.dwNumberOfProcessors; i++) {
            p[i].Number = i;
            p[i].MaxMhz = p[i].CurrentMhz = p[i].MhzLimit = 2000;
            p[i].MaxIdleState = p[i].CurrentIdleState = 0;
        }
        return STATUS_SUCCESS_;
    }
    case 5:                                          /* SystemBatteryState: the ACPI batteries */
        if (!out || outlen < sizeof(BATTERY_STATE_)) return STATUS_BUFFER_TOO_SMALL_;
        return NtPowerInformation(5, in, inlen, out, outlen);
    case 4:                                          /* SystemPowerCapabilities */
    case 12:                                         /* ThermalInformation */
    case 14: case 15:                                /* LastWakeTime, LastSleepTime */
        return NtPowerInformation(level, in, inlen, out, outlen);
    default:
        if (out && outlen) ZeroMemory(out, outlen);
        return out ? STATUS_SUCCESS_ : STATUS_INVALID_PARAMETER_;
    }
}

/* The active scheme: Balanced {381b4222-f694-41f0-9685-ff5bb260df2e} */
static const GUID g_balanced = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
POWRPROF DWORD WINAPI PowerGetActiveScheme(HKEY root, GUID **scheme)
{
    (void)root;
    GUID *g = LocalAlloc(LMEM_FIXED, sizeof(GUID));
    if (!g) return ERROR_NOT_ENOUGH_MEMORY;
    *g = g_balanced;
    *scheme = g;
    return ERROR_SUCCESS;
}
POWRPROF DWORD WINAPI PowerSetActiveScheme(HKEY root, const GUID *scheme) { (void)root; (void)scheme; return ERROR_SUCCESS; }
POWRPROF DWORD WINAPI PowerReadACValueIndex(HKEY root, const GUID *s, const GUID *sub, const GUID *set, LPDWORD v)
{ (void)root; (void)s; (void)sub; (void)set; *v = 0; return ERROR_SUCCESS; }
POWRPROF DWORD WINAPI PowerReadDCValueIndex(HKEY root, const GUID *s, const GUID *sub, const GUID *set, LPDWORD v)
{ (void)root; (void)s; (void)sub; (void)set; *v = 0; return ERROR_SUCCESS; }
/* The value of a setting by type: every setting reads as the DWORD 0 */
static DWORD read_value(LPDWORD type, LPBYTE buf, LPDWORD size)
{
    if (type) *type = REG_DWORD;
    if (!size) return buf ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    if (!buf) { *size = sizeof(DWORD); return ERROR_SUCCESS; }
    if (*size < sizeof(DWORD)) { *size = sizeof(DWORD); return ERROR_MORE_DATA; }
    *size = sizeof(DWORD);
    ZeroMemory(buf, sizeof(DWORD));
    return ERROR_SUCCESS;
}
POWRPROF DWORD WINAPI PowerReadACValue(HKEY root, const GUID *s, const GUID *sub, const GUID *set, LPDWORD type, LPBYTE buf, LPDWORD size)
{ (void)root; (void)s; (void)sub; (void)set; return read_value(type, buf, size); }
POWRPROF DWORD WINAPI PowerReadDCValue(HKEY root, const GUID *s, const GUID *sub, const GUID *set, LPDWORD type, LPBYTE buf, LPDWORD size)
{ (void)root; (void)s; (void)sub; (void)set; return read_value(type, buf, size); }
/* POWER_PLATFORM_ROLE: Mobile (2) with a battery, else Desktop (1) */
POWRPROF int WINAPI PowerDeterminePlatformRoleEx(ULONG version)
{
    (void)version;
    SYSTEM_POWER_STATUS ps;
    return GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255 ? 2 : 1;
}
POWRPROF DWORD WINAPI PowerRegisterSuspendResumeNotification(DWORD flags, HANDLE recipient, PVOID *h)
{ (void)flags; (void)recipient; *h = (PVOID)(ULONG_PTR)0x5E01; return ERROR_SUCCESS; }
POWRPROF DWORD WINAPI PowerUnregisterSuspendResumeNotification(PVOID h) { (void)h; return ERROR_SUCCESS; }
/* Power setting and effective power mode notifications: NovaOS changes
 * neither setting while running (no power plans, no battery saver), so a
 * registration succeeds and nothing is ever sent to it */
POWRPROF DWORD WINAPI PowerSettingRegisterNotification(const GUID *setting, DWORD flags, HANDLE recipient, PVOID *h)
{
    (void)flags; (void)recipient;
    if (!setting || !h) return ERROR_INVALID_PARAMETER;
    *h = (PVOID)(ULONG_PTR)0x5E02;
    return ERROR_SUCCESS;
}
POWRPROF DWORD WINAPI PowerSettingUnregisterNotification(PVOID h) { return h ? ERROR_SUCCESS : ERROR_INVALID_HANDLE; }
POWRPROF HRESULT WINAPI PowerRegisterForEffectivePowerModeNotifications(ULONG version, PVOID callback, PVOID ctx, PVOID *h)
{
    (void)version; (void)ctx;
    if (!callback || !h) return E_INVALIDARG;
    *h = (PVOID)(ULONG_PTR)0x5E03;
    return S_OK;
}
POWRPROF HRESULT WINAPI PowerUnregisterFromEffectivePowerModeNotifications(PVOID h) { return h ? S_OK : E_INVALIDARG; }
/* SYSTEM_POWER_CAPABILITIES: a power button, the lid, S3, S5, thermal
 * control and the batteries, as the kernel finds them */
POWRPROF BOOLEAN WINAPI GetPwrCapabilities(PVOID caps)
{
    if (NtPowerInformation(4, NULL, 0, caps, 76) < 0) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return TRUE;
}
/* S3 where the firmware offers it (SetSuspendState fails where it doesn't);
 * no hibernation */
POWRPROF BOOLEAN WINAPI IsPwrSuspendAllowed(void) { return TRUE; }
POWRPROF BOOLEAN WINAPI IsPwrHibernateAllowed(void) { return FALSE; }
POWRPROF BOOLEAN WINAPI IsPwrShutdownAllowed(void) { return TRUE; }
/* Sleep; returns once the machine is awake again */
POWRPROF BOOLEAN WINAPI SetSuspendState(BOOLEAN hib, BOOLEAN force, BOOLEAN wake)
{
    (void)force; (void)wake;
    LONG s = NtInitiatePowerAction(hib ? 3 : 2, hib ? 5 : 4, 0, FALSE);   /* PowerSystemHibernate / Sleeping3 */
    if (s) { SetLastError(s == (LONG)0xC00000BB ? ERROR_NOT_SUPPORTED : ERROR_GEN_FAILURE); return FALSE; }
    return TRUE;
}
