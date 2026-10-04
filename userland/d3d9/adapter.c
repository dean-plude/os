/*
 * adapter.c — d3d9's own IDirect3D9(Ex) when DXVK is not installed.
 *
 * On Windows, Direct3DCreate9 always gives an object: even with no display
 * driver, the "Microsoft Basic Display Adapter" is an adapter, and its
 * identifier carries the display card's PCI vendor and device IDs.
 * Programs read it to decide how to draw: Qt (and so GOG GALAXY's client)
 * looks the IDs up in its GPU blocklist, where vendor 0, device 0 is
 * "Standard VGA" with OpenGL turned off, so with no object Qt never tries
 * the system's OpenGL even when Mesa 3D is installed.
 *
 * So NovaOS's object lists one adapter per monitor, as Windows does, each
 * named by its display card (NtNovaGuiCtl CTL_ADAPTER: the PCI display
 * controllers, in bus order) and reporting the monitor's display modes.
 * There is no Direct3D 9 driver behind it: the device checks, the caps and
 * CreateDevice answer D3DERR_NOTAVAILABLE, so programs take their software
 * paths as they did when Direct3DCreate9 returned NULL.
 */
#include <windows.h>

#define D3DERR_NOTAVAILABLE_  ((HRESULT)0x8876086AL)
#define D3DERR_INVALIDCALL_   ((HRESULT)0x8876086CL)
#define D3DFMT_X8R8G8B8_      22
#define D3DSCANLINEORDERING_PROGRESSIVE_ 1
#define D3DDISPLAYROTATION_IDENTITY_     1
#define CTL_ADAPTER           37

typedef LONG_PTR (WINAPI *NtNovaGuiCtl_t)(INT_PTR h, ULONG op, ULONG_PTR arg, void *ptr);

typedef struct {
    char Driver[512], Description[512], DeviceName[32];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
} D3DADAPTER_IDENTIFIER9;
typedef struct { UINT Width, Height, RefreshRate; DWORD Format; } D3DDISPLAYMODE;
typedef struct { UINT Size, Width, Height, RefreshRate; DWORD Format, ScanLineOrdering; } D3DDISPLAYMODEEX;
typedef struct { UINT Size; DWORD Format, ScanLineOrdering; } D3DDISPLAYMODEFILTER;

typedef struct D3D D3D;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(D3D *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(D3D *);
    ULONG   (WINAPI *Release)(D3D *);
    HRESULT (WINAPI *RegisterSoftwareDevice)(D3D *, void *);
    UINT    (WINAPI *GetAdapterCount)(D3D *);
    HRESULT (WINAPI *GetAdapterIdentifier)(D3D *, UINT, DWORD, D3DADAPTER_IDENTIFIER9 *);
    UINT    (WINAPI *GetAdapterModeCount)(D3D *, UINT, DWORD);
    HRESULT (WINAPI *EnumAdapterModes)(D3D *, UINT, DWORD, UINT, D3DDISPLAYMODE *);
    HRESULT (WINAPI *GetAdapterDisplayMode)(D3D *, UINT, D3DDISPLAYMODE *);
    HRESULT (WINAPI *CheckDeviceType)(D3D *, UINT, DWORD, DWORD, DWORD, BOOL);
    HRESULT (WINAPI *CheckDeviceFormat)(D3D *, UINT, DWORD, DWORD, DWORD, DWORD, DWORD);
    HRESULT (WINAPI *CheckDeviceMultiSampleType)(D3D *, UINT, DWORD, DWORD, BOOL, DWORD, DWORD *);
    HRESULT (WINAPI *CheckDepthStencilMatch)(D3D *, UINT, DWORD, DWORD, DWORD, DWORD);
    HRESULT (WINAPI *CheckDeviceFormatConversion)(D3D *, UINT, DWORD, DWORD, DWORD);
    HRESULT (WINAPI *GetDeviceCaps)(D3D *, UINT, DWORD, void *);
    HMONITOR (WINAPI *GetAdapterMonitor)(D3D *, UINT);
    HRESULT (WINAPI *CreateDevice)(D3D *, UINT, DWORD, HWND, DWORD, void *, void **);
    /* IDirect3D9Ex */
    UINT    (WINAPI *GetAdapterModeCountEx)(D3D *, UINT, const D3DDISPLAYMODEFILTER *);
    HRESULT (WINAPI *EnumAdapterModesEx)(D3D *, UINT, const D3DDISPLAYMODEFILTER *, UINT, D3DDISPLAYMODEEX *);
    HRESULT (WINAPI *GetAdapterDisplayModeEx)(D3D *, UINT, D3DDISPLAYMODEEX *, DWORD *);
    HRESULT (WINAPI *CreateDeviceEx)(D3D *, UINT, DWORD, HWND, DWORD, void *, D3DDISPLAYMODEEX *, void **);
    HRESULT (WINAPI *GetAdapterLUID)(D3D *, UINT, LUID *);
} D3DVtbl;
struct D3D { const D3DVtbl *vtbl; LONG refs; BOOL ex; };

