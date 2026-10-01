/*
 * powrprof.dll — power management.  NovaOS runs every processor at one
 * fixed speed on mains power, with the "Balanced" scheme active.
 */
#include <windows.h>

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
    case 5: {                                        /* SystemBatteryState: on mains, no battery */
        if (!out || outlen < sizeof(BATTERY_STATE_)) return STATUS_BUFFER_TOO_SMALL_;
        BATTERY_STATE_ *b = out;
        ZeroMemory(b, sizeof(*b));
        b->AcOnLine = TRUE;
        b->EstimatedTime = 0xFFFFFFFF;
        return STATUS_SUCCESS_;
    }
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
POWRPROF DWORD WINAPI PowerRegisterSuspendResumeNotification(DWORD flags, HANDLE recipient, PVOID *h)
{ (void)flags; (void)recipient; *h = (PVOID)(ULONG_PTR)0x5E01; return ERROR_SUCCESS; }
POWRPROF DWORD WINAPI PowerUnregisterSuspendResumeNotification(PVOID h) { (void)h; return ERROR_SUCCESS; }
POWRPROF BOOLEAN WINAPI GetPwrCapabilities(PVOID caps) { ZeroMemory(caps, 76); return TRUE; }
POWRPROF BOOLEAN WINAPI IsPwrSuspendAllowed(void) { return FALSE; }
POWRPROF BOOLEAN WINAPI IsPwrHibernateAllowed(void) { return FALSE; }
POWRPROF BOOLEAN WINAPI IsPwrShutdownAllowed(void) { return TRUE; }
POWRPROF BOOLEAN WINAPI SetSuspendState(BOOLEAN hib, BOOLEAN force, BOOLEAN wake) { (void)hib; (void)force; (void)wake; return FALSE; }
