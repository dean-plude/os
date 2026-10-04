/* d3d9test.exe — NovaOS's own d3d9.dll, as programs that import it find it
 * (Qt WebEngine's, and so GOG GALAXY's, among them): it is the system
 * folder's, it has every export Windows' d3d9.dll has, and
 *   d3d9test none   (no DXVK): Direct3DCreate9 and Direct3DCreate9Ex give
 *                   NovaOS's object, which lists an adapter per monitor as
 *                   Windows' basic display adapter does: named by the
 *                   display card's PCI IDs (QEMU's: vendor 0x1234, so Qt's
 *                   GPU blocklist does not take it for "Standard VGA" and
 *                   turn OpenGL off), with the monitor and its modes, but no
 *                   device (D3DERR_NOTAVAILABLE); the PIX and debug exports
 *                   answer as they do with no tool attached;
 *   d3d9test dxvk   (DXVK from the App Store, as d3d9_dxvk.dll): the calls
 *                   reach DXVK, whose IDirect3D9 lists an adapter. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define D3D_SDK_VERSION_ 32
#define D3DERR_NOTAVAILABLE_ ((HRESULT)0x8876086AL)

#define D3DERR_INVALIDCALL_ ((HRESULT)0x8876086CL)
#define D3DFMT_X8R8G8B8_ 22
#define D3DDEVTYPE_HAL_ 1
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING_ 0x20

typedef struct {
    char Driver[512], Description[512], DeviceName[32];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
} Ident;
typedef struct { UINT Width, Height, RefreshRate; DWORD Format; } Mode;

/* IDirect3D9 (and IDirect3D9Ex's GetAdapterLUID) */
typedef struct D3D9 { struct {
    HRESULT (WINAPI *QueryInterface)(struct D3D9 *, const GUID *, void **);
    ULONG (WINAPI *AddRef)(struct D3D9 *);
    ULONG (WINAPI *Release)(struct D3D9 *);
    void *reg;
    UINT (WINAPI *GetAdapterCount)(struct D3D9 *);
    HRESULT (WINAPI *GetAdapterIdentifier)(struct D3D9 *, UINT, DWORD, Ident *);
    UINT (WINAPI *GetAdapterModeCount)(struct D3D9 *, UINT, DWORD);
    HRESULT (WINAPI *EnumAdapterModes)(struct D3D9 *, UINT, DWORD, UINT, Mode *);
    HRESULT (WINAPI *GetAdapterDisplayMode)(struct D3D9 *, UINT, Mode *);
    HRESULT (WINAPI *CheckDeviceType)(struct D3D9 *, UINT, DWORD, DWORD, DWORD, BOOL);
    void *check[4];
    HRESULT (WINAPI *GetDeviceCaps)(struct D3D9 *, UINT, DWORD, void *);
    HMONITOR (WINAPI *GetAdapterMonitor)(struct D3D9 *, UINT);
    HRESULT (WINAPI *CreateDevice)(struct D3D9 *, UINT, DWORD, HWND, DWORD, void *, void **);
    void *ex[4];
    HRESULT (WINAPI *GetAdapterLUID)(struct D3D9 *, UINT, LUID *);
} *vtbl; } D3D9;

static const GUID IID_IDirect3D9Ex_ = { 0x02177241, 0x69fc, 0x400c, { 0x8f, 0xf1, 0x93, 0xa4, 0x4d, 0xf6, 0x86, 0x1d } };