static const GUID IID_IUnknown_     = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IDirect3D9_   = { 0x81bdcbca, 0x64d4, 0x426d, { 0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c } };
static const GUID IID_IDirect3D9Ex_ = { 0x02177241, 0x69fc, 0x400c, { 0x8f, 0xf1, 0x93, 0xa4, 0x4d, 0xf6, 0x86, 0x1d } };

static int same(const GUID *a, const GUID *b) { return !__builtin_memcmp(a, b, sizeof(GUID)); }

/* ---- the monitors and their display cards ---------------------------------- */
#define MAX_ADAPTERS 16

static BOOL CALLBACK add_monitor(HMONITOR m, HDC dc, RECT *r, LPARAM lp)
{
    (void)dc; (void)r;
    HMONITOR *list = (HMONITOR *)lp;
    for (int i = 0; i < MAX_ADAPTERS; i++)
        if (!list[i]) { list[i] = m; return TRUE; }
    return FALSE;
}

/* The monitors, in EnumDisplayMonitors' order (DISPLAY1 first); returns how many */
static UINT monitors(HMONITOR list[MAX_ADAPTERS])
{
    UINT n = 0;
    ZeroMemory(list, MAX_ADAPTERS * sizeof(HMONITOR));
    EnumDisplayMonitors(NULL, NULL, add_monitor, (LPARAM)list);
    while (n < MAX_ADAPTERS && list[n]) n++;
    return n ? n : 1;                               /* (always the primary) */
}

/* Adapter @i's display card { vendor, device, subsystem, revision, class }:
 * the i-th PCI display controller, or the first for a monitor past them */
static BOOL card(UINT i, UINT32 out[5])
{
    NtNovaGuiCtl_t ctl = (NtNovaGuiCtl_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtNovaGuiCtl");
    ZeroMemory(out, 5 * sizeof(UINT32));
    if (!ctl) return FALSE;
    return ctl(0, CTL_ADAPTER, i, out) || ctl(0, CTL_ADAPTER, 0, out);
}

static BOOL monitor_name(UINT i, WCHAR name[32])
{
    HMONITOR list[MAX_ADAPTERS];
    MONITORINFOEXW mi;
    if (i >= monitors(list)) return FALSE;
    mi.cbSize = sizeof(mi);
    if (!list[i] || !GetMonitorInfoW(list[i], (MONITORINFO *)&mi)) {
        lstrcpyW(name, L"\\\\.\\DISPLAY1");
        return TRUE;
    }
    lstrcpynW(name, mi.szDevice, 32);
    return TRUE;
}

/* Mode @n of adapter @i (ENUM_CURRENT_SETTINGS: the current one); every
 * NovaOS mode is 32 bits per pixel, D3DFMT_X8R8G8B8 */
static BOOL mode(UINT i, DWORD n, D3DDISPLAYMODE *m)
{
    WCHAR name[32];
    DEVMODEW dm;
    if (!monitor_name(i, name)) return FALSE;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(name, n, &dm)) return FALSE;
    m->Width = dm.dmPelsWidth;
    m->Height = dm.dmPelsHeight;
    m->RefreshRate = dm.dmDisplayFrequency;
    m->Format = D3DFMT_X8R8G8B8_;
    return TRUE;
}

/* ---- IDirect3D9 ------------------------------------------------------------ */
static ULONG WINAPI d_addref(D3D *d) { return (ULONG)InterlockedIncrement(&d->refs); }
static ULONG WINAPI d_release(D3D *d)
{
    LONG r = InterlockedDecrement(&d->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, d);
    return (ULONG)r;
}

