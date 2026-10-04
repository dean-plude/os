/*
 * setupapi.dll — device installation and enumeration (SetupDi*).  NovaOS
 * has no Plug and Play device tree (see cfgmgr32); the devices it lists
 * are its HID devices, the game controllers (novapad.h): under the HID
 * device interface class (GUID_DEVINTERFACE_HID, what HidD_GetHidGuid
 * returns) each has the path CreateFile opens for hid.dll, and under the
 * HIDClass setup class a device with its instance ID and properties.
 * Every other set is empty: enumeration ends at once, with
 * ERROR_NO_MORE_ITEMS, as on a machine without such devices.
 */
#include <windows.h>
#include <novapad.h>
#include <string.h>

#define SETUPAPI __declspec(dllexport)
#define ERROR_NO_MORE_ITEMS_ 259
#define ERROR_INVALID_DATA_  13
#define ERROR_INVALID_USER_BUFFER 1784
#define DIGCF_PRESENT          0x02
#define DIGCF_ALLCLASSES       0x04
#define DIGCF_DEVICEINTERFACE  0x10
#define SPINT_ACTIVE           0x01
#define SET_MAGIC              0x53455431u      /* "SET1" */

static const GUID g_hid_iface = { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
static const GUID g_hid_class = { 0x745a17a0, 0x74d3, 0x11d0, { 0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda } };

typedef struct { DWORD cbSize; GUID InterfaceClassGuid; DWORD Flags; ULONG_PTR Reserved; } IfaceData;
typedef struct { DWORD cbSize; GUID ClassGuid; DWORD DevInst; ULONG_PTR Reserved; } DevData;
typedef struct { DWORD cbSize; WCHAR DevicePath[1]; } IfaceDetailW;
typedef struct { DWORD cbSize; CHAR DevicePath[1]; } IfaceDetailA;

/* A device information set: the controllers present when it was made */
typedef struct {
    DWORD magic;
    int n;
    NovaPadInfo pad[NOVA_PAD_SLOTS];
} Set;

static HANDLE new_set(BOOL hid)
{
    Set *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Set));
    if (!s) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    s->magic = SET_MAGIC;
    for (int slot = 0; hid && slot < NOVA_PAD_SLOTS; slot++)
        if (nova_pad_info(slot, &s->pad[s->n])) s->n++;
    return s;
}

static Set *set_of(HANDLE h)
{
    Set *s = h;
    if (!s || h == INVALID_HANDLE_VALUE || s->magic != SET_MAGIC) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    return s;
}

/* Whether a set asked for with class @cls and @flags holds the HID devices */
static BOOL wants_hid(const GUID *cls, DWORD flags)
{
    if (flags & DIGCF_ALLCLASSES) return TRUE;
    if (!cls) return FALSE;
    return (flags & DIGCF_DEVICEINTERFACE) ? !memcmp(cls, &g_hid_iface, sizeof(GUID)) : !memcmp(cls, &g_hid_class, sizeof(GUID));
}

SETUPAPI HANDLE WINAPI SetupDiGetClassDevsW(const GUID *cls, LPCWSTR enumerator, HWND parent, DWORD flags)
{ (void)enumerator; (void)parent; return new_set(wants_hid(cls, flags)); }
SETUPAPI HANDLE WINAPI SetupDiGetClassDevsA(const GUID *cls, LPCSTR enumerator, HWND parent, DWORD flags)
{ (void)enumerator; (void)parent; return new_set(wants_hid(cls, flags)); }
SETUPAPI HANDLE WINAPI SetupDiGetClassDevsExW(const GUID *cls, LPCWSTR enumerator, HWND parent, DWORD flags, HANDLE set, LPCWSTR machine, PVOID r)
{ (void)enumerator; (void)parent; (void)set; (void)machine; (void)r; return new_set(wants_hid(cls, flags)); }
SETUPAPI HANDLE WINAPI SetupDiCreateDeviceInfoList(const GUID *cls, HWND parent) { (void)cls; (void)parent; return new_set(FALSE); }
SETUPAPI BOOL WINAPI SetupDiDestroyDeviceInfoList(HANDLE set)
{
    Set *s = set_of(set);
    if (!s) return FALSE;
    s->magic = 0;
    HeapFree(GetProcessHeap(), 0, s);
    return TRUE;
}

