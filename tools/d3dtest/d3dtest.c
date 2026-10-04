/* d3dtest — Direct3D 9 and 11 through d3d9.dll, d3d11.dll and dxgi.dll
 * (DXVK on Mesa's lavapipe Vulkan on NovaOS): devices, clears, a
 * fixed-function Direct3D 9 triangle, render-target read-back, and a few
 * seconds of presented frames from each.
 * Build: x86_64-w64-mingw32-gcc -O2 -o d3dtest.exe d3dtest.c -ld3d9 -ld3d11 -ldxgi -luser32 -lgdi32 -lole32
 *        (i686-w64-mingw32-gcc for the 32-bit one).  Usage: d3dtest [seconds [9|11]], d3dtest angle,
 *        d3dtest fps [seconds], d3dtest shared
 *
 * d3dtest fps [seconds]: the frame-rate test.  A Direct3D 9 scene that
 * keeps the rasterizer busy (64 blended full-window quads at 640x480) runs
 * once on Mesa's Venus (Vulkan on the host's GPU through a virtio-gpu) and
 * once on lavapipe (Vulkan on NovaOS's CPU), each in a child process whose
 * VK_DRIVER_FILES names the driver; Venus must draw more frames per second.
 *
 * d3dtest angle: Direct3D 11 brought up the way ANGLE's D3D11 back end
 * (Chromium's GPU process, so Steam's browser, WebView2 and Qt WebEngine)
 * does it: the screen's DC as the EGL display, the first adapter that is not
 * Microsoft's, a device on it from the feature levels ANGLE asks for, the
 * DXGI 1.2 device, the adapter's description, factory and driver version,
 * the feature and format queries, a DXGI 1.2 swap chain on a window, and a
 * WARP device whose adapter comes from the device.
 *
 * d3dtest shared: a Direct3D 11 texture shared by NT handle, as Chromium
 * shares its frames with Qt WebEngine (Galaxy) or with its browser process:
 * device A makes it (D3D11_RESOURCE_MISC_SHARED_NTHANDLE) and clears it,
 * device B opens the handle (OpenSharedResource1) and reads A's colour,
 * B's write is read back by A, a child process opens a copy of the
 * handle and reads it too, and a shared fence (ID3D11Fence, which Chromium
 * signals for Qt) signalled on one device completes on the other.  On Mesa's lavapipe (VK_DRIVER_FILES), where
 * NovaOS's Vulkan loader provides the sharing. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, fail;
static void check(const char *what, int ok) { if (ok) pass++; else { fail++; printf("FAIL %s\n", what); } }

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
}

static int px_near(DWORD p, int r, int g, int b)        /* p: 0xAARRGGBB */
{
    return abs((int)(p >> 16 & 0xFF) - r) < 8 && abs((int)(p >> 8 & 0xFF) - g) < 8 && abs((int)(p & 0xFF) - b) < 8;
}