/* NovaOS's own object, as Qt's GPU detection reads it */
static void own_object(D3D9 *d, D3D9 *ex)
{
    UINT n = d->vtbl->GetAdapterCount(d);
    printf("adapters %u\n", n);
    CHECK("an adapter per monitor", n == (UINT)GetSystemMetrics(80 /* SM_CMONITORS */));
    Ident id;
    memset(&id, 0xCC, sizeof(id));
    HRESULT hr = d->vtbl->GetAdapterIdentifier(d, 0, 0, &id);
    CHECK("GetAdapterIdentifier: S_OK", hr == S_OK);
    printf("adapter 0: '%s' (%s), %s, %04lx:%04lx subsystem %08lx rev %lu\n", id.Description, id.Driver,
           id.DeviceName, id.VendorId, id.DeviceId, id.SubSysId, id.Revision);
    CHECK("the display card's vendor (not 0: Qt's \"Standard VGA\")", id.VendorId != 0 && id.VendorId != 0xFFFF);
    CHECK("the display card's device", id.DeviceId != 0);
    CHECK("a description", id.Description[0] && id.Description[0] != (char)0xCC);
    CHECK("the device name \\\\.\\DISPLAY1", !strcmp(id.DeviceName, "\\\\.\\DISPLAY1"));
    CHECK("no adapter past the monitors", d->vtbl->GetAdapterIdentifier(d, n, 0, &id) == D3DERR_INVALIDCALL_);
    Mode cur = { 0 }, m = { 0 };
    CHECK("GetAdapterDisplayMode", d->vtbl->GetAdapterDisplayMode(d, 0, &cur) == S_OK);
    printf("mode %ux%u %u Hz, format %lu\n", cur.Width, cur.Height, cur.RefreshRate, cur.Format);
    DEVMODEA dm = { .dmSize = sizeof(dm) };
    CHECK("the display's mode", EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) &&
                                cur.Width == dm.dmPelsWidth && cur.Height == dm.dmPelsHeight);
    CHECK("32 bits per pixel: X8R8G8B8", cur.Format == D3DFMT_X8R8G8B8_);
    UINT modes = d->vtbl->GetAdapterModeCount(d, 0, D3DFMT_X8R8G8B8_);
    printf("modes %u\n", modes);
    CHECK("display modes", modes >= 1);
    CHECK("EnumAdapterModes", modes && d->vtbl->EnumAdapterModes(d, 0, D3DFMT_X8R8G8B8_, 0, &m) == S_OK && m.Width && m.Height);
    CHECK("no 16-bit modes", d->vtbl->GetAdapterModeCount(d, 0, 23 /* R5G6B5 */) == 0);
    CHECK("the monitor", d->vtbl->GetAdapterMonitor(d, 0) == MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    CHECK("no Direct3D 9 driver: CheckDeviceType", d->vtbl->CheckDeviceType(d, 0, D3DDEVTYPE_HAL_, D3DFMT_X8R8G8B8_,
                                                                           D3DFMT_X8R8G8B8_, TRUE) == D3DERR_NOTAVAILABLE_);
    unsigned char caps[512];
    CHECK("no Direct3D 9 driver: GetDeviceCaps", d->vtbl->GetDeviceCaps(d, 0, D3DDEVTYPE_HAL_, caps) == D3DERR_NOTAVAILABLE_);
    void *dev = (void *)1;
    unsigned char pp[64] = { 0 };
    CHECK("no Direct3D 9 driver: CreateDevice", d->vtbl->CreateDevice(d, 0, D3DDEVTYPE_HAL_, GetDesktopWindow(),
                                                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING_, pp, &dev) ==
                                                 D3DERR_NOTAVAILABLE_ && dev == NULL);
    void *q = NULL;
    CHECK("IDirect3D9 is not IDirect3D9Ex", d->vtbl->QueryInterface(d, &IID_IDirect3D9Ex_, &q) == E_NOINTERFACE && !q);
    CHECK("IDirect3D9Ex", ex->vtbl->QueryInterface(ex, &IID_IDirect3D9Ex_, &q) == S_OK && q == ex);
    if (q) ex->vtbl->Release(ex);
    LUID l0 = { 0 }, l1 = { 0 };
    CHECK("GetAdapterLUID", ex->vtbl->GetAdapterLUID(ex, 0, &l0) == S_OK && ex->vtbl->GetAdapterLUID(ex, 0, &l1) == S_OK &&
                            l0.LowPart == l1.LowPart && (l0.LowPart || l0.HighPart));
}

static const char *const exports[] = {
    "Direct3DCreate9", "Direct3DCreate9Ex", "Direct3DCreate9On12", "Direct3DCreate9On12Ex",
    "Direct3DShaderValidatorCreate9", "Direct3D9EnableMaximizedWindowedModeShim",
    "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_GetStatus", "D3DPERF_QueryRepeatFrame",
    "D3DPERF_SetMarker", "D3DPERF_SetOptions", "D3DPERF_SetRegion",
    "DebugSetLevel", "DebugSetMute", "PSGPError", "PSGPSampleTexture",
};

static BOOL in_module(void *p, HMODULE m)
{
    HMODULE of = NULL;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCSTR)p, &of) && of == m;
}

