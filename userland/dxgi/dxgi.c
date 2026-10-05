/*
 * dxgi.dll — DirectX Graphics Infrastructure, NovaOS's own.
 *
 * NovaOS has no display adapter driver.  Like Windows on a PC without one,
 * its factory lists one adapter, the Microsoft Basic Render Driver (WARP:
 * a software adapter with no outputs), and makes swap chains for the
 * software device NovaOS's d3d11.dll creates on it, for windows and for
 * DirectComposition (dcomp.dll); see novadx.h.  Chromium's software
 * compositor draws through exactly these, and enumerates the adapters for
 * its GPU information.
 *
 * When DXVK's dxgi is installed (as dxgi_dxvk.dll beside this one), the
 * factory functions hand programs DXVK's factory instead, so Direct3D 10
 * and 11 work system-wide.  The Khronos Vulkan loader (which programs
 * often carry beside their .exe, in place of NovaOS's vulkan-1.dll) still
 * gets NovaOS's: it loads System32\dxgi.dll and needs a factory before it
 * reads its drivers, and DXVK's factory starts Vulkan, so giving it DXVK's
 * would have DXVK and the loader start each other until DXVK's own lock
 * deadlocks.  (The loader skips software adapters.)
 */
#include <windows.h>
#include <novadx.h>

#define DXGIAPI __declspec(dllexport)
#define DXGI_ERROR_FRAME_STATISTICS_DISJOINT_  ((HRESULT)0x887A000BL)
#define DXGI_ERROR_NOT_CURRENTLY_AVAILABLE_    ((HRESULT)0x887A0022L)
#define DXGI_PRESENT_TEST_                     0x00000001
#define DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL_      3
#define DXGI_SWAP_EFFECT_FLIP_DISCARD_         4
#define DXGI_USAGE_RENDER_TARGET_OUTPUT_       0x20
#define D3D11_BIND_SHADER_RESOURCE_            0x8
#define D3D11_BIND_RENDER_TARGET_              0x20

typedef struct Factory Factory;
typedef struct Adapter Adapter;
typedef struct Chain Chain;
static HRESULT own_factory(UINT flags, const GUID *iid, void **out);

