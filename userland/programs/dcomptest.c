/* dcomptest.exe — Direct3D without a GPU, the way Chromium's software
 * compositor (the GPU process of WebView2, Steam's and Galaxy's browsers)
 * draws: dxgi.dll's factory and its one adapter, the Microsoft Basic Render
 * Driver; a WARP device from d3d11.dll; a DirectComposition device, target
 * and visual (dcomp.dll) showing a swap chain made for composition; frames
 * drawn into a mapped staging texture, copied into the swap chain's buffer
 * and presented with dirty rectangles; a swap chain on a window; and the
 * window Chromium composites into, made by the GPU process as a child of a
 * hidden window of its own and parented in the browser's window by the
 * browser process (SetParent across processes).  Without DXVK.
 *
 *   dcomptest            runs the checks
 *   dcomptest gpu HWND   the "GPU process": makes the child window, posts it
 *                        to HWND, and draws into it once it is parented
 *
 * The software device is 64-bit only; the 32-bit run checks the factory,
 * the adapter and the cross-process parenting (drawing with GDI). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <novadx.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); fflush(stdout); } } while (0)

/* method @i of COM object @o, as type @T */
#define VT(o, i, T) ((T)((void **)*(void **)(o))[i])
typedef HRESULT (STDMETHODCALLTYPE *QI_t)(void *, const GUID *, void **);
typedef ULONG   (STDMETHODCALLTYPE *Rel_t)(void *);
#define RELEASE(o) do { if (o) VT(o, 2, Rel_t)(o); (o) = NULL; } while (0)

#define DC_HELLO  (WM_APP + 1)      /* gpu -> host: wParam = its child window */
#define DC_DRAW   (WM_APP + 2)      /* host -> gpu (to the child window): wParam = the parent it should have */
#define DC_RESULT (WM_APP + 3)      /* gpu -> host: wParam = checks passed, lParam = failed ones (bits) */

static const char *g_step = "start";
static HMODULE g_dxgi, g_d3d11, g_dcomp;

static DWORD WINAPI watchdog(void *arg)
{
    (void)arg;
    Sleep(120000);
    printf("FAIL: hung at: %s\n", g_step);
    fflush(stdout);
    ExitProcess(3);
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    } while ((LONG)(end - GetTickCount()) > 0);
}

static COLORREF pixel(HWND h, int x, int y)
{
    HDC dc = GetDC(h);
    COLORREF c = GetPixel(dc, x, y);
    ReleaseDC(h, dc);
    return c;
}

/* ---- Direct3D helpers ----------------------------------------------------- */
typedef HRESULT (WINAPI *CreateFactory_t)(const GUID *, void **);
typedef HRESULT (WINAPI *CreateDevice_t)(void *, UINT, HMODULE, UINT, const UINT *, UINT, UINT, void **, UINT *, void **);
typedef HRESULT (WINAPI *CreateDeviceSC_t)(void *, UINT, HMODULE, UINT, const UINT *, UINT, UINT, const void *, void **,
                                           void **, UINT *, void **);
typedef HRESULT (WINAPI *CreateDComp_t)(void *, const GUID *, void **);
typedef HRESULT (STDMETHODCALLTYPE *Tex2D_t)(void *, const ND3D11_TEXTURE2D_DESC *, const void *, void **);
typedef HRESULT (STDMETHODCALLTYPE *Map_t)(void *, void *, UINT, UINT, UINT, ND3D11_MAPPED_SUBRESOURCE *);
typedef void    (STDMETHODCALLTYPE *Unmap_t)(void *, void *, UINT);
typedef void    (STDMETHODCALLTYPE *CopyRegion_t)(void *, void *, UINT, UINT, UINT, UINT, void *, UINT, const ND3D11_BOX *);
typedef HRESULT (STDMETHODCALLTYPE *GetBuffer_t)(void *, UINT, const GUID *, void **);
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(void *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Present1_t)(void *, UINT, UINT, const NDXGI_PRESENT_PARAMETERS *);

#define D3D_DRIVER_TYPE_HARDWARE 1
#define D3D_DRIVER_TYPE_WARP     5
#define D3D11_SDK_VERSION        7
#define D3D11_USAGE_STAGING      3
#define D3D11_CPU_ACCESS_RW      0x30000
#define D3D11_MAP_READ_WRITE     3

/* The pieces of Chromium's software output device */
typedef struct { void *dev, *ctx, *factory, *dcomp, *target, *visual, *chain, *staging; } Out;