static HWND make_window(const char *title)
{
    RECT r = { 0, 0, 320, 240 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowA("d3dtest", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 120,
                         r.right - r.left, r.bottom - r.top, 0, 0, GetModuleHandleA(NULL), 0);
}

/* ---- Direct3D 9 ---------------------------------------------------------- */
struct vtx { float x, y, z, rhw; DWORD c; };

static void test_d3d9(int secs)
{
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    check("Direct3DCreate9", d3d != NULL);
    if (!d3d) return;
    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id)))
        printf("D3D9 adapter  %s (%s)\n", id.Description, id.Driver);
    check("D3D9 adapters", IDirect3D9_GetAdapterCount(d3d) >= 1);

    HWND w = make_window("Direct3D 9 test");
    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 320;
    pp.BackBufferHeight = 240;
    pp.hDeviceWindow = w;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    check("D3D9 CreateDevice", SUCCEEDED(hr) && dev);
    if (!dev) { printf("CreateDevice: 0x%08lx\n", (unsigned long)hr); IDirect3D9_Release(d3d); DestroyWindow(w); return; }

    struct vtx tri[3] = {
        { 160,  30, 0.5f, 1, 0xFFFF0000 },
        { 290, 210, 0.5f, 1, 0xFF00FF00 },
        {  30, 210, 0.5f, 1, 0xFF0000FF },
    };
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    /* one frame, read back through an offscreen plain surface */
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0x20, 0x40, 0x80), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
    check("D3D9 DrawPrimitiveUP", SUCCEEDED(hr));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 320, 240, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    hr = bb && sys ? IDirect3DDevice9_GetRenderTargetData(dev, bb, sys) : E_FAIL;
    check("D3D9 GetRenderTargetData", SUCCEEDED(hr));
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        DWORD corner = *(DWORD *)((BYTE *)lr.pBits + 5 * lr.Pitch + 5 * 4);
        DWORD mid = *(DWORD *)((BYTE *)lr.pBits + 150 * lr.Pitch + 160 * 4);
        check("D3D9 clear colour", px_near(corner, 0x20, 0x40, 0x80));
        check("D3D9 triangle drawn", !px_near(mid, 0x20, 0x40, 0x80) && (mid & 0xFFFFFF) != 0);
        printf("D3D9 pixels  corner %06lx  centre %06lx\n", (unsigned long)(corner & 0xFFFFFF), (unsigned long)(mid & 0xFFFFFF));
        IDirect3DSurface9_UnlockRect(sys);
    }
    if (sys) IDirect3DSurface9_Release(sys);
    if (bb) IDirect3DSurface9_Release(bb);
    check("D3D9 Present", SUCCEEDED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL)));

    /* animated frames */
    DWORD t0 = GetTickCount(), frames = 0;
    while (GetTickCount() - t0 < (DWORD)secs * 1000) {
        pump();
        float a = (GetTickCount() - t0) / 1000.0f;
        DWORD c = D3DCOLOR_XRGB((int)(64 + 63 * (1 + __builtin_sinf(a))), 0x40, 0x80);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, c, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
        IDirect3DDevice9_EndScene(dev);
        if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { check("D3D9 Present loop", 0); break; }
        frames++;
    }
    if (secs) printf("D3D9 %lu frames in %lu ms\n", (unsigned long)frames, (unsigned long)(GetTickCount() - t0));
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(w);
}

/* ---- Direct3D 11 --------------------------------------------------------- */
static void test_d3d11(int secs)
{
    IDXGIFactory1 *fac = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac);
    check("CreateDXGIFactory1", SUCCEEDED(hr) && fac);
    if (fac) {
        IDXGIAdapter1 *ad = NULL;
        if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(fac, 0, &ad))) {
            DXGI_ADAPTER_DESC1 d;
            IDXGIAdapter1_GetDesc1(ad, &d);
            printf("DXGI adapter  %ls, %lu MB\n", d.Description, (unsigned long)(d.DedicatedVideoMemory >> 20));
            check("DXGI adapter", 1);
            IDXGIAdapter1_Release(ad);
        } else check("DXGI adapter", 0);
        IDXGIFactory1_Release(fac);
    }

    HWND w = make_window("Direct3D 11 test");
    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferDesc.Width = 320;
    sd.BufferDesc.Height = 240;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = w;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl = 0;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                       &sd, &sc, &dev, &fl, &ctx);
    check("D3D11CreateDeviceAndSwapChain", SUCCEEDED(hr) && dev && sc);
    if (FAILED(hr) || !dev || !sc) { printf("D3D11CreateDeviceAndSwapChain: 0x%08lx\n", (unsigned long)hr); DestroyWindow(w); return; }
    printf("D3D11 feature level %x.%x\n", fl >> 12, (fl >> 8) & 0xF);
    check("D3D11 feature level >= 10.0", fl >= D3D_FEATURE_LEVEL_10_0);

    ID3D11Texture2D *back = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&back);
    ID3D11RenderTargetView *rtv = NULL;
    hr = back ? ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv) : E_FAIL;
    check("D3D11 CreateRenderTargetView", SUCCEEDED(hr));

    /* clear, then copy to a staging texture and read it back */
    const float col[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    if (rtv) ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = 320; td.Height = 240; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stage = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &stage);
    check("D3D11 staging texture", stage != NULL);
    if (stage && back) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stage, (ID3D11Resource *)back);
        D3D11_MAPPED_SUBRESOURCE m;
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m);
        check("D3D11 Map", SUCCEEDED(hr));
        if (SUCCEEDED(hr)) {
            BYTE *p = (BYTE *)m.pData + 100 * m.RowPitch + 100 * 4;           /* R, G, B, A */
            printf("D3D11 pixel  %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
            check("D3D11 clear colour", abs(p[0] - 0x40) < 4 && abs(p[1] - 0x80) < 4 && abs(p[2] - 0xBF) < 4);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stage, 0);
        }
    }
    check("D3D11 Present", SUCCEEDED(IDXGISwapChain_Present(sc, 0, 0)));

    DWORD t0 = GetTickCount(), frames = 0;
    while (GetTickCount() - t0 < (DWORD)secs * 1000) {
        pump();
        float a = (GetTickCount() - t0) / 1000.0f;
        float c[4] = { 0.5f + 0.5f * __builtin_sinf(a), 0.3f, 0.5f + 0.5f * __builtin_cosf(a), 1.0f };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
        if (FAILED(IDXGISwapChain_Present(sc, 0, 0))) { check("D3D11 Present loop", 0); break; }
        frames++;
    }
    if (secs) printf("D3D11 %lu frames in %lu ms\n", (unsigned long)frames, (unsigned long)(GetTickCount() - t0));

    if (stage) ID3D11Texture2D_Release(stage);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (back) ID3D11Texture2D_Release(back);
    IDXGISwapChain_Release(sc);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    DestroyWindow(w);
}

