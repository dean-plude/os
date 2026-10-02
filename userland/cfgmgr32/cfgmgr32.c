/*
 * cfgmgr32.dll — the Configuration Manager: the device tree and each
 * device's registry keys.  NovaOS has no Plug and Play device tree, so
 * device lists come back empty and device nodes are not found; programs
 * then fall back to their other means (the Vulkan loader, for one, reads
 * its drivers from HKLM\SOFTWARE\Khronos\Vulkan\Drivers).
 */
#include <windows.h>

#define CFGMGR32 __declspec(dllexport)
typedef DWORD CONFIGRET;
typedef DWORD DEVINST, *PDEVINST;
#define CR_SUCCESS          0x00
#define CR_INVALID_POINTER  0x03
#define CR_BUFFER_SMALL     0x1A
#define CR_NO_SUCH_DEVNODE  0x0D
#define CR_NO_SUCH_VALUE    0x25

static CONFIGRET list_size(PULONG len)
{
    if (!len) return CR_INVALID_POINTER;
    *len = 1;                                       /* just the list's final NUL */
    return CR_SUCCESS;
}
CFGMGR32 CONFIGRET WINAPI CM_Get_Device_ID_List_SizeW(PULONG len, LPCWSTR filter, ULONG flags)
{ (void)filter; (void)flags; return list_size(len); }
CFGMGR32 CONFIGRET WINAPI CM_Get_Device_ID_List_SizeA(PULONG len, LPCSTR filter, ULONG flags)
{ (void)filter; (void)flags; return list_size(len); }

CFGMGR32 CONFIGRET WINAPI CM_Get_Device_ID_ListW(LPCWSTR filter, WCHAR *buf, ULONG len, ULONG flags)
{
    (void)filter; (void)flags;
    if (!buf) return CR_INVALID_POINTER;
    if (len < 1) return CR_BUFFER_SMALL;
    buf[0] = 0;
    return CR_SUCCESS;
}
CFGMGR32 CONFIGRET WINAPI CM_Get_Device_ID_ListA(LPCSTR filter, char *buf, ULONG len, ULONG flags)
{
    (void)filter; (void)flags;
    if (!buf) return CR_INVALID_POINTER;
    if (len < 1) return CR_BUFFER_SMALL;
    buf[0] = 0;
    return CR_SUCCESS;
}

CFGMGR32 CONFIGRET WINAPI CM_Locate_DevNodeW(PDEVINST dn, LPCWSTR id, ULONG flags)
{ (void)id; (void)flags; if (dn) *dn = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Locate_DevNodeA(PDEVINST dn, LPCSTR id, ULONG flags)
{ (void)id; (void)flags; if (dn) *dn = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Get_Child(PDEVINST child, DEVINST dn, ULONG flags)
{ (void)dn; (void)flags; if (child) *child = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Get_Sibling(PDEVINST sib, DEVINST dn, ULONG flags)
{ (void)dn; (void)flags; if (sib) *sib = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Get_Parent(PDEVINST parent, DEVINST dn, ULONG flags)
{ (void)dn; (void)flags; if (parent) *parent = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Get_DevNode_Status(PULONG status, PULONG problem, DEVINST dn, ULONG flags)
{
    (void)dn; (void)flags;
    if (status) *status = 0;
    if (problem) *problem = 0;
    return CR_NO_SUCH_DEVNODE;
}
CFGMGR32 CONFIGRET WINAPI CM_Get_Device_IDW(DEVINST dn, WCHAR *buf, ULONG len, ULONG flags)
{ (void)dn; (void)flags; if (buf && len) buf[0] = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Get_Device_IDA(DEVINST dn, char *buf, ULONG len, ULONG flags)
{ (void)dn; (void)flags; if (buf && len) buf[0] = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Get_DevNode_Registry_PropertyW(DEVINST dn, ULONG prop, PULONG type, PVOID buf, PULONG len, ULONG flags)
{ (void)dn; (void)prop; (void)type; (void)buf; (void)flags; if (len) *len = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Get_DevNode_Registry_PropertyA(DEVINST dn, ULONG prop, PULONG type, PVOID buf, PULONG len, ULONG flags)
{ (void)dn; (void)prop; (void)type; (void)buf; (void)flags; if (len) *len = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Get_DevNode_PropertyW(DEVINST dn, const void *key, PULONG type, PBYTE buf, PULONG len, ULONG flags)
{ (void)dn; (void)key; (void)type; (void)buf; (void)flags; if (len) *len = 0; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Open_DevNode_Key(DEVINST dn, DWORD sam, ULONG profile, DWORD disp, HKEY *key, ULONG flags)
{ (void)dn; (void)sam; (void)profile; (void)disp; (void)flags; if (key) *key = NULL; return CR_NO_SUCH_DEVNODE; }
CFGMGR32 CONFIGRET WINAPI CM_Get_Device_Interface_List_SizeW(PULONG len, LPGUID cls, LPCWSTR id, ULONG flags)
{ (void)cls; (void)id; (void)flags; return list_size(len); }
CFGMGR32 CONFIGRET WINAPI CM_Get_Device_Interface_ListW(LPGUID cls, LPCWSTR id, WCHAR *buf, ULONG len, ULONG flags)
{ (void)cls; return CM_Get_Device_ID_ListW(id, buf, len, flags); }
CFGMGR32 DWORD WINAPI CM_MapCrToWin32Err(CONFIGRET cr, DWORD dflt)
{
    switch (cr) {
    case CR_SUCCESS:         return ERROR_SUCCESS;
    case CR_INVALID_POINTER: return 1784;          /* ERROR_INVALID_USER_BUFFER */
    case CR_BUFFER_SMALL:    return ERROR_INSUFFICIENT_BUFFER;
    case CR_NO_SUCH_DEVNODE:
    case CR_NO_SUCH_VALUE:   return ERROR_NOT_FOUND;
    default:                 return dflt;
    }
}

/* Device arrival/removal notifications: with no device tree nothing ever
 * arrives, so a registration is a token that is never called back */
static LONG g_notify_tokens;
CFGMGR32 CONFIGRET WINAPI CM_Register_Notification(PVOID filter, PVOID ctx, PVOID callback, HANDLE *out)
{
    (void)filter; (void)ctx;
    if (!callback || !out) return CR_INVALID_POINTER;
    *out = (HANDLE)(ULONG_PTR)(0x10000 + 4 * (ULONG)InterlockedIncrement(&g_notify_tokens));
    return CR_SUCCESS;
}
CFGMGR32 CONFIGRET WINAPI CM_Unregister_Notification(HANDLE h) { (void)h; return CR_SUCCESS; }
