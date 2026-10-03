/*
 * face.c — NovaOS DirectWrite: font faces (IDWriteFontFace1), glyph run
 * outlines and glyph run analysis (the anti-aliased coverage that
 * renderers such as Skia and Direct2D blend)
 */
#include "dwrite_int.h"

#define OBLIQUE_SHEAR 0.3333f     /* the slant of simulated oblique faces */

extern const void *const face_vtbl[];

FontFace *font_face_from(void *iface)
{
    return iface && *(void **)iface == (void *)face_vtbl ? iface : NULL;
}

static int iround(double x) { return (int)dw_floor(x + 0.5); }

/* -----------------------------------------------------------------------
 * IDWriteFontFace / IDWriteFontFace1
 * ----------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE fc_qi(FontFace *f, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDWriteFontFace) ||
        IsEqualGUID(riid, &IID_IDWriteFontFace1)) {
        *out = f; InterlockedIncrement(&f->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fc_addref(FontFace *f) { return (ULONG)InterlockedIncrement(&f->ref); }
static ULONG STDMETHODCALLTYPE fc_release(FontFace *f)
{
    LONG r = InterlockedDecrement(&f->ref);
    if (!r) {
        font_data_release(f->face->file);
        COM_RELEASE(f->file);
        dw_free(f);
    }
    return (ULONG)r;
}
static UINT32 STDMETHODCALLTYPE fc_type(FontFace *f) { return f->face->file->face_type; }
static HRESULT STDMETHODCALLTYPE fc_files(FontFace *f, UINT32 *n, void **files)
{
    if (files) {
        if (*n < 1) return E_INVALIDARG;
        COM_ADDREF(f->file);
        files[0] = f->file;
    }
    *n = 1;
    return S_OK;
}
static UINT32 STDMETHODCALLTYPE fc_index(FontFace *f) { return f->index; }
static UINT32 STDMETHODCALLTYPE fc_sims(FontFace *f) { return f->sims; }
static BOOL STDMETHODCALLTYPE fc_symbol(FontFace *f) { return f->face->symbol; }
static void STDMETHODCALLTYPE fc_metrics(FontFace *f, DW_FONT_METRICS *m) { *m = f->face->metrics.m; }
static UINT16 STDMETHODCALLTYPE fc_glyph_count(FontFace *f) { return f->face->num_glyphs; }

static HRESULT STDMETHODCALLTYPE fc_design_glyph_metrics(FontFace *f, const UINT16 *glyphs, UINT32 n,
                                                         DW_GLYPH_METRICS *m, BOOL sideways)
{
    (void)sideways;
    if (!glyphs || !m) return E_INVALIDARG;
    for (UINT32 i = 0; i < n; i++) {
        face_glyph_metrics(f->face, glyphs[i], &m[i]);
        if ((f->sims & SIM_BOLD) && m[i].advanceWidth) m[i].advanceWidth += f->face->upem / 32;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fc_glyph_indices(FontFace *f, const UINT32 *cps, UINT32 n, UINT16 *out)
{
    if (!cps || !out) return E_INVALIDARG;
    for (UINT32 i = 0; i < n; i++) out[i] = face_glyph_index(f->face, cps[i]);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fc_table(FontFace *f, UINT32 tag, const void **data, UINT32 *size, void **ctx, BOOL *exists)
{
    char t[4] = { (char)tag, (char)(tag >> 8), (char)(tag >> 16), (char)(tag >> 24) };
    UINT32 n = 0;
    const BYTE *p = sfnt_table(f->face, t, &n);
    *data = p;
    *size = p ? n : 0;
    *ctx = NULL;
    *exists = p != NULL;
    return S_OK;
}
static void STDMETHODCALLTYPE fc_release_table(FontFace *f, void *ctx) { (void)f; (void)ctx; }

/* ---- outlines into an ID2D1SimplifiedGeometrySink ---- */
typedef void (STDMETHODCALLTYPE *SinkFillMode)(void *, UINT32);
typedef void (STDMETHODCALLTYPE *SinkBegin)(void *, D2_POINT, UINT32);
typedef void (STDMETHODCALLTYPE *SinkLines)(void *, const D2_POINT *, UINT32);
typedef void (STDMETHODCALLTYPE *SinkBeziers)(void *, const D2_BEZIER *, UINT32);
typedef void (STDMETHODCALLTYPE *SinkEnd)(void *, UINT32);