/* ---- Direct3D 11 as ANGLE brings it up ------------------------------------ */
typedef HRESULT (WINAPI *CreateDevice_t)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                         UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/* (Renderer11::callD3D11CreateDevice: on E_INVALIDARG, again without 11_1) */
static HRESULT angle_create(CreateDevice_t create, IDXGIAdapter *ad, D3D_DRIVER_TYPE type, ID3D11Device **dev,
                            D3D_FEATURE_LEVEL *fl, ID3D11DeviceContext **ctx)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3 };
    HRESULT hr = create(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : type, NULL, 0, levels, 5, D3D11_SDK_VERSION, dev, fl, ctx);
    if (hr == E_INVALIDARG) hr = create(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : type, NULL, 0, levels + 1, 4, D3D11_SDK_VERSION,
                                        dev, fl, ctx);
    return hr;
}

static void angle_formats(ID3D11Device *dev)
{
    static const DXGI_FORMAT fmts[] = { DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM,
                                        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT };
    UINT ok = 0;
    for (int i = 0; i < 6; i++) {
        UINT sup = 0, q = 0;
        if (SUCCEEDED(ID3D11Device_CheckFormatSupport(dev, fmts[i], &sup)) && sup) {
            ok++;
            ID3D11Device_CheckMultisampleQualityLevels(dev, fmts[i], 4, &q);
        }
    }
    check("CheckFormatSupport answers RGBA8, BGRA8 and D24S8", ok >= 3);
    D3D11_FEATURE_DATA_D3D11_OPTIONS o = { 0 };
    check("CheckFeatureSupport D3D11_OPTIONS",
          SUCCEEDED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o))));
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2 = { 0 };
    check("CheckFeatureSupport D3D11_OPTIONS2",
          SUCCEEDED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2))));
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 f2 = { DXGI_FORMAT_R8G8B8A8_UNORM, 0 };
    check("CheckFeatureSupport FORMAT_SUPPORT2",
          SUCCEEDED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_FORMAT_SUPPORT2, &f2, sizeof(f2))));
}

