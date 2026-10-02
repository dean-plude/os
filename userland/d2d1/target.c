/*
 * target.c — render targets: HWND, DC and bitmap targets over one 32-bit
 * premultiplied surface (a DIB section for the first two), the drawing
 * calls, axis-aligned clips, layers, drawing state, meshes, and GDI interop
 *
 * A layer saves the pixels under it; drawing then goes straight to the
 * surface, and PopLayer blends the saved pixels back by 1 - opacity x mask
 * x opacity-brush alpha.  For source-over drawing that interpolation is
 * exactly compositing the layer's content over what was below.
 */
#include "d2d_int.h"
#include <windows.h>
#include <stddef.h>

extern const void *const rt_hwnd_vtbl[], *const rt_dc_vtbl[], *const rt_bitmap_vtbl[], *const gdi_vtbl[];

Target *target_of(void *iface)
{
    if (!iface) return NULL;
    const void *v = *(const void **)iface;
    return v == rt_hwnd_vtbl || v == rt_dc_vtbl || v == rt_bitmap_vtbl ? iface : NULL;
}

/* ---- state helpers ---- */
MAT target_world(Target *t)
{
    MAT s = { t->dpix / 96, 0, 0, t->dpiy / 96, 0, 0 };
    return mat_mul(&t->transform, &s);
}

Clip target_clip(Target *t)
{
    if (t->nclips) return t->clips[t->nclips - 1];
    Clip c = { 0, 0, t->s.w, t->s.h };
    return c;
}

static void push_clip(Target *t, Clip c)
{
    Clip cur = target_clip(t);
    if (c.x0 < cur.x0) c.x0 = cur.x0;
    if (c.y0 < cur.y0) c.y0 = cur.y0;
    if (c.x1 > cur.x1) c.x1 = cur.x1;
    if (c.y1 > cur.y1) c.y1 = cur.y1;
    if (c.x1 < c.x0) c.x1 = c.x0;
    if (c.y1 < c.y0) c.y1 = c.y0;
    if (t->nclips < MAX_CLIPS) t->clips[t->nclips++] = c;
}

/* the device rectangle covering @r under the world transform */
static Clip device_rect(Target *t, const RCF *r)
{
    MAT w = target_world(t);
    PT p[4] = { { r->left, r->top }, { r->right, r->top }, { r->right, r->bottom }, { r->left, r->bottom } };
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int i = 0; i < 4; i++) {
        PT q = mat_apply(&w, p[i]);
        x0 = fminf(x0, q.x); x1 = fmaxf(x1, q.x);
        y0 = fminf(y0, q.y); y1 = fmaxf(y1, q.y);
    }
    Clip c;
    c.x0 = x0 < -1e8f ? -100000000 : (int)floorf(x0 + 0.5f);
    c.y0 = y0 < -1e8f ? -100000000 : (int)floorf(y0 + 0.5f);
    c.x1 = x1 > 1e8f ? 100000000 : (int)floorf(x1 + 0.5f);
    c.y1 = y1 > 1e8f ? 100000000 : (int)floorf(y1 + 0.5f);
    return c;
}

static int check(Target *t)
{
    if (!t->drawing) {
        if (SUCCEEDED(t->error)) t->error = D2DERR_WRONG_STATE;
        return 0;
    }
    return 1;
}

void target_fill(Target *t, const Path *path, void *brush, void *opacity_brush)
{
    if (!check(t)) return;
    Brush *b = brush_of(brush);
    if (!b) return;
    MAT w = target_world(t);
    Paint p, op;
    if (!brush_paint(b, &w, &p)) return;
    if (opacity_brush) {
        Brush *ob = brush_of(opacity_brush);
        if (!ob || !brush_paint(ob, &w, &op)) return;
        p.alpha = &op;
    }
    Clip c = target_clip(t);
    raster_fill(&t->s, &c, NULL, path, &w, t->aa == 1, &p);
}

void target_stroke(Target *t, const Path *path, void *brush, float width, void *style)
{
    if (!check(t)) return;
    Brush *b = brush_of(brush);
    if (!b) return;
    MAT w = target_world(t);
    Paint p;
    if (!brush_paint(b, &w, &p)) return;
    Clip c = target_clip(t);
    raster_stroke(&t->s, &c, NULL, path, &w, width, stroke_style_of(style), t->aa == 1, &p);
}

/* ---- surfaces ---- */
static void free_surface(Target *t)
{
    if (t->mdc) {
        if (t->old_bm) SelectObject(t->mdc, t->old_bm);
        DeleteDC(t->mdc);
    }
    if (t->dib) DeleteObject(t->dib);
    t->mdc = NULL;
    t->dib = NULL;
    t->old_bm = NULL;
    if (t->kind != TGT_BITMAP) memset(&t->s, 0, sizeof(t->s));
}

HRESULT target_resize(Target *t, int w, int h)
{
    if (t->kind == TGT_BITMAP) return D2DERR_UNSUPPORTED_OPERATION;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 16384 || h > 16384) return E_INVALIDARG;
    free_surface(t);
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    t->dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!t->dib || !bits) return E_OUTOFMEMORY;
    t->mdc = CreateCompatibleDC(NULL);
    if (!t->mdc) return E_OUTOFMEMORY;
    t->old_bm = SelectObject(t->mdc, t->dib);
    t->s.px = bits;
    t->s.w = w;
    t->s.h = h;
    t->s.stride = w;
    memset(bits, 0, (size_t)w * h * 4);
    return S_OK;
}