/* ---- The Basic Render Driver ---------------------------------------------- */
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Adapter *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Adapter *);
    ULONG   (STDMETHODCALLTYPE *Release)(Adapter *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(Adapter *, const GUID *, UINT, const void *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(Adapter *, const GUID *, const IUnknown *);
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(Adapter *, const GUID *, UINT *, void *);
    HRESULT (STDMETHODCALLTYPE *GetParent)(Adapter *, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *EnumOutputs)(Adapter *, UINT, void **);
    HRESULT (STDMETHODCALLTYPE *GetDesc)(Adapter *, void *);
    HRESULT (STDMETHODCALLTYPE *CheckInterfaceSupport)(Adapter *, const GUID *, LARGE_INTEGER *);
    HRESULT (STDMETHODCALLTYPE *GetDesc1)(Adapter *, void *);
    HRESULT (STDMETHODCALLTYPE *GetDesc2)(Adapter *, void *);
    HRESULT (STDMETHODCALLTYPE *RegisterHardwareContentProtectionTeardownStatusEvent)(Adapter *, HANDLE, DWORD *);
    void    (STDMETHODCALLTYPE *UnregisterHardwareContentProtectionTeardownStatus)(Adapter *, DWORD);
    HRESULT (STDMETHODCALLTYPE *QueryVideoMemoryInfo)(Adapter *, UINT, UINT, void *);
    HRESULT (STDMETHODCALLTYPE *SetVideoMemoryReservation)(Adapter *, UINT, UINT, UINT64);
    HRESULT (STDMETHODCALLTYPE *RegisterVideoMemoryBudgetChangeNotificationEvent)(Adapter *, HANDLE, DWORD *);
    void    (STDMETHODCALLTYPE *UnregisterVideoMemoryBudgetChangeNotification)(Adapter *, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetDesc3)(Adapter *, void *);
} AdapterVtbl;
struct Adapter { const AdapterVtbl *vtbl; LONG refs; };

/* The same LUID in every process (Chromium matches adapters by it across
 * its processes) */
static const LUID g_warp_luid = { 0x0000e18c, 0 };

static HRESULT STDMETHODCALLTYPE a_qi(Adapter *a, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (novadx_same(iid, &NIID_IUnknown) || novadx_same(iid, &NIID_IDXGIObject) || novadx_same(iid, &NIID_IDXGIAdapter) ||
        novadx_same(iid, &NIID_IDXGIAdapter1) || novadx_same(iid, &NIID_IDXGIAdapter2) || novadx_same(iid, &NIID_IDXGIAdapter3) ||
        novadx_same(iid, &NIID_IDXGIAdapter4) || novadx_same(iid, &NIID_INovaSoftAdapter)) {
        InterlockedIncrement(&a->refs);
        *out = a;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE a_addref(Adapter *a) { return (ULONG)InterlockedIncrement(&a->refs); }
static ULONG STDMETHODCALLTYPE a_release(Adapter *a)
{
    LONG r = InterlockedDecrement(&a->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, a);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE a_setpriv(Adapter *a, const GUID *g, UINT n, const void *d) { (void)a; (void)g; (void)n; (void)d; return S_OK; }
static HRESULT STDMETHODCALLTYPE a_setprivif(Adapter *a, const GUID *g, const IUnknown *u) { (void)a; (void)g; (void)u; return S_OK; }
static HRESULT STDMETHODCALLTYPE a_getpriv(Adapter *a, const GUID *g, UINT *n, void *d)
{ (void)a; (void)g; (void)d; if (n) *n = 0; return NDXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE a_getparent(Adapter *a, const GUID *iid, void **out) { (void)a; return own_factory(0, iid, out); }
static HRESULT STDMETHODCALLTYPE a_enumoutputs(Adapter *a, UINT i, void **out)
{ (void)a; (void)i; if (!out) return E_INVALIDARG; *out = NULL; return NDXGI_ERROR_NOT_FOUND; }   /* (a render-only adapter) */

static void warp_desc(NDXGI_ADAPTER_DESC3 *d)
{
    __builtin_memset(d, 0, sizeof(*d));
    lstrcpyW(d->Description, NOVADX_WARP_NAME);
    d->VendorId = NOVADX_WARP_VENDOR;
    d->DeviceId = NOVADX_WARP_DEVICE;
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    UINT64 half = GlobalMemoryStatusEx(&m) ? m.ullTotalPhys / 2 : (UINT64)256 << 20;
#ifndef _WIN64
    if (half > 0xC0000000u) half = 0xC0000000u;
#endif
    d->SharedSystemMemory = (SIZE_T)half;
    d->AdapterLuid = g_warp_luid;
    d->Flags = NDXGI_ADAPTER_FLAG_SOFTWARE;
}
/* DXGI_ADAPTER_DESC (no Flags), DESC1 (+ Flags), DESC2 and DESC3 (+ the
 * preemption granularities) */
static HRESULT STDMETHODCALLTYPE a_getdesc(Adapter *a, void *out)
{
    (void)a;
    if (!out) return E_INVALIDARG;
    NDXGI_ADAPTER_DESC3 d;
    warp_desc(&d);
    __builtin_memcpy(out, &d, offsetof(NDXGI_ADAPTER_DESC3, Flags));
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE a_getdesc1(Adapter *a, void *out)
{
    (void)a;
    if (!out) return E_INVALIDARG;
    NDXGI_ADAPTER_DESC3 d;
    warp_desc(&d);
    __builtin_memcpy(out, &d, offsetof(NDXGI_ADAPTER_DESC3, GraphicsPreemptionGranularity));
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE a_getdesc2(Adapter *a, void *out)
{
    (void)a;
    if (!out) return E_INVALIDARG;
    warp_desc(out);
    return S_OK;
}
/* the user-mode driver's version, as Chromium reads it: WARP's is the
 * system's (10.0.19045.1) */
static HRESULT STDMETHODCALLTYPE a_checkif(Adapter *a, const GUID *iid, LARGE_INTEGER *umd)
{
    (void)a;
    if (!novadx_same(iid, &NIID_IDXGIDevice)) return NDXGI_ERROR_UNSUPPORTED;
    if (umd) { umd->HighPart = 10 << 16; umd->LowPart = (19045u << 16) | 1; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE a_regevent(Adapter *a, HANDLE e, DWORD *cookie) { (void)a; (void)e; if (cookie) *cookie = 0; return S_OK; }
static void STDMETHODCALLTYPE a_unreg(Adapter *a, DWORD cookie) { (void)a; (void)cookie; }
static HRESULT STDMETHODCALLTYPE a_vidmem(Adapter *a, UINT node, UINT group, void *out)
{
    (void)node;
    if (!out) return E_INVALIDARG;
    UINT64 *q = out;                                        /* DXGI_QUERY_VIDEO_MEMORY_INFO */
    NDXGI_ADAPTER_DESC3 d;
    warp_desc(&d);
    q[0] = group == 0 ? 0 : d.SharedSystemMemory;           /* budget: local (none) or non-local */
    q[1] = 0;
    q[2] = q[0] / 2;
    q[3] = 0;
    (void)a;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE a_reserve(Adapter *a, UINT node, UINT group, UINT64 n) { (void)a; (void)node; (void)group; (void)n; return S_OK; }

static const AdapterVtbl g_adapter_vtbl = {
    a_qi, a_addref, a_release, a_setpriv, a_setprivif, a_getpriv, a_getparent, a_enumoutputs, a_getdesc, a_checkif,
    a_getdesc1, a_getdesc2, a_regevent, a_unreg, a_vidmem, a_reserve, a_regevent, a_unreg, a_getdesc2,
};

static HRESULT warp_adapter(const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    Adapter *a = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    a->vtbl = &g_adapter_vtbl;
    a->refs = 1;
    HRESULT hr = a_qi(a, iid, out);
    a_release(a);
    return hr;
}

/* ---- Swap chains on the software device ------------------------------------ */
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Chain *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Chain *);
    ULONG   (STDMETHODCALLTYPE *Release)(Chain *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(Chain *, const GUID *, UINT, const void *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(Chain *, const GUID *, const IUnknown *);
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(Chain *, const GUID *, UINT *, void *);
    HRESULT (STDMETHODCALLTYPE *GetParent)(Chain *, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *GetDevice)(Chain *, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *Present)(Chain *, UINT, UINT);
    HRESULT (STDMETHODCALLTYPE *GetBuffer)(Chain *, UINT, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *SetFullscreenState)(Chain *, BOOL, void *);
    HRESULT (STDMETHODCALLTYPE *GetFullscreenState)(Chain *, BOOL *, void **);
    HRESULT (STDMETHODCALLTYPE *GetDesc)(Chain *, NDXGI_SWAP_CHAIN_DESC *);
    HRESULT (STDMETHODCALLTYPE *ResizeBuffers)(Chain *, UINT, UINT, UINT, UINT, UINT);
    HRESULT (STDMETHODCALLTYPE *ResizeTarget)(Chain *, const NDXGI_MODE_DESC *);
    HRESULT (STDMETHODCALLTYPE *GetContainingOutput)(Chain *, void **);
    HRESULT (STDMETHODCALLTYPE *GetFrameStatistics)(Chain *, void *);
    HRESULT (STDMETHODCALLTYPE *GetLastPresentCount)(Chain *, UINT *);
    HRESULT (STDMETHODCALLTYPE *GetDesc1)(Chain *, NDXGI_SWAP_CHAIN_DESC1 *);
    HRESULT (STDMETHODCALLTYPE *GetFullscreenDesc)(Chain *, void *);
    HRESULT (STDMETHODCALLTYPE *GetHwnd)(Chain *, HWND *);
    HRESULT (STDMETHODCALLTYPE *GetCoreWindow)(Chain *, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *Present1)(Chain *, UINT, UINT, const NDXGI_PRESENT_PARAMETERS *);
    BOOL    (STDMETHODCALLTYPE *IsTemporaryMonoSupported)(Chain *);
    HRESULT (STDMETHODCALLTYPE *GetRestrictToOutput)(Chain *, void **);
    HRESULT (STDMETHODCALLTYPE *SetBackgroundColor)(Chain *, const void *);
    HRESULT (STDMETHODCALLTYPE *GetBackgroundColor)(Chain *, void *);
    HRESULT (STDMETHODCALLTYPE *SetRotation)(Chain *, UINT);
    HRESULT (STDMETHODCALLTYPE *GetRotation)(Chain *, UINT *);
} ChainVtbl;

/* One buffer: what GetBuffer(0) gives is what the next present shows, and
 * it keeps the last frame, so a present with dirty rectangles shows the
 * rest unchanged, as a flip-model chain's runtime-copied back buffer does */
struct Chain {
    const ChainVtbl *vtbl;
    INovaSwapChain nova;
    LONG refs;
    IUnknown *device;
    HWND hwnd;                          /* (0: for DirectComposition) */
    NDXGI_SWAP_CHAIN_DESC1 desc;
    IUnknown *buffer;                   /* its ID3D11Texture2D */
    INovaSoftTexture *bits;
    NovaPresentSink sink;
    void *sink_ctx;
    UINT presents, rotation;
    BYTE *rgba_tmp;                     /* (R8G8B8A8 buffers: the BGRA copy shown) */
    size_t rgba_size;
};
#define CHAIN_OF_NOVA(n) ((Chain *)((BYTE *)(n) - offsetof(Chain, nova)))

typedef HRESULT (STDMETHODCALLTYPE *CreateTexture2D_t)(IUnknown *, const ND3D11_TEXTURE2D_DESC *, const void *, IUnknown **);

static int chain_format(UINT f)
{
    return f == NDXGI_FORMAT_B8G8R8A8_UNORM || f == NDXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == NDXGI_FORMAT_B8G8R8X8_UNORM ||
           f == NDXGI_FORMAT_R8G8B8A8_UNORM || f == NDXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

static void chain_drop_buffer(Chain *c)
{
    if (c->bits) { c->bits->lpVtbl->Release(c->bits); c->bits = NULL; }
    if (c->buffer) { c->buffer->lpVtbl->Release(c->buffer); c->buffer = NULL; }
}

static HRESULT chain_make_buffer(Chain *c)
{
    ND3D11_TEXTURE2D_DESC td;
    __builtin_memset(&td, 0, sizeof(td));
    td.Width = c->desc.Width; td.Height = c->desc.Height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = c->desc.Format;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET_ | D3D11_BIND_SHADER_RESOURCE_;
    CreateTexture2D_t create = (CreateTexture2D_t)((void **)*(void **)c->device)[5];
    IUnknown *t = NULL;
    HRESULT hr = create(c->device, &td, NULL, &t);
    if (FAILED(hr)) return hr;
    void *b = NULL;
    hr = t->lpVtbl->QueryInterface(t, &NIID_INovaSoftTexture, &b);
    if (FAILED(hr)) { t->lpVtbl->Release(t); return NDXGI_ERROR_UNSUPPORTED; }
    c->buffer = t;
    c->bits = b;
    return S_OK;
}

/* The buffer's pixels as BGRA */
static BYTE *chain_frame(Chain *c, UINT *pitch)
{
    if (!c->bits) return NULL;
    ND3D11_TEXTURE2D_DESC td;
    BYTE *p = c->bits->lpVtbl->Bits(c->bits, pitch, &td);
    if (!p || (td.Format != NDXGI_FORMAT_R8G8B8A8_UNORM && td.Format != NDXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) return p;
    size_t need = (size_t)*pitch * td.Height;
    if (c->rgba_size < need) {
        HeapFree(GetProcessHeap(), 0, c->rgba_tmp);
        c->rgba_tmp = HeapAlloc(GetProcessHeap(), 0, need);
        c->rgba_size = c->rgba_tmp ? need : 0;
        if (!c->rgba_tmp) return NULL;
    }
    for (size_t i = 0; i < need; i += 4) {
        c->rgba_tmp[i] = p[i + 2]; c->rgba_tmp[i + 1] = p[i + 1]; c->rgba_tmp[i + 2] = p[i]; c->rgba_tmp[i + 3] = p[i + 3];
    }
    return c->rgba_tmp;
}

static HRESULT STDMETHODCALLTYPE c_qi(Chain *c, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (novadx_same(iid, &NIID_IUnknown) || novadx_same(iid, &NIID_IDXGIObject) || novadx_same(iid, &NIID_IDXGIDeviceSubObject) ||
        novadx_same(iid, &NIID_IDXGISwapChain) || novadx_same(iid, &NIID_IDXGISwapChain1)) {
        InterlockedIncrement(&c->refs);
        *out = c;
        return S_OK;
    }
    if (novadx_same(iid, &NIID_INovaSwapChain)) {
        InterlockedIncrement(&c->refs);
        *out = &c->nova;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE c_addref(Chain *c) { return (ULONG)InterlockedIncrement(&c->refs); }
static ULONG STDMETHODCALLTYPE c_release(Chain *c)
{
    LONG r = InterlockedDecrement(&c->refs);
    if (!r) {
        chain_drop_buffer(c);
        if (c->device) c->device->lpVtbl->Release(c->device);
        HeapFree(GetProcessHeap(), 0, c->rgba_tmp);
        HeapFree(GetProcessHeap(), 0, c);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE c_setpriv(Chain *c, const GUID *g, UINT n, const void *d) { (void)c; (void)g; (void)n; (void)d; return S_OK; }
static HRESULT STDMETHODCALLTYPE c_setprivif(Chain *c, const GUID *g, const IUnknown *u) { (void)c; (void)g; (void)u; return S_OK; }
static HRESULT STDMETHODCALLTYPE c_getpriv(Chain *c, const GUID *g, UINT *n, void *d)
{ (void)c; (void)g; (void)d; if (n) *n = 0; return NDXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE c_getparent(Chain *c, const GUID *iid, void **out) { (void)c; return own_factory(0, iid, out); }
static HRESULT STDMETHODCALLTYPE c_getdevice(Chain *c, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    return c->device->lpVtbl->QueryInterface(c->device, iid, out);
}

static HRESULT chain_present(Chain *c, UINT flags, const RECT *dirty)
{
    if (flags & DXGI_PRESENT_TEST_) return S_OK;
    c->presents++;
    if (c->sink) { c->sink(c->sink_ctx, dirty); return S_OK; }
    if (!c->hwnd) return S_OK;                              /* (a composition chain no visual shows yet) */
    if (!IsWindow(c->hwnd)) return NDXGI_ERROR_INVALID_CALL;
    UINT pitch = 0;
    BYTE *p = chain_frame(c, &pitch);
    if (!p) return S_OK;
    HDC dc = GetDC(c->hwnd);
    if (!dc) return S_OK;
    novadx_blit(dc, 0, 0, p, pitch, c->desc.Width, c->desc.Height, dirty);
    ReleaseDC(c->hwnd, dc);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE c_present(Chain *c, UINT sync, UINT flags) { (void)sync; return chain_present(c, flags, NULL); }
static HRESULT STDMETHODCALLTYPE c_present1(Chain *c, UINT sync, UINT flags, const NDXGI_PRESENT_PARAMETERS *pp)
{
    (void)sync;
    RECT u, *dirty = NULL;
    if (pp && pp->DirtyRectsCount && pp->pDirtyRects) {     /* (the rectangles' union) */
        u = pp->pDirtyRects[0];
        for (UINT i = 1; i < pp->DirtyRectsCount; i++) UnionRect(&u, &u, &pp->pDirtyRects[i]);
        dirty = &u;
    }
    return chain_present(c, flags, dirty);
}
static HRESULT STDMETHODCALLTYPE c_getbuffer(Chain *c, UINT i, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (i >= (c->desc.BufferCount ? c->desc.BufferCount : 1) || !c->buffer) return NDXGI_ERROR_INVALID_CALL;
    return c->buffer->lpVtbl->QueryInterface(c->buffer, iid, out);
}
static HRESULT STDMETHODCALLTYPE c_setfs(Chain *c, BOOL fs, void *target)
{ (void)c; (void)target; return fs ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE_ : S_OK; }
static HRESULT STDMETHODCALLTYPE c_getfs(Chain *c, BOOL *fs, void **target)
{
    (void)c;
    if (fs) *fs = FALSE;
    if (target) *target = NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_getdesc(Chain *c, NDXGI_SWAP_CHAIN_DESC *d)
{
    if (!d) return E_INVALIDARG;
    __builtin_memset(d, 0, sizeof(*d));
    d->BufferDesc.Width = c->desc.Width;
    d->BufferDesc.Height = c->desc.Height;
    d->BufferDesc.Format = c->desc.Format;
    d->BufferDesc.RefreshRate.Numerator = 60;
    d->BufferDesc.RefreshRate.Denominator = 1;
    d->SampleDesc = c->desc.SampleDesc;
    d->BufferUsage = c->desc.BufferUsage;
    d->BufferCount = c->desc.BufferCount;
    d->OutputWindow = c->hwnd;
    d->Windowed = TRUE;
    d->SwapEffect = c->desc.SwapEffect;
    d->Flags = c->desc.Flags;
    return S_OK;
}

static void client_size(HWND h, UINT *w, UINT *ht)
{
    RECT r = { 0, 0, 0, 0 };
    if (h) GetClientRect(h, &r);
    if (!*w) *w = r.right > r.left ? (UINT)(r.right - r.left) : 8;
    if (!*ht) *ht = r.bottom > r.top ? (UINT)(r.bottom - r.top) : 8;
}

static HRESULT STDMETHODCALLTYPE c_resize(Chain *c, UINT count, UINT w, UINT h, UINT fmt, UINT flags)
{
    if (fmt && !chain_format(fmt)) return NDXGI_ERROR_INVALID_CALL;
    client_size(c->hwnd, &w, &h);
    NDXGI_SWAP_CHAIN_DESC1 old = c->desc;
    if (count) c->desc.BufferCount = count;
    if (fmt) c->desc.Format = fmt;
    c->desc.Width = w; c->desc.Height = h;
    c->desc.Flags = flags;
    chain_drop_buffer(c);
    HRESULT hr = chain_make_buffer(c);
    if (FAILED(hr)) { c->desc = old; if (FAILED(chain_make_buffer(c))) return hr; return hr; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_resizetarget(Chain *c, const NDXGI_MODE_DESC *m) { (void)c; return m ? S_OK : E_INVALIDARG; }
static HRESULT STDMETHODCALLTYPE c_getoutput(Chain *c, void **out) { (void)c; if (out) *out = NULL; return NDXGI_ERROR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE c_getstats(Chain *c, void *s) { (void)c; (void)s; return DXGI_ERROR_FRAME_STATISTICS_DISJOINT_; }
static HRESULT STDMETHODCALLTYPE c_getcount(Chain *c, UINT *n) { if (!n) return E_INVALIDARG; *n = c->presents; return S_OK; }
static HRESULT STDMETHODCALLTYPE c_getdesc1(Chain *c, NDXGI_SWAP_CHAIN_DESC1 *d) { if (!d) return E_INVALIDARG; *d = c->desc; return S_OK; }
static HRESULT STDMETHODCALLTYPE c_getfsdesc(Chain *c, void *d)
{
    if (!d) return E_INVALIDARG;
    if (!c->hwnd) return NDXGI_ERROR_INVALID_CALL;
    UINT *f = d;                                            /* DXGI_SWAP_CHAIN_FULLSCREEN_DESC */
    f[0] = 60; f[1] = 1; f[2] = 0; f[3] = 0; f[4] = TRUE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_gethwnd(Chain *c, HWND *h)
{
    if (!h) return E_INVALIDARG;
    *h = c->hwnd;
    return c->hwnd ? S_OK : NDXGI_ERROR_INVALID_CALL;
}
static HRESULT STDMETHODCALLTYPE c_getcore(Chain *c, const GUID *iid, void **out) { (void)c; (void)iid; if (out) *out = NULL; return NDXGI_ERROR_INVALID_CALL; }
static BOOL STDMETHODCALLTYPE c_mono(Chain *c) { (void)c; return FALSE; }
static HRESULT STDMETHODCALLTYPE c_getrestrict(Chain *c, void **out) { (void)c; if (out) *out = NULL; return S_OK; }
static HRESULT STDMETHODCALLTYPE c_setbg(Chain *c, const void *col) { (void)c; (void)col; return S_OK; }
static HRESULT STDMETHODCALLTYPE c_getbg(Chain *c, void *col)
{ (void)c; if (!col) return E_INVALIDARG; __builtin_memset(col, 0, 16); return S_OK; }
static HRESULT STDMETHODCALLTYPE c_setrot(Chain *c, UINT r) { c->rotation = r; return S_OK; }
static HRESULT STDMETHODCALLTYPE c_getrot(Chain *c, UINT *r) { if (!r) return E_INVALIDARG; *r = c->rotation ? c->rotation : 1; return S_OK; }

static const ChainVtbl g_chain_vtbl = {
    c_qi, c_addref, c_release, c_setpriv, c_setprivif, c_getpriv, c_getparent, c_getdevice, c_present, c_getbuffer,
    c_setfs, c_getfs, c_getdesc, c_resize, c_resizetarget, c_getoutput, c_getstats, c_getcount, c_getdesc1, c_getfsdesc,
    c_gethwnd, c_getcore, c_present1, c_mono, c_getrestrict, c_setbg, c_getbg, c_setrot, c_getrot,
};

/* the private side, which dcomp.dll's visuals use */
static HRESULT STDMETHODCALLTYPE n_qi(INovaSwapChain *n, const GUID *iid, void **out) { return c_qi(CHAIN_OF_NOVA(n), iid, out); }
static ULONG STDMETHODCALLTYPE n_addref(INovaSwapChain *n) { return c_addref(CHAIN_OF_NOVA(n)); }
static ULONG STDMETHODCALLTYPE n_release(INovaSwapChain *n) { return c_release(CHAIN_OF_NOVA(n)); }
static void STDMETHODCALLTYPE n_setsink(INovaSwapChain *n, NovaPresentSink sink, void *ctx)
{
    Chain *c = CHAIN_OF_NOVA(n);
    c->sink = sink;
    c->sink_ctx = ctx;
}
static BYTE *STDMETHODCALLTYPE n_frame(INovaSwapChain *n, UINT *pitch, UINT *w, UINT *h)
{
    Chain *c = CHAIN_OF_NOVA(n);
    UINT p = 0;
    BYTE *bits = chain_frame(c, &p);
    if (pitch) *pitch = p;
    if (w) *w = c->desc.Width;
    if (h) *h = c->desc.Height;
    return c->presents ? bits : NULL;                       /* (nothing to show before the first present) */
}
static const INovaSwapChainVtbl g_nova_chain_vtbl = { n_qi, n_addref, n_release, n_setsink, n_frame };

static HRESULT make_chain(IUnknown *device, HWND hwnd, const NDXGI_SWAP_CHAIN_DESC1 *desc, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device || !desc) return NDXGI_ERROR_INVALID_CALL;
    IUnknown *soft = NULL;
    if (FAILED(device->lpVtbl->QueryInterface(device, &NIID_INovaSoftDevice, (void **)&soft))) return NDXGI_ERROR_UNSUPPORTED;
    soft->lpVtbl->Release(soft);
    if (!chain_format(desc->Format) || desc->SampleDesc.Count > 1) return NDXGI_ERROR_INVALID_CALL;
    if (!hwnd && (desc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL_ && desc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD_))
        return NDXGI_ERROR_INVALID_CALL;                    /* (composition takes flip-model chains only) */
    Chain *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) return E_OUTOFMEMORY;
    c->vtbl = &g_chain_vtbl;
    c->nova.lpVtbl = &g_nova_chain_vtbl;
    c->refs = 1;
    c->device = device;
    device->lpVtbl->AddRef(device);
    c->hwnd = hwnd;
    c->desc = *desc;
    if (!c->desc.BufferCount) c->desc.BufferCount = 1;
    client_size(hwnd, &c->desc.Width, &c->desc.Height);
    HRESULT hr = chain_make_buffer(c);
    if (FAILED(hr)) { c_release(c); return hr; }
    *out = c;
    return S_OK;
}

/* ---- NovaOS's factory: IDXGIFactory7 -------------------------------------- */
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Factory *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Factory *);
    ULONG   (STDMETHODCALLTYPE *Release)(Factory *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(Factory *, const GUID *, UINT, const void *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(Factory *, const GUID *, const IUnknown *);
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(Factory *, const GUID *, UINT *, void *);
    HRESULT (STDMETHODCALLTYPE *GetParent)(Factory *, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *EnumAdapters)(Factory *, UINT, void **);
    HRESULT (STDMETHODCALLTYPE *MakeWindowAssociation)(Factory *, HWND, UINT);
    HRESULT (STDMETHODCALLTYPE *GetWindowAssociation)(Factory *, HWND *);
    HRESULT (STDMETHODCALLTYPE *CreateSwapChain)(Factory *, IUnknown *, NDXGI_SWAP_CHAIN_DESC *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateSoftwareAdapter)(Factory *, HMODULE, void **);
    HRESULT (STDMETHODCALLTYPE *EnumAdapters1)(Factory *, UINT, void **);
    BOOL    (STDMETHODCALLTYPE *IsCurrent)(Factory *);
    BOOL    (STDMETHODCALLTYPE *IsWindowedStereoEnabled)(Factory *);
    HRESULT (STDMETHODCALLTYPE *CreateSwapChainForHwnd)(Factory *, IUnknown *, HWND, const NDXGI_SWAP_CHAIN_DESC1 *, const void *, void *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateSwapChainForCoreWindow)(Factory *, IUnknown *, IUnknown *, const NDXGI_SWAP_CHAIN_DESC1 *, void *, void **);
    HRESULT (STDMETHODCALLTYPE *GetSharedResourceAdapterLuid)(Factory *, HANDLE, LUID *);
    HRESULT (STDMETHODCALLTYPE *RegisterStereoStatusWindow)(Factory *, HWND, UINT, DWORD *);
    HRESULT (STDMETHODCALLTYPE *RegisterStereoStatusEvent)(Factory *, HANDLE, DWORD *);
    void    (STDMETHODCALLTYPE *UnregisterStereoStatus)(Factory *, DWORD);
    HRESULT (STDMETHODCALLTYPE *RegisterOcclusionStatusWindow)(Factory *, HWND, UINT, DWORD *);
    HRESULT (STDMETHODCALLTYPE *RegisterOcclusionStatusEvent)(Factory *, HANDLE, DWORD *);
    void    (STDMETHODCALLTYPE *UnregisterOcclusionStatus)(Factory *, DWORD);
    HRESULT (STDMETHODCALLTYPE *CreateSwapChainForComposition)(Factory *, IUnknown *, const NDXGI_SWAP_CHAIN_DESC1 *, void *, void **);
    UINT    (STDMETHODCALLTYPE *GetCreationFlags)(Factory *);
    HRESULT (STDMETHODCALLTYPE *EnumAdapterByLuid)(Factory *, LUID, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *EnumWarpAdapter)(Factory *, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *CheckFeatureSupport)(Factory *, UINT, void *, UINT);
    HRESULT (STDMETHODCALLTYPE *EnumAdapterByGpuPreference)(Factory *, UINT, UINT, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *RegisterAdaptersChangedEvent)(Factory *, HANDLE, DWORD *);
    HRESULT (STDMETHODCALLTYPE *UnregisterAdaptersChangedEvent)(Factory *, DWORD);
} FactoryVtbl;
struct Factory { const FactoryVtbl *vtbl; LONG refs; HWND assoc; UINT flags; };

static HRESULT STDMETHODCALLTYPE f_qi(Factory *f, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (novadx_same(iid, &NIID_IUnknown) || novadx_same(iid, &NIID_IDXGIObject) || novadx_same(iid, &NIID_IDXGIFactory) ||
        novadx_same(iid, &NIID_IDXGIFactory1) || novadx_same(iid, &NIID_IDXGIFactory2) || novadx_same(iid, &NIID_IDXGIFactory3) ||
        novadx_same(iid, &NIID_IDXGIFactory4) || novadx_same(iid, &NIID_IDXGIFactory5) || novadx_same(iid, &NIID_IDXGIFactory6) ||
        novadx_same(iid, &NIID_IDXGIFactory7)) {
        InterlockedIncrement(&f->refs);
        *out = f;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE f_addref(Factory *f) { return (ULONG)InterlockedIncrement(&f->refs); }
static ULONG STDMETHODCALLTYPE f_release(Factory *f)
{
    LONG r = InterlockedDecrement(&f->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, f);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE f_setpriv(Factory *f, const GUID *g, UINT n, const void *d) { (void)f; (void)g; (void)n; (void)d; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_setprivif(Factory *f, const GUID *g, const IUnknown *u) { (void)f; (void)g; (void)u; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_getpriv(Factory *f, const GUID *g, UINT *n, void *d)
{ (void)f; (void)g; (void)d; if (n) *n = 0; return NDXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE f_getparent(Factory *f, const GUID *g, void **out) { (void)f; (void)g; if (out) *out = NULL; return E_NOINTERFACE; }
static HRESULT STDMETHODCALLTYPE f_enum(Factory *f, UINT i, void **out)
{
    (void)f;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    return i ? NDXGI_ERROR_NOT_FOUND : warp_adapter(&NIID_IDXGIAdapter1, out);
}
static HRESULT STDMETHODCALLTYPE f_assoc(Factory *f, HWND h, UINT flags) { (void)flags; f->assoc = h; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_getassoc(Factory *f, HWND *h) { if (!h) return E_INVALIDARG; *h = f->assoc; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_swapchain(Factory *f, IUnknown *dev, NDXGI_SWAP_CHAIN_DESC *d, void **out)
{
    (void)f;
    if (!d) { if (out) *out = NULL; return NDXGI_ERROR_INVALID_CALL; }
    NDXGI_SWAP_CHAIN_DESC1 d1;
    __builtin_memset(&d1, 0, sizeof(d1));
    d1.Width = d->BufferDesc.Width; d1.Height = d->BufferDesc.Height; d1.Format = d->BufferDesc.Format;
    d1.SampleDesc = d->SampleDesc;
    d1.BufferUsage = d->BufferUsage; d1.BufferCount = d->BufferCount;
    d1.SwapEffect = d->SwapEffect; d1.Flags = d->Flags;
    if (!d->OutputWindow) { if (out) *out = NULL; return NDXGI_ERROR_INVALID_CALL; }
    return make_chain(dev, d->OutputWindow, &d1, out);
}
static HRESULT STDMETHODCALLTYPE f_soft(Factory *f, HMODULE m, void **out) { (void)f; (void)m; if (out) *out = NULL; return NDXGI_ERROR_UNSUPPORTED; }
static BOOL STDMETHODCALLTYPE f_current(Factory *f) { (void)f; return TRUE; }
static BOOL STDMETHODCALLTYPE f_stereo(Factory *f) { (void)f; return FALSE; }
static HRESULT STDMETHODCALLTYPE f_forhwnd(Factory *f, IUnknown *dev, HWND h, const NDXGI_SWAP_CHAIN_DESC1 *d, const void *fs,
                                           void *restrict_to, void **out)
{
    (void)f; (void)fs; (void)restrict_to;
    if (!h) { if (out) *out = NULL; return NDXGI_ERROR_INVALID_CALL; }
    return make_chain(dev, h, d, out);
}
static HRESULT STDMETHODCALLTYPE f_forcore(Factory *f, IUnknown *dev, IUnknown *w, const NDXGI_SWAP_CHAIN_DESC1 *d, void *r, void **out)
{ (void)f; (void)dev; (void)w; (void)d; (void)r; if (out) *out = NULL; return NDXGI_ERROR_INVALID_CALL; }
static HRESULT STDMETHODCALLTYPE f_sharedluid(Factory *f, HANDLE h, LUID *l) { (void)f; (void)h; (void)l; return E_INVALIDARG; }
static HRESULT STDMETHODCALLTYPE f_regwin(Factory *f, HWND h, UINT m, DWORD *cookie) { (void)f; (void)h; (void)m; if (cookie) *cookie = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_regevent(Factory *f, HANDLE e, DWORD *cookie) { (void)f; (void)e; if (cookie) *cookie = 0; return S_OK; }
static void STDMETHODCALLTYPE f_unreg(Factory *f, DWORD cookie) { (void)f; (void)cookie; }
static HRESULT STDMETHODCALLTYPE f_forcomp(Factory *f, IUnknown *dev, const NDXGI_SWAP_CHAIN_DESC1 *d, void *restrict_to, void **out)
{
    (void)f; (void)restrict_to;
    if (d && (!d->Width || !d->Height)) { if (out) *out = NULL; return NDXGI_ERROR_INVALID_CALL; }
    return make_chain(dev, NULL, d, out);
}
static UINT STDMETHODCALLTYPE f_flags(Factory *f) { return f->flags; }
static HRESULT STDMETHODCALLTYPE f_byluid(Factory *f, LUID l, const GUID *iid, void **out)
{
    (void)f;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (l.LowPart != g_warp_luid.LowPart || l.HighPart != g_warp_luid.HighPart) return E_INVALIDARG;
    return warp_adapter(iid, out);
}
static HRESULT STDMETHODCALLTYPE f_warp(Factory *f, const GUID *iid, void **out) { (void)f; return warp_adapter(iid, out); }
static HRESULT STDMETHODCALLTYPE f_feature(Factory *f, UINT feature, void *data, UINT size)
{
    (void)f;
    if (feature != 0 || !data || size != sizeof(BOOL)) return E_INVALIDARG;   /* DXGI_FEATURE_PRESENT_ALLOW_TEARING */
    *(BOOL *)data = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_bypref(Factory *f, UINT i, UINT pref, const GUID *iid, void **out)
{
    (void)f; (void)pref;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    return i ? NDXGI_ERROR_NOT_FOUND : warp_adapter(iid, out);
}
static HRESULT STDMETHODCALLTYPE f_unregac(Factory *f, DWORD cookie) { (void)f; (void)cookie; return S_OK; }

static const FactoryVtbl g_factory_vtbl = {
    f_qi, f_addref, f_release, f_setpriv, f_setprivif, f_getpriv, f_getparent, f_enum,
    f_assoc, f_getassoc, f_swapchain, f_soft, f_enum, f_current,
    f_stereo, f_forhwnd, f_forcore, f_sharedluid, f_regwin, f_regevent, f_unreg, f_regwin, f_regevent, f_unreg, f_forcomp,
    f_flags, f_byluid, f_warp, f_feature, f_bypref, f_regevent, f_unregac,
};

static HRESULT own_factory(UINT flags, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    Factory *f = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->vtbl = &g_factory_vtbl;
    f->refs = 1;
    f->flags = flags;
    HRESULT hr = f_qi(f, iid, out);
    f_release(f);
    return hr;
}

/* ---- DXVK's, when installed ---------------------------------------------- */
static HMODULE dxvk(void)
{
    static HMODULE m;
    static LONG looked;
    if (!InterlockedCompareExchange(&looked, 1, 0)) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
        if (n && n < MAX_PATH - 16) {
            lstrcatA(path, "\\dxgi_dxvk.dll");
            m = LoadLibraryA(path);
        }
        InterlockedExchange(&looked, 2);
    }
    while (looked == 1) Sleep(0);
    return m;
}

/* Is @addr (a caller's return address) in the Vulkan loader? */
static BOOL from_vulkan_loader(void *addr)
{
    HMODULE m = NULL, v = GetModuleHandleA("vulkan-1.dll");
    return v && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)addr, &m) && m == v;
}

typedef HRESULT (WINAPI *CreateFn)(const GUID *, void **);
typedef HRESULT (WINAPI *Create2Fn)(UINT, const GUID *, void **);

static FARPROC dxvk_proc(void *caller, const char *name)
{
    if (from_vulkan_loader(caller)) return NULL;
    HMODULE m = dxvk();
    return m ? GetProcAddress(m, name) : NULL;
}

DXGIAPI HRESULT WINAPI CreateDXGIFactory(const GUID *iid, void **out)
{
    CreateFn fn = (CreateFn)dxvk_proc(__builtin_return_address(0), "CreateDXGIFactory");
    return fn ? fn(iid, out) : own_factory(0, iid, out);
}

DXGIAPI HRESULT WINAPI CreateDXGIFactory1(const GUID *iid, void **out)
{
    CreateFn fn = (CreateFn)dxvk_proc(__builtin_return_address(0), "CreateDXGIFactory1");
    return fn ? fn(iid, out) : own_factory(0, iid, out);
}

DXGIAPI HRESULT WINAPI CreateDXGIFactory2(UINT flags, const GUID *iid, void **out)
{
    Create2Fn fn = (Create2Fn)dxvk_proc(__builtin_return_address(0), "CreateDXGIFactory2");
    return fn ? fn(flags, iid, out) : own_factory(flags, iid, out);
}

DXGIAPI HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, const GUID *iid, void **out)
{
    Create2Fn fn = (Create2Fn)dxvk_proc(__builtin_return_address(0), "DXGIGetDebugInterface1");
    if (fn) return fn(flags, iid, out);
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

DXGIAPI HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void)
{
    typedef HRESULT (WINAPI *Fn)(void);
    Fn fn = (Fn)dxvk_proc(__builtin_return_address(0), "DXGIDeclareAdapterRemovalSupport");
    return fn ? fn() : S_OK;
}