static void test_angle(void)
{
    /* the EGL display: Chromium passes GetDC(NULL), which ANGLE takes only
     * when WindowFromDC finds its window */
    HDC screen = GetDC(NULL);
    HWND desk = WindowFromDC(screen);
    check("WindowFromDC(GetDC(NULL)) is the desktop window", desk && desk == GetDesktopWindow());
    ReleaseDC(NULL, screen);

    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    CreateDevice_t create = d3d11 ? (CreateDevice_t)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;
    check("d3d11.dll exports D3D11CreateDevice", create != NULL);
    if (!create) return;

    /* Renderer11::initializeDXGIAdapter: the first adapter that isn't
     * Microsoft's (WARP), else the first */
    IDXGIFactory1 *fac = NULL;
    check("CreateDXGIFactory1", SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac)) && fac);
    if (!fac) return;
    IDXGIAdapter *ad = NULL, *t = NULL;
    for (UINT i = 0; !ad && SUCCEEDED(IDXGIFactory1_EnumAdapters(fac, i, &t)); i++) {
        DXGI_ADAPTER_DESC d;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(t, &d)) && d.VendorId != 0x1414) ad = t;
        else IDXGIAdapter_Release(t);
    }
    if (!ad) IDXGIFactory1_EnumAdapters(fac, 0, &ad);
    check("DXGI adapter for ANGLE", ad != NULL);
    IDXGIFactory1_Release(fac);
    if (!ad) return;

    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl = 0;
    HRESULT hr = angle_create(create, ad, D3D_DRIVER_TYPE_HARDWARE, &dev, &fl, &ctx);
    check("D3D11CreateDevice on the adapter", SUCCEEDED(hr) && dev && ctx);
    if (!dev || !ctx) { printf("D3D11CreateDevice: 0x%08lx\n", (unsigned long)hr); IDXGIAdapter_Release(ad); return; }
    printf("ANGLE feature level %x.%x\n", fl >> 12, (fl >> 8) & 0xF);
    check("feature level >= 10.0", fl >= D3D_FEATURE_LEVEL_10_0);

    /* Renderer11::initialize: DXGI 1.2 (for HWNDs of other processes), the
     * 11.1 and 11.3 contexts, the adapter's description and factory */
    IDXGIDevice2 *dxdev = NULL;
    check("IDXGIDevice2 from the device", SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice2, (void **)&dxdev)));
    ID3D11DeviceContext1 *ctx1 = NULL;
    check("ID3D11DeviceContext1", SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&ctx1)));
    ID3D11DeviceContext3 *ctx3 = NULL;
    ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext3, (void **)&ctx3);
    DXGI_ADAPTER_DESC desc;
    check("adapter GetDesc", SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)));
    printf("ANGLE adapter  %ls (vendor %04x device %04x)\n", desc.Description, desc.VendorId, desc.DeviceId);
    IDXGIFactory *parent = NULL;
    check("adapter GetParent(IDXGIFactory)", SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&parent)) && parent);
    LARGE_INTEGER umd = { 0 };
    check("adapter CheckInterfaceSupport(IDXGIDevice)",
          SUCCEEDED(IDXGIAdapter_CheckInterfaceSupport(ad, &IID_IDXGIDevice, &umd)));
    printf("ANGLE driver version %u.%u.%u.%u\n", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
           LOWORD(umd.LowPart));
    if (ctx3) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3 = { 0 };
        check("CheckFeatureSupport D3D11_OPTIONS3",
              SUCCEEDED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3))));
    }
    angle_formats(dev);
    ID3DUserDefinedAnnotation *ann = NULL;
    if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3DUserDefinedAnnotation, (void **)&ann)))
        ID3DUserDefinedAnnotation_Release(ann);

    /* NativeWindow11Win32::createSwapChain: a DXGI 1.2 swap chain on the
     * window, cleared and presented */
    IDXGIFactory2 *fac2 = NULL;
    check("IDXGIFactory2 from the factory", parent && SUCCEEDED(IDXGIFactory_QueryInterface(parent, &IID_IDXGIFactory2, (void **)&fac2)));
    HWND w = make_window("ANGLE's Direct3D 11");
    if (fac2) {
        DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
        sd.Width = 320; sd.Height = 240; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_BACK_BUFFER;
        sd.BufferCount = 1; sd.Scaling = DXGI_SCALING_STRETCH; sd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
        IDXGISwapChain1 *sc = NULL;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(fac2, (IUnknown *)dev, w, &sd, NULL, NULL, &sc);
        check("CreateSwapChainForHwnd", SUCCEEDED(hr) && sc);
        if (sc) {
            IDXGIFactory2_MakeWindowAssociation(fac2, w, DXGI_MWA_NO_ALT_ENTER);
            ID3D11Texture2D *back = NULL;
            ID3D11RenderTargetView *rtv = NULL;
            IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&back);
            if (back) ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
            check("swap chain render target", rtv != NULL);
            const float col[4] = { 0.1f, 0.6f, 0.3f, 1.0f };
            if (rtv) ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
            check("swap chain Present", SUCCEEDED(IDXGISwapChain1_Present(sc, 0, 0)));
            pump();
            if (rtv) ID3D11RenderTargetView_Release(rtv);
            if (back) ID3D11Texture2D_Release(back);
            IDXGISwapChain1_Release(sc);
        }
        IDXGIFactory2_Release(fac2);
    }
    DestroyWindow(w);
    if (parent) IDXGIFactory_Release(parent);
    if (ctx3) ID3D11DeviceContext3_Release(ctx3);
    if (ctx1) ID3D11DeviceContext1_Release(ctx1);
    if (dxdev) IDXGIDevice2_Release(dxdev);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    IDXGIAdapter_Release(ad);

    /* EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE: no adapter, the WARP
     * driver type, and the adapter from the device
     * (Renderer11::initializeAdapterFromDevice) */
    dev = NULL; ctx = NULL;
    hr = angle_create(create, NULL, D3D_DRIVER_TYPE_WARP, &dev, &fl, &ctx);
    check("D3D11CreateDevice(D3D_DRIVER_TYPE_WARP)", SUCCEEDED(hr) && dev);
    if (dev) {
        IDXGIDevice *dd = NULL;
        IDXGIAdapter *wa = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dd))) {
            IDXGIDevice_GetParent(dd, &IID_IDXGIAdapter, (void **)&wa);
            IDXGIDevice_Release(dd);
        }
        check("WARP device's adapter", wa != NULL);
        if (wa) IDXGIAdapter_Release(wa);
        if (ctx) ID3D11DeviceContext_Release(ctx);
        ID3D11Device_Release(dev);
    }
}