/* ---- ID2D1Layer ---- */
typedef struct { const void *vtbl; LONG ref; void *factory; SZ size; } LayerObj;
static HRESULT STDMETHODCALLTYPE ly_qi(LayerObj *l, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Resource) || IsEqualIID(riid, &IID_ID2D1Layer)) {
        *out = l;
        InterlockedIncrement(&l->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ly_addref(LayerObj *l) { return InterlockedIncrement(&l->ref); }
static ULONG STDMETHODCALLTYPE ly_release(LayerObj *l)
{
    LONG r = InterlockedDecrement(&l->ref);
    if (!r) { factory_release(l->factory); d_free(l); }
    return r;
}
static void STDMETHODCALLTYPE ly_factory(LayerObj *l, void **f) { *f = l->factory; factory_addref(l->factory); }
static SZ *STDMETHODCALLTYPE ly_size(LayerObj *l, SZ *r) { *r = l->size; return r; }
static const void *const ly_vtbl[] = { ly_qi, ly_addref, ly_release, ly_factory, ly_size };

/* ---- ID2D1Mesh and its tessellation sink ---- */
typedef struct { const void *vtbl; LONG ref; void *factory; Path path; } Mesh;
typedef struct { const void *vtbl; LONG ref; Mesh *mesh; int closed; } TessSink;
static const void *const mesh_vtbl[];

static HRESULT STDMETHODCALLTYPE me_qi(Mesh *m, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Resource) || IsEqualIID(riid, &IID_ID2D1Mesh)) {
        *out = m;
        InterlockedIncrement(&m->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE me_addref(Mesh *m) { return InterlockedIncrement(&m->ref); }
static ULONG STDMETHODCALLTYPE me_release(Mesh *m)
{
    LONG r = InterlockedDecrement(&m->ref);
    if (!r) { path_free(&m->path); factory_release(m->factory); d_free(m); }
    return r;
}
static void STDMETHODCALLTYPE me_factory(Mesh *m, void **f) { *f = m->factory; factory_addref(m->factory); }

static HRESULT STDMETHODCALLTYPE ts_qi(TessSink *s, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1TessellationSink)) {
        *out = s;
        InterlockedIncrement(&s->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ts_addref(TessSink *s) { return InterlockedIncrement(&s->ref); }
static ULONG STDMETHODCALLTYPE ts_release(TessSink *s)
{
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) { me_release(s->mesh); d_free(s); }
    return r;
}
static void STDMETHODCALLTYPE ts_add(TessSink *s, const TRIANGLE *t, UINT32 n)
{
    if (s->closed) return;
    for (UINT32 i = 0; i < n; i++) {
        PT a = t[i].point1, b = t[i].point2, c = t[i].point3;
        /* one orientation for all, so neighbours don't cancel */
        if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) < 0) { PT x = b; b = c; c = x; }
        path_begin(&s->mesh->path, a, 1);
        path_line(&s->mesh->path, b);
        path_line(&s->mesh->path, c);
        path_end(&s->mesh->path, 1);
    }
}
static HRESULT STDMETHODCALLTYPE ts_close(TessSink *s)
{
    if (s->closed) return D2DERR_WRONG_STATE;
    s->closed = 1;
    return S_OK;
}
static const void *const ts_vtbl[] = { ts_qi, ts_addref, ts_release, ts_add, ts_close };

static HRESULT STDMETHODCALLTYPE me_open(Mesh *m, void **out)
{
    if (!out) return E_POINTER;
    TessSink *s = d_alloc(sizeof(*s));
    if (!s) return E_OUTOFMEMORY;
    s->vtbl = ts_vtbl;
    s->ref = 1;
    s->mesh = m;
    me_addref(m);
    *out = s;
    return S_OK;
}
static const void *const mesh_vtbl[] = { me_qi, me_addref, me_release, me_factory, me_open };

/* ---- IUnknown, ID2D1Resource ---- */
static HRESULT STDMETHODCALLTYPE t_qi(Target *t, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    const GUID *kind = t->kind == TGT_HWND ? &IID_ID2D1HwndRenderTarget :
                       t->kind == TGT_DC ? &IID_ID2D1DCRenderTarget : &IID_ID2D1BitmapRenderTarget;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Resource) ||
        IsEqualIID(riid, &IID_ID2D1RenderTarget) || IsEqualIID(riid, kind)) {
        *out = t;
        InterlockedIncrement(&t->ref);
        return S_OK;
    }
    if (IsEqualIID(riid, &IID_ID2D1GdiInteropRenderTarget) && t->mdc) {
        *out = &t->gdi_vtbl;
        InterlockedIncrement(&t->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE t_addref(Target *t) { return InterlockedIncrement(&t->ref); }
static void pop_layer(Target *t);
static ULONG STDMETHODCALLTYPE t_release(Target *t)
{
    LONG r = InterlockedDecrement(&t->ref);
    if (!r) {
        while (t->nlayers) pop_layer(t);
        free_surface(t);
        if (t->bitmap) COM_RELEASE(t->bitmap);
        COM_RELEASE(t->text_params);
        factory_release(t->factory);
        d_free(t);
    }
    return r;
}
static void STDMETHODCALLTYPE t_factory(Target *t, void **f) { *f = t->factory; factory_addref(t->factory); }

/* ---- resources ---- */
static HRESULT STDMETHODCALLTYPE t_create_bitmap(Target *t, SZU size, const void *src, UINT32 pitch,
                                                 const BMPPROPS *props, void **out)
{
    if (!out) return E_POINTER;
    BMPPROPS p = { { 87, 1 }, t->dpix, t->dpiy };
    if (props) {
        p = *props;
        if (p.dpiX == 0 && p.dpiY == 0) { p.dpiX = t->dpix; p.dpiY = t->dpiy; }
    }
    Bitmap *b;
    HRESULT hr = bitmap_create(t->factory, size, src, pitch, &p, &b);
    *out = SUCCEEDED(hr) ? b : NULL;
    return hr;
}

static const GUID WIC_PBGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };
static const GUID WIC_BGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };
static const GUID WIC_BGR = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0e } };

