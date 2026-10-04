/*
 * setupapi.dll — opening a device or device interface by name.  With no
 * device tree (setupapi.c) no device instance ID or interface path names
 * one, so each is "not found", as on Windows for a device that is not
 * present; the set is left as it was.
 */
#include <windows.h>

#define SETUPAPI __declspec(dllexport)
#define ERROR_NO_SUCH_DEVINST_            0xE000020B
#define ERROR_NO_SUCH_DEVICE_INTERFACE_   0xE0000225

static BOOL not_found(HANDLE set, const void *name, DWORD err)
{
    if (!set || set == INVALID_HANDLE_VALUE) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    SetLastError(name ? err : ERROR_INVALID_PARAMETER);
    return FALSE;
}

SETUPAPI BOOL WINAPI SetupDiOpenDeviceInfoW(HANDLE set, LPCWSTR id, HWND parent, DWORD flags, PVOID dev)
{ (void)parent; (void)flags; (void)dev; return not_found(set, id, ERROR_NO_SUCH_DEVINST_); }
SETUPAPI BOOL WINAPI SetupDiOpenDeviceInfoA(HANDLE set, LPCSTR id, HWND parent, DWORD flags, PVOID dev)
{ (void)parent; (void)flags; (void)dev; return not_found(set, id, ERROR_NO_SUCH_DEVINST_); }
SETUPAPI BOOL WINAPI SetupDiOpenDeviceInterfaceW(HANDLE set, LPCWSTR path, DWORD flags, PVOID iface)
{ (void)flags; (void)iface; return not_found(set, path, ERROR_NO_SUCH_DEVICE_INTERFACE_); }
SETUPAPI BOOL WINAPI SetupDiOpenDeviceInterfaceA(HANDLE set, LPCSTR path, DWORD flags, PVOID iface)
{ (void)flags; (void)iface; return not_found(set, path, ERROR_NO_SUCH_DEVICE_INTERFACE_); }