static HRESULT STDMETHODCALLTYPE fc_outline(FontFace *f, float em, const UINT16 *glyphs, const float *advances,
                                            const DW_GLYPH_OFFSET *offsets, UINT32 n, BOOL sideways, BOOL rtl, void *sink)
{
    (void)sideways;
    if (!sink || (n && !glyphs)) return E_INVALIDARG;
    FaceData *fd = f->face;
    float s = em / fd->upem;
    float shear = (f->sims & SIM_OBLIQUE) ? OBLIQUE_SHEAR : 0;
    ((SinkFillMode)VT(sink)[3])(sink, 1 /* D2D1_FILL_MODE_WINDING */);
    float pen = 0;
    for (UINT32 i = 0; i < n; i++) {
        float adv;
        if (advances) adv = advances[i];
        else {
            DW_GLYPH_METRICS gm;
            face_glyph_metrics(fd, glyphs[i], &gm);
            adv = gm.advanceWidth * s;
        }
        float ox, oy = 0;
        if (rtl) { pen -= adv; ox = pen; } else ox = pen;
        if (offsets) {
            ox += rtl ? -offsets[i].advanceOffset : offsets[i].advanceOffset;
            oy -= offsets[i].ascenderOffset;
        }
        if (!rtl) pen += adv;

        GlyphVertex *v = NULL;
        int nv = face_glyph_shape(fd, glyphs[i], &v);
        BOOL open = FALSE;
        D2_POINT cur = { 0, 0 };
#define PT(px, py) ((D2_POINT){ ox + ((px) + shear * (py)) * s, oy - (py) * s })
        for (int k = 0; k < nv; k++) {
            D2_POINT p = PT(v[k].x, v[k].y);
            switch (v[k].type) {
            case GV_MOVE:
                if (open) ((SinkEnd)VT(sink)[8])(sink, 1 /* D2D1_FIGURE_END_CLOSED */);
                ((SinkBegin)VT(sink)[5])(sink, p, 0 /* D2D1_FIGURE_BEGIN_FILLED */);
                open = TRUE;
                break;
            case GV_LINE:
                ((SinkLines)VT(sink)[6])(sink, &p, 1);
                break;
            case GV_QUAD: {
                D2_POINT q = PT(v[k].cx, v[k].cy);
                D2_BEZIER b = {
                    { cur.x + (q.x - cur.x) * (2.0f / 3), cur.y + (q.y - cur.y) * (2.0f / 3) },
                    { p.x + (q.x - p.x) * (2.0f / 3), p.y + (q.y - p.y) * (2.0f / 3) },
                    p };
                ((SinkBeziers)VT(sink)[7])(sink, &b, 1);
                break;
            }
            case GV_CUBIC: {
                D2_BEZIER b = { PT(v[k].cx, v[k].cy), PT(v[k].cx1, v[k].cy1), p };
                ((SinkBeziers)VT(sink)[7])(sink, &b, 1);
                break;
            }
            }
            cur = p;
        }
#undef PT
        if (open) ((SinkEnd)VT(sink)[8])(sink, 1);
        face_free_shape(v);
    }
    return S_OK;
}

static UINT32 recommend(float px, UINT32 measuring, void *params)
{
    if (params) {
        UINT32 m = ((UINT32 (STDMETHODCALLTYPE *)(void *))VT(params)[7])(params);
        if (m != RMODE_DEFAULT) return m;
    }
    if (px >= 100) return RMODE_OUTLINE;
    return measuring == 1 ? RMODE_GDI_CLASSIC : measuring == 2 ? RMODE_GDI_NATURAL : RMODE_NATURAL_SYMMETRIC;
}
static HRESULT STDMETHODCALLTYPE fc_recommended(FontFace *f, float em, float ppd, UINT32 measuring, void *params, UINT32 *mode)
{
    (void)f;
    *mode = recommend(em * ppd, measuring, params);
    return S_OK;
}