static HRESULT STDMETHODCALLTYPE t_bitmap_from_wic(Target *t, void *wic, const BMPPROPS *props, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    if (!wic) return E_INVALIDARG;
    UINT w = 0, h = 0;
    GUID fmt;
    HRESULT hr = VSLOT(wic, 3, HRESULT (STDMETHODCALLTYPE *)(void *, UINT *, UINT *))(wic, &w, &h);
    if (FAILED(hr)) return hr;
    hr = VSLOT(wic, 4, HRESULT (STDMETHODCALLTYPE *)(void *, GUID *))(wic, &fmt);
    if (FAILED(hr)) return hr;
    int straight = IsEqualGUID(&fmt, &WIC_BGRA), opaque = IsEqualGUID(&fmt, &WIC_BGR);
    if (!straight && !opaque && !IsEqualGUID(&fmt, &WIC_PBGRA)) return D2DERR_UNSUPPORTED_PIXEL_FORMAT;
    if (!w || !h || w > 16384 || h > 16384) return E_INVALIDARG;
    BYTE *buf = d_alloc((size_t)w * h * 4);
    if (!buf) return E_OUTOFMEMORY;
    hr = VSLOT(wic, 7, HRESULT (STDMETHODCALLTYPE *)(void *, const void *, UINT, UINT, BYTE *))(wic, NULL, w * 4,
                                                                                            w * h * 4, buf);
    if (SUCCEEDED(hr)) {
        if (straight)
            for (size_t i = 0; i < (size_t)w * h; i++) {
                BYTE *p = buf + i * 4;
                p[0] = (BYTE)(p[0] * p[3] / 255);
                p[1] = (BYTE)(p[1] * p[3] / 255);
                p[2] = (BYTE)(p[2] * p[3] / 255);
            }
        BMPPROPS p = { { 87, opaque ? 3u : 1u }, t->dpix, t->dpiy };
        if (props && (props->dpiX || props->dpiY)) { p.dpiX = props->dpiX; p.dpiY = props->dpiY; }
        SZU size = { w, h };
        hr = t_create_bitmap(t, size, buf, w * 4, &p, out);
    }
    d_free(buf);
    return hr;
}

