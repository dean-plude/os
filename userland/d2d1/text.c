/*
 * text.c — text through DirectWrite: glyph runs become outlines from
 * IDWriteFontFace::GetGlyphRunOutline and are filled like any geometry;
 * layouts are drawn by IDWriteTextLayout::Draw calling back into our
 * IDWriteTextRenderer; DrawText makes a layout with the shared
 * IDWriteFactory first.  A drawing effect that is one of our brushes
 * colours its range.
 */
#include "d2d_int.h"
#include <windows.h>

enum { TEXT_AA_ALIASED = 3, OPT_NO_SNAP = 1, OPT_CLIP = 2 };

void text_draw_glyph_run(Target *t, PT origin, const DW_GLYPHRUN *run, void *brush)
{
    if (!run->fontFace || !run->glyphCount) return;
    Path p;
    path_init(&p);
    void *sink = geometry_sink_new(&p);
    if (!sink) return;
    typedef HRESULT (STDMETHODCALLTYPE *Outline)(void *, float, const UINT16 *, const float *, const DW_OFFSET *,
                                                 UINT32, BOOL, BOOL, void *);
    HRESULT hr = VSLOT(run->fontFace, 14, Outline)(run->fontFace, run->fontEmSize, run->glyphIndices,
                                                   run->glyphAdvances, run->glyphOffsets, run->glyphCount,
                                                   run->isSideways, run->bidiLevel & 1, sink);
    COM_RELEASE(sink);
    if (SUCCEEDED(hr)) {
        MAT saved = t->transform, shift = { 1, 0, 0, 1, origin.x, origin.y };
        UINT32 aa = t->aa;
        t->transform = mat_mul(&shift, &saved);
        t->aa = t->text_aa == TEXT_AA_ALIASED;
        target_fill(t, &p, brush, NULL);
        t->transform = saved;
        t->aa = aa;
    }
    path_free(&p);
}

/* ---- IDWriteTextRenderer ---- */
typedef struct {
    const void *vtbl;
    Target *t;
    void *brush;
    UINT32 options;
} Renderer;

static HRESULT STDMETHODCALLTYPE r_qi(Renderer *r, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDWritePixelSnapping_) ||
        IsEqualIID(riid, &IID_IDWriteTextRenderer_)) {
        *out = r;
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE r_addref(Renderer *r) { (void)r; return 1; }
static ULONG STDMETHODCALLTYPE r_release(Renderer *r) { (void)r; return 1; }
static HRESULT STDMETHODCALLTYPE r_snapping(Renderer *r, void *ctx, BOOL *off)
{
    (void)ctx;
    *off = (r->options & OPT_NO_SNAP) != 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_transform(Renderer *r, void *ctx, MAT *m)
{
    (void)ctx;
    *m = r->t->transform;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_ppd(Renderer *r, void *ctx, float *ppd)
{
    (void)ctx;
    *ppd = r->t->dpix / 96;
    return S_OK;
}
static void *effect_brush(Renderer *r, void *effect)
{
    return brush_of(effect) ? effect : r->brush;
}
static HRESULT STDMETHODCALLTYPE r_glyphs(Renderer *r, void *ctx, float x, float y, UINT32 mode,
                                          const DW_GLYPHRUN *run, const void *desc, void *effect)
{
    (void)ctx; (void)mode; (void)desc;
    PT o = { x, y };
    if (run) text_draw_glyph_run(r->t, o, run, effect_brush(r, effect));
    return S_OK;
}
static void line_rect(Renderer *r, float x, float y, float width, float offset, float thickness, UINT32 dir,
                      void *effect)
{
    RCF rc = { x, y + offset, x + width, y + offset + thickness };
    if (dir == 1) { rc.left = x - width; rc.right = x; }   /* right to left */
    Path p;
    path_init(&p);
    path_rect(&p, &rc);
    target_fill(r->t, &p, effect_brush(r, effect), NULL);
    path_free(&p);
}
static HRESULT STDMETHODCALLTYPE r_underline(Renderer *r, void *ctx, float x, float y, const DW_UNDERLINE *u,
                                             void *effect)
{
    (void)ctx;
    if (u) line_rect(r, x, y, u->width, u->offset, u->thickness, u->readingDirection, effect);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_strike(Renderer *r, void *ctx, float x, float y, const DW_STRIKETHROUGH *s,
                                          void *effect)
{
    (void)ctx;
    if (s) line_rect(r, x, y, s->width, s->offset, s->thickness, s->readingDirection, effect);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_inline(Renderer *r, void *ctx, float x, float y, void *obj, BOOL sideways,
                                          BOOL rtl, void *effect)
{
    if (!obj) return E_INVALIDARG;
    typedef HRESULT (STDMETHODCALLTYPE *Draw)(void *, void *, void *, float, float, BOOL, BOOL, void *);
    return VSLOT(obj, 3, Draw)(obj, ctx, r, x, y, sideways, rtl, effect);
}
static const void *const r_vtbl[] = { r_qi, r_addref, r_release, r_snapping, r_transform, r_ppd, r_glyphs,
                                      r_underline, r_strike, r_inline };

void text_draw_layout(Target *t, PT origin, void *layout, void *brush, UINT32 options)
{
    Renderer r = { r_vtbl, t, brush, options };
    int clipped = 0;
    if (options & OPT_CLIP) {
        typedef float (STDMETHODCALLTYPE *GetF)(void *);
        float w = VSLOT(layout, 42, GetF)(layout), h = VSLOT(layout, 43, GetF)(layout);
        RCF rc = { origin.x, origin.y, origin.x + w, origin.y + h };
        typedef void (STDMETHODCALLTYPE *PushClip)(void *, const RCF *, UINT32);
        VSLOT(t, 45, PushClip)(t, &rc, t->aa);
        clipped = 1;
    }
    typedef HRESULT (STDMETHODCALLTYPE *Draw)(void *, void *, void *, float, float);
    VSLOT(layout, 58, Draw)(layout, NULL, &r, origin.x, origin.y);
    if (clipped) {
        typedef void (STDMETHODCALLTYPE *PopClip)(void *);
        VSLOT(t, 46, PopClip)(t);
    }
}

/* the process's shared IDWriteFactory */
static void *dwrite_factory(void)
{
    static void *volatile factory;
    if (!factory) {
        HMODULE m = LoadLibraryW(L"dwrite.dll");
        typedef HRESULT (WINAPI *Create)(UINT32, REFIID, void **);
        Create create = m ? (Create)GetProcAddress(m, "DWriteCreateFactory") : NULL;
        void *f = NULL;
        if (create && SUCCEEDED(create(0, &IID_IDWriteFactory_, &f)) && f &&
            InterlockedCompareExchangePointer((void *volatile *)&factory, f, NULL) != NULL)
            COM_RELEASE(f);     /* another thread won */
    }
    return factory;
}

void text_draw_string(Target *t, const WCHAR *s, UINT32 n, void *format, const RCF *rc, void *brush, UINT32 options,
                      UINT32 measuring)
{
    (void)measuring;
    void *f = dwrite_factory(), *layout = NULL;
    if (!f) return;
    typedef HRESULT (STDMETHODCALLTYPE *CreateLayout)(void *, const WCHAR *, UINT32, void *, float, float, void **);
    float w = rc->right - rc->left, h = rc->bottom - rc->top;
    if (FAILED(VSLOT(f, 18, CreateLayout)(f, s, n, format, w > 0 ? w : 0, h > 0 ? h : 0, &layout)) || !layout) return;
    PT o = { rc->left, rc->top };
    text_draw_layout(t, o, layout, brush, options);
    COM_RELEASE(layout);
}
