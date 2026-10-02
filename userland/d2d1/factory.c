/*
 * factory.c — Direct2D's factory, drawing state blocks, memory and matrix
 * helpers, and the exported math functions
 */
#include "d2d_int.h"
#include <windows.h>

int _fltused = 0x9875;      /* the compiler references it for float code */

void *d_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
void *d_realloc(void *p, size_t n)
{
    if (!p) return d_alloc(n);
    return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n ? n : 1);
}
void d_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* ---- matrices ---- */
const MAT MAT_IDENTITY = { 1, 0, 0, 1, 0, 0 };

MAT mat_mul(const MAT *a, const MAT *b)
{
    MAT r;
    r.m11 = a->m11 * b->m11 + a->m12 * b->m21;
    r.m12 = a->m11 * b->m12 + a->m12 * b->m22;
    r.m21 = a->m21 * b->m11 + a->m22 * b->m21;
    r.m22 = a->m21 * b->m12 + a->m22 * b->m22;
    r.dx = a->dx * b->m11 + a->dy * b->m21 + b->dx;
    r.dy = a->dx * b->m12 + a->dy * b->m22 + b->dy;
    return r;
}

PT mat_apply(const MAT *m, PT p)
{
    PT r = { p.x * m->m11 + p.y * m->m21 + m->dx, p.x * m->m12 + p.y * m->m22 + m->dy };
    return r;
}

BOOL mat_invert(const MAT *m, MAT *out)
{
    float det = m->m11 * m->m22 - m->m12 * m->m21;
    if (det == 0.0f || !isfinite(det)) return FALSE;
    MAT r;
    r.m11 = m->m22 / det;  r.m12 = -m->m12 / det;
    r.m21 = -m->m21 / det; r.m22 = m->m11 / det;
    r.dx = -(m->dx * r.m11 + m->dy * r.m21);
    r.dy = -(m->dx * r.m12 + m->dy * r.m22);
    *out = r;
    return TRUE;
}

int mat_is_identity(const MAT *m)
{
    return m->m11 == 1 && m->m12 == 0 && m->m21 == 0 && m->m22 == 1 && m->dx == 0 && m->dy == 0;
}

/* ---- drawing state blocks ---- */
typedef struct {
    const void *vtbl;
    LONG ref;
    void *factory;
    STATEDESC d;
    void *params;           /* IDWriteRenderingParams */
} StateBlock;

