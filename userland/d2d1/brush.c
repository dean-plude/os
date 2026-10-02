/*
 * brush.c — brushes (solid, linear and radial gradients, bitmap), gradient
 * stop collections, and bitmaps
 */
#include "d2d_int.h"
#include <windows.h>
#include <stdlib.h>

/* ---- colours ---- */
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

UINT32 premul(const COLORF *c, float opacity)
{
    float a = clamp01(c->a * opacity);
    unsigned A = (unsigned)(a * 255 + 0.5f);
    unsigned R = (unsigned)(clamp01(c->r) * a * 255 + 0.5f), G = (unsigned)(clamp01(c->g) * a * 255 + 0.5f);
    unsigned B = (unsigned)(clamp01(c->b) * a * 255 + 0.5f);
    return A << 24 | R << 16 | G << 8 | B;
}

static float to_linear(float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }
static float to_srgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f; }

/* ---- gradient stop collections ---- */
extern const void *const gs_vtbl[];

static HRESULT STDMETHODCALLTYPE gs_qi(GradientStops *g, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Resource) ||
        IsEqualIID(riid, &IID_ID2D1GradientStopCollection)) {
        *out = g;
        InterlockedIncrement(&g->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE gs_addref(GradientStops *g) { return InterlockedIncrement(&g->ref); }
static ULONG STDMETHODCALLTYPE gs_release(GradientStops *g)
{
    LONG r = InterlockedDecrement(&g->ref);
    if (!r) {
        factory_release(g->factory);
        d_free(g->stops);
        d_free(g);
    }
    return r;
}
static void STDMETHODCALLTYPE gs_factory(GradientStops *g, void **f) { *f = g->factory; factory_addref(g->factory); }
static UINT32 STDMETHODCALLTYPE gs_count(GradientStops *g) { return g->n; }
static void STDMETHODCALLTYPE gs_get(GradientStops *g, GSTOP *s, UINT32 n)
{
    for (UINT32 i = 0; i < n && i < g->n; i++) s[i] = g->stops[i];
}
static UINT32 STDMETHODCALLTYPE gs_gamma(GradientStops *g) { return g->gamma; }
static UINT32 STDMETHODCALLTYPE gs_extend(GradientStops *g) { return g->extend; }
const void *const gs_vtbl[] = { gs_qi, gs_addref, gs_release, gs_factory, gs_count, gs_get, gs_gamma, gs_extend };

static int cmp_stop(const void *a, const void *b)
{
    float x = ((const GSTOP *)a)->position, y = ((const GSTOP *)b)->position;
    return x < y ? -1 : x > y;
}

static void build_lut(GradientStops *g)
{
    GSTOP *s = d_alloc(sizeof(GSTOP) * g->n);
    if (!s) return;
    memcpy(s, g->stops, sizeof(GSTOP) * g->n);
    qsort(s, g->n, sizeof(GSTOP), cmp_stop);
    int lin = g->gamma == 1;
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        COLORF c;
        if (t <= s[0].position) c = s[0].color;
        else if (t >= s[g->n - 1].position) c = s[g->n - 1].color;
        else {
            UINT32 k = 1;
            while (k < g->n && s[k].position < t) k++;
            const GSTOP *a = &s[k - 1], *b = &s[k];
            float span = b->position - a->position, f = span > 0 ? (t - a->position) / span : 1;
            if (lin) {
                c.r = to_srgb(to_linear(a->color.r) + (to_linear(b->color.r) - to_linear(a->color.r)) * f);
                c.g = to_srgb(to_linear(a->color.g) + (to_linear(b->color.g) - to_linear(a->color.g)) * f);
                c.b = to_srgb(to_linear(a->color.b) + (to_linear(b->color.b) - to_linear(a->color.b)) * f);
            } else {
                c.r = a->color.r + (b->color.r - a->color.r) * f;
                c.g = a->color.g + (b->color.g - a->color.g) * f;
                c.b = a->color.b + (b->color.b - a->color.b) * f;
            }
            c.a = a->color.a + (b->color.a - a->color.a) * f;
        }
        g->lut[i] = premul(&c, 1);
    }
    d_free(s);
}

HRESULT stops_create(void *factory, const GSTOP *stops, UINT32 n, UINT32 gamma, UINT32 extend, void **out)
{
    *out = 0;
    if (!stops || !n) return E_INVALIDARG;
    GradientStops *g = d_alloc(sizeof(*g));
    if (!g) return E_OUTOFMEMORY;
    g->vtbl = gs_vtbl;
    g->ref = 1;
    g->factory = factory;
    factory_addref(factory);
    g->stops = d_alloc(sizeof(GSTOP) * n);
    if (!g->stops) { gs_release(g); return E_OUTOFMEMORY; }
    memcpy(g->stops, stops, sizeof(GSTOP) * n);
    g->n = n;
    g->gamma = gamma;
    g->extend = extend;
    build_lut(g);
    *out = g;
    return S_OK;
}

/* ---- bitmaps ---- */
extern const void *const bm_vtbl[];

Bitmap *bitmap_of(void *iface)
{
    return iface && *(const void **)iface == (const void *)bm_vtbl ? iface : NULL;
}

static HRESULT STDMETHODCALLTYPE bm_qi(Bitmap *b, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Resource) ||
        IsEqualIID(riid, &IID_ID2D1Image) || IsEqualIID(riid, &IID_ID2D1Bitmap)) {
        *out = b;
        InterlockedIncrement(&b->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE bm_addref(Bitmap *b) { return InterlockedIncrement(&b->ref); }
static ULONG STDMETHODCALLTYPE bm_release(Bitmap *b)
{
    LONG r = InterlockedDecrement(&b->ref);
    if (!r) {
        factory_release(b->factory);
        d_free(b->s.px);
        d_free(b);
    }
    return r;
}
static void STDMETHODCALLTYPE bm_factory(Bitmap *b, void **f) { *f = b->factory; factory_addref(b->factory); }
static SZ *STDMETHODCALLTYPE bm_size(Bitmap *b, SZ *r)
{
    r->width = b->s.w * 96.0f / b->dpix;
    r->height = b->s.h * 96.0f / b->dpiy;
    return r;
}
static SZU *STDMETHODCALLTYPE bm_pixel_size(Bitmap *b, SZU *r)
{
    r->width = b->s.w;
    r->height = b->s.h;
    return r;
}
static PIXFMT *STDMETHODCALLTYPE bm_format(Bitmap *b, PIXFMT *r) { *r = b->fmt; return r; }
static void STDMETHODCALLTYPE bm_dpi(Bitmap *b, float *x, float *y)
{
    if (x) *x = b->dpix;
    if (y) *y = b->dpiy;
}

static void copy_rect(Surface *dst, int dx, int dy, const Surface *src, const RCU *r)
{
    int sx = 0, sy = 0, w = src->w, h = src->h;
    if (r) {
        sx = r->left; sy = r->top;
        w = (int)r->right - (int)r->left;
        h = (int)r->bottom - (int)r->top;
    }
    if (sx < 0 || sy < 0 || sx + w > src->w || sy + h > src->h) return;
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    for (int y = 0; y < h; y++)
        memmove(dst->px + (size_t)(dy + y) * dst->stride + dx, src->px + (size_t)(sy + y) * src->stride + sx,
                sizeof(UINT32) * (w > 0 ? w : 0));
}

static HRESULT STDMETHODCALLTYPE bm_copy_bitmap(Bitmap *b, const PTU *at, void *src, const RCU *r)
{
    Bitmap *s = bitmap_of(src);
    if (!s) return E_INVALIDARG;
    copy_rect(&b->s, at ? at->x : 0, at ? at->y : 0, &s->s, r);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bm_copy_target(Bitmap *b, const PTU *at, void *rt, const RCU *r)
{
    Target *t = target_of(rt);
    if (!t) return E_INVALIDARG;
    copy_rect(&b->s, at ? at->x : 0, at ? at->y : 0, &t->s, r);
    return S_OK;
}

static void store_pixels(Bitmap *b, int x0, int y0, int w, int h, const BYTE *src, UINT32 pitch)
{
    UINT32 fmt = b->fmt.format, alpha = b->fmt.alphaMode;
    for (int y = 0; y < h; y++) {
        if (y0 + y >= b->s.h) break;
        const BYTE *s = src + (size_t)y * pitch;
        UINT32 *d = b->s.px + (size_t)(y0 + y) * b->s.stride + x0;
        for (int x = 0; x < w && x0 + x < b->s.w; x++) {
            UINT32 v;
            if (fmt == 65) v = (UINT32)s[x] << 24;
            else {
                const BYTE *p = s + 4 * x;
                if (fmt == 28) v = (UINT32)p[3] << 24 | (UINT32)p[0] << 16 | (UINT32)p[1] << 8 | p[2];
                else v = (UINT32)p[3] << 24 | (UINT32)p[2] << 16 | (UINT32)p[1] << 8 | p[0];
                if (alpha == 3) v |= 0xFF000000u;
                else if (alpha == 2) {
                    unsigned a = v >> 24;
                    v = (v & 0xFF000000u) | ((((v >> 16) & 255) * a / 255) << 16) |
                        ((((v >> 8) & 255) * a / 255) << 8) | ((v & 255) * a / 255);
                }
            }
            d[x] = v;
        }
    }
}

static HRESULT STDMETHODCALLTYPE bm_copy_memory(Bitmap *b, const RCU *r, const void *src, UINT32 pitch)
{
    if (!src) return E_INVALIDARG;
    int x0 = r ? r->left : 0, y0 = r ? r->top : 0;
    int w = r ? (int)r->right - x0 : b->s.w, h = r ? (int)r->bottom - y0 : b->s.h;
    store_pixels(b, x0, y0, w, h, src, pitch);
    return S_OK;
}

const void *const bm_vtbl[] = { bm_qi, bm_addref, bm_release, bm_factory, bm_size, bm_pixel_size, bm_format,
                                bm_dpi, bm_copy_bitmap, bm_copy_target, bm_copy_memory };

HRESULT bitmap_create(void *factory, SZU size, const void *src, UINT32 pitch, const BMPPROPS *props, Bitmap **out)
{
    *out = 0;
    if (!size.width || !size.height || size.width > 16384 || size.height > 16384) return E_INVALIDARG;
    UINT32 fmt = props ? props->pixelFormat.format : 87, alpha = props ? props->pixelFormat.alphaMode : 1;
    if (fmt == 0) fmt = 87;
    if (alpha == 0) alpha = fmt == 65 ? 2 : 1;
    if (fmt != 87 && fmt != 28 && fmt != 65) return D2DERR_UNSUPPORTED_PIXEL_FORMAT;
    if (alpha == 2 && fmt != 65) return D2DERR_UNSUPPORTED_PIXEL_FORMAT;
    Bitmap *b = d_alloc(sizeof(*b));
    if (!b) return E_OUTOFMEMORY;
    b->vtbl = bm_vtbl;
    b->ref = 1;
    b->factory = factory;
    factory_addref(factory);
    b->s.w = size.width;
    b->s.h = size.height;
    b->s.stride = size.width;
    b->s.px = d_alloc(sizeof(UINT32) * size.width * size.height);
    if (!b->s.px) { bm_release(b); return E_OUTOFMEMORY; }
    b->fmt.format = fmt;
    b->fmt.alphaMode = alpha;
    b->dpix = props && props->dpiX > 0 ? props->dpiX : 96;
    b->dpiy = props && props->dpiY > 0 ? props->dpiY : 96;
    if (src) store_pixels(b, 0, 0, size.width, size.height, src, pitch);
    *out = b;
    return S_OK;
}

/* ---- brushes ---- */
extern const void *const br_solid_vtbl[], *const br_bitmap_vtbl[], *const br_linear_vtbl[], *const br_radial_vtbl[];

Brush *brush_of(void *iface)
{
    if (!iface) return NULL;
    const void *v = *(const void **)iface;
    return v == br_solid_vtbl || v == br_bitmap_vtbl || v == br_linear_vtbl || v == br_radial_vtbl ? iface : NULL;
}

static HRESULT STDMETHODCALLTYPE br_qi(Brush *b, REFIID riid, void **out)
{
    static const GUID *const kinds[] = { &IID_ID2D1SolidColorBrush, &IID_ID2D1LinearGradientBrush,
                                         &IID_ID2D1RadialGradientBrush, &IID_ID2D1BitmapBrush };
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Resource) ||
        IsEqualIID(riid, &IID_ID2D1Brush) || IsEqualIID(riid, kinds[b->kind])) {
        *out = b;
        InterlockedIncrement(&b->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE br_addref(Brush *b) { return InterlockedIncrement(&b->ref); }
static ULONG STDMETHODCALLTYPE br_release(Brush *b)
{
    LONG r = InterlockedDecrement(&b->ref);
    if (!r) {
        if (b->stops) gs_release(b->stops);
        if (b->bitmap) bm_release(b->bitmap);
        factory_release(b->factory);
        d_free(b);
    }
    return r;
}
static void STDMETHODCALLTYPE br_factory(Brush *b, void **f) { *f = b->factory; factory_addref(b->factory); }
static void STDMETHODCALLTYPE br_set_opacity(Brush *b, float o) { b->opacity = o; }
static void STDMETHODCALLTYPE br_set_transform(Brush *b, const MAT *m) { if (m) b->transform = *m; }
static float STDMETHODCALLTYPE br_opacity(Brush *b) { return b->opacity; }
static void STDMETHODCALLTYPE br_transform(Brush *b, MAT *m) { *m = b->transform; }
#define BRUSH_METHODS br_qi, br_addref, br_release, br_factory, br_set_opacity, br_set_transform, br_opacity, br_transform

static void STDMETHODCALLTYPE sc_set(Brush *b, const COLORF *c) { if (c) b->color = *c; }
static COLORF *STDMETHODCALLTYPE sc_get(Brush *b, COLORF *r) { *r = b->color; return r; }
const void *const br_solid_vtbl[] = { BRUSH_METHODS, sc_set, sc_get };

static void STDMETHODCALLTYPE bb_set_x(Brush *b, UINT32 m) { b->extend_x = m; }
static void STDMETHODCALLTYPE bb_set_y(Brush *b, UINT32 m) { b->extend_y = m; }
static void STDMETHODCALLTYPE bb_set_interp(Brush *b, UINT32 m) { b->interp = m; }
static void STDMETHODCALLTYPE bb_set_bitmap(Brush *b, void *bmp)
{
    Bitmap *n = bitmap_of(bmp);
    if (n) bm_addref(n);
    if (b->bitmap) bm_release(b->bitmap);
    b->bitmap = n;
}
static UINT32 STDMETHODCALLTYPE bb_x(Brush *b) { return b->extend_x; }
static UINT32 STDMETHODCALLTYPE bb_y(Brush *b) { return b->extend_y; }
static UINT32 STDMETHODCALLTYPE bb_interp(Brush *b) { return b->interp; }
static void STDMETHODCALLTYPE bb_bitmap(Brush *b, void **out)
{
    *out = b->bitmap;
    if (b->bitmap) bm_addref(b->bitmap);
}
const void *const br_bitmap_vtbl[] = { BRUSH_METHODS, bb_set_x, bb_set_y, bb_set_interp, bb_set_bitmap,
                                       bb_x, bb_y, bb_interp, bb_bitmap };

static void STDMETHODCALLTYPE lg_set_start(Brush *b, PT p) { b->p0 = p; }
static void STDMETHODCALLTYPE lg_set_end(Brush *b, PT p) { b->p1 = p; }
static PT *STDMETHODCALLTYPE lg_start(Brush *b, PT *r) { *r = b->p0; return r; }
static PT *STDMETHODCALLTYPE lg_end(Brush *b, PT *r) { *r = b->p1; return r; }
static void STDMETHODCALLTYPE gr_stops(Brush *b, void **out)
{
    *out = b->stops;
    if (b->stops) gs_addref(b->stops);
}
const void *const br_linear_vtbl[] = { BRUSH_METHODS, lg_set_start, lg_set_end, lg_start, lg_end, gr_stops };

static void STDMETHODCALLTYPE rg_set_center(Brush *b, PT p) { b->p0 = p; }
static void STDMETHODCALLTYPE rg_set_origin(Brush *b, PT p) { b->p1 = p; }
static void STDMETHODCALLTYPE rg_set_rx(Brush *b, float r) { b->rx = r; }
static void STDMETHODCALLTYPE rg_set_ry(Brush *b, float r) { b->ry = r; }
static PT *STDMETHODCALLTYPE rg_center(Brush *b, PT *r) { *r = b->p0; return r; }
static PT *STDMETHODCALLTYPE rg_origin(Brush *b, PT *r) { *r = b->p1; return r; }
static float STDMETHODCALLTYPE rg_rx(Brush *b) { return b->rx; }
static float STDMETHODCALLTYPE rg_ry(Brush *b) { return b->ry; }
const void *const br_radial_vtbl[] = { BRUSH_METHODS, rg_set_center, rg_set_origin, rg_set_rx, rg_set_ry,
                                       rg_center, rg_origin, rg_rx, rg_ry, gr_stops };

HRESULT brush_create(void *factory, int kind, const BRUSHPROPS *bp, Brush **out)
{
    static const void *const *vt[] = { br_solid_vtbl, br_linear_vtbl, br_radial_vtbl, br_bitmap_vtbl };
    *out = 0;
    Brush *b = d_alloc(sizeof(*b));
    if (!b) return E_OUTOFMEMORY;
    b->vtbl = vt[kind];
    b->ref = 1;
    b->kind = kind;
    b->factory = factory;
    factory_addref(factory);
    b->opacity = bp ? bp->opacity : 1;
    b->transform = bp ? bp->transform : MAT_IDENTITY;
    *out = b;
    return S_OK;
}

void brush_set_stops(Brush *b, void *stops)
{
    GradientStops *g = stops && *(const void **)stops == (const void *)gs_vtbl ? stops : NULL;
    if (g) gs_addref(g);
    b->stops = g;
}

int brush_paint(Brush *b, const MAT *world, Paint *p)
{
    memset(p, 0, sizeof(*p));
    p->kind = b->kind;
    p->opacity = clamp01(b->opacity);
    if (b->kind == PAINT_SOLID) {
        p->color = premul(&b->color, p->opacity);
        p->opacity = 1;
        return p->color != 0;
    }
    MAT dev = mat_mul(&b->transform, world);
    if (!mat_invert(&dev, &p->inv)) return 0;
    if (b->kind == PAINT_BITMAP) {
        if (!b->bitmap) return 0;
        MAT px = { b->bitmap->dpix / 96, 0, 0, b->bitmap->dpiy / 96, 0, 0 };
        p->inv = mat_mul(&p->inv, &px);
        p->bmp = &b->bitmap->s;
        p->extend = b->extend_x;
        p->extend_y = b->extend_y;
        p->linear_filter = b->interp == 1;
        return 1;
    }
    if (!b->stops) return 0;
    p->lut = b->stops->lut;
    p->extend = b->stops->extend;
    p->p0 = b->p0;
    p->p1 = b->p1;
    p->rx = b->rx;
    p->ry = b->ry;
    return 1;
}