static HRESULT WINAPI d_qi(D3D *d, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (same(iid, &IID_IUnknown_) || same(iid, &IID_IDirect3D9_) || (d->ex && same(iid, &IID_IDirect3D9Ex_))) {
        d_addref(d);
        *out = d;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI d_register(D3D *d, void *init) { (void)d; (void)init; return D3DERR_NOTAVAILABLE_; }

static UINT WINAPI d_count(D3D *d)
{
    HMONITOR list[MAX_ADAPTERS];
    (void)d;
    return monitors(list);
}

static HRESULT WINAPI d_ident(D3D *d, UINT i, DWORD flags, D3DADAPTER_IDENTIFIER9 *id)
{
    WCHAR name[32];
    DISPLAY_DEVICEW dd;
    UINT32 c[5];
    (void)flags;
    if (!id || i >= d_count(d) || !monitor_name(i, name)) return D3DERR_INVALIDCALL_;
    ZeroMemory(id, sizeof(*id));
    dd.cb = sizeof(dd);
    if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) lstrcpyW(dd.DeviceString, L"NovaOS Display Adapter");
    WideCharToMultiByte(CP_ACP, 0, dd.DeviceString, -1, id->Description, sizeof(id->Description), NULL, NULL);
    lstrcpyA(id->Driver, "NovaOS");                 /* (no user-mode Direct3D driver) */
    WideCharToMultiByte(CP_ACP, 0, name, -1, id->DeviceName, sizeof(id->DeviceName), NULL, NULL);
    id->DriverVersion.HighPart = 10 << 16;          /* 10.0.0.0 */
    card(i, c);
    id->VendorId = c[0];
    id->DeviceId = c[1];
    id->SubSysId = c[2];
    id->Revision = c[3];
    /* the same card, the same identifier (Windows': per driver and card) */
    id->DeviceIdentifier.Data1 = 0x4e6f7661;        /* "Nova" */
    id->DeviceIdentifier.Data2 = (WORD)c[0];
    id->DeviceIdentifier.Data3 = (WORD)c[1];
    for (int k = 0; k < 4; k++) id->DeviceIdentifier.Data4[k] = (BYTE)(c[2] >> (8 * k));
    id->DeviceIdentifier.Data4[4] = (BYTE)c[3];
    id->WHQLLevel = 0;                              /* not certified */
    return S_OK;
}

static UINT WINAPI d_modecount(D3D *d, UINT i, DWORD fmt)
{
    D3DDISPLAYMODE m;
    UINT n = 0;
    if (i >= d_count(d) || fmt != D3DFMT_X8R8G8B8_) return 0;
    while (mode(i, n, &m)) n++;
    return n;
}

static HRESULT WINAPI d_enummodes(D3D *d, UINT i, DWORD fmt, UINT n, D3DDISPLAYMODE *m)
{
    if (!m || i >= d_count(d)) return D3DERR_INVALIDCALL_;
    if (fmt != D3DFMT_X8R8G8B8_) return D3DERR_NOTAVAILABLE_;
    return mode(i, n, m) ? S_OK : D3DERR_INVALIDCALL_;
}

static HRESULT WINAPI d_curmode(D3D *d, UINT i, D3DDISPLAYMODE *m)
{
    if (!m || i >= d_count(d)) return D3DERR_INVALIDCALL_;
    return mode(i, ENUM_CURRENT_SETTINGS, m) ? S_OK : D3DERR_INVALIDCALL_;
}

static HRESULT WINAPI d_checktype(D3D *d, UINT i, DWORD t, DWORD f, DWORD b, BOOL w)
{ (void)t; (void)f; (void)b; (void)w; return i < d_count(d) ? D3DERR_NOTAVAILABLE_ : D3DERR_INVALIDCALL_; }
static HRESULT WINAPI d_checkformat(D3D *d, UINT i, DWORD t, DWORD af, DWORD u, DWORD r, DWORD f)
{ (void)t; (void)af; (void)u; (void)r; (void)f; return i < d_count(d) ? D3DERR_NOTAVAILABLE_ : D3DERR_INVALIDCALL_; }
static HRESULT WINAPI d_checkms(D3D *d, UINT i, DWORD t, DWORD f, BOOL w, DWORD ms, DWORD *q)
{ (void)t; (void)f; (void)w; (void)ms; if (q) *q = 0; return i < d_count(d) ? D3DERR_NOTAVAILABLE_ : D3DERR_INVALIDCALL_; }
static HRESULT WINAPI d_checkds(D3D *d, UINT i, DWORD t, DWORD af, DWORD rf, DWORD dsf)
{ (void)t; (void)af; (void)rf; (void)dsf; return i < d_count(d) ? D3DERR_NOTAVAILABLE_ : D3DERR_INVALIDCALL_; }
static HRESULT WINAPI d_checkconv(D3D *d, UINT i, DWORD t, DWORD s, DWORD f)
{ (void)t; (void)s; (void)f; return i < d_count(d) ? D3DERR_NOTAVAILABLE_ : D3DERR_INVALIDCALL_; }
static HRESULT WINAPI d_caps(D3D *d, UINT i, DWORD t, void *caps)
{ (void)t; (void)caps; return i < d_count(d) ? D3DERR_NOTAVAILABLE_ : D3DERR_INVALIDCALL_; }

static HMONITOR WINAPI d_monitor(D3D *d, UINT i)
{
    HMONITOR list[MAX_ADAPTERS];
    (void)d;
    if (i >= monitors(list)) return NULL;
    if (list[i]) return list[i];
    POINT p = { 0, 0 };
    return MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
}

static HRESULT WINAPI d_create(D3D *d, UINT i, DWORD t, HWND w, DWORD fl, void *pp, void **out)
{
    (void)t; (void)w; (void)fl; (void)pp;
    if (out) *out = NULL;
    return i < d_count(d) && out ? D3DERR_NOTAVAILABLE_ : D3DERR_INVALIDCALL_;
}

/* ---- IDirect3D9Ex ---------------------------------------------------------- */
static BOOL filter_ok(const D3DDISPLAYMODEFILTER *f)
{
    return !f || f->Format == D3DFMT_X8R8G8B8_;
}

static UINT WINAPI d_modecountex(D3D *d, UINT i, const D3DDISPLAYMODEFILTER *f)
{
    return filter_ok(f) ? d_modecount(d, i, D3DFMT_X8R8G8B8_) : 0;
}

static void to_ex(const D3DDISPLAYMODE *m, D3DDISPLAYMODEEX *x)
{
    x->Size = sizeof(*x);
    x->Width = m->Width;
    x->Height = m->Height;
    x->RefreshRate = m->RefreshRate;
    x->Format = m->Format;
    x->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE_;
}

static HRESULT WINAPI d_enummodesex(D3D *d, UINT i, const D3DDISPLAYMODEFILTER *f, UINT n, D3DDISPLAYMODEEX *x)
{
    D3DDISPLAYMODE m;
    if (!x || i >= d_count(d)) return D3DERR_INVALIDCALL_;
    if (!filter_ok(f)) return D3DERR_NOTAVAILABLE_;
    if (!mode(i, n, &m)) return D3DERR_INVALIDCALL_;
    to_ex(&m, x);
    return S_OK;
}

static HRESULT WINAPI d_curmodeex(D3D *d, UINT i, D3DDISPLAYMODEEX *x, DWORD *rot)
{
    D3DDISPLAYMODE m;
    if (i >= d_count(d)) return D3DERR_INVALIDCALL_;
    if (x) {
        if (!mode(i, ENUM_CURRENT_SETTINGS, &m)) return D3DERR_INVALIDCALL_;
        to_ex(&m, x);
    }
    if (rot) *rot = D3DDISPLAYROTATION_IDENTITY_;
    return S_OK;
}

static HRESULT WINAPI d_createex(D3D *d, UINT i, DWORD t, HWND w, DWORD fl, void *pp, D3DDISPLAYMODEEX *fs, void **out)
{
    (void)fs;
    return d_create(d, i, t, w, fl, pp, out);
}

static HRESULT WINAPI d_luid(D3D *d, UINT i, LUID *l)
{
    if (!l || i >= d_count(d)) return D3DERR_INVALIDCALL_;
    l->LowPart = 0x4e6f7600 | i;                    /* one per adapter, the same in every process */
    l->HighPart = 0;
    return S_OK;
}

static const D3DVtbl g_vtbl = {
    d_qi, d_addref, d_release, d_register, d_count, d_ident, d_modecount, d_enummodes, d_curmode,
    d_checktype, d_checkformat, d_checkms, d_checkds, d_checkconv, d_caps, d_monitor, d_create,
    d_modecountex, d_enummodesex, d_curmodeex, d_createex, d_luid,
};

/* NovaOS's IDirect3D9 (@ex: IDirect3D9Ex too) */
void *nova_d3d9(BOOL ex)
{
    D3D *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) return NULL;
    d->vtbl = &g_vtbl;
    d->refs = 1;
    d->ex = ex;
    return d;
}