static HRESULT STDMETHODCALLTYPE sb_qi(StateBlock *b, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Resource) ||
        IsEqualIID(riid, &IID_ID2D1DrawingStateBlock)) {
        *out = b;
        InterlockedIncrement(&b->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sb_addref(StateBlock *b) { return InterlockedIncrement(&b->ref); }
static ULONG STDMETHODCALLTYPE sb_release(StateBlock *b)
{
    LONG r = InterlockedDecrement(&b->ref);
    if (!r) {
        COM_RELEASE(b->params);
        factory_release(b->factory);
        d_free(b);
    }
    return r;
}
static void STDMETHODCALLTYPE sb_factory(StateBlock *b, void **f) { *f = b->factory; factory_addref(b->factory); }
static void STDMETHODCALLTYPE sb_get(StateBlock *b, STATEDESC *d) { *d = b->d; }
static void STDMETHODCALLTYPE sb_set(StateBlock *b, const STATEDESC *d) { b->d = *d; }
static void STDMETHODCALLTYPE sb_set_params(StateBlock *b, void *p)
{
    if (p) COM_ADDREF(p);
    COM_RELEASE(b->params);
    b->params = p;
}
static void STDMETHODCALLTYPE sb_get_params(StateBlock *b, void **p)
{
    *p = b->params;
    if (b->params) COM_ADDREF(b->params);
}
static const void *const sb_vtbl[] = { sb_qi, sb_addref, sb_release, sb_factory, sb_get, sb_set, sb_set_params,
                                       sb_get_params };

void state_block_get(void *iface, STATEDESC *d, void **params)
{
    StateBlock *b = iface;
    *d = b->d;
    *params = b->params;
}
void state_block_set(void *iface, const STATEDESC *d, void *params)
{
    StateBlock *b = iface;
    b->d = *d;
    sb_set_params(b, params);
}

/* ---- the factory ---- */
void factory_addref(void *f) { InterlockedIncrement(&((Factory *)f)->ref); }
void factory_release(void *f)
{
    if (f && !InterlockedDecrement(&((Factory *)f)->ref)) d_free(f);
}

static HRESULT STDMETHODCALLTYPE f_qi(Factory *f, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Factory)) {
        *out = f;
        factory_addref(f);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE f_addref(Factory *f) { return InterlockedIncrement(&f->ref); }
static ULONG STDMETHODCALLTYPE f_release(Factory *f)
{
    LONG r = InterlockedDecrement(&f->ref);
    if (!r) d_free(f);
    return r;
}
static HRESULT STDMETHODCALLTYPE f_reload(Factory *f) { (void)f; return S_OK; }

static float desktop_dpi(void)
{
    HDC dc = GetDC(0);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(0, dc);
    return d > 0 ? (float)d : 96.0f;
}
static void STDMETHODCALLTYPE f_dpi(Factory *f, float *x, float *y)
{
    (void)f;
    float d = desktop_dpi();
    if (x) *x = d;
    if (y) *y = d;
}

static HRESULT new_geometry(Factory *f, int kind, Geometry **g, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    HRESULT hr = geometry_create(f, kind, g);
    if (FAILED(hr)) return hr;
    *out = *g;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE f_rect(Factory *f, const RCF *r, void **out)
{
    Geometry *g;
    HRESULT hr = new_geometry(f, G_RECT, &g, out);
    if (FAILED(hr)) return hr;
    g->rect = *r;
    path_rect(&g->path, r);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_rrect(Factory *f, const RRECT *r, void **out)
{
    Geometry *g;
    HRESULT hr = new_geometry(f, G_RRECT, &g, out);
    if (FAILED(hr)) return hr;
    g->rrect = *r;
    path_rrect(&g->path, r);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_ellipse(Factory *f, const ELLIPSE_ *e, void **out)
{
    Geometry *g;
    HRESULT hr = new_geometry(f, G_ELLIPSE, &g, out);
    if (FAILED(hr)) return hr;
    g->ell = *e;
    path_ellipse(&g->path, e);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_group(Factory *f, UINT32 fill, void **geoms, UINT32 n, void **out)
{
    Geometry *g;
    HRESULT hr = new_geometry(f, G_GROUP, &g, out);
    if (FAILED(hr)) return hr;
    g->children = d_alloc(sizeof(*g->children) * (n ? n : 1));
    for (UINT32 i = 0; i < n; i++) {
        Geometry *c = geometry_of(geoms[i]);
        if (!c) continue;
        COM_ADDREF(geoms[i]);
        g->children[g->nchildren++] = c;
        path_copy(&g->path, geometry_path(c), &MAT_IDENTITY);
    }
    g->path.fill_mode = fill;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_transformed(Factory *f, void *src, const MAT *m, void **out)
{
    Geometry *s = geometry_of(src), *g;
    if (!s) return E_INVALIDARG;
    HRESULT hr = new_geometry(f, G_TRANSFORMED, &g, out);
    if (FAILED(hr)) return hr;
    COM_ADDREF(src);
    g->source = s;
    g->transform = m ? *m : MAT_IDENTITY;
    path_copy(&g->path, geometry_path(s), &g->transform);
    g->path.fill_mode = geometry_path(s)->fill_mode;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_path(Factory *f, void **out)
{
    Geometry *g;
    return new_geometry(f, G_PATH, &g, out);
}
static HRESULT STDMETHODCALLTYPE f_stroke(Factory *f, const STROKEPROPS *p, const float *dashes, UINT32 n, void **out)
{
    if (!out) return E_POINTER;
    return stroke_style_create(f, p, dashes, n, out);
}
static HRESULT STDMETHODCALLTYPE f_state(Factory *f, const STATEDESC *d, void *params, void **out)
{
    if (!out) return E_POINTER;
    StateBlock *b = d_alloc(sizeof(*b));
    if (!b) return E_OUTOFMEMORY;
    b->vtbl = sb_vtbl;
    b->ref = 1;
    b->factory = f;
    factory_addref(f);
    if (d) b->d = *d;
    else b->d.transform = MAT_IDENTITY;
    sb_set_params(b, params);
    *out = b;
    return S_OK;
}

/* IWICBitmap: a bitmap render target that writes back into the WIC bitmap
   isn't supported; programs get the error and draw another way */
static HRESULT STDMETHODCALLTYPE f_wic_rt(Factory *f, void *wic, const RTPROPS *p, void **out)
{
    (void)f; (void)wic; (void)p;
    if (out) *out = 0;
    return D2DERR_UNSUPPORTED_OPERATION;
}

static HRESULT STDMETHODCALLTYPE f_hwnd_rt(Factory *f, const RTPROPS *p, const HWNDRTPROPS *hp, void **out)
{
    if (!out || !hp) return E_POINTER;
    *out = 0;
    if (!IsWindow(hp->hwnd)) return E_INVALIDARG;
    Target *t;
    HRESULT hr = target_create(f, TGT_HWND, p, &t);
    if (FAILED(hr)) return hr;
    t->hwnd = hp->hwnd;
    int w = hp->pixelSize.width, h = hp->pixelSize.height;
    if (!w || !h) {
        RECT rc;
        GetClientRect(hp->hwnd, &rc);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }
    hr = target_resize(t, w, h);
    if (FAILED(hr)) { COM_RELEASE(t); return hr; }
    *out = t;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_dxgi_rt(Factory *f, void *surface, const RTPROPS *p, void **out)
{
    (void)f; (void)surface; (void)p;
    if (out) *out = 0;
    return D2DERR_UNSUPPORTED_OPERATION;
}
static HRESULT STDMETHODCALLTYPE f_dc_rt(Factory *f, const RTPROPS *p, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Target *t;
    HRESULT hr = target_create(f, TGT_DC, p, &t);
    if (FAILED(hr)) return hr;
    *out = t;
    return S_OK;
}

static const void *const f_vtbl[] = {
    f_qi, f_addref, f_release, f_reload, f_dpi, f_rect, f_rrect, f_ellipse, f_group, f_transformed, f_path,
    f_stroke, f_state, f_wic_rt, f_hwnd_rt, f_dxgi_rt, f_dc_rt,
};

D2D HRESULT WINAPI D2D1CreateFactory(UINT32 type, REFIID iid, const void *opts, void **out)
{
    (void)opts;
    if (!out) return E_POINTER;
    *out = 0;
    if (type > 1) return E_INVALIDARG;
    Factory *f = d_alloc(sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->vtbl = f_vtbl;
    f->ref = 1;
    f->type = type;
    HRESULT hr = f_qi(f, iid, out);
    f_release(f);
    return hr;
}

D2D HRESULT WINAPI D2D1CreateDevice(void *dxgi, const void *props, void **out)
{
    (void)dxgi; (void)props;
    if (out) *out = 0;
    return E_NOTIMPL;
}
D2D HRESULT WINAPI D2D1CreateDeviceContext(void *surface, const void *props, void **out)
{
    (void)surface; (void)props;
    if (out) *out = 0;
    return E_NOTIMPL;
}

/* ---- exported helpers ---- */
D2D void WINAPI D2D1MakeRotateMatrix(float angle, PT c, MAT *m)
{
    float a = angle * 3.14159265358979f / 180.0f, s = sinf(a), k = cosf(a);
    m->m11 = k;  m->m12 = s;
    m->m21 = -s; m->m22 = k;
    m->dx = c.x - c.x * k + c.y * s;
    m->dy = c.y - c.x * s - c.y * k;
}
D2D void WINAPI D2D1MakeSkewMatrix(float ax, float ay, PT c, MAT *m)
{
    float tx = tanf(ax * 3.14159265358979f / 180.0f), ty = tanf(ay * 3.14159265358979f / 180.0f);
    m->m11 = 1;  m->m12 = ty;
    m->m21 = tx; m->m22 = 1;
    m->dx = -c.y * tx;
    m->dy = -c.x * ty;
}
D2D BOOL WINAPI D2D1IsMatrixInvertible(const MAT *m)
{
    return m->m11 * m->m22 - m->m12 * m->m21 != 0.0f;
}
D2D BOOL WINAPI D2D1InvertMatrix(MAT *m)
{
    return mat_invert(m, m);
}
D2D void WINAPI D2D1SinCos(float a, float *s, float *c) { *s = sinf(a); *c = cosf(a); }
D2D float WINAPI D2D1Tan(float a) { return tanf(a); }
D2D float WINAPI D2D1Vec3Length(float x, float y, float z) { return sqrtf(x * x + y * y + z * z); }
D2D COLORF WINAPI D2D1ConvertColorSpace(UINT32 src, UINT32 dst, const COLORF *c)
{
    (void)src; (void)dst;
    return *c;
}
D2D float WINAPI D2D1ComputeMaximumScaleFactor(const MAT *m)
{
    float a = m->m11 * m->m11 + m->m12 * m->m12, b = m->m11 * m->m21 + m->m12 * m->m22;
    float d = m->m21 * m->m21 + m->m22 * m->m22;
    float t = (a + d) / 2, q = sqrtf((a - d) * (a - d) / 4 + b * b);
    return sqrtf(t + q);
}