static BOOL no_more(void) { SetLastError(ERROR_NO_MORE_ITEMS_); return FALSE; }

static void dev_data(int i, DevData *d)
{
    d->ClassGuid = g_hid_class;
    d->DevInst = 0x100 + (DWORD)i;
    d->Reserved = (ULONG_PTR)i + 1;
}

SETUPAPI BOOL WINAPI SetupDiEnumDeviceInfo(HANDLE set, DWORD i, PVOID info)
{
    Set *s = set_of(set);
    if (!s) return FALSE;
    DevData *d = info;
    if (!d || d->cbSize != sizeof(DevData)) { SetLastError(ERROR_INVALID_USER_BUFFER); return FALSE; }
    if (i >= (DWORD)s->n) return no_more();
    dev_data((int)i, d);
    return TRUE;
}

SETUPAPI BOOL WINAPI SetupDiEnumDeviceInterfaces(HANDLE set, PVOID dev, const GUID *cls, DWORD i, PVOID info)
{
    Set *s = set_of(set);
    if (!s) return FALSE;
    IfaceData *d = info;
    if (!d || d->cbSize != sizeof(IfaceData)) { SetLastError(ERROR_INVALID_USER_BUFFER); return FALSE; }
    if (!cls || !!memcmp(cls, &g_hid_iface, sizeof(GUID))) return no_more();
    int first = 0, last = s->n;                       /* one device's: its single interface */
    if (dev) { first = (int)((DevData *)dev)->Reserved - 1; last = first + 1; }
    if (first < 0 || (DWORD)first + i >= (DWORD)last) return no_more();
    d->InterfaceClassGuid = g_hid_iface;
    d->Flags = SPINT_ACTIVE;
    d->Reserved = (ULONG_PTR)first + i + 1;
    return TRUE;
}

/* The set's device @info names (its index), or -1 */
static int dev_index(Set *s, const void *info, BOOL iface)
{
    ULONG_PTR r = info ? iface ? ((const IfaceData *)info)->Reserved : ((const DevData *)info)->Reserved : 0;
    if (!r || r > (ULONG_PTR)s->n) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    return (int)r - 1;
}

