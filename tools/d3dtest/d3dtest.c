/* d3dtest — Direct3D 9 and 11 through d3d9.dll, d3d11.dll and dxgi.dll
 * (DXVK on Mesa's lavapipe Vulkan on NovaOS): devices, clears, a
 * fixed-function Direct3D 9 triangle, render-target read-back, and a few
 * seconds of presented frames from each.
 * Build: x86_64-w64-mingw32-gcc -O2 -o d3dtest.exe d3dtest.c -ld3d9 -ld3d11 -ldxgi -luser32 -lgdi32 -lole32
 *        (i686-w64-mingw32-gcc for the 32-bit one).  Usage: d3dtest [seconds] */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
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

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 3;
    const char *only = argc > 2 ? argv[2] : "";
    setvbuf(stdout, NULL, _IONBF, 0);
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "d3dtest";
    RegisterClassA(&wc);
    if (strcmp(only, "11")) test_d3d9(secs);
    if (strcmp(only, "9")) test_d3d11(secs);
    printf("d3dtest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
