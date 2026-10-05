/*
 * soft.c — d3d11.dll's software device, on the Basic Render Driver.
 *
 * What a program without a GPU gets from D3D11CreateDevice with
 * D3D_DRIVER_TYPE_WARP (or the Basic Render Driver adapter) when DXVK is
 * not installed: a device of feature level 9_1 whose textures live in
 * memory, that the CPU creates, maps, updates and copies, and that
 * dxgi.dll's swap chains present (novadx.h).  That is the whole of what
 * Chromium's software compositor does with it: it draws each frame into a
 * mapped staging texture with Skia, copies the changed part into the swap
 * chain's buffer and presents.  The device does not rasterize: shaders,
 * views and draws are not implemented, and only a request that accepts
 * feature level 9_1 gets it (ANGLE asks for 9_3 and up, so it never takes
 * this device for a renderer).
 *
 * 64-bit only: the device's and context's ~160 unimplemented methods share
 * one stub, which only the x64 calling convention allows.
 */
#include <windows.h>
#include <novadx.h>

#define D3D_FEATURE_LEVEL_9_1_              0x9100
#define D3D11_RESOURCE_DIMENSION_TEXTURE2D_ 3
#define D3D11_FORMAT_SUPPORT_TEXTURE2D_     0x20
#define D3D11_FORMAT_SUPPORT_CPU_LOCKABLE_  0x40000000

HRESULT soft_create_device(UINT flags, void **device, UINT *level, void **context);

#ifdef _WIN64

typedef struct Device Device;
typedef struct Texture Texture;

static UINT format_bytes(UINT f)
{
    switch (f) {
    case NDXGI_FORMAT_R8G8B8A8_TYPELESS: case NDXGI_FORMAT_R8G8B8A8_UNORM: case NDXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case NDXGI_FORMAT_B8G8R8A8_UNORM: case NDXGI_FORMAT_B8G8R8X8_UNORM: case NDXGI_FORMAT_B8G8R8A8_TYPELESS:
    case NDXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case NDXGI_FORMAT_R10G10B10A2_UNORM: case NDXGI_FORMAT_R32_FLOAT:
        return 4;
    case NDXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case NDXGI_FORMAT_R8G8_UNORM: return 2;
    case NDXGI_FORMAT_R8_UNORM: case NDXGI_FORMAT_A8_UNORM: return 1;
    }
    return 0;
}

static HRESULT STDMETHODCALLTYPE notimpl(void) { return E_NOTIMPL; }

/* ---- The device ----------------------------------------------------------- */
struct Device {
    void *const *vtbl;                  /* ID3D11Device */
    void *const *dxgi;                  /* IDXGIDevice1 */
    void *const *ctx;                   /* its immediate ID3D11DeviceContext */
    LONG refs;
    UINT flags;
};
#define DEV_OF_DXGI(p) ((Device *)((BYTE *)(p) - offsetof(Device, dxgi)))
#define DEV_OF_CTX(p)  ((Device *)((BYTE *)(p) - offsetof(Device, ctx)))

