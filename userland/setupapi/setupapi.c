/*
 * setupapi.dll — device installation and enumeration (SetupDi*).  NovaOS
 * has no Plug and Play device tree (see cfgmgr32), so every device
 * information set is empty: enumeration ends at once, with
 * ERROR_NO_MORE_ITEMS, as on a machine without such devices.
 */
#include <windows.h>

#define SETUPAPI __declspec(dllexport)
#define ERROR_NO_MORE_ITEMS_ 259

/* An empty set is a real handle (programs compare it with
 * INVALID_HANDLE_VALUE and destroy it) */
static HANDLE empty_set(void)
{
    void *p = HeapAlloc(GetProcessHeap(), 0, 16);
    if (!p) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    return p;
}

SETUPAPI HANDLE WINAPI SetupDiGetClassDevsW(const GUID *cls, LPCWSTR enumerator, HWND parent, DWORD flags)
{ (void)cls; (void)enumerator; (void)parent; (void)flags; return empty_set(); }
SETUPAPI HANDLE WINAPI SetupDiGetClassDevsA(const GUID *cls, LPCSTR enumerator, HWND parent, DWORD flags)
{ (void)cls; (void)enumerator; (void)parent; (void)flags; return empty_set(); }
SETUPAPI HANDLE WINAPI SetupDiGetClassDevsExW(const GUID *cls, LPCWSTR enumerator, HWND parent, DWORD flags, HANDLE set, LPCWSTR machine, PVOID r)
{ (void)cls; (void)enumerator; (void)parent; (void)flags; (void)set; (void)machine; (void)r; return empty_set(); }
SETUPAPI HANDLE WINAPI SetupDiCreateDeviceInfoList(const GUID *cls, HWND parent) { (void)cls; (void)parent; return empty_set(); }
SETUPAPI BOOL WINAPI SetupDiDestroyDeviceInfoList(HANDLE set)
{
    if (!set || set == INVALID_HANDLE_VALUE) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    HeapFree(GetProcessHeap(), 0, set);
    return TRUE;
}

static BOOL no_more(void) { SetLastError(ERROR_NO_MORE_ITEMS_); return FALSE; }
SETUPAPI BOOL WINAPI SetupDiEnumDeviceInfo(HANDLE set, DWORD i, PVOID info) { (void)set; (void)i; (void)info; return no_more(); }
SETUPAPI BOOL WINAPI SetupDiEnumDeviceInterfaces(HANDLE set, PVOID dev, const GUID *cls, DWORD i, PVOID info)
{ (void)set; (void)dev; (void)cls; (void)i; (void)info; return no_more(); }

/* With no devices in any set, there is never a device to describe */
static BOOL no_device(void) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
SETUPAPI BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(HANDLE set, PVOID iface, PVOID detail, DWORD n, PDWORD need, PVOID dev)
{ (void)set; (void)iface; (void)detail; (void)n; (void)need; (void)dev; return no_device(); }
SETUPAPI BOOL WINAPI SetupDiGetDeviceInterfaceDetailA(HANDLE set, PVOID iface, PVOID detail, DWORD n, PDWORD need, PVOID dev)
{ (void)set; (void)iface; (void)detail; (void)n; (void)need; (void)dev; return no_device(); }
SETUPAPI BOOL WINAPI SetupDiGetDeviceRegistryPropertyW(HANDLE set, PVOID dev, DWORD prop, PDWORD type, PBYTE buf, DWORD n, PDWORD need)
{ (void)set; (void)dev; (void)prop; (void)type; (void)buf; (void)n; (void)need; return no_device(); }
SETUPAPI BOOL WINAPI SetupDiGetDeviceRegistryPropertyA(HANDLE set, PVOID dev, DWORD prop, PDWORD type, PBYTE buf, DWORD n, PDWORD need)
{ (void)set; (void)dev; (void)prop; (void)type; (void)buf; (void)n; (void)need; return no_device(); }
SETUPAPI BOOL WINAPI SetupDiGetDevicePropertyW(HANDLE set, PVOID dev, const void *key, PULONG type, PBYTE buf, DWORD n, PDWORD need, DWORD flags)
{ (void)set; (void)dev; (void)key; (void)type; (void)buf; (void)n; (void)need; (void)flags; return no_device(); }
SETUPAPI BOOL WINAPI SetupDiGetDeviceInstanceIdW(HANDLE set, PVOID dev, LPWSTR id, DWORD n, PDWORD need)
{ (void)set; (void)dev; (void)id; (void)n; (void)need; return no_device(); }
SETUPAPI HKEY WINAPI SetupDiOpenDevRegKey(HANDLE set, PVOID dev, DWORD scope, DWORD profile, DWORD type, REGSAM sam)
{
    (void)set; (void)dev; (void)scope; (void)profile; (void)type; (void)sam;
    SetLastError(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
}
SETUPAPI BOOL WINAPI SetupDiClassGuidsFromNameW(LPCWSTR name, GUID *list, DWORD n, PDWORD need)
{ (void)name; (void)list; (void)n; if (need) *need = 0; return TRUE; }