static void out_free(Out *o)
{
    RELEASE(o->staging); RELEASE(o->visual); RELEASE(o->target); RELEASE(o->chain);
    RELEASE(o->dcomp); RELEASE(o->factory); RELEASE(o->ctx); RELEASE(o->dev);
}

/* OutputDeviceBacking::GetOrCreateDXObjects and
 * SoftwareOutputDeviceWinSwapChain::ResizeDelegated, on @hwnd */
static int out_make(Out *o, HWND hwnd, UINT w, UINT h, int checks)
{
    memset(o, 0, sizeof(*o));
    CreateFactory_t cf = (CreateFactory_t)GetProcAddress(g_dxgi, "CreateDXGIFactory1");
    CreateDevice_t cd = (CreateDevice_t)GetProcAddress(g_d3d11, "D3D11CreateDevice");
    CreateDComp_t cdc = g_dcomp ? (CreateDComp_t)GetProcAddress(g_dcomp, "DCompositionCreateDevice") : NULL;
    int ok = cf && cd && cdc;
    ok = ok && SUCCEEDED(cf(&NIID_IDXGIFactory2, &o->factory));
    if (checks) CHECK("CreateDXGIFactory1(IDXGIFactory2)", ok);
    ok = ok && SUCCEEDED(cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 1 /* SINGLETHREADED */, NULL, 0, D3D11_SDK_VERSION, &o->dev, NULL, NULL));
    if (checks) CHECK("D3D11CreateDevice(WARP)", ok);
    ok = ok && SUCCEEDED(cdc(NULL, &NIID_IDCompositionDevice, &o->dcomp));
    if (checks) CHECK("DCompositionCreateDevice", ok);
    if (!ok) return 0;
    VT(o->dev, 40, void (STDMETHODCALLTYPE *)(void *, void **))(o->dev, &o->ctx);              /* GetImmediateContext */
    ok = SUCCEEDED(VT(o->dcomp, 6, HRESULT (STDMETHODCALLTYPE *)(void *, HWND, BOOL, void **))(o->dcomp, hwnd, TRUE, &o->target));
    ok = ok && SUCCEEDED(VT(o->dcomp, 7, HRESULT (STDMETHODCALLTYPE *)(void *, void **))(o->dcomp, &o->visual));
    ok = ok && SUCCEEDED(VT(o->target, 3, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(o->target, o->visual));   /* SetRoot */
    if (checks) CHECK("target, visual, SetRoot", ok);
    NDXGI_SWAP_CHAIN_DESC1 d = { w, h, NDXGI_FORMAT_B8G8R8A8_UNORM, FALSE, { 1, 0 }, 0, 2, 0 /* STRETCH */,
                                 3 /* FLIP_SEQUENTIAL */, 1 /* PREMULTIPLIED */, 0 };
    ok = ok && SUCCEEDED(VT(o->factory, 24, HRESULT (STDMETHODCALLTYPE *)(void *, void *, const void *, void *, void **))(
                   o->factory, o->dev, &d, NULL, &o->chain));                                   /* CreateSwapChainForComposition */
    if (checks) CHECK("CreateSwapChainForComposition", ok);
    ok = ok && SUCCEEDED(VT(o->visual, 15, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(o->visual, o->chain));   /* SetContent */
    ok = ok && SUCCEEDED(VT(o->dcomp, 3, HRESULT (STDMETHODCALLTYPE *)(void *))(o->dcomp));    /* Commit */
    if (checks) CHECK("SetContent, Commit", ok);
    ND3D11_TEXTURE2D_DESC td = { w, h, 1, 1, NDXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_RW, 0 };
    ok = ok && SUCCEEDED(VT(o->dev, 5, Tex2D_t)(o->dev, &td, NULL, &o->staging));
    if (checks) CHECK("staging texture", ok);
    return ok;
}

/* BeginPaintDelegated / EndPaintDelegated: fill @r of the staging texture
 * with @bgra, copy that part into the swap chain's buffer, Present1 it */
static int out_frame(Out *o, RECT r, DWORD bgra)
{
    ND3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(VT(o->ctx, 14, Map_t)(o->ctx, o->staging, 0, D3D11_MAP_READ_WRITE, 0, &m)) || !m.pData) return 0;
    for (LONG y = r.top; y < r.bottom; y++)
        for (LONG x = r.left; x < r.right; x++) ((DWORD *)((BYTE *)m.pData + (size_t)y * m.RowPitch))[x] = bgra;
    VT(o->ctx, 15, Unmap_t)(o->ctx, o->staging, 0);
    void *buf = NULL;
    if (FAILED(VT(o->chain, 9, GetBuffer_t)(o->chain, 0, &NIID_ID3D11Texture2D, &buf))) return 0;
    ND3D11_BOX box = { (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
    VT(o->ctx, 46, CopyRegion_t)(o->ctx, buf, 0, (UINT)r.left, (UINT)r.top, 0, o->staging, 0, &box);
    RELEASE(buf);
    NDXGI_PRESENT_PARAMETERS pp = { 1, &r, NULL, NULL };
    return SUCCEEDED(VT(o->chain, 22, Present1_t)(o->chain, 0, 0, &pp));
}

/* ---- The factory and the adapter (both widths) ---------------------------- */
static void check_adapter(void)
{
    g_step = "the factory";
    CreateFactory_t cf = (CreateFactory_t)GetProcAddress(g_dxgi, "CreateDXGIFactory1");
    void *f = NULL, *f7 = NULL, *a = NULL, *a4 = NULL, *x = NULL;
    CHECK("CreateDXGIFactory1", cf && SUCCEEDED(cf(&NIID_IDXGIFactory1, &f)));
    if (!f) return;
    CHECK("QueryInterface IDXGIFactory7", SUCCEEDED(VT(f, 0, QI_t)(f, &NIID_IDXGIFactory7, &f7)));
    typedef HRESULT (STDMETHODCALLTYPE *Enum_t)(void *, UINT, void **);
    CHECK("EnumAdapters1(0)", SUCCEEDED(VT(f, 12, Enum_t)(f, 0, &a)));
    CHECK("EnumAdapters1(1): one adapter", VT(f, 12, Enum_t)(f, 1, &x) == NDXGI_ERROR_NOT_FOUND && !x);
    if (!a) { RELEASE(f7); RELEASE(f); return; }
    NDXGI_ADAPTER_DESC3 d;
    memset(&d, 0, sizeof(d));
    CHECK("GetDesc1", SUCCEEDED(VT(a, 10, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(a, &d)));
    CHECK("the Microsoft Basic Render Driver", !wcscmp(d.Description, L"Microsoft Basic Render Driver"));
    CHECK("VendorId 0x1414, DeviceId 0x8c", d.VendorId == 0x1414 && d.DeviceId == 0x8c);
    CHECK("a software adapter (DXGI_ADAPTER_FLAG_SOFTWARE)", (d.Flags & NDXGI_ADAPTER_FLAG_SOFTWARE) != 0);
    CHECK("system memory, no video memory", d.SharedSystemMemory > 0 && !d.DedicatedVideoMemory);
    CHECK("EnumOutputs(0): render-only", VT(a, 7, Enum_t)(a, 0, &x) == NDXGI_ERROR_NOT_FOUND && !x);
    LARGE_INTEGER umd = { { 0, 0 } };
    CHECK("CheckInterfaceSupport(IDXGIDevice): the driver version",
          SUCCEEDED(VT(a, 9, HRESULT (STDMETHODCALLTYPE *)(void *, const GUID *, LARGE_INTEGER *))(a, &NIID_IDXGIDevice, &umd)) &&
          umd.HighPart >> 16 == 10);
    CHECK("QueryInterface IDXGIAdapter4", SUCCEEDED(VT(a, 0, QI_t)(a, &NIID_IDXGIAdapter4, &a4)));
    if (f7) {
        void *w = NULL, *l = NULL;
        CHECK("EnumWarpAdapter", SUCCEEDED(VT(f7, 27, HRESULT (STDMETHODCALLTYPE *)(void *, const GUID *, void **))(
                                      f7, &NIID_IDXGIAdapter1, &w)));
        CHECK("EnumAdapterByLuid", SUCCEEDED(VT(f7, 26, HRESULT (STDMETHODCALLTYPE *)(void *, LUID, const GUID *, void **))(
                                        f7, d.AdapterLuid, &NIID_IDXGIAdapter1, &l)));
        RELEASE(w); RELEASE(l);
    }
    RELEASE(a4); RELEASE(a); RELEASE(f7); RELEASE(f);

    g_step = "hardware devices";
    CreateDevice_t cd = (CreateDevice_t)GetProcAddress(g_d3d11, "D3D11CreateDevice");
    void *dev = NULL;
    CHECK("no hardware Direct3D 11 device without DXVK",
          cd && FAILED(cd(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, NULL)) && !dev);
    RELEASE(dev);
}

/* ---- Chromium's software output in this process (64-bit) ------------------ */
static void check_composition(HWND top, HWND child)
{
    g_step = "WARP devices";
    CreateDevice_t cd = (CreateDevice_t)GetProcAddress(g_d3d11, "D3D11CreateDevice");
    void *dev = NULL, *dxgidev = NULL, *a = NULL;
    UINT level = 0;
    CHECK("D3D11CreateDevice(WARP): feature level 9_1",
          SUCCEEDED(cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &level, NULL)) && level == 0x9100);
    if (dev) {
        CHECK("QueryInterface IDXGIDevice", SUCCEEDED(VT(dev, 0, QI_t)(dev, &NIID_IDXGIDevice, &dxgidev)));
        NDXGI_ADAPTER_DESC3 d;
        memset(&d, 0, sizeof(d));
        if (dxgidev && SUCCEEDED(VT(dxgidev, 7, HRESULT (STDMETHODCALLTYPE *)(void *, void **))(dxgidev, &a)))
            VT(a, 8, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(a, &d);
        CHECK("its adapter: the Basic Render Driver", d.VendorId == 0x1414 && d.DeviceId == 0x8c);
        RELEASE(a); RELEASE(dxgidev); RELEASE(dev);
    }
    const UINT angle[] = { 0xb000, 0xa000, 0x9300 };       /* ANGLE's levels: not this device */
    CHECK("WARP at feature levels 9_3 and up: refused (it does not rasterize)",
          FAILED(cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, angle, 3, D3D11_SDK_VERSION, &dev, NULL, NULL)) && !dev);

    g_step = "composition";
    Out o;
    int made = out_make(&o, child, 200, 120, 1);
    if (made) {
        RECT all = { 0, 0, 200, 120 }, part = { 100, 50, 150, 100 };
        CHECK("frame 1 presented", out_frame(&o, all, 0xFFFF0000));                 /* red */
        COLORREF c = pixel(child, 10, 10);
        CHECK("frame 1 in the window", c == RGB(255, 0, 0));
        if (c != RGB(255, 0, 0)) printf("pixel %06lx\n", (unsigned long)c);
        CHECK("frame 2 (a dirty rectangle) presented", out_frame(&o, part, 0xFF00FF00));   /* green */
        CHECK("frame 2: the rectangle drawn", pixel(child, 120, 70) == RGB(0, 255, 0));
        CHECK("frame 2: the rest kept", pixel(child, 10, 10) == RGB(255, 0, 0) && pixel(child, 160, 70) == RGB(255, 0, 0));
        void *v2 = NULL;
        VT(o.dcomp, 7, HRESULT (STDMETHODCALLTYPE *)(void *, void **))(o.dcomp, &v2);
        if (v2) {                                           /* a child visual at an offset, showing the same chain */
            VT(o.visual, 15, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(o.visual, NULL);
            VT(v2, 4, HRESULT (STDMETHODCALLTYPE *)(void *, float))(v2, 20.0f);           /* SetOffsetX */
            VT(v2, 15, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(v2, o.chain);
            VT(o.visual, 16, HRESULT (STDMETHODCALLTYPE *)(void *, void *, BOOL, void *))(o.visual, v2, TRUE, NULL);
            VT(o.dcomp, 3, HRESULT (STDMETHODCALLTYPE *)(void *))(o.dcomp);
            RECT strip = { 0, 0, 10, 120 };
            out_frame(&o, strip, 0xFF0000FF);                                               /* blue, at x 20..30 */
            CHECK("a child visual's offset", pixel(child, 25, 60) == RGB(0, 0, 255) && pixel(child, 15, 60) == RGB(255, 0, 0));
            RELEASE(v2);
        }
    }
    out_free(&o);
    CHECK("everything released", 1);

    g_step = "a swap chain on a window";
    CreateFactory_t cf = (CreateFactory_t)GetProcAddress(g_dxgi, "CreateDXGIFactory1");
    void *f = NULL, *chain = NULL, *buf = NULL, *ctx = NULL;
    cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    cf(&NIID_IDXGIFactory2, &f);
    NDXGI_SWAP_CHAIN_DESC1 d1 = { 0, 0, NDXGI_FORMAT_B8G8R8A8_UNORM, FALSE, { 1, 0 }, 0x20, 2, 0, 3, 0, 0 };
    CHECK("CreateSwapChainForHwnd", dev && f && SUCCEEDED(VT(f, 15, HRESULT (STDMETHODCALLTYPE *)(void *, void *, HWND,
                                                     const void *, const void *, void *, void **))(f, dev, top, &d1, NULL, NULL, &chain)));
    if (chain) {
        NDXGI_SWAP_CHAIN_DESC1 got;
        VT(chain, 18, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(chain, &got);
        RECT cr;
        GetClientRect(top, &cr);
        CHECK("its size: the window's client area", got.Width == (UINT)cr.right && got.Height == (UINT)cr.bottom);
        VT(chain, 9, GetBuffer_t)(chain, 0, &NIID_ID3D11Texture2D, &buf);
        DWORD row[64];
        for (int i = 0; i < 64; i++) row[i] = 0xFF00FFFF;   /* cyan */
        ND3D11_BOX box = { 300, 10, 0, 364, 11, 1 };
        typedef void (STDMETHODCALLTYPE *Update_t)(void *, void *, UINT, const ND3D11_BOX *, const void *, UINT, UINT);
        for (UINT y = 10; y < 40; y++) { box.top = y; box.bottom = y + 1; VT(ctx, 48, Update_t)(ctx, buf, 0, &box, row, 256, 0); }
        CHECK("Present", SUCCEEDED(VT(chain, 8, Present_t)(chain, 0, 0)));
        CHECK("drawn in the window", pixel(top, 320, 20) == RGB(0, 255, 255));
    }
    RELEASE(buf); RELEASE(chain); RELEASE(f); RELEASE(ctx); RELEASE(dev);

    g_step = "D3D11CreateDeviceAndSwapChain";
    CreateDeviceSC_t cdsc = (CreateDeviceSC_t)GetProcAddress(g_d3d11, "D3D11CreateDeviceAndSwapChain");
    NDXGI_SWAP_CHAIN_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.BufferDesc.Format = NDXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = 0x20;
    sd.BufferCount = 1;
    sd.OutputWindow = top;
    sd.Windowed = TRUE;
    CHECK("D3D11CreateDeviceAndSwapChain(WARP)",
          cdsc && SUCCEEDED(cdsc(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &chain, &dev, NULL, NULL)) &&
          chain && dev);
    RELEASE(chain); RELEASE(dev);
}

/* ---- The GPU process: its compositing window, parented by the browser ----- */
static HWND g_host, g_child;
static int g_tries;

static LRESULT CALLBACK gpu_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m != DC_DRAW) return DefWindowProcW(h, m, wp, lp);
    HWND parent = (HWND)wp;
    if (GetParent(h) != parent && ++g_tries < 200) {         /* (the browser's SetParent reaches us on its own) */
        Sleep(20);
        PostMessageW(h, DC_DRAW, wp, lp);
        return 0;
    }
    int ok = 0, bad = 0, bit = 0;
#define C(cond) do { if (cond) ok++; else bad |= 1 << bit; bit++; } while (0)
    C(GetParent(h) == parent);                              /* bit 0: WS_CHILD of the browser's window */
    C(GetAncestor(h, GA_PARENT) == parent);                 /* bit 1 */
    C(IsWindowVisible(h));                                  /* bit 2 */
    /* SoftwareOutputDeviceWinSwapChain::UpdateWindowSize */
    C(SetWindowPos(h, NULL, 0, 0, 200, 120, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOOWNERZORDER | SWP_NOZORDER));   /* bit 3 */
    RECT cr;
    C(GetClientRect(h, &cr) && cr.right == 200 && cr.bottom == 120);   /* bit 4 */
#ifdef _WIN64
    Out o;
    RECT all = { 0, 0, 200, 120 };
    C(out_make(&o, h, 200, 120, 0) && out_frame(&o, all, 0xFFFF0000));  /* bit 5: composited, red */
#else
    HDC dc = GetDC(h);
    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
    RECT all = { 0, 0, 200, 120 };
    C(FillRect(dc, &all, red));                             /* bit 5: drawn with GDI, red */
    DeleteObject(red);
    ReleaseDC(h, dc);
#endif
    C(pixel(h, 100, 60) == RGB(255, 0, 0));                 /* bit 6 */
#undef C
    PostMessageW(g_host, DC_RESULT, (WPARAM)ok, (LPARAM)bad);
#ifdef _WIN64
    out_free(&o);
#endif
    return 0;
}

static int gpu_main(HWND host)
{
    g_host = host;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = gpu_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"Intermediate D3D Window";
    RegisterClassW(&wc);
    /* gl::ChildWindowWin: a hidden popup, and the child that the browser
     * then parents in its own window */
    HWND popup = CreateWindowExW(WS_EX_TOOLWINDOW, L"Intermediate D3D Window", L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    HWND child = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_LAYERED | WS_EX_TRANSPARENT | 0x00200000 /* NOREDIRECTIONBITMAP */,
                                 L"Intermediate D3D Window", L"", WS_CHILD | WS_DISABLED | WS_VISIBLE, 0, 0, 1, 1, popup, NULL,
                                 wc.hInstance, NULL);
    if (!popup || !child) return 2;
    if (!PostMessageW(host, DC_HELLO, (WPARAM)child, 0)) return 4;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

/* ---- The browser ----------------------------------------------------------- */
static int g_result_ok = -1, g_result_bad = -1;

/* a window that draws nothing of its own (Chromium's draws only through DirectComposition) */
static LRESULT CALLBACK bare_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, wp, lp);
}

static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case DC_HELLO: g_child = (HWND)wp; return 0;
    case DC_RESULT: g_result_ok = (int)wp; g_result_bad = (int)lp; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void check_cross_process(HWND browser)
{
    g_step = "starting the GPU process";
    char self[MAX_PATH], cl[MAX_PATH + 64];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    snprintf(cl, sizeof(cl), "\"%s\" gpu %llx", self, (unsigned long long)(ULONG_PTR)browser);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CHECK("CreateProcess", 0); return; }
    for (int i = 0; i < 1000 && !g_child; i++) pump(20);
    CHECK("the GPU process's child window arrived", g_child != NULL);
    if (g_child) {
        g_step = "SetParent across processes";
        SetParent(g_child, browser);                        /* gfx::RenderingWindowManager::RegisterChild */
        PostMessageW(g_child, DC_DRAW, (WPARAM)browser, 0);
        for (int i = 0; i < 1000 && g_result_bad == -1; i++) pump(20);
        CHECK("the GPU process sees its parent, sizes and draws its window", g_result_bad == 0);
        if (g_result_bad) printf("gpu checks: %d passed, failed bits %#x\n", g_result_ok, g_result_bad);
        POINT o = { 0, 0 };
        ClientToScreen(browser, &o);
        RECT r = { 0, 0, 0, 0 };
        int placed = 0;
        for (int i = 0; i < 100 && !placed; i++) {
            placed = GetWindowRect(g_child, &r) && r.left == o.x && r.top == o.y && r.right - r.left == 200 && r.bottom - r.top == 120;
            if (!placed) pump(30);
        }
        CHECK("the window is over the browser's client area", placed);
        if (!placed) printf("child at %ld,%ld %ldx%ld, wanted %ld,%ld 200x120\n", r.left, r.top, r.right - r.left, r.bottom - r.top, o.x, o.y);
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

int main(int argc, char **argv)
{
    g_dxgi = LoadLibraryW(L"dxgi.dll");
    g_d3d11 = LoadLibraryW(L"d3d11.dll");
    g_dcomp = LoadLibraryW(L"dcomp.dll");
    if (argc >= 3 && !strcmp(argv[1], "gpu")) return gpu_main((HWND)(ULONG_PTR)strtoull(argv[2], NULL, 16));
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    CHECK("dxgi.dll, d3d11.dll and dcomp.dll load", g_dxgi && g_d3d11 && g_dcomp);
    if (!g_dxgi || !g_d3d11) { printf("dcomptest: %d passed, %d failed\n", pass, fail); return 1; }
    check_adapter();

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = host_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"dcomptest";
    wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
    RegisterClassW(&wc);
    HWND top = CreateWindowExW(0, L"dcomptest", L"dcomptest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 480, 320, NULL, NULL,
                               wc.hInstance, NULL);
    WNDCLASSW bc = { 0 };
    bc.lpfnWndProc = bare_proc;
    bc.hInstance = wc.hInstance;
    bc.lpszClassName = L"dcomptest_bare";
    RegisterClassW(&bc);
    HWND child = CreateWindowExW(0, L"dcomptest_bare", L"", WS_CHILD | WS_VISIBLE, 0, 0, 200, 120, top, NULL, wc.hInstance, NULL);
    pump(200);
#ifdef _WIN64
    check_composition(top, child);
#endif
    DestroyWindow(child);
    pump(100);
    check_cross_process(top);
    DestroyWindow(top);
    printf("dcomptest: %d passed, %d failed (%d-bit)\n", pass, fail, (int)sizeof(void *) * 8);
    return fail ? 1 : 0;
}