/* ---- Textures ------------------------------------------------------------- */
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Texture *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Texture *);
    ULONG   (STDMETHODCALLTYPE *Release)(Texture *);
    void    (STDMETHODCALLTYPE *GetDevice)(Texture *, void **);
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(Texture *, const GUID *, UINT *, void *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(Texture *, const GUID *, UINT, const void *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(Texture *, const GUID *, const IUnknown *);
    void    (STDMETHODCALLTYPE *GetType)(Texture *, UINT *);
    void    (STDMETHODCALLTYPE *SetEvictionPriority)(Texture *, UINT);
    UINT    (STDMETHODCALLTYPE *GetEvictionPriority)(Texture *);
    void    (STDMETHODCALLTYPE *GetDesc)(Texture *, ND3D11_TEXTURE2D_DESC *);
} TextureVtbl;

struct Texture {
    const TextureVtbl *vtbl;
    INovaSoftTexture nova;
    LONG refs;
    Device *dev;
    ND3D11_TEXTURE2D_DESC desc;
    UINT bpp, pitch, priority;
    BYTE *bits;                         /* (the first subresource only) */
};
#define TEX_OF_NOVA(p) ((Texture *)((BYTE *)(p) - offsetof(Texture, nova)))

static ULONG STDMETHODCALLTYPE d_addref(Device *d);
static ULONG STDMETHODCALLTYPE d_release(Device *d);

static HRESULT STDMETHODCALLTYPE t_qi(Texture *t, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (novadx_same(iid, &NIID_IUnknown) || novadx_same(iid, &NIID_ID3D11DeviceChild) ||
        novadx_same(iid, &NIID_ID3D11Resource) || novadx_same(iid, &NIID_ID3D11Texture2D)) {
        InterlockedIncrement(&t->refs);
        *out = t;
        return S_OK;
    }
    if (novadx_same(iid, &NIID_INovaSoftTexture)) {
        InterlockedIncrement(&t->refs);
        *out = &t->nova;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE t_addref(Texture *t) { return (ULONG)InterlockedIncrement(&t->refs); }
static ULONG STDMETHODCALLTYPE t_release(Texture *t)
{
    LONG r = InterlockedDecrement(&t->refs);
    if (!r) {
        VirtualFree(t->bits, 0, MEM_RELEASE);
        d_release(t->dev);
        HeapFree(GetProcessHeap(), 0, t);
    }
    return (ULONG)r;
}
static void STDMETHODCALLTYPE t_getdevice(Texture *t, void **out) { if (out) { d_addref(t->dev); *out = t->dev; } }
static HRESULT STDMETHODCALLTYPE t_getpriv(Texture *t, const GUID *g, UINT *n, void *d)
{ (void)t; (void)g; (void)d; if (n) *n = 0; return NDXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE t_setpriv(Texture *t, const GUID *g, UINT n, const void *d) { (void)t; (void)g; (void)n; (void)d; return S_OK; }
static HRESULT STDMETHODCALLTYPE t_setprivif(Texture *t, const GUID *g, const IUnknown *u) { (void)t; (void)g; (void)u; return S_OK; }
static void STDMETHODCALLTYPE t_gettype(Texture *t, UINT *dim) { (void)t; if (dim) *dim = D3D11_RESOURCE_DIMENSION_TEXTURE2D_; }
static void STDMETHODCALLTYPE t_setevict(Texture *t, UINT p) { t->priority = p; }
static UINT STDMETHODCALLTYPE t_getevict(Texture *t) { return t->priority; }
static void STDMETHODCALLTYPE t_getdesc(Texture *t, ND3D11_TEXTURE2D_DESC *d) { if (d) *d = t->desc; }
static const TextureVtbl g_tex_vtbl = {
    t_qi, t_addref, t_release, t_getdevice, t_getpriv, t_setpriv, t_setprivif, t_gettype, t_setevict, t_getevict, t_getdesc,
};

static HRESULT STDMETHODCALLTYPE tn_qi(INovaSoftTexture *n, const GUID *iid, void **out) { return t_qi(TEX_OF_NOVA(n), iid, out); }
static ULONG STDMETHODCALLTYPE tn_addref(INovaSoftTexture *n) { return t_addref(TEX_OF_NOVA(n)); }
static ULONG STDMETHODCALLTYPE tn_release(INovaSoftTexture *n) { return t_release(TEX_OF_NOVA(n)); }
static BYTE *STDMETHODCALLTYPE tn_bits(INovaSoftTexture *n, UINT *pitch, ND3D11_TEXTURE2D_DESC *desc)
{
    Texture *t = TEX_OF_NOVA(n);
    if (pitch) *pitch = t->pitch;
    if (desc) *desc = t->desc;
    return t->bits;
}
static const INovaSoftTextureVtbl g_tex_nova_vtbl = { tn_qi, tn_addref, tn_release, tn_bits };

/* @r as one of our textures (NULL: another kind of resource) */
static Texture *tex_of(void *r)
{
    Texture *t = r;
    return t && t->vtbl == &g_tex_vtbl ? t : NULL;
}

/* ---- ID3D11Device --------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE d_qi(Device *d, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (novadx_same(iid, &NIID_IUnknown) || novadx_same(iid, &NIID_ID3D11Device) || novadx_same(iid, &NIID_INovaSoftDevice)) {
        InterlockedIncrement(&d->refs);
        *out = d;
        return S_OK;
    }
    if (novadx_same(iid, &NIID_IDXGIObject) || novadx_same(iid, &NIID_IDXGIDevice) || novadx_same(iid, &NIID_IDXGIDevice1)) {
        InterlockedIncrement(&d->refs);
        *out = &d->dxgi;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE d_addref(Device *d) { return (ULONG)InterlockedIncrement(&d->refs); }
static ULONG STDMETHODCALLTYPE d_release(Device *d)
{
    LONG r = InterlockedDecrement(&d->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, d);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE d_tex2d(Device *d, const ND3D11_TEXTURE2D_DESC *desc, const ND3D11_SUBRESOURCE_DATA *init,
                                         void **out)
{
    if (!desc) return E_INVALIDARG;
    UINT bpp = format_bytes(desc->Format);
    if (!bpp || !desc->Width || !desc->Height || desc->Width > 16384 || desc->Height > 16384 || desc->SampleDesc.Count > 1)
        return E_INVALIDARG;
    if (!out) return S_FALSE;                               /* (only checking the description) */
    *out = NULL;
    Texture *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t));
    if (!t) return E_OUTOFMEMORY;
    t->pitch = (desc->Width * bpp + 15) & ~15u;
    t->bits = VirtualAlloc(NULL, (SIZE_T)t->pitch * desc->Height, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!t->bits) { HeapFree(GetProcessHeap(), 0, t); return E_OUTOFMEMORY; }
    t->vtbl = &g_tex_vtbl;
    t->nova.lpVtbl = &g_tex_nova_vtbl;
    t->refs = 1;
    t->dev = d;
    d_addref(d);
    t->desc = *desc;
    if (!t->desc.MipLevels) t->desc.MipLevels = 1;
    t->bpp = bpp;
    if (init && init->pSysMem) {
        UINT row = desc->Width * bpp, sp = init->SysMemPitch ? init->SysMemPitch : row;
        for (UINT y = 0; y < desc->Height; y++)
            __builtin_memcpy(t->bits + (size_t)y * t->pitch, (const BYTE *)init->pSysMem + (size_t)y * sp, row);
    }
    *out = t;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_fmt(Device *d, UINT fmt, UINT *support)
{
    (void)d;
    if (!support) return E_INVALIDARG;
    *support = format_bytes(fmt) ? D3D11_FORMAT_SUPPORT_TEXTURE2D_ | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE_ : 0;
    return *support ? S_OK : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE d_msaa(Device *d, UINT fmt, UINT count, UINT *levels)
{
    (void)d;
    if (!levels) return E_INVALIDARG;
    *levels = count == 1 && format_bytes(fmt) ? 1 : 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_feature(Device *d, UINT f, void *data, UINT size) { (void)d; (void)f; (void)data; (void)size; return E_INVALIDARG; }
static HRESULT STDMETHODCALLTYPE d_getpriv(Device *d, const GUID *g, UINT *n, void *p)
{ (void)d; (void)g; (void)p; if (n) *n = 0; return NDXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE d_setpriv(Device *d, const GUID *g, UINT n, const void *p) { (void)d; (void)g; (void)n; (void)p; return S_OK; }
static HRESULT STDMETHODCALLTYPE d_setprivif(Device *d, const GUID *g, const IUnknown *u) { (void)d; (void)g; (void)u; return S_OK; }
static UINT STDMETHODCALLTYPE d_level(Device *d) { (void)d; return D3D_FEATURE_LEVEL_9_1_; }
static UINT STDMETHODCALLTYPE d_flags(Device *d) { return d->flags; }
static HRESULT STDMETHODCALLTYPE d_removed(Device *d) { (void)d; return S_OK; }
static void STDMETHODCALLTYPE d_context(Device *d, void **out) { if (out) { d_addref(d); *out = &d->ctx; } }
static HRESULT STDMETHODCALLTYPE d_setexc(Device *d, UINT f) { (void)d; (void)f; return S_OK; }
static UINT STDMETHODCALLTYPE d_getexc(Device *d) { (void)d; return 0; }

static void *const g_dev_vtbl[43] = {
    [0 ... 42] = (void *)notimpl,
    [0] = (void *)d_qi, [1] = (void *)d_addref, [2] = (void *)d_release,
    [5] = (void *)d_tex2d,
    [29] = (void *)d_fmt, [30] = (void *)d_msaa, [33] = (void *)d_feature,
    [34] = (void *)d_getpriv, [35] = (void *)d_setpriv, [36] = (void *)d_setprivif,
    [37] = (void *)d_level, [38] = (void *)d_flags, [39] = (void *)d_removed, [40] = (void *)d_context,
    [41] = (void *)d_setexc, [42] = (void *)d_getexc,
};

/* ---- IDXGIDevice1 --------------------------------------------------------- */
static HRESULT warp_adapter(const GUID *iid, void **out)
{
    typedef HRESULT (WINAPI *CreateFn)(const GUID *, void **);
    *out = NULL;
    HMODULE m = LoadLibraryW(L"dxgi.dll");
    CreateFn create = m ? (CreateFn)GetProcAddress(m, "CreateDXGIFactory1") : NULL;
    IUnknown *f = NULL;
    if (!create || FAILED(create(&NIID_IDXGIFactory1, (void **)&f))) return E_FAIL;
    typedef HRESULT (STDMETHODCALLTYPE *EnumFn)(IUnknown *, UINT, IUnknown **);
    IUnknown *a = NULL;
    HRESULT hr = ((EnumFn)((void **)*(void **)f)[7])(f, 0, &a);  /* EnumAdapters(0): the Basic Render Driver */
    f->lpVtbl->Release(f);
    if (FAILED(hr)) return hr;
    hr = a->lpVtbl->QueryInterface(a, iid, out);
    a->lpVtbl->Release(a);
    return hr;
}

static HRESULT STDMETHODCALLTYPE x_qi(void *p, const GUID *iid, void **out) { return d_qi(DEV_OF_DXGI(p), iid, out); }
static ULONG STDMETHODCALLTYPE x_addref(void *p) { return d_addref(DEV_OF_DXGI(p)); }
static ULONG STDMETHODCALLTYPE x_release(void *p) { return d_release(DEV_OF_DXGI(p)); }
static HRESULT STDMETHODCALLTYPE x_setpriv(void *p, const GUID *g, UINT n, const void *d) { (void)p; (void)g; (void)n; (void)d; return S_OK; }
static HRESULT STDMETHODCALLTYPE x_setprivif(void *p, const GUID *g, const IUnknown *u) { (void)p; (void)g; (void)u; return S_OK; }
static HRESULT STDMETHODCALLTYPE x_getpriv(void *p, const GUID *g, UINT *n, void *d)
{ (void)p; (void)g; (void)d; if (n) *n = 0; return NDXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE x_getparent(void *p, const GUID *iid, void **out) { (void)p; if (!out) return E_POINTER; return warp_adapter(iid, out); }
static HRESULT STDMETHODCALLTYPE x_getadapter(void *p, void **out) { (void)p; if (!out) return E_POINTER; return warp_adapter(&NIID_IDXGIAdapter, out); }
static HRESULT STDMETHODCALLTYPE x_residency(void *p, IUnknown *const *res, UINT *status, UINT n)
{
    (void)p; (void)res;
    if (!status) return E_INVALIDARG;
    for (UINT i = 0; i < n; i++) status[i] = 1;             /* DXGI_RESIDENCY_FULLY_RESIDENT */
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE x_setprio(void *p, INT prio) { (void)p; return prio < -7 || prio > 7 ? E_INVALIDARG : S_OK; }
static HRESULT STDMETHODCALLTYPE x_getprio(void *p, INT *prio) { (void)p; if (!prio) return E_POINTER; *prio = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE x_setlat(void *p, UINT n) { (void)p; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE x_getlat(void *p, UINT *n) { (void)p; if (!n) return E_POINTER; *n = 3; return S_OK; }

static void *const g_dxgi_vtbl[14] = {
    (void *)x_qi, (void *)x_addref, (void *)x_release, (void *)x_setpriv, (void *)x_setprivif, (void *)x_getpriv,
    (void *)x_getparent, (void *)x_getadapter, (void *)notimpl, (void *)x_residency, (void *)x_setprio, (void *)x_getprio,
    (void *)x_setlat, (void *)x_getlat,
};

/* ---- ID3D11DeviceContext -------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE c_qi(void *p, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (novadx_same(iid, &NIID_IUnknown) || novadx_same(iid, &NIID_ID3D11DeviceChild) || novadx_same(iid, &NIID_ID3D11DeviceContext)) {
        d_addref(DEV_OF_CTX(p));
        *out = p;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE c_addref(void *p) { return d_addref(DEV_OF_CTX(p)); }
static ULONG STDMETHODCALLTYPE c_release(void *p) { return d_release(DEV_OF_CTX(p)); }
static void STDMETHODCALLTYPE c_getdevice(void *p, void **out) { if (out) { Device *d = DEV_OF_CTX(p); d_addref(d); *out = d; } }

static HRESULT STDMETHODCALLTYPE c_map(void *p, void *res, UINT sub, UINT type, UINT flags, ND3D11_MAPPED_SUBRESOURCE *m)
{
    (void)p; (void)type; (void)flags;
    Texture *t = tex_of(res);
    if (!t || sub || !m) { if (m) __builtin_memset(m, 0, sizeof(*m)); return E_INVALIDARG; }
    m->pData = t->bits;
    m->RowPitch = t->pitch;
    m->DepthPitch = t->pitch * t->desc.Height;
    return S_OK;
}
static void STDMETHODCALLTYPE c_unmap(void *p, void *res, UINT sub) { (void)p; (void)res; (void)sub; }

/* Copy @w x @h pixels from (@sx, @sy) of @s to (@dx, @dy) of @d, clipped */
static void copy_rect(Texture *d, int dx, int dy, Texture *s, int sx, int sy, int w, int h)
{
    if (!d || !s || d->bpp != s->bpp) return;
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > (int)s->desc.Width) w = (int)s->desc.Width - sx;
    if (sy + h > (int)s->desc.Height) h = (int)s->desc.Height - sy;
    if (dx + w > (int)d->desc.Width) w = (int)d->desc.Width - dx;
    if (dy + h > (int)d->desc.Height) h = (int)d->desc.Height - dy;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++)
        __builtin_memmove(d->bits + (size_t)(dy + y) * d->pitch + (size_t)dx * d->bpp,
                          s->bits + (size_t)(sy + y) * s->pitch + (size_t)sx * s->bpp, (size_t)w * d->bpp);
}

static void STDMETHODCALLTYPE c_copyregion(void *p, void *dst, UINT dsub, UINT x, UINT y, UINT z, void *src, UINT ssub,
                                           const ND3D11_BOX *box)
{
    (void)p; (void)z;
    Texture *d = tex_of(dst), *s = tex_of(src);
    if (!d || !s || dsub || ssub) return;
    int sx = 0, sy = 0, w = (int)s->desc.Width, h = (int)s->desc.Height;
    if (box) {
        if (box->right <= box->left || box->bottom <= box->top) return;
        sx = (int)box->left; sy = (int)box->top; w = (int)(box->right - box->left); h = (int)(box->bottom - box->top);
    }
    copy_rect(d, (int)x, (int)y, s, sx, sy, w, h);
}
static void STDMETHODCALLTYPE c_copy(void *p, void *dst, void *src)
{
    (void)p;
    Texture *d = tex_of(dst), *s = tex_of(src);
    if (d && s && d->desc.Width == s->desc.Width && d->desc.Height == s->desc.Height)
        copy_rect(d, 0, 0, s, 0, 0, (int)s->desc.Width, (int)s->desc.Height);
}
static void STDMETHODCALLTYPE c_update(void *p, void *dst, UINT sub, const ND3D11_BOX *box, const void *data, UINT pitch, UINT dpitch)
{
    (void)p; (void)dpitch;
    Texture *d = tex_of(dst);
    if (!d || sub || !data) return;
    int x = 0, y = 0, w = (int)d->desc.Width, h = (int)d->desc.Height;
    if (box) {
        if (box->right <= box->left || box->bottom <= box->top) return;
        x = (int)box->left; y = (int)box->top; w = (int)(box->right - box->left); h = (int)(box->bottom - box->top);
    }
    if (x + w > (int)d->desc.Width) w = (int)d->desc.Width - x;
    if (y + h > (int)d->desc.Height) h = (int)d->desc.Height - y;
    if (!pitch) pitch = (UINT)w * d->bpp;
    for (int r = 0; r < h; r++)
        __builtin_memcpy(d->bits + (size_t)(y + r) * d->pitch + (size_t)x * d->bpp, (const BYTE *)data + (size_t)r * pitch,
                         (size_t)w * d->bpp);
}
static void STDMETHODCALLTYPE c_void(void *p) { (void)p; }
static UINT STDMETHODCALLTYPE c_zero(void *p) { (void)p; return 0; }    /* GetType (immediate), GetContextFlags */

static void *const g_ctx_vtbl[115] = {
    [0 ... 114] = (void *)notimpl,
    [0] = (void *)c_qi, [1] = (void *)c_addref, [2] = (void *)c_release, [3] = (void *)c_getdevice,
    [4] = (void *)d_getpriv, [5] = (void *)d_setpriv, [6] = (void *)d_setprivif,
    [14] = (void *)c_map, [15] = (void *)c_unmap,
    [46] = (void *)c_copyregion, [47] = (void *)c_copy, [48] = (void *)c_update,
    [110] = (void *)c_void, [111] = (void *)c_void, [112] = (void *)c_zero, [113] = (void *)c_zero,
};

HRESULT soft_create_device(UINT flags, void **device, UINT *level, void **context)
{
    if (level) *level = D3D_FEATURE_LEVEL_9_1_;
    if (!device && !context) return S_FALSE;               /* (only asking whether it would work) */
    Device *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) return E_OUTOFMEMORY;
    d->vtbl = g_dev_vtbl;
    d->dxgi = g_dxgi_vtbl;
    d->ctx = g_ctx_vtbl;
    d->refs = 1;
    d->flags = flags;
    if (context) d_context(d, context);
    if (device) *device = d;
    else d_release(d);
    return S_OK;
}

#else  /* 32-bit: no software device */

HRESULT soft_create_device(UINT flags, void **device, UINT *level, void **context)
{
    (void)flags;
    if (device) *device = NULL;
    if (context) *context = NULL;
    if (level) *level = 0;
    return NDXGI_ERROR_UNSUPPORTED;
}

#endif
