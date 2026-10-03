/*
 * d3d11.dll — Direct3D 11, NovaOS's own front.
 *
 * NovaOS has no display adapter driver of its own.  When DXVK is installed
 * from the App Store (as d3d11_dxvk.dll beside this one), every entry point
 * hands the call to DXVK, so Direct3D 11 runs on Mesa's lavapipe; without
 * it, device creation fails the way Windows does when no adapter supports
 * Direct3D 11, and programs fall back to their software paths.
 */
#include <windows.h>

#define D3D11API __declspec(dllexport)
#define DXGI_ERROR_UNSUPPORTED_ ((HRESULT)0x887A0004L)

static FARPROC dxvk(const char *fn)
{
    static HMODULE m;
    static LONG tried;
    if (!InterlockedExchange(&tried, 1)) m = LoadLibraryW(L"d3d11_dxvk.dll");
    return m ? GetProcAddress(m, fn) : NULL;
}

typedef HRESULT (WINAPI *CreateDevice_t)(void *, UINT, HMODULE, UINT, const UINT *, UINT, UINT, void **, UINT *, void **);
typedef HRESULT (WINAPI *CreateDeviceAndSwapChain_t)(void *, UINT, HMODULE, UINT, const UINT *, UINT, UINT, const void *,
                                                     void **, void **, UINT *, void **);
typedef HRESULT (WINAPI *CoreCreateDevice_t)(void *, void *, UINT, const UINT *, UINT, void **);
typedef HRESULT (WINAPI *On12CreateDevice_t)(void *, UINT, const UINT *, UINT, void *const *, UINT, UINT, void **, void **, UINT *);

static void none(void **a, void **b, UINT *level)
{
    if (a) *a = NULL;
    if (b) *b = NULL;
    if (level) *level = 0;
}

D3D11API HRESULT WINAPI D3D11CreateDevice(void *adapter, UINT type, HMODULE sw, UINT flags, const UINT *levels, UINT n,
                                          UINT sdk, void **device, UINT *level, void **context)
{
    CreateDevice_t f = (CreateDevice_t)dxvk("D3D11CreateDevice");
    if (f) return f(adapter, type, sw, flags, levels, n, sdk, device, level, context);
    none(device, context, level);
    return DXGI_ERROR_UNSUPPORTED_;
}

D3D11API HRESULT WINAPI D3D11CreateDeviceAndSwapChain(void *adapter, UINT type, HMODULE sw, UINT flags, const UINT *levels,
                                                      UINT n, UINT sdk, const void *desc, void **swapchain, void **device,
                                                      UINT *level, void **context)
{
    CreateDeviceAndSwapChain_t f = (CreateDeviceAndSwapChain_t)dxvk("D3D11CreateDeviceAndSwapChain");
    if (f) return f(adapter, type, sw, flags, levels, n, sdk, desc, swapchain, device, level, context);
    if (swapchain) *swapchain = NULL;
    none(device, context, level);
    return DXGI_ERROR_UNSUPPORTED_;
}

/* (DXVK's d3d10core.dll creates its devices through this one) */
D3D11API HRESULT WINAPI D3D11CoreCreateDevice(void *factory, void *adapter, UINT flags, const UINT *levels, UINT n,
                                              void **device)
{
    CoreCreateDevice_t f = (CoreCreateDevice_t)dxvk("D3D11CoreCreateDevice");
    if (f) return f(factory, adapter, flags, levels, n, device);
    none(device, NULL, NULL);
    return DXGI_ERROR_UNSUPPORTED_;
}

D3D11API HRESULT WINAPI D3D11On12CreateDevice(void *d3d12, UINT flags, const UINT *levels, UINT n, void *const *queues,
                                              UINT nq, UINT mask, void **device, void **context, UINT *level)
{
    On12CreateDevice_t f = (On12CreateDevice_t)dxvk("D3D11On12CreateDevice");
    if (f) return f(d3d12, flags, levels, n, queues, nq, mask, device, context, level);
    none(device, context, level);
    return DXGI_ERROR_UNSUPPORTED_;
}