/* ---- the frame-rate test ------------------------------------------------ */
#define FPS_W 640
#define FPS_H 480
#define FPS_QUADS 64

/* The child: draw the scene for @secs seconds; prints "fps-result ADAPTER|FRAMES|MS" */
static int fps_child(int secs)
{
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("FAIL Direct3DCreate9\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id = { 0 };
    IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id);
    RECT r = { 0, 0, FPS_W, FPS_H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND w = CreateWindowA("d3dtest", "Direct3D 9 frame rate", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60,
                           r.right - r.left, r.bottom - r.top, 0, 0, GetModuleHandleA(NULL), 0);
    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = FPS_W;
    pp.BackBufferHeight = FPS_H;
    pp.hDeviceWindow = w;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *dev = NULL;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
        printf("FAIL CreateDevice\n");
        return 1;
    }
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    static struct vtx q[FPS_QUADS * 6];
    for (int i = 0; i < FPS_QUADS; i++) {
        float x0 = (float)(i % 8), y0 = (float)(i / 8 % 8);
        DWORD c = D3DCOLOR_ARGB(0x20, 40 + i * 3, 255 - i * 3, 128 + (i & 7) * 16);
        struct vtx a = { x0, y0, 0.5f, 1, c }, b = { FPS_W - 8 + x0, y0, 0.5f, 1, c },
                   d = { x0, FPS_H - 8 + y0, 0.5f, 1, c }, e = { FPS_W - 8 + x0, FPS_H - 8 + y0, 0.5f, 1, c };
        q[i * 6] = a; q[i * 6 + 1] = b; q[i * 6 + 2] = d;
        q[i * 6 + 3] = b; q[i * 6 + 4] = e; q[i * 6 + 5] = d;
    }
    DWORD t0 = GetTickCount(), frames = 0;
    while (GetTickCount() - t0 < (DWORD)secs * 1000) {
        pump();
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0x10, 0x10, (frames * 4) & 0xFF), 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, FPS_QUADS * 2, q, sizeof(q[0]));
        IDirect3DDevice9_EndScene(dev);
        if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("FAIL Present\n"); return 1; }
        frames++;
    }
    DWORD ms = GetTickCount() - t0;
    printf("fps-result %s|%lu|%lu\n", id.Description, (unsigned long)frames, (unsigned long)ms);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(w);
    return 0;
}

/* Run the child on the driver whose manifest is @manifest; its frames per
 * second (or -1), the adapter it reported in @adapter */