static HRESULT STDMETHODCALLTYPE t_shared_bitmap(Target *t, REFIID riid, void *data, const BMPPROPS *props, void **out)
{
    (void)t; (void)riid; (void)props;
    if (!out) return E_POINTER;
    *out = 0;
    Bitmap *b = bitmap_of(data);
    if (!b) return D2DERR_UNSUPPORTED_OPERATION;
    COM_ADDREF(b);
    *out = b;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE t_bitmap_brush(Target *t, void *bitmap, const BMPBRUSHPROPS *bp,
                                                const BRUSHPROPS *props, void **out)
{
    if (!out) return E_POINTER;
    Brush *b;
    HRESULT hr = brush_create(t->factory, PAINT_BITMAP, props, &b);
    *out = 0;
    if (FAILED(hr)) return hr;
    if (bp) { b->extend_x = bp->extendModeX; b->extend_y = bp->extendModeY; b->interp = bp->interpolationMode; }
    else b->interp = 1;
    Bitmap *bm = bitmap_of(bitmap);
    if (bm) { COM_ADDREF(bm); b->bitmap = bm; }
    *out = b;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE t_solid_brush(Target *t, const COLORF *c, const BRUSHPROPS *props, void **out)
{
    if (!out || !c) return E_POINTER;
    Brush *b;
    HRESULT hr = brush_create(t->factory, PAINT_SOLID, props, &b);
    *out = 0;
    if (FAILED(hr)) return hr;
    b->color = *c;
    *out = b;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE t_stops(Target *t, const GSTOP *s, UINT32 n, UINT32 gamma, UINT32 extend, void **out)
{
    if (!out) return E_POINTER;
    return stops_create(t->factory, s, n, gamma, extend, out);
}

static HRESULT STDMETHODCALLTYPE t_linear(Target *t, const LINPROPS *lp, const BRUSHPROPS *props, void *stops,
                                          void **out)
{
    if (!out || !lp) return E_POINTER;
    Brush *b;
    HRESULT hr = brush_create(t->factory, PAINT_LINEAR, props, &b);
    *out = 0;
    if (FAILED(hr)) return hr;
    b->p0 = lp->startPoint;
    b->p1 = lp->endPoint;
    brush_set_stops(b, stops);
    *out = b;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE t_radial(Target *t, const RADPROPS *rp, const BRUSHPROPS *props, void *stops,
                                          void **out)
{
    if (!out || !rp) return E_POINTER;
    Brush *b;
    HRESULT hr = brush_create(t->factory, PAINT_RADIAL, props, &b);
    *out = 0;
    if (FAILED(hr)) return hr;
    b->p0 = rp->center;
    b->p1 = rp->gradientOriginOffset;
    b->rx = rp->radiusX;
    b->ry = rp->radiusY;
    brush_set_stops(b, stops);
    *out = b;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE t_compatible(Target *t, const SZ *size, const SZU *psize, const PIXFMT *fmt,
                                              UINT32 options, void **out)
{
    (void)options;
    if (!out) return E_POINTER;
    *out = 0;
    if (fmt && fmt->format && fmt->format != 87) return D2DERR_UNSUPPORTED_PIXEL_FORMAT;
    float dpix = t->dpix, dpiy = t->dpiy;
    SZU px;
    if (psize && psize->width && psize->height) {
        px = *psize;
        if (size && size->width > 0 && size->height > 0) {
            dpix = px.width * 96 / size->width;
            dpiy = px.height * 96 / size->height;
        }
    } else if (size && size->width > 0 && size->height > 0) {
        px.width = (UINT32)ceilf(size->width * dpix / 96);
        px.height = (UINT32)ceilf(size->height * dpiy / 96);
    } else {
        px.width = t->s.w;
        px.height = t->s.h;
    }
    BMPPROPS bp = { { 87, fmt && fmt->alphaMode == 3 ? 3u : 1u }, dpix, dpiy };
    Bitmap *bm;
    HRESULT hr = bitmap_create(t->factory, px, NULL, 0, &bp, &bm);
    if (FAILED(hr)) return hr;
    RTPROPS rp = { 0, bp.pixelFormat, dpix, dpiy, 0, 0 };
    Target *n;
    hr = target_create(t->factory, TGT_BITMAP, &rp, &n);
    if (FAILED(hr)) { COM_RELEASE(bm); return hr; }
    n->bitmap = bm;
    n->s = bm->s;
    n->aa = t->aa;
    n->text_aa = t->text_aa;
    *out = n;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE t_layer(Target *t, const SZ *size, void **out)
{
    if (!out) return E_POINTER;
    LayerObj *l = d_alloc(sizeof(*l));
    if (!l) return E_OUTOFMEMORY;
    l->vtbl = ly_vtbl;
    l->ref = 1;
    l->factory = t->factory;
    factory_addref(t->factory);
    if (size) l->size = *size;
    *out = l;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE t_mesh(Target *t, void **out)
{
    if (!out) return E_POINTER;
    Mesh *m = d_alloc(sizeof(*m));
    if (!m) return E_OUTOFMEMORY;
    m->vtbl = mesh_vtbl;
    m->ref = 1;
    m->factory = t->factory;
    factory_addref(t->factory);
    path_init(&m->path);
    m->path.fill_mode = FILL_WINDING;
    *out = m;
    return S_OK;
}

/* ---- drawing ---- */
static void STDMETHODCALLTYPE t_line(Target *t, PT a, PT b, void *brush, float w, void *style)
{
    Path p;
    path_init(&p);
    path_begin(&p, a, 0);
    path_line(&p, b);
    path_end(&p, 0);
    target_stroke(t, &p, brush, w, style);
    path_free(&p);
}
static void STDMETHODCALLTYPE t_draw_rect(Target *t, const RCF *r, void *brush, float w, void *style)
{
    Path p;
    path_init(&p);
    path_rect(&p, r);
    target_stroke(t, &p, brush, w, style);
    path_free(&p);
}
static void STDMETHODCALLTYPE t_fill_rect(Target *t, const RCF *r, void *brush)
{
    Path p;
    path_init(&p);
    path_rect(&p, r);
    target_fill(t, &p, brush, NULL);
    path_free(&p);
}
static void STDMETHODCALLTYPE t_draw_rrect(Target *t, const RRECT *r, void *brush, float w, void *style)
{
    Path p;
    path_init(&p);
    path_rrect(&p, r);
    target_stroke(t, &p, brush, w, style);
    path_free(&p);
}
static void STDMETHODCALLTYPE t_fill_rrect(Target *t, const RRECT *r, void *brush)
{
    Path p;
    path_init(&p);
    path_rrect(&p, r);
    target_fill(t, &p, brush, NULL);
    path_free(&p);
}
static void STDMETHODCALLTYPE t_draw_ellipse(Target *t, const ELLIPSE_ *e, void *brush, float w, void *style)
{
    Path p;
    path_init(&p);
    path_ellipse(&p, e);
    target_stroke(t, &p, brush, w, style);
    path_free(&p);
}
static void STDMETHODCALLTYPE t_fill_ellipse(Target *t, const ELLIPSE_ *e, void *brush)
{
    Path p;
    path_init(&p);
    path_ellipse(&p, e);
    target_fill(t, &p, brush, NULL);
    path_free(&p);
}
static void STDMETHODCALLTYPE t_draw_geometry(Target *t, void *geometry, void *brush, float w, void *style)
{
    Geometry *g = geometry_of(geometry);
    if (g) target_stroke(t, geometry_path(g), brush, w, style);
}
static void STDMETHODCALLTYPE t_fill_geometry(Target *t, void *geometry, void *brush, void *opacity_brush)
{
    Geometry *g = geometry_of(geometry);
    if (g) target_fill(t, geometry_path(g), brush, opacity_brush);
}
static void STDMETHODCALLTYPE t_fill_mesh(Target *t, void *mesh, void *brush)
{
    if (mesh && *(const void **)mesh == (const void *)mesh_vtbl) target_fill(t, &((Mesh *)mesh)->path, brush, NULL);
}

/* the paint that maps bitmap @b's @src (DIPs) onto @dst (user space) */
static int bitmap_paint(Target *t, Bitmap *b, const RCF *dst, const RCF *src, int linear, Paint *p, RCF *d)
{
    SZ bs = { b->s.w * 96.0f / b->dpix, b->s.h * 96.0f / b->dpiy };
    RCF s = src ? *src : (RCF){ 0, 0, bs.width, bs.height };
    *d = dst ? *dst : (RCF){ 0, 0, s.right - s.left, s.bottom - s.top };
    float sw = s.right - s.left, sh = s.bottom - s.top;
    if (sw == 0 || sh == 0 || d->right == d->left || d->bottom == d->top) return 0;
    /* user from bitmap DIPs */
    MAT ub = { (d->right - d->left) / sw, 0, 0, (d->bottom - d->top) / sh, 0, 0 };
    ub.dx = d->left - s.left * ub.m11;
    ub.dy = d->top - s.top * ub.m22;
    MAT w = target_world(t), dev = mat_mul(&ub, &w), inv;
    if (!mat_invert(&dev, &inv)) return 0;
    MAT px = { b->dpix / 96, 0, 0, b->dpiy / 96, 0, 0 };
    memset(p, 0, sizeof(*p));
    p->kind = PAINT_BITMAP;
    p->opacity = 1;
    p->inv = mat_mul(&inv, &px);
    p->bmp = &b->s;
    p->extend = p->extend_y = EXTEND_CLAMP;
    p->linear_filter = linear;
    return 1;
}

static void fill_rect_paint(Target *t, const RCF *r, const Paint *p)
{
    Path path;
    path_init(&path);
    path_rect(&path, r);
    MAT w = target_world(t);
    Clip c = target_clip(t);
    raster_fill(&t->s, &c, NULL, &path, &w, t->aa == 1, p);
    path_free(&path);
}

static void STDMETHODCALLTYPE t_opacity_mask(Target *t, void *mask, void *brush, UINT32 content, const RCF *dst,
                                             const RCF *src)
{
    (void)content;
    if (!check(t)) return;
    Bitmap *b = bitmap_of(mask);
    Brush *br = brush_of(brush);
    if (!b || !br) return;
    Paint mp, p;
    RCF d;
    MAT w = target_world(t);
    if (!bitmap_paint(t, b, dst, src, 1, &mp, &d) || !brush_paint(br, &w, &p)) return;
    p.alpha = &mp;
    fill_rect_paint(t, &d, &p);
}

static void STDMETHODCALLTYPE t_draw_bitmap(Target *t, void *bitmap, const RCF *dst, float opacity, UINT32 interp,
                                            const RCF *src)
{
    if (!check(t)) return;
    Bitmap *b = bitmap_of(bitmap);
    if (!b) return;
    Paint p;
    RCF d;
    if (!bitmap_paint(t, b, dst, src, interp == 1, &p, &d)) return;
    p.opacity = opacity < 0 ? 0 : opacity > 1 ? 1 : opacity;
    fill_rect_paint(t, &d, &p);
}

static void STDMETHODCALLTYPE t_draw_text(Target *t, const WCHAR *s, UINT32 n, void *format, const RCF *rc,
                                          void *brush, UINT32 options, UINT32 measuring)
{
    if (!check(t) || !s || !format || !rc) return;
    text_draw_string(t, s, n, format, rc, brush, options, measuring);
}
static void STDMETHODCALLTYPE t_draw_layout(Target *t, PT origin, void *layout, void *brush, UINT32 options)
{
    if (!check(t) || !layout) return;
    text_draw_layout(t, origin, layout, brush, options);
}
static void STDMETHODCALLTYPE t_draw_glyphs(Target *t, PT origin, const DW_GLYPHRUN *run, void *brush, UINT32 mode)
{
    (void)mode;
    if (!check(t) || !run) return;
    text_draw_glyph_run(t, origin, run, brush);
}

/* ---- state ---- */
static void STDMETHODCALLTYPE t_set_transform(Target *t, const MAT *m) { if (m) t->transform = *m; }
static void STDMETHODCALLTYPE t_get_transform(Target *t, MAT *m) { *m = t->transform; }
static void STDMETHODCALLTYPE t_set_aa(Target *t, UINT32 m) { t->aa = m; }
static UINT32 STDMETHODCALLTYPE t_get_aa(Target *t) { return t->aa; }
static void STDMETHODCALLTYPE t_set_text_aa(Target *t, UINT32 m) { t->text_aa = m; }
static UINT32 STDMETHODCALLTYPE t_get_text_aa(Target *t) { return t->text_aa; }
static void STDMETHODCALLTYPE t_set_params(Target *t, void *p)
{
    if (p) COM_ADDREF(p);
    COM_RELEASE(t->text_params);
    t->text_params = p;
}
static void STDMETHODCALLTYPE t_get_params(Target *t, void **p)
{
    *p = t->text_params;
    if (*p) COM_ADDREF(*p);
}
static void STDMETHODCALLTYPE t_set_tags(Target *t, UINT64 a, UINT64 b) { t->tag1 = a; t->tag2 = b; }
static void STDMETHODCALLTYPE t_get_tags(Target *t, UINT64 *a, UINT64 *b)
{
    if (a) *a = t->tag1;
    if (b) *b = t->tag2;
}

/* ---- layers ---- */
static void STDMETHODCALLTYPE t_push_layer(Target *t, const LAYERPARAMS *lp, void *layer)
{
    if (!check(t) || !lp) return;
    if (t->nlayers == MAX_LAYERS) { t->error = E_OUTOFMEMORY; return; }
    LayerState *L = &t->layers[t->nlayers];
    memset(L, 0, sizeof(*L));
    L->clip_depth = t->nclips;
    RCF cb = lp->contentBounds;
    Clip dc = isfinite(cb.left) && isfinite(cb.top) && isfinite(cb.right) && isfinite(cb.bottom) ?
              device_rect(t, &cb) : (Clip){ -100000000, -100000000, 100000000, 100000000 };
    push_clip(t, dc);
    L->region = target_clip(t);
    L->opacity = lp->opacity < 0 ? 0 : lp->opacity > 1 ? 1 : lp->opacity;
    int w = L->region.x1 - L->region.x0, h = L->region.y1 - L->region.y0;
    if (w > 0 && h > 0) {
        L->saved.w = w;
        L->saved.h = h;
        L->saved.stride = w;
        L->saved.px = d_alloc(sizeof(UINT32) * w * h);
        if (L->saved.px)
            for (int y = 0; y < h; y++)
                memcpy(L->saved.px + (size_t)y * w,
                       t->s.px + (size_t)(L->region.y0 + y) * t->s.stride + L->region.x0, sizeof(UINT32) * w);
    }
    Geometry *g = geometry_of(lp->geometricMask);
    if (g && w > 0 && h > 0) {
        MAT world = target_world(t), m = mat_mul(&lp->maskTransform, &world);
        raster_mask(&L->mask, &L->region, geometry_path(g), &m, lp->maskAntialiasMode == 1);
    }
    Brush *ob = brush_of(lp->opacityBrush);
    if (ob) {
        MAT world = target_world(t);
        if (brush_paint(ob, &world, &L->opaint)) {
            COM_ADDREF(ob);
            L->opbrush = ob;
        } else L->opacity = 0;
    }
    if (layer) COM_ADDREF(layer);
    L->layer = layer;
    t->nlayers++;
}

static void pop_layer(Target *t)
{
    LayerState *L = &t->layers[--t->nlayers];
    int w = L->saved.w, h = L->saved.h;
    UINT32 *alpha = L->opbrush ? d_alloc(sizeof(UINT32) * (w ? w : 1)) : NULL;
    if (L->saved.px)
        for (int y = 0; y < h; y++) {
            UINT32 *cur = t->s.px + (size_t)(L->region.y0 + y) * t->s.stride + L->region.x0;
            const UINT32 *old = L->saved.px + (size_t)y * w;
            if (alpha) paint_span(&L->opaint, L->region.x0, L->region.y0 + y, w, alpha);
            for (int x = 0; x < w; x++) {
                float k = L->opacity;
                if (L->mask.a) k *= L->mask.a[(size_t)y * L->mask.w + x] * (1.0f / 255);
                if (alpha) k *= (alpha[x] >> 24) * (1.0f / 255);
                unsigned f = (unsigned)(k * 256 + 0.5f);
                if (f >= 256) continue;
                UINT32 a = old[x], b = cur[x];
                UINT32 rb = (((a & 0x00FF00FF) * (256 - f) + (b & 0x00FF00FF) * f) >> 8) & 0x00FF00FF;
                UINT32 ag = ((((a >> 8) & 0x00FF00FF) * (256 - f) + ((b >> 8) & 0x00FF00FF) * f)) & 0xFF00FF00;
                cur[x] = rb | ag;
            }
        }
    d_free(alpha);
    d_free(L->saved.px);
    d_free(L->mask.a);
    if (L->opbrush) COM_RELEASE(L->opbrush);
    if (L->layer) COM_RELEASE(L->layer);
    t->nclips = L->clip_depth;
}

static void STDMETHODCALLTYPE t_pop_layer(Target *t)
{
    if (!check(t)) return;
    if (!t->nlayers) { t->error = D2DERR_PUSH_POP_UNBALANCED; return; }
    pop_layer(t);
}

static HRESULT STDMETHODCALLTYPE t_flush(Target *t, UINT64 *a, UINT64 *b)
{
    t_get_tags(t, a, b);
    return t->error;
}

static void STDMETHODCALLTYPE t_save_state(Target *t, void *block)
{
    if (!block) return;
    STATEDESC d = { t->aa, t->text_aa, t->tag1, t->tag2, t->transform };
    state_block_set(block, &d, t->text_params);
}
static void STDMETHODCALLTYPE t_restore_state(Target *t, void *block)
{
    if (!block) return;
    STATEDESC d;
    void *params;
    state_block_get(block, &d, &params);
    t->aa = d.antialiasMode;
    t->text_aa = d.textAntialiasMode;
    t->tag1 = d.tag1;
    t->tag2 = d.tag2;
    t->transform = d.transform;
    t_set_params(t, params);
}

static void STDMETHODCALLTYPE t_push_clip(Target *t, const RCF *r, UINT32 aa)
{
    (void)aa;
    if (!r) return;
    push_clip(t, device_rect(t, r));
}
static void STDMETHODCALLTYPE t_pop_clip(Target *t)
{
    int floor_ = t->nlayers ? t->layers[t->nlayers - 1].clip_depth + 1 : 0;
    if (t->nclips > floor_) t->nclips--;
    else t->error = D2DERR_PUSH_POP_UNBALANCED;
}

static void STDMETHODCALLTYPE t_clear(Target *t, const COLORF *c)
{
    if (!check(t)) return;
    UINT32 v = c ? premul(c, 1) : 0;
    if (t->fmt.alphaMode == 3) v |= 0xFF000000u;
    Clip cl = target_clip(t);
    for (int y = cl.y0; y < cl.y1; y++)
        for (int x = cl.x0; x < cl.x1; x++) t->s.px[(size_t)y * t->s.stride + x] = v;
}

static void opaque(Target *t)
{
    for (int y = 0; y < t->s.h; y++) {
        UINT32 *row = t->s.px + (size_t)y * t->s.stride;
        for (int x = 0; x < t->s.w; x++) row[x] |= 0xFF000000u;
    }
}

static void STDMETHODCALLTYPE t_begin(Target *t)
{
    t->drawing = 1;
    t->error = S_OK;
    if (t->kind == TGT_DC && t->bound_dc && t->mdc) {
        BitBlt(t->mdc, 0, 0, t->s.w, t->s.h, t->bound_dc, t->bound_rc.left, t->bound_rc.top, SRCCOPY);
        opaque(t);
    }
}

static HRESULT STDMETHODCALLTYPE t_end(Target *t, UINT64 *a, UINT64 *b)
{
    HRESULT hr = t->drawing ? t->error : D2DERR_WRONG_STATE;
    if (t->nlayers || t->nclips) {
        while (t->nlayers) pop_layer(t);
        t->nclips = 0;
        if (SUCCEEDED(hr)) hr = D2DERR_PUSH_POP_UNBALANCED;
    }
    if (t->drawing) {
        GdiFlush();
        if (t->kind == TGT_HWND && t->mdc) {
            HDC dc = GetDC(t->hwnd);
            if (dc) {
                BitBlt(dc, 0, 0, t->s.w, t->s.h, t->mdc, 0, 0, SRCCOPY);
                ReleaseDC(t->hwnd, dc);
            }
        } else if (t->kind == TGT_DC && t->bound_dc && t->mdc) {
            BitBlt(t->bound_dc, t->bound_rc.left, t->bound_rc.top, t->s.w, t->s.h, t->mdc, 0, 0, SRCCOPY);
        }
    }
    t->drawing = 0;
    t_get_tags(t, a, b);
    t->error = S_OK;
    return hr;
}

static PIXFMT *STDMETHODCALLTYPE t_format(Target *t, PIXFMT *r) { *r = t->fmt; return r; }
static void STDMETHODCALLTYPE t_set_dpi(Target *t, float x, float y)
{
    if (x == 0 && y == 0) { x = y = 96; }
    if (x > 0 && y > 0) { t->dpix = x; t->dpiy = y; }
}
static void STDMETHODCALLTYPE t_get_dpi(Target *t, float *x, float *y)
{
    if (x) *x = t->dpix;
    if (y) *y = t->dpiy;
}
static SZ *STDMETHODCALLTYPE t_size(Target *t, SZ *r)
{
    r->width = t->s.w * 96.0f / t->dpix;
    r->height = t->s.h * 96.0f / t->dpiy;
    return r;
}
static SZU *STDMETHODCALLTYPE t_pixel_size(Target *t, SZU *r)
{
    r->width = t->s.w;
    r->height = t->s.h;
    return r;
}
static UINT32 STDMETHODCALLTYPE t_max_bitmap(Target *t) { (void)t; return 16384; }
static BOOL STDMETHODCALLTYPE t_supported(Target *t, const RTPROPS *p)
{
    (void)t;
    if (!p) return FALSE;
    if (p->pixelFormat.format && p->pixelFormat.format != 87) return FALSE;
    if (p->pixelFormat.alphaMode == 2) return FALSE;
    return p->type != 2;    /* no hardware rendering */
}

/* the kinds */
static UINT32 STDMETHODCALLTYPE h_state(Target *t) { return IsIconic(t->hwnd) ? 1 : 0; }
static HRESULT STDMETHODCALLTYPE h_resize(Target *t, const SZU *s)
{
    if (!s) return E_POINTER;
    if (t->drawing) return D2DERR_WRONG_STATE;
    return target_resize(t, s->width, s->height);
}
static HWND STDMETHODCALLTYPE h_hwnd(Target *t) { return t->hwnd; }

static HRESULT STDMETHODCALLTYPE dc_bind(Target *t, HDC dc, const RECT *rc)
{
    if (!dc || !rc) return E_INVALIDARG;
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    if (!t->mdc || w != t->s.w || h != t->s.h) {
        HRESULT hr = target_resize(t, w, h);
        if (FAILED(hr)) return hr;
    }
    t->bound_dc = dc;
    t->bound_rc = *rc;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE bt_bitmap(Target *t, void **out)
{
    if (!out) return E_POINTER;
    *out = t->bitmap;
    if (t->bitmap) COM_ADDREF(t->bitmap);
    return S_OK;
}

#define RT_METHODS \
    t_qi, t_addref, t_release, t_factory, t_create_bitmap, t_bitmap_from_wic, t_shared_bitmap, t_bitmap_brush, \
    t_solid_brush, t_stops, t_linear, t_radial, t_compatible, t_layer, t_mesh, t_line, t_draw_rect, t_fill_rect, \
    t_draw_rrect, t_fill_rrect, t_draw_ellipse, t_fill_ellipse, t_draw_geometry, t_fill_geometry, t_fill_mesh, \
    t_opacity_mask, t_draw_bitmap, t_draw_text, t_draw_layout, t_draw_glyphs, t_set_transform, t_get_transform, \
    t_set_aa, t_get_aa, t_set_text_aa, t_get_text_aa, t_set_params, t_get_params, t_set_tags, t_get_tags, \
    t_push_layer, t_pop_layer, t_flush, t_save_state, t_restore_state, t_push_clip, t_pop_clip, t_clear, t_begin, \
    t_end, t_format, t_set_dpi, t_get_dpi, t_size, t_pixel_size, t_max_bitmap, t_supported

const void *const rt_hwnd_vtbl[] = { RT_METHODS, h_state, h_resize, h_hwnd };
const void *const rt_dc_vtbl[] = { RT_METHODS, dc_bind };
const void *const rt_bitmap_vtbl[] = { RT_METHODS, bt_bitmap };

/* ---- ID2D1GdiInteropRenderTarget ---- */
static Target *from_gdi(void *p) { return (Target *)((char *)p - offsetof(Target, gdi_vtbl)); }
static HRESULT STDMETHODCALLTYPE gi_qi(void *p, REFIID riid, void **out) { return t_qi(from_gdi(p), riid, out); }
static ULONG STDMETHODCALLTYPE gi_addref(void *p) { return t_addref(from_gdi(p)); }
static ULONG STDMETHODCALLTYPE gi_release(void *p) { return t_release(from_gdi(p)); }
static HRESULT STDMETHODCALLTYPE gi_getdc(void *p, UINT32 mode, HDC *dc)
{
    Target *t = from_gdi(p);
    (void)mode;
    if (!dc) return E_POINTER;
    *dc = t->mdc;
    if (!t->drawing) return D2DERR_WRONG_STATE;
    GdiFlush();
    return t->mdc ? S_OK : D2DERR_UNSUPPORTED_OPERATION;
}
static HRESULT STDMETHODCALLTYPE gi_releasedc(void *p, const RECT *rc)
{
    Target *t = from_gdi(p);
    (void)rc;
    GdiFlush();
    if (t->fmt.alphaMode == 3) opaque(t);
    return S_OK;
}
const void *const gdi_vtbl[] = { gi_qi, gi_addref, gi_release, gi_getdc, gi_releasedc };

HRESULT target_create(void *factory, int kind, const RTPROPS *props, Target **out)
{
    *out = 0;
    PIXFMT fmt = { 87, kind == TGT_BITMAP ? 1u : 3u };
    float dpix = 0, dpiy = 0;
    if (props) {
        if (props->pixelFormat.format && props->pixelFormat.format != 87) return D2DERR_UNSUPPORTED_PIXEL_FORMAT;
        if (props->pixelFormat.alphaMode == 2) return D2DERR_UNSUPPORTED_PIXEL_FORMAT;
        if (props->pixelFormat.alphaMode) fmt.alphaMode = props->pixelFormat.alphaMode;
        dpix = props->dpiX;
        dpiy = props->dpiY;
    }
    if (kind == TGT_DC && (!props || !props->pixelFormat.alphaMode)) return D2DERR_UNSUPPORTED_PIXEL_FORMAT;
    Target *t = d_alloc(sizeof(*t));
    if (!t) return E_OUTOFMEMORY;
    t->vtbl = kind == TGT_HWND ? rt_hwnd_vtbl : kind == TGT_DC ? rt_dc_vtbl : rt_bitmap_vtbl;
    t->gdi_vtbl = gdi_vtbl;
    t->ref = 1;
    t->factory = factory;
    factory_addref(factory);
    t->kind = kind;
    t->fmt = fmt;
    if (dpix <= 0 || dpiy <= 0) {
        HDC dc = GetDC(0);
        int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(0, dc);
        dpix = dpiy = d > 0 ? (float)d : 96;
    }
    t->dpix = dpix;
    t->dpiy = dpiy;
    t->transform = MAT_IDENTITY;
    *out = t;
    return S_OK;
}