/* GDI-compatible values: design units rounded to whole pixels at the given size */
static float pixel_em(float em, float ppd, const DW_MATRIX *m)
{
    float s = em * ppd;
    if (m) { float k = m->m22 < 0 ? -m->m22 : m->m22; if (k > 0) s *= k; }
    return s > 0 ? s : 1;
}
static INT32 gdi_round(INT32 v, float px, UINT16 upem)
{
    return (INT32)(iround(v * px / upem) * (double)upem / px + (v < 0 ? -0.5 : 0.5));
}
static void gdi_metrics1(FontFace *f, float em, float ppd, const DW_MATRIX *t, DW_FONT_METRICS1 *m)
{
    *m = f->face->metrics;
    float px = pixel_em(em, ppd, t);
    UINT16 u = f->face->upem;
    m->m.ascent = (UINT16)gdi_round(m->m.ascent, px, u);
    m->m.descent = (UINT16)gdi_round(m->m.descent, px, u);
    m->m.lineGap = (INT16)gdi_round(m->m.lineGap, px, u);
    m->m.capHeight = (UINT16)gdi_round(m->m.capHeight, px, u);
    m->m.xHeight = (UINT16)gdi_round(m->m.xHeight, px, u);
    m->m.underlinePosition = (INT16)gdi_round(m->m.underlinePosition, px, u);
    m->m.underlineThickness = (UINT16)gdi_round(m->m.underlineThickness, px, u);
    m->m.strikethroughPosition = (INT16)gdi_round(m->m.strikethroughPosition, px, u);
    m->m.strikethroughThickness = (UINT16)gdi_round(m->m.strikethroughThickness, px, u);
    if (!m->m.underlineThickness) m->m.underlineThickness = (UINT16)(u / px + 0.5f);
    if (!m->m.strikethroughThickness) m->m.strikethroughThickness = (UINT16)(u / px + 0.5f);
}
static HRESULT STDMETHODCALLTYPE fc_gdi_metrics(FontFace *f, float em, float ppd, const DW_MATRIX *t, DW_FONT_METRICS *m)
{
    DW_FONT_METRICS1 m1;
    gdi_metrics1(f, em, ppd, t, &m1);
    *m = m1.m;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fc_gdi_glyph_metrics(FontFace *f, float em, float ppd, const DW_MATRIX *t, BOOL natural,
                                                      const UINT16 *glyphs, UINT32 n, DW_GLYPH_METRICS *m, BOOL sideways)
{
    (void)natural;
    HRESULT hr = fc_design_glyph_metrics(f, glyphs, n, m, sideways);
    if (FAILED(hr)) return hr;
    float px = pixel_em(em, ppd, t);
    UINT16 u = f->face->upem;
    for (UINT32 i = 0; i < n; i++) {
        m[i].advanceWidth = (UINT32)gdi_round((INT32)m[i].advanceWidth, px, u);
        m[i].advanceHeight = (UINT32)gdi_round((INT32)m[i].advanceHeight, px, u);
        m[i].leftSideBearing = gdi_round(m[i].leftSideBearing, px, u);
        m[i].rightSideBearing = gdi_round(m[i].rightSideBearing, px, u);
        m[i].topSideBearing = gdi_round(m[i].topSideBearing, px, u);
        m[i].bottomSideBearing = gdi_round(m[i].bottomSideBearing, px, u);
        m[i].verticalOriginY = gdi_round(m[i].verticalOriginY, px, u);
    }
    return S_OK;
}
static void STDMETHODCALLTYPE fc_metrics1(FontFace *f, DW_FONT_METRICS1 *m) { *m = f->face->metrics; }
static HRESULT STDMETHODCALLTYPE fc_gdi_metrics1(FontFace *f, float em, float ppd, const DW_MATRIX *t, DW_FONT_METRICS1 *m)
{
    gdi_metrics1(f, em, ppd, t, m);
    return S_OK;
}
static void STDMETHODCALLTYPE fc_caret(FontFace *f, DW_CARET_METRICS *c)
{
    *c = f->face->caret;
    if ((f->sims & SIM_OBLIQUE) && c->slopeRun == 0) {
        c->slopeRise = (INT16)f->face->upem;
        c->slopeRun = (INT16)(f->face->upem * OBLIQUE_SHEAR);
    }
}
static HRESULT STDMETHODCALLTYPE fc_ranges(FontFace *f, UINT32 max, DW_UNICODE_RANGE *r, UINT32 *count)
{
    *count = face_unicode_ranges(f->face, r, max);
    return *count > max ? E_NOT_SUFFICIENT_BUFFER_ : S_OK;
}
static BOOL STDMETHODCALLTYPE fc_mono(FontFace *f) { return f->face->mono; }
static HRESULT STDMETHODCALLTYPE fc_design_advances(FontFace *f, UINT32 n, const UINT16 *glyphs, INT32 *adv, BOOL sideways)
{
    for (UINT32 i = 0; i < n; i++) {
        DW_GLYPH_METRICS m;
        face_glyph_metrics(f->face, glyphs[i], &m);
        adv[i] = (INT32)(sideways ? m.advanceHeight : m.advanceWidth);
        if ((f->sims & SIM_BOLD) && adv[i] && !sideways) adv[i] += f->face->upem / 32;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fc_gdi_advances(FontFace *f, float em, float ppd, const DW_MATRIX *t, BOOL natural,
                                                 BOOL sideways, UINT32 n, const UINT16 *glyphs, INT32 *adv)
{
    (void)natural;
    fc_design_advances(f, n, glyphs, adv, sideways);
    float px = pixel_em(em, ppd, t);
    for (UINT32 i = 0; i < n; i++) adv[i] = gdi_round(adv[i], px, f->face->upem);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fc_kerning(FontFace *f, UINT32 n, const UINT16 *glyphs, INT32 *adj)
{
    for (UINT32 i = 0; i < n; i++) adj[i] = i + 1 < n ? face_kern(f->face, glyphs[i], glyphs[i + 1]) : 0;
    return S_OK;
}
static BOOL STDMETHODCALLTYPE fc_has_kerning(FontFace *f) { return f->face->has_kerning; }
static HRESULT STDMETHODCALLTYPE fc_recommended1(FontFace *f, float em, float dpi_x, float dpi_y, const DW_MATRIX *t,
                                                 BOOL sideways, UINT32 threshold, UINT32 measuring, UINT32 *mode)
{
    (void)f; (void)dpi_x; (void)sideways; (void)threshold;
    *mode = recommend(pixel_em(em, dpi_y / 96.0f, t), measuring, NULL);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fc_vertical_variants(FontFace *f, UINT32 n, const UINT16 *nominal, UINT16 *vertical)
{
    (void)f;
    memcpy(vertical, nominal, n * sizeof(UINT16));
    return S_OK;
}
static BOOL STDMETHODCALLTYPE fc_has_vertical(FontFace *f) { (void)f; return FALSE; }

const void *const face_vtbl[] = {
    fc_qi, fc_addref, fc_release, fc_type, fc_files, fc_index, fc_sims, fc_symbol, fc_metrics, fc_glyph_count,
    fc_design_glyph_metrics, fc_glyph_indices, fc_table, fc_release_table, fc_outline, fc_recommended,
    fc_gdi_metrics, fc_gdi_glyph_metrics,
    fc_metrics1, fc_gdi_metrics1, fc_caret, fc_ranges, fc_mono, fc_design_advances, fc_gdi_advances,
    fc_kerning, fc_has_kerning, fc_recommended1, fc_vertical_variants, fc_has_vertical,
};

HRESULT font_face_create(FontFile *file, UINT32 index, UINT32 sims, FontFace **out)
{
    *out = NULL;
    FontData *d = NULL;
    HRESULT hr = font_file_data(file, &d);
    if (FAILED(hr)) return hr;
    FaceData *fd = font_face_data(d, index);
    if (!fd) { font_data_release(d); return DWRITE_E_FILEFORMAT; }
    FontFace *f = dw_zalloc(sizeof(*f));
    if (!f) { font_data_release(d); return E_OUTOFMEMORY; }
    f->vtbl = face_vtbl;
    f->ref = 1;
    f->file = file;
    COM_ADDREF(file);
    f->face = fd;           /* holds the reference font_file_data took on d */
    f->index = index;
    f->sims = sims & (SIM_BOLD | SIM_OBLIQUE);
    *out = f;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * IDWriteGlyphRunAnalysis
 * ----------------------------------------------------------------------- */
typedef struct {
    const void *const *vtbl;
    LONG   ref;
    UINT32 mode;
    RECT   bounds;          /* device pixels; empty when the run draws nothing */
    BYTE  *alpha;           /* 8-bit coverage over bounds */
} Analysis;

static HRESULT STDMETHODCALLTYPE an_qi(Analysis *a, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDWriteGlyphRunAnalysis)) {
        *out = a; InterlockedIncrement(&a->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE an_addref(Analysis *a) { return (ULONG)InterlockedIncrement(&a->ref); }
static ULONG STDMETHODCALLTYPE an_release(Analysis *a)
{
    LONG r = InterlockedDecrement(&a->ref);
    if (!r) { dw_free(a->alpha); dw_free(a); }
    return (ULONG)r;
}
static BOOL type_matches(Analysis *a, UINT32 type)
{
    return (a->mode == RMODE_ALIASED) == (type == TEX_ALIASED_1x1);
}
static HRESULT STDMETHODCALLTYPE an_bounds(Analysis *a, UINT32 type, RECT *r)
{
    if (type > TEX_CLEARTYPE_3x1) return E_INVALIDARG;
    if (type_matches(a, type)) *r = a->bounds;
    else memset(r, 0, sizeof(*r));
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE an_texture(Analysis *a, UINT32 type, const RECT *r, BYTE *buf, UINT32 size)
{
    if (type > TEX_CLEARTYPE_3x1 || !r || !buf) return E_INVALIDARG;
    int w = r->right - r->left, h = r->bottom - r->top, bpp = type == TEX_CLEARTYPE_3x1 ? 3 : 1;
    if (w <= 0 || h <= 0) return S_OK;
    if ((UINT64)w * h * bpp > size) return E_NOT_SUFFICIENT_BUFFER_;
    memset(buf, 0, (SIZE_T)w * h * bpp);
    if (!type_matches(a, type) || !a->alpha) return S_OK;
    int bw = a->bounds.right - a->bounds.left;
    for (int y = 0; y < h; y++) {
        int sy = r->top + y;
        if (sy < a->bounds.top || sy >= a->bounds.bottom) continue;
        const BYTE *src = a->alpha + (SIZE_T)(sy - a->bounds.top) * bw;
        BYTE *dst = buf + (SIZE_T)y * w * bpp;
        for (int x = 0; x < w; x++) {
            int sx = r->left + x;
            if (sx < a->bounds.left || sx >= a->bounds.right) continue;
            BYTE v = src[sx - a->bounds.left];
            if (bpp == 3) { dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = v; }
            else dst[x] = v;
        }
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE an_blend_params(Analysis *a, void *params, float *gamma, float *contrast, float *cleartype)
{
    (void)a;
    *gamma = 1.8f; *contrast = 0.5f; *cleartype = 0.0f;
    if (params) {
        *gamma = ((float (STDMETHODCALLTYPE *)(void *))VT(params)[3])(params);
        *contrast = ((float (STDMETHODCALLTYPE *)(void *))VT(params)[4])(params);
        *cleartype = ((float (STDMETHODCALLTYPE *)(void *))VT(params)[5])(params);
    }
    return S_OK;
}
static const void *const an_vtbl[] = { an_qi, an_addref, an_release, an_bounds, an_texture, an_blend_params };

/* the font-units → device matrix of glyph i, whose origin is (gx, gy) in DIPs */
static void glyph_matrix(const DW_MATRIX *m, float s, float shear, float gx, float gy, DW_MATRIX *g)
{
    /* DIP point = (gx + (fx + shear*fy)*s, gy - fy*s), then through m */
    g->m11 = s * m->m11;
    g->m12 = s * m->m12;
    g->m21 = shear * s * m->m11 - s * m->m21;
    g->m22 = shear * s * m->m12 - s * m->m22;
    g->dx = gx * m->m11 + gy * m->m21 + m->dx;
    g->dy = gx * m->m12 + gy * m->m22 + m->dy;
}

HRESULT glyph_run_analysis_create(const DW_GLYPH_RUN *run, float ppd, const DW_MATRIX *t, UINT32 mode,
                                  float ox, float oy, void **out)
{
    *out = NULL;
    FontFace *f = run ? font_face_from(run->fontFace) : NULL;
    if (!f || (run->glyphCount && !run->glyphIndices) || ppd <= 0) return E_INVALIDARG;
    if (mode == RMODE_OUTLINE) return E_INVALIDARG;
    Analysis *a = dw_zalloc(sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    a->vtbl = an_vtbl;
    a->ref = 1;
    a->mode = mode == RMODE_ALIASED ? RMODE_ALIASED : RMODE_NATURAL_SYMMETRIC;

    DW_MATRIX m = t ? *t : (DW_MATRIX){ 1, 0, 0, 1, 0, 0 };
    m.m11 *= ppd; m.m12 *= ppd; m.m21 *= ppd; m.m22 *= ppd; m.dx *= ppd; m.dy *= ppd;
    FaceData *fd = f->face;
    float s = run->fontEmSize / fd->upem;
    float shear = (f->sims & SIM_OBLIQUE) ? OBLIQUE_SHEAR : 0;
    BOOL rtl = run->bidiLevel & 1;
    /* simulated bold: the glyph drawn again, shifted right */
    float bold = (f->sims & SIM_BOLD) ? run->fontEmSize * ppd / 32 : 0;
    if (bold > 0 && bold < 0.5f) bold = 0.5f;

    UINT32 n = run->glyphCount;
    DW_MATRIX *gm = dw_alloc((n ? n : 1) * sizeof(DW_MATRIX));
    if (!gm) { dw_free(a); return E_OUTOFMEMORY; }
    float pen = 0;
    int x0 = 0x7FFFFFFF, y0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, y1 = -0x7FFFFFFF;
    for (UINT32 i = 0; i < n; i++) {
        float adv;
        if (run->glyphAdvances) adv = run->glyphAdvances[i];
        else { DW_GLYPH_METRICS g; face_glyph_metrics(fd, run->glyphIndices[i], &g); adv = g.advanceWidth * s; }
        float gx, gy = oy;
        if (rtl) { pen -= adv; gx = ox + pen; } else gx = ox + pen;
        if (run->glyphOffsets) {
            gx += rtl ? -run->glyphOffsets[i].advanceOffset : run->glyphOffsets[i].advanceOffset;
            gy -= run->glyphOffsets[i].ascenderOffset;
        }
        if (!rtl) pen += adv;
        glyph_matrix(&m, s, shear, gx, gy, &gm[i]);
        int r[4];
        face_glyph_bounds(fd, run->glyphIndices[i], &gm[i], r);
        if (r[0] >= r[2]) continue;
        if (r[0] < x0) x0 = r[0];
        if (r[1] < y0) y0 = r[1];
        if (r[2] + (int)dw_ceil(bold) > x1) x1 = r[2] + (int)dw_ceil(bold);
        if (r[3] > y1) y1 = r[3];
    }
    if (x0 < x1 && y0 < y1 && (UINT64)(x1 - x0) * (y1 - y0) < 64u * 1024 * 1024) {
        int w = x1 - x0, h = y1 - y0;
        a->alpha = dw_zalloc((SIZE_T)w * h);
        if (a->alpha) {
            a->bounds.left = x0; a->bounds.top = y0; a->bounds.right = x1; a->bounds.bottom = y1;
            for (UINT32 i = 0; i < n; i++) {
                face_raster_glyph(fd, run->glyphIndices[i], &gm[i], a->alpha, w, h, x0, y0);
                if (bold > 0) {
                    DW_MATRIX b = gm[i];
                    b.dx += bold;
                    face_raster_glyph(fd, run->glyphIndices[i], &b, a->alpha, w, h, x0, y0);
                }
            }
            if (a->mode == RMODE_ALIASED)
                for (SIZE_T k = 0; k < (SIZE_T)w * h; k++) a->alpha[k] = a->alpha[k] >= 128 ? 255 : 0;
        }
    }
    dw_free(gm);
    *out = a;
    return S_OK;
}