static double fps_run(const char *manifest, int secs, char *adapter, int cap)
{
    char exe[MAX_PATH], cmd[2 * MAX_PATH], out[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    GetTempPathA(sizeof(out), out);
    lstrcatA(out, "d3dtest-fps.txt");
    DeleteFileA(out);
    snprintf(cmd, sizeof(cmd), "\"%s\" fpsrun %d \"%s\"", exe, secs, out);
    SetEnvironmentVariableA("VK_DRIVER_FILES", manifest);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, (DWORD)(secs + 600) * 1000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    SetEnvironmentVariableA("VK_DRIVER_FILES", NULL);
    char line[512] = "";
    FILE *f = fopen(out, "r");
    if (!f) return -1;
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    char *bar = strchr(line, '|');
    if (strncmp(line, "fps-result ", 11) || !bar) return -1;
    *bar = 0;
    snprintf(adapter, cap, "%s", line + 11);
    unsigned long frames = 0, ms = 0;
    if (sscanf(bar + 1, "%lu|%lu", &frames, &ms) != 2 || !ms) return -1;
    return frames * 1000.0 / ms;
}

static int fps_test(int secs)
{
    char sys[MAX_PATH], venus[MAX_PATH], lvp[MAX_PATH], a_venus[256] = "", a_lvp[256] = "";
    GetSystemDirectoryA(sys, sizeof(sys));      /* (SysWOW64 for the 32-bit one) */
    const char *bits = sizeof(void *) == 8 ? "x86_64" : "x86";
    snprintf(venus, sizeof(venus), "%s\\virtio_icd.%s.json", sys, bits);
    snprintf(lvp, sizeof(lvp), "%s\\lvp_icd.%s.json", sys, bits);
    printf("fps: Venus for %d s\n", secs);
    double v = fps_run(venus, secs, a_venus, sizeof(a_venus));
    printf("Venus     %.2f frames/s  (%s)\n", v, a_venus);
    printf("fps: lavapipe for %d s\n", secs);
    double l = fps_run(lvp, secs, a_lvp, sizeof(a_lvp));
    printf("lavapipe  %.2f frames/s  (%s)\n", l, a_lvp);
    check("Venus ran", v > 0);
    check("Venus is the adapter", strstr(a_venus, "Venus") != NULL);
    check("lavapipe ran", l > 0);
    check("lavapipe is the adapter", strstr(a_lvp, "llvmpipe") != NULL && !strstr(a_lvp, "Venus"));
    check("Venus beats lavapipe", v > 0 && l > 0 && v > l);
    if (v > 0 && l > 0) printf("Venus is %.1fx lavapipe\n", v / l);
    printf("d3dtest fps: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

/* ---- Direct3D 11 textures shared by handle -------------------------------- */
static ID3D11Device *shared_device(ID3D11DeviceContext **ctx)
{
    ID3D11Device *dev = NULL;
    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, fl = 0;
    *ctx = NULL;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1, D3D11_SDK_VERSION, &dev, &fl, ctx)))
        return NULL;
    return dev;
}

/* The texel at (8, 8) of @tex on @dev (0xAARRGGBB; 0 if it could not be read) */
static DWORD shared_texel(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D *stage = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &stage))) return 0;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stage, (ID3D11Resource *)tex);
    D3D11_MAPPED_SUBRESOURCE m;
    DWORD px = 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m))) {
        px = *(DWORD *)((BYTE *)m.pData + 8 * m.RowPitch + 8 * 4);        /* B, G, R, A */
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stage, 0);
    }
    ID3D11Texture2D_Release(stage);
    return px;
}

static void shared_clear(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const float *c)
{
    ID3D11RenderTargetView *rtv = NULL;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv))) return;
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11DeviceContext_Flush(ctx);
}

/* Open @h on a device of its own; the texel, or 0 */
static DWORD shared_open_texel(HANDLE h, D3D11_TEXTURE2D_DESC *desc)
{
    ID3D11DeviceContext *ctx;
    ID3D11Device *dev = shared_device(&ctx);
    ID3D11Device1 *dev1 = NULL;
    ID3D11Texture2D *tex = NULL;
    DWORD px = 0;
    if (dev && SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1)) &&
        SUCCEEDED(ID3D11Device1_OpenSharedResource1(dev1, h, &IID_ID3D11Texture2D, (void **)&tex))) {
        if (desc) ID3D11Texture2D_GetDesc(tex, desc);
        px = shared_texel(dev, ctx, tex);
        ID3D11Texture2D_Release(tex);
    }
    if (dev1) ID3D11Device1_Release(dev1);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    return px;
}

/* The child: sharedrun HANDLE FILE; writes "shared-result AARRGGBB" */
static int shared_child(HANDLE h)
{
    printf("shared-result %08lx\n", (unsigned long)shared_open_texel(h, NULL));
    return 0;
}

