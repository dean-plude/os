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
SETUPAPI BOOL WINAPI SetupDiGetDeviceInstanceIdA(HANDLE set, PVOID dev, LPSTR id, DWORD n, PDWORD need)
{ (void)set; (void)dev; (void)id; (void)n; (void)need; return no_device(); }

/* Device setup classes exist without devices: the class GUIDs Windows
 * defines for the names programs ask about (no device ever matches them) */
static const struct { const char *name; GUID guid; } g_classes[] = {
    { "Bluetooth",    { 0xe0cbf06c, 0xcd8b, 0x4647, { 0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74 } } },
    { "Display",      { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } } },
    { "HIDClass",     { 0x745a17a0, 0x74d3, 0x11d0, { 0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda } } },
    { "Image",        { 0x6bdd1fc6, 0x810f, 0x11d0, { 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f } } },
    { "Keyboard",     { 0x4d36e96b, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } } },
    { "Media",        { 0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } } },
    { "Mouse",        { 0x4d36e96f, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } } },
    { "Net",          { 0x4d36e972, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } } },
    { "Ports",        { 0x4d36e978, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } } },
    { "System",       { 0x4d36e97d, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } } },
    { "USB",          { 0x36fc9e60, 0xc465, 0x11cf, { 0x80, 0x56, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } } },
    { "XnaComposite", { 0x05f5cfe2, 0x4733, 0x4950, { 0xa6, 0xbb, 0x07, 0xaa, 0xd0, 0x1a, 0x3a, 0x84 } } },
};

SETUPAPI BOOL WINAPI SetupDiClassGuidsFromNameA(LPCSTR name, GUID *list, DWORD n, PDWORD need)
{
    if (!name || !need) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD k = 0;
    for (DWORD i = 0; i < sizeof(g_classes) / sizeof(g_classes[0]); i++)
        if (!lstrcmpiA(name, g_classes[i].name)) { if (list && k < n) list[k] = g_classes[i].guid; k++; }
    *need = k;
    if (k > n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    return TRUE;
}

SETUPAPI BOOL WINAPI SetupDiClassGuidsFromNameW(LPCWSTR name, GUID *list, DWORD n, PDWORD need)
{
    char a[64];
    if (!name || !WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof(a), 0, 0)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return SetupDiClassGuidsFromNameA(a, list, n, need);
}

/* The CM_* calls setupapi exports for cfgmgr32 (forwarders on Windows) */
typedef DWORD (WINAPI *cm_status_fn)(PULONG, PULONG, DWORD, ULONG);
typedef DWORD (WINAPI *cm_locate_fn)(PDWORD, LPCSTR, ULONG);
static FARPROC cfgmgr(const char *fn)
{
    HMODULE m = LoadLibraryA("cfgmgr32.dll");
    return m ? GetProcAddress(m, fn) : 0;
}
SETUPAPI DWORD WINAPI CM_Get_DevNode_Status(PULONG status, PULONG problem, DWORD dn, ULONG flags)
{
    cm_status_fn f = (cm_status_fn)cfgmgr("CM_Get_DevNode_Status");
    return f ? f(status, problem, dn, flags) : 0x0D /* CR_NO_SUCH_DEVNODE */;
}
SETUPAPI DWORD WINAPI CM_Locate_DevNodeA(PDWORD dn, LPCSTR id, ULONG flags)
{
    cm_locate_fn f = (cm_locate_fn)cfgmgr("CM_Locate_DevNodeA");
    return f ? f(dn, id, flags) : 0x0D /* CR_NO_SUCH_DEVNODE */;
}
