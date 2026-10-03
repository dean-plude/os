/*
 * d3d11.dll — Direct3D 11, NovaOS's own front for DXVK's.
 *
 * Programs often import d3d11.dll whether or not they draw with it (Qt's
 * GUI library does, so KeePassXC needs it to start).  NovaOS has no
 * Direct3D of its own, so without DXVK every entry point answers
 * DXGI_ERROR_UNSUPPORTED, as Windows does on a machine with no Direct3D 11
 * device, and the program takes its other drawing path.  When the App
 * Store's DXVK is installed (its d3d11.dll as d3d11_dxvk.dll beside this
 * one), the calls go to DXVK.
 */
#include <windows.h>

#define D3D11API __declspec(dllexport)
#define DXGI_ERROR_UNSUPPORTED_ ((HRESULT)0x887A0004L)

static HMODULE dxvk(void)
{
    static HMODULE m;
    static LONG looked;
    if (!InterlockedCompareExchange(&looked, 1, 0)) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 20);
        if (n && n < MAX_PATH - 20) {
            lstrcatA(path, "\\d3d11_dxvk.dll");
            m = LoadLibraryA(path);
        }
        InterlockedExchange(&looked, 2);
    }
    while (looked == 1) Sleep(0);
    return m;
}

static FARPROC dxvk_proc(const char *name)
{
    HMODULE m = dxvk();
    return m ? GetProcAddress(m, name) : NULL;
}

/* the out-pointers a failed call must clear */
static void clear(void **a, void **b, void **c)
{
    if (a) *a = NULL;
    if (b) *b = NULL;
    if (c) *c = NULL;
}

typedef HRESULT (WINAPI *CreateDeviceFn)(void *, UINT, HMODULE, UINT, const UINT *, UINT, UINT, void **, UINT *,
                                         void **);
D3D11API HRESULT WINAPI D3D11CreateDevice(void *adapter, UINT type, HMODULE sw, UINT flags, const UINT *levels,
                                          UINT nlevels, UINT sdk, void **device, UINT *level, void **context)
{
    CreateDeviceFn fn = (CreateDeviceFn)dxvk_proc("D3D11CreateDevice");
    if (fn) return fn(adapter, type, sw, flags, levels, nlevels, sdk, device, level, context);
    clear(device, context, NULL);
    if (level) *level = 0;
    return DXGI_ERROR_UNSUPPORTED_;
}

typedef HRESULT (WINAPI *CreateDeviceSwapFn)(void *, UINT, HMODULE, UINT, const UINT *, UINT, UINT, const void *,
                                             void **, void **, UINT *, void **);
D3D11API HRESULT WINAPI D3D11CreateDeviceAndSwapChain(void *adapter, UINT type, HMODULE sw, UINT flags,
                                                      const UINT *levels, UINT nlevels, UINT sdk, const void *desc,
                                                      void **swap, void **device, UINT *level, void **context)
{
    CreateDeviceSwapFn fn = (CreateDeviceSwapFn)dxvk_proc("D3D11CreateDeviceAndSwapChain");
    if (fn) return fn(adapter, type, sw, flags, levels, nlevels, sdk, desc, swap, device, level, context);
    clear(swap, device, context);
    if (level) *level = 0;
    return DXGI_ERROR_UNSUPPORTED_;
}

/* (d3d10core.dll's way in) */
typedef HRESULT (WINAPI *CoreCreateDeviceFn)(void *, void *, UINT, const UINT *, UINT, void **);
D3D11API HRESULT WINAPI D3D11CoreCreateDevice(void *factory, void *adapter, UINT flags, const UINT *levels,
                                              UINT nlevels, void **device)
{
    CoreCreateDeviceFn fn = (CoreCreateDeviceFn)dxvk_proc("D3D11CoreCreateDevice");
    if (fn) return fn(factory, adapter, flags, levels, nlevels, device);
    clear(device, NULL, NULL);
    return DXGI_ERROR_UNSUPPORTED_;
}

typedef HRESULT (WINAPI *On12CreateDeviceFn)(void *, UINT, const UINT *, UINT, void *const *, UINT, UINT, void **,
                                             void **, UINT *);
D3D11API HRESULT WINAPI D3D11On12CreateDevice(void *d3d12, UINT flags, const UINT *levels, UINT nlevels,
                                              void *const *queues, UINT nqueues, UINT mask, void **device,
                                              void **context, UINT *level)
{
    On12CreateDeviceFn fn = (On12CreateDeviceFn)dxvk_proc("D3D11On12CreateDevice");
    if (fn) return fn(d3d12, flags, levels, nlevels, queues, nqueues, mask, device, context, level);
    clear(device, context, NULL);
    if (level) *level = 0;
    return DXGI_ERROR_UNSUPPORTED_;
}