int main(int argc, char **argv)
{
    BOOL want_dxvk = argc > 1 && !strcmp(argv[1], "dxvk");
    if (argc < 2 || (!want_dxvk && strcmp(argv[1], "none"))) {
        printf("usage: d3d9test none|dxvk\n");
        return 2;
    }

    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    CHECK("d3d9.dll loads", d3d9 != NULL);
    if (!d3d9) { printf("d3d9test: %d passed, %d failed\n", pass, fail); return 1; }

    char path[MAX_PATH], sys[MAX_PATH];
    GetModuleFileNameA(d3d9, path, MAX_PATH);
    UINT n = GetSystemDirectoryA(sys, MAX_PATH);    /* SysWOW64 in a 32-bit process */
    lstrcatA(sys, "\\d3d9.dll");
    printf("d3d9.dll is %s\n", path);
    CHECK("d3d9.dll is the system folder's", n && !lstrcmpiA(path, sys));

    for (size_t i = 0; i < sizeof(exports) / sizeof(*exports); i++) {
        if (!GetProcAddress(d3d9, exports[i])) printf("missing export %s\n", exports[i]);
        CHECK(exports[i], GetProcAddress(d3d9, exports[i]) != NULL);
    }

    D3D9 *(WINAPI *create)(UINT) = (D3D9 *(WINAPI *)(UINT))GetProcAddress(d3d9, "Direct3DCreate9");
    HRESULT (WINAPI *create_ex)(UINT, D3D9 **) = (HRESULT (WINAPI *)(UINT, D3D9 **))GetProcAddress(d3d9, "Direct3DCreate9Ex");
    D3D9 *(WINAPI *create_12)(UINT, void *, UINT) = (D3D9 *(WINAPI *)(UINT, void *, UINT))GetProcAddress(d3d9, "Direct3DCreate9On12");
    void *(WINAPI *validator)(void) = (void *(WINAPI *)(void))GetProcAddress(d3d9, "Direct3DShaderValidatorCreate9");
    int (WINAPI *begin)(DWORD, const WCHAR *) = (int (WINAPI *)(DWORD, const WCHAR *))GetProcAddress(d3d9, "D3DPERF_BeginEvent");
    int (WINAPI *end)(void) = (int (WINAPI *)(void))GetProcAddress(d3d9, "D3DPERF_EndEvent");
    DWORD (WINAPI *status)(void) = (DWORD (WINAPI *)(void))GetProcAddress(d3d9, "D3DPERF_GetStatus");
    BOOL (WINAPI *repeat)(void) = (BOOL (WINAPI *)(void))GetProcAddress(d3d9, "D3DPERF_QueryRepeatFrame");
    void (WINAPI *marker)(DWORD, const WCHAR *) = (void (WINAPI *)(DWORD, const WCHAR *))GetProcAddress(d3d9, "D3DPERF_SetMarker");
    void (WINAPI *shim)(UINT) = (void (WINAPI *)(UINT))GetProcAddress(d3d9, "Direct3D9EnableMaximizedWindowedModeShim");
    if (fail) { printf("d3d9test: %d passed, %d failed\n", pass, fail); return 1; }

    /* the markers and switches answer the same either way: no profiler attached */
    CHECK("D3DPERF_GetStatus: no profiler", status() == 0);
    CHECK("D3DPERF_QueryRepeatFrame: no", repeat() == FALSE);
    marker(0xFFFF0000, L"d3d9test");
    shim(1);
    int b0 = begin(0xFF00FF00, L"outer"), b1 = begin(0xFF0000FF, L"inner");
    int e1 = end(), e0 = end();
    printf("D3DPERF events %d %d %d %d\n", b0, b1, e1, e0);
    if (!want_dxvk) {
        CHECK("D3DPERF_BeginEvent counts the open events", b0 == 0 && b1 == 1);
        CHECK("D3DPERF_EndEvent counts down", e1 == 1 && e0 == 0);
        CHECK("D3DPERF_EndEvent with none open", end() == -1);
    }

    HMODULE dxvk = GetModuleHandleA("d3d9_dxvk.dll");
    D3D9 *d = create(D3D_SDK_VERSION_);
    D3D9 *ex = (D3D9 *)(ULONG_PTR)1;
    HRESULT hr = create_ex(D3D_SDK_VERSION_, &ex);
    printf("Direct3DCreate9 %p, Direct3DCreate9Ex %08lx %p\n", (void *)d, (unsigned long)hr, (void *)ex);
    if (!want_dxvk) {
        CHECK("no DXVK loaded", dxvk == NULL && GetModuleHandleA("d3d9_dxvk.dll") == NULL);
        CHECK("Direct3DCreate9: NovaOS's object", d != NULL && in_module(d->vtbl->GetAdapterCount, d3d9));
        CHECK("Direct3DCreate9Ex: S_OK", hr == S_OK && ex != NULL && ex != (D3D9 *)(ULONG_PTR)1);
        if (d && hr == S_OK && ex) own_object(d, ex);
        if (d) CHECK("Release", d->vtbl->Release(d) == 0);
        if (hr == S_OK && ex) CHECK("Release (Ex)", ex->vtbl->Release(ex) == 0);
        CHECK("Direct3DCreate9On12: NULL", create_12(D3D_SDK_VERSION_, NULL, 0) == NULL);
        CHECK("Direct3DShaderValidatorCreate9: NULL", validator() == NULL);
    } else {
        dxvk = GetModuleHandleA("d3d9_dxvk.dll");
        CHECK("DXVK's d3d9 (d3d9_dxvk.dll) loaded", dxvk != NULL);
        CHECK("Direct3DCreate9: an object", d != NULL);
        CHECK("Direct3DCreate9Ex: S_OK", hr == S_OK && ex != NULL);
        if (d) {
            CHECK("the object is DXVK's", in_module(d->vtbl->GetAdapterCount, dxvk));
            UINT adapters = d->vtbl->GetAdapterCount(d);
            printf("adapters %u\n", adapters);
            CHECK("an adapter", adapters >= 1);
            d->vtbl->Release(d);
        }
        if (hr == S_OK && ex) ex->vtbl->Release(ex);
    }

    FreeLibrary(d3d9);
    printf("d3d9test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