static int shared_test(void)
{
    char sys[MAX_PATH], lvp[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof(sys));      /* (SysWOW64 for the 32-bit one) */
    snprintf(lvp, sizeof(lvp), "%s\\lvp_icd.%s.json", sys, sizeof(void *) == 8 ? "x86_64" : "x86");
    SetEnvironmentVariableA("VK_DRIVER_FILES", lvp);

    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device *dev = shared_device(&ctx);
    check("device A", dev != NULL);
    if (!dev) { printf("d3dtest shared: %d passed, %d failed\n", pass, fail); return 1; }

    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
    check("shared texture made", SUCCEEDED(hr) && tex);
    if (!tex) { printf("CreateTexture2D: 0x%08lx\n", (unsigned long)hr); printf("d3dtest shared: %d passed, %d failed\n", pass, fail); return 1; }

    const float orange[4] = { 1.0f, 0.5f, 0.0f, 1.0f }, teal[4] = { 0.0f, 0.5f, 0.5f, 1.0f };
    shared_clear(dev, ctx, tex, orange);
    DWORD a = shared_texel(dev, ctx, tex);
    printf("device A texel  %08lx\n", (unsigned long)a);
    check("device A drew", px_near(a, 0xFF, 0x80, 0x00));

    IDXGIResource1 *res = NULL;
    HANDLE h = NULL;
    hr = ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&res);
    if (SUCCEEDED(hr)) hr = IDXGIResource1_CreateSharedHandle(res, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                              NULL, &h);
    printf("CreateSharedHandle  0x%08lx\n", (unsigned long)hr);
    check("shared handle", SUCCEEDED(hr) && h);
    HANDLE kmt = NULL;
    check("no global handle for an NT-handle texture",
          res && IDXGIResource1_GetSharedHandle(res, &kmt) == E_INVALIDARG);

    D3D11_TEXTURE2D_DESC got = { 0 };
    DWORD b = h ? shared_open_texel(h, &got) : 0;
    printf("device B texel  %08lx (%ux%u, format %u)\n", (unsigned long)b, got.Width, got.Height, got.Format);
    check("device B opened it as A made it", got.Width == 64 && got.Height == 64 && got.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    check("device B reads A's colour", px_near(b, 0xFF, 0x80, 0x00));

    /* B draws, A reads */
    ID3D11DeviceContext *bctx = NULL;
    ID3D11Device *bdev = shared_device(&bctx);
    ID3D11Device1 *b1 = NULL;
    ID3D11Texture2D *btex = NULL;
    if (bdev && h && SUCCEEDED(ID3D11Device_QueryInterface(bdev, &IID_ID3D11Device1, (void **)&b1)))
        ID3D11Device1_OpenSharedResource1(b1, h, &IID_ID3D11Texture2D, (void **)&btex);
    if (btex) {
        shared_clear(bdev, bctx, btex, teal);
        shared_texel(bdev, bctx, btex);                     /* (waits for B's clear) */
    }
    DWORD a2 = shared_texel(dev, ctx, tex);
    printf("device A texel after B  %08lx\n", (unsigned long)a2);
    check("device A reads B's colour", btex && px_near(a2, 0x00, 0x80, 0x80));

    /* A shared fence: A signals, B sees it; B signals, A sees it */
    ID3D11Device5 *a5 = NULL, *b5 = NULL;
    ID3D11DeviceContext4 *actx4 = NULL, *bctx4 = NULL;
    ID3D11Fence *afence = NULL, *bfence = NULL;
    HANDLE fh = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device5, (void **)&a5)) &&
        SUCCEEDED(ID3D11Device5_CreateFence(a5, 1, D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, (void **)&afence)))
        hr = ID3D11Fence_CreateSharedHandle(afence, NULL, GENERIC_ALL, NULL, &fh);
    printf("fence CreateSharedHandle  0x%08lx\n", (unsigned long)hr);
    check("shared fence handle", afence && fh);
    if (bdev && fh && SUCCEEDED(ID3D11Device_QueryInterface(bdev, &IID_ID3D11Device5, (void **)&b5)))
        ID3D11Device5_OpenSharedFence(b5, fh, &IID_ID3D11Fence, (void **)&bfence);
    check("device B opened the fence", bfence != NULL);
    if (afence && bfence &&
        SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext4, (void **)&actx4)) &&
        SUCCEEDED(ID3D11DeviceContext_QueryInterface(bctx, &IID_ID3D11DeviceContext4, (void **)&bctx4))) {
        HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        ID3D11Fence_SetEventOnCompletion(bfence, 5, ev);
        ID3D11DeviceContext4_Signal(actx4, afence, 5);
        ID3D11DeviceContext_Flush(ctx);
        DWORD w = WaitForSingleObject(ev, 20000);
        printf("B's fence after A signalled 5: %llu\n", (unsigned long long)ID3D11Fence_GetCompletedValue(bfence));
        check("device B sees A's signal", w == WAIT_OBJECT_0 && ID3D11Fence_GetCompletedValue(bfence) >= 5);
        ID3D11Fence_SetEventOnCompletion(afence, 7, ev);
        ID3D11DeviceContext4_Signal(bctx4, bfence, 7);
        ID3D11DeviceContext_Flush(bctx);
        w = WaitForSingleObject(ev, 20000);
        printf("A's fence after B signalled 7: %llu\n", (unsigned long long)ID3D11Fence_GetCompletedValue(afence));
        check("device A sees B's signal", w == WAIT_OBJECT_0 && ID3D11Fence_GetCompletedValue(afence) >= 7);
        CloseHandle(ev);
    } else {
        check("device B sees A's signal", 0);
        check("device A sees B's signal", 0);
    }
    if (actx4) ID3D11DeviceContext4_Release(actx4);
    if (bctx4) ID3D11DeviceContext4_Release(bctx4);
    if (bfence) ID3D11Fence_Release(bfence);
    if (afence) ID3D11Fence_Release(afence);
    if (fh) CloseHandle(fh);
    if (b5) ID3D11Device5_Release(b5);
    if (a5) ID3D11Device5_Release(a5);

    /* Another process, through a copy of the handle it inherits */
    char exe[MAX_PATH], cmd[2 * MAX_PATH], out[MAX_PATH], line[256] = "";
    HANDLE dup = NULL;
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    GetTempPathA(sizeof(out), out);
    lstrcatA(out, "d3dtest-shared.txt");
    DeleteFileA(out);
    if (h) DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS);
    snprintf(cmd, sizeof(cmd), "\"%s\" sharedrun %lu \"%s\"", exe, (unsigned long)(ULONG_PTR)dup, out);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (dup && CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 600000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        FILE *f = fopen(out, "r");
        if (f) { if (!fgets(line, sizeof(line), f)) line[0] = 0; fclose(f); }
    }
    unsigned long c = 0;
    int read = sscanf(line, "shared-result %lx", &c) == 1;
    printf("other process texel  %08lx\n", c);
    check("another process reads it", read && px_near((DWORD)c, 0x00, 0x80, 0x80));

    if (dup) CloseHandle(dup);
    if (btex) ID3D11Texture2D_Release(btex);
    if (b1) ID3D11Device1_Release(b1);
    if (bctx) ID3D11DeviceContext_Release(bctx);
    if (bdev) ID3D11Device_Release(bdev);
    if (h) CloseHandle(h);
    if (res) IDXGIResource1_Release(res);
    ID3D11Texture2D_Release(tex);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    printf("d3dtest shared: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "fpsrun")) {      /* the frame-rate test's child: fpsrun SECS FILE */
        setvbuf(stdout, NULL, _IONBF, 0);
        WNDCLASSA wc = { 0 };
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "d3dtest";
        RegisterClassA(&wc);
        if (argc > 3) freopen(argv[3], "w", stdout);
        return fps_child(argc > 2 ? atoi(argv[2]) : 5);
    }
    if (argc > 3 && !strcmp(argv[1], "sharedrun")) {   /* the sharing test's child: sharedrun HANDLE FILE */
        freopen(argv[3], "w", stdout);
        return shared_child((HANDLE)(ULONG_PTR)strtoul(argv[2], NULL, 10));
    }
    if (argc > 1 && !strcmp(argv[1], "shared")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        return shared_test();
    }
    if (argc > 1 && !strcmp(argv[1], "fps")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        return fps_test(argc > 2 ? atoi(argv[2]) : 10);
    }
    int secs = argc > 1 ? atoi(argv[1]) : 3;
    const char *only = argc > 2 ? argv[2] : "";
    setvbuf(stdout, NULL, _IONBF, 0);
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "d3dtest";
    RegisterClassA(&wc);
    if (argc > 1 && !strcmp(argv[1], "angle")) test_angle();
    else {
        if (strcmp(only, "11")) test_d3d9(secs);
        if (strcmp(only, "9")) test_d3d11(secs);
    }
    printf("d3dtest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