static BOOL iface_detail(HANDLE set, PVOID iface, PVOID detail, DWORD n, PDWORD need, PVOID dev, BOOL wide)
{
    Set *s = set_of(set);
    if (!s) return FALSE;
    int i = dev_index(s, iface, TRUE);
    if (i < 0) return FALSE;
    WCHAR path[96];
    DWORD len = (DWORD)nova_pad_path(&s->pad[i], path, TRUE) + 1;
    DWORD head = (DWORD)__builtin_offsetof(IfaceDetailW, DevicePath);
    DWORD size = head + len * (wide ? sizeof(WCHAR) : 1);
    if (need) *need = size;
    if (dev) {
        if (((DevData *)dev)->cbSize != sizeof(DevData)) { SetLastError(ERROR_INVALID_USER_BUFFER); return FALSE; }
        dev_data(i, dev);
    }
    if (!detail) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    DWORD want = wide ? sizeof(IfaceDetailW) : sizeof(IfaceDetailA);
    if (((IfaceDetailW *)detail)->cbSize != want) { SetLastError(ERROR_INVALID_USER_BUFFER); return FALSE; }
    if (n < size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (DWORD k = 0; k < len; k++) {
        if (wide) ((IfaceDetailW *)detail)->DevicePath[k] = path[k];
        else ((IfaceDetailA *)detail)->DevicePath[k] = (CHAR)path[k];
    }
    return TRUE;
}

SETUPAPI BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(HANDLE set, PVOID iface, PVOID detail, DWORD n, PDWORD need, PVOID dev)
{ return iface_detail(set, iface, detail, n, need, dev, TRUE); }
SETUPAPI BOOL WINAPI SetupDiGetDeviceInterfaceDetailA(HANDLE set, PVOID iface, PVOID detail, DWORD n, PDWORD need, PVOID dev)
{ return iface_detail(set, iface, detail, n, need, dev, FALSE); }

/* A HID device's instance ID: "HID\VID_vvvv&PID_pppp[&IG_00]\8&SERIAL&0&0000" */
static int instance_id(const NovaPadInfo *p, WCHAR *out)
{
    WCHAR path[96];
    nova_pad_path(p, path, FALSE);                    /* \\?\HID#...#8&...&0000#{guid} */
    int n = 0;
    for (const WCHAR *c = path + 4; *c && *c != L'{'; c++) out[n++] = *c == L'#' ? L'\\' : *c;
    if (n && out[n - 1] == L'\\') n--;
    out[n] = 0;
    return n;
}

/* A device registry property as a string (@multi: a list, NUL after each) */
static BOOL put_prop(const WCHAR *v, BOOL multi, PDWORD type, PBYTE buf, DWORD n, PDWORD need, BOOL wide)
{
    DWORD chars = (DWORD)lstrlenW(v) + 1 + (multi ? 1 : 0);
    DWORD size = chars * (wide ? sizeof(WCHAR) : 1);
    if (type) *type = multi ? REG_MULTI_SZ : REG_SZ;
    if (need) *need = size;
    if (!buf || n < size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (DWORD k = 0; k < chars; k++) {
        WCHAR c = k < chars - (multi ? 1 : 0) ? v[k] : 0;
        if (wide) ((WCHAR *)buf)[k] = c;
        else buf[k] = (BYTE)c;
    }
    return TRUE;
}

static BOOL registry_property(HANDLE set, PVOID dev, DWORD prop, PDWORD type, PBYTE buf, DWORD n, PDWORD need, BOOL wide)
{
    Set *s = set_of(set);
    if (!s) return FALSE;
    int i = dev_index(s, dev, FALSE);
    if (i < 0) return FALSE;
    WCHAR v[128];
    switch (prop) {
    case 0x00:                                        /* SPDRP_DEVICEDESC */
        return put_prop(L"HID-compliant game controller", FALSE, type, buf, n, need, wide);
    case 0x01: {                                      /* SPDRP_HARDWAREID */
        int len = instance_id(&s->pad[i], v);
        while (len && v[len] != L'\\') len--;
        v[len] = 0;
        return put_prop(v, TRUE, type, buf, n, need, wide);
    }
    case 0x07: return put_prop(L"HIDClass", FALSE, type, buf, n, need, wide);           /* SPDRP_CLASS */
    case 0x08: return put_prop(L"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}", FALSE, type, buf, n, need, wide);   /* SPDRP_CLASSGUID */
    case 0x09:                                        /* SPDRP_DRIVER */
        lstrcpyW(v, L"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}\\0000");
        v[lstrlenW(v) - 1] = (WCHAR)(L'0' + i);
        return put_prop(v, FALSE, type, buf, n, need, wide);
    case 0x0B: return put_prop(L"(Standard system devices)", FALSE, type, buf, n, need, wide);   /* SPDRP_MFG */
    case 0x0E:                                        /* SPDRP_PHYSICAL_DEVICE_OBJECT_NAME */
        lstrcpyW(v, L"\\Device\\00000100");
        v[lstrlenW(v) - 1] = (WCHAR)(L'0' + i);
        return put_prop(v, FALSE, type, buf, n, need, wide);
    }
    SetLastError(ERROR_INVALID_DATA_);
    return FALSE;
}

SETUPAPI BOOL WINAPI SetupDiGetDeviceRegistryPropertyW(HANDLE set, PVOID dev, DWORD prop, PDWORD type, PBYTE buf, DWORD n, PDWORD need)
{ return registry_property(set, dev, prop, type, buf, n, need, TRUE); }
SETUPAPI BOOL WINAPI SetupDiGetDeviceRegistryPropertyA(HANDLE set, PVOID dev, DWORD prop, PDWORD type, PBYTE buf, DWORD n, PDWORD need)
{ return registry_property(set, dev, prop, type, buf, n, need, FALSE); }

/* Unified device properties (DEVPKEY_*): none kept */
SETUPAPI BOOL WINAPI SetupDiGetDevicePropertyW(HANDLE set, PVOID dev, const void *key, PULONG type, PBYTE buf, DWORD n, PDWORD need, DWORD flags)
{
    (void)key; (void)buf; (void)n; (void)flags;
    Set *s = set_of(set);
    if (!s || dev_index(s, dev, FALSE) < 0) return FALSE;
    if (type) *type = 0;
    if (need) *need = 0;
    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

static BOOL instance_id_of(HANDLE set, PVOID dev, void *id, DWORD n, PDWORD need, BOOL wide)
{
    Set *s = set_of(set);
    if (!s) return FALSE;
    int i = dev_index(s, dev, FALSE);
    if (i < 0) return FALSE;
    WCHAR v[128];
    DWORD len = (DWORD)instance_id(&s->pad[i], v) + 1;
    if (need) *need = len;
    if (!id || n < len) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (DWORD k = 0; k < len; k++) {
        if (wide) ((WCHAR *)id)[k] = v[k];
        else ((CHAR *)id)[k] = (CHAR)v[k];
    }
    return TRUE;
}

SETUPAPI BOOL WINAPI SetupDiGetDeviceInstanceIdW(HANDLE set, PVOID dev, LPWSTR id, DWORD n, PDWORD need)
{ return instance_id_of(set, dev, id, n, need, TRUE); }
SETUPAPI BOOL WINAPI SetupDiGetDeviceInstanceIdA(HANDLE set, PVOID dev, LPSTR id, DWORD n, PDWORD need)
{ return instance_id_of(set, dev, id, n, need, FALSE); }

/* No device has registry keys of its own */
SETUPAPI HKEY WINAPI SetupDiOpenDevRegKey(HANDLE set, PVOID dev, DWORD scope, DWORD profile, DWORD type, REGSAM sam)
{
    (void)set; (void)dev; (void)scope; (void)profile; (void)type; (void)sam;
    SetLastError(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
}

static BOOL no_device(void) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

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

SETUPAPI HKEY WINAPI SetupDiOpenDeviceInterfaceRegKey(HANDLE set, PVOID iface, DWORD reserved, REGSAM sam)
{
    (void)set; (void)iface; (void)reserved; (void)sam;
    SetLastError(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
}
SETUPAPI BOOL WINAPI SetupDiGetDeviceInterfaceAlias(HANDLE set, PVOID iface, const GUID *cls, PVOID alias)
{ (void)set; (void)iface; (void)cls; (void)alias; return no_device(); }

/* setupapi.dll carries Configuration Manager calls too (as cfgmgr32.dll's):
 * with no device tree, no device node is found */
#define CR_NO_SUCH_DEVNODE_ 0x0D
SETUPAPI DWORD WINAPI CM_Locate_DevNodeW(PDWORD dn, LPCWSTR id, ULONG flags)
{ (void)id; (void)flags; if (dn) *dn = 0; return CR_NO_SUCH_DEVNODE_; }

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
typedef DWORD (WINAPI *cm_parent_fn)(PDWORD, DWORD, ULONG);
typedef DWORD (WINAPI *cm_id_fn)(DWORD, void *, ULONG, ULONG);
SETUPAPI DWORD WINAPI CM_Get_Parent(PDWORD parent, DWORD dn, ULONG flags)
{
    cm_parent_fn f = (cm_parent_fn)cfgmgr("CM_Get_Parent");
    return f ? f(parent, dn, flags) : 0x0D /* CR_NO_SUCH_DEVNODE */;
}
SETUPAPI DWORD WINAPI CM_Get_Device_IDA(DWORD dn, char *buf, ULONG len, ULONG flags)
{
    cm_id_fn f = (cm_id_fn)cfgmgr("CM_Get_Device_IDA");
    return f ? f(dn, buf, len, flags) : 0x0D /* CR_NO_SUCH_DEVNODE */;
}
SETUPAPI DWORD WINAPI CM_Get_Device_IDW(DWORD dn, WCHAR *buf, ULONG len, ULONG flags)
{
    cm_id_fn f = (cm_id_fn)cfgmgr("CM_Get_Device_IDW");
    return f ? f(dn, buf, len, flags) : 0x0D /* CR_NO_SUCH_DEVNODE */;
}
