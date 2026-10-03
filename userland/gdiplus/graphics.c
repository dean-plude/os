/*
 * graphics.c — GDI+ graphics objects and drawing: state, world transforms,
 * clipping, brushes, pens, lines, curves, shapes, images, matrices and
 * regions.  Drawing on a device context reads the affected part of it into
 * a DIB, draws there with plutovg and copies it back, so the DC's own
 * clipping still applies.
 */
#include "gdip.h"

/* ---- graphics --------------------------------------------------------- */

static GpGraphics *graphics_new(void)
{
    GpGraphics *g = xalloc(sizeof *g);
    if (!g) return NULL;
    plutovg_matrix_init_identity(&g->world);
    g->page_unit = UnitDisplay;
    g->page_scale = 1.f;
    return g;
}

GDIPAPI GpStatus GDIPCALL GdipCreateFromHDC(HDC hdc, GpGraphics **out)
{
    if (!hdc || !out) return OutOfMemory;            /* GDI+'s answer to a null DC */
    GpGraphics *g = graphics_new();
    if (!g) return OutOfMemory;
    g->hdc = hdc;
    *out = g;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreateFromHDC2(HDC hdc, HANDLE dev, GpGraphics **out) { (void)dev; return GdipCreateFromHDC(hdc, out); }

GDIPAPI GpStatus GDIPCALL GdipCreateFromHWND(HWND w, GpGraphics **out)
{
    if (!out) return InvalidParameter;
    HDC hdc = GetDC(w);
    if (!hdc) return Win32Error;
    GpStatus st = GdipCreateFromHDC(hdc, out);
    if (st == Ok) (*out)->hwnd = w; else ReleaseDC(w, hdc);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipCreateFromHWNDICM(HWND w, GpGraphics **out) { return GdipCreateFromHWND(w, out); }

GDIPAPI GpStatus GDIPCALL GdipGetImageGraphicsContext(GpImage *i, GpGraphics **out)
{
    if (!i || !out) return InvalidParameter;
    GpGraphics *g = graphics_new();
    if (!g) return OutOfMemory;
    g->img = i;
    g->c = plutovg_canvas_create(i->s);
    if (!g->c) { xfree(g); return OutOfMemory; }
    *out = g;
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipDeleteGraphics(GpGraphics *g)
{
    if (!g) return InvalidParameter;
    if (g->c) plutovg_canvas_destroy(g->c);
    if (g->hwnd) ReleaseDC(g->hwnd, g->hdc);
    gdip_region_reset(&g->clip);
    xfree(g);
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipGetDC(GpGraphics *g, HDC *hdc)
{
    if (!g || !hdc) return InvalidParameter;
    if (!g->hdc) return NotImplemented;
    *hdc = g->hdc;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipReleaseDC(GpGraphics *g, HDC hdc) { return g && hdc == g->hdc ? Ok : InvalidParameter; }

#define STATE_PAIR(Name, field, type)                                                              \
    GDIPAPI GpStatus GDIPCALL GdipSet##Name(GpGraphics *g, type v) { if (!g) return InvalidParameter; g->field = (int)v; return Ok; } \
    GDIPAPI GpStatus GDIPCALL GdipGet##Name(GpGraphics *g, type *v) { if (!g || !v) return InvalidParameter; *v = (type)g->field; return Ok; }
STATE_PAIR(SmoothingMode, smoothing, INT)
STATE_PAIR(PixelOffsetMode, pixel_offset, INT)
STATE_PAIR(InterpolationMode, interpolation, INT)
STATE_PAIR(CompositingMode, compositing, INT)
STATE_PAIR(CompositingQuality, compositing_quality, INT)
STATE_PAIR(TextRenderingHint, text_hint, INT)

/* ---- page units --------------------------------------------------------- */

/* a length in a GDI+ unit, in pixels (96 per inch) */
REAL gdip_unit_to_pixels(int unit, REAL v)
{
    switch (unit) {
    case UnitPoint: return v * 96.f / 72.f;
    case UnitInch: return v * 96.f;
    case UnitDocument: return v * 96.f / 300.f;
    case UnitMillimeter: return v * 96.f / 25.4f;
    default: return v;                              /* world, display, pixel */
    }
}

GDIPAPI GpStatus GDIPCALL GdipSetPageUnit(GpGraphics *g, INT unit)
{
    if (!g || unit < UnitWorld || unit > UnitMillimeter || unit == UnitWorld) return InvalidParameter;
    g->page_unit = unit;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPageUnit(GpGraphics *g, INT *unit) { if (!g || !unit) return InvalidParameter; *unit = g->page_unit; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPageScale(GpGraphics *g, REAL s) { if (!g || s <= 0) return InvalidParameter; g->page_scale = s; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPageScale(GpGraphics *g, REAL *s) { if (!g || !s) return InvalidParameter; *s = g->page_scale; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetDpiX(GpGraphics *g, REAL *d) { if (!g || !d) return InvalidParameter; *d = 96.f; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetDpiY(GpGraphics *g, REAL *d) { if (!g || !d) return InvalidParameter; *d = 96.f; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetRenderingOrigin(GpGraphics *g, INT x, INT y) { (void)x; (void)y; return g ? Ok : InvalidParameter; }

/* world -> device: the world transform, then the page unit and scale */
static void world_to_device(const GpGraphics *g, plutovg_matrix_t *m)
{
    REAL s = gdip_unit_to_pixels(g->page_unit, g->page_scale);
    plutovg_matrix_t page;
    plutovg_matrix_init_scale(&page, s, s);
    plutovg_matrix_multiply(m, &g->world, &page);
}

/* ---- matrices ----------------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipCreateMatrix(GpMatrix **out)
{
    if (!out) return InvalidParameter;
    *out = xalloc(sizeof **out);
    if (!*out) return OutOfMemory;
    plutovg_matrix_init_identity(*out);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreateMatrix2(REAL m11, REAL m12, REAL m21, REAL m22, REAL dx, REAL dy, GpMatrix **out)
{
    GpStatus st = GdipCreateMatrix(out);
    if (st == Ok) plutovg_matrix_init(*out, m11, m12, m21, m22, dx, dy);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipCloneMatrix(GpMatrix *m, GpMatrix **out)
{
    if (!m) return InvalidParameter;
    GpStatus st = GdipCreateMatrix(out);
    if (st == Ok) **out = *m;
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipDeleteMatrix(GpMatrix *m) { if (!m) return InvalidParameter; xfree(m); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetMatrixElements(GpMatrix *m, REAL m11, REAL m12, REAL m21, REAL m22, REAL dx, REAL dy)
{ if (!m) return InvalidParameter; plutovg_matrix_init(m, m11, m12, m21, m22, dx, dy); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetMatrixElements(const GpMatrix *m, REAL *e)
{
    if (!m || !e) return InvalidParameter;
    e[0] = m->a; e[1] = m->b; e[2] = m->c; e[3] = m->d; e[4] = m->e; e[5] = m->f;
    return Ok;
}
/* order: MatrixOrderPrepend (the default) applies `t` before the matrix */
static void combine(GpMatrix *m, const plutovg_matrix_t *t, INT order)
{
    if (order == MatrixOrderAppend) plutovg_matrix_multiply(m, m, t);
    else plutovg_matrix_multiply(m, t, m);
}
GDIPAPI GpStatus GDIPCALL GdipMultiplyMatrix(GpMatrix *m, const GpMatrix *t, INT order) { if (!m || !t) return InvalidParameter; combine(m, t, order); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipTranslateMatrix(GpMatrix *m, REAL dx, REAL dy, INT order)
{ if (!m) return InvalidParameter; plutovg_matrix_t t; plutovg_matrix_init_translate(&t, dx, dy); combine(m, &t, order); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipScaleMatrix(GpMatrix *m, REAL sx, REAL sy, INT order)
{ if (!m) return InvalidParameter; plutovg_matrix_t t; plutovg_matrix_init_scale(&t, sx, sy); combine(m, &t, order); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipRotateMatrix(GpMatrix *m, REAL deg, INT order)
{ if (!m) return InvalidParameter; plutovg_matrix_t t; plutovg_matrix_init_rotate(&t, deg * 3.14159265f / 180.f); combine(m, &t, order); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipShearMatrix(GpMatrix *m, REAL sx, REAL sy, INT order)
{ if (!m) return InvalidParameter; plutovg_matrix_t t; plutovg_matrix_init(&t, 1, sy, sx, 1, 0, 0); combine(m, &t, order); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipInvertMatrix(GpMatrix *m) { if (!m) return InvalidParameter; return plutovg_matrix_invert(m, m) ? Ok : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipIsMatrixIdentity(const GpMatrix *m, BOOL *r)
{
    if (!m || !r) return InvalidParameter;
    *r = m->a == 1 && m->b == 0 && m->c == 0 && m->d == 1 && m->e == 0 && m->f == 0;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipIsMatrixInvertible(const GpMatrix *m, BOOL *r)
{ if (!m || !r) return InvalidParameter; plutovg_matrix_t t; *r = plutovg_matrix_invert(m, &t); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipTransformMatrixPoints(GpMatrix *m, GpPointF *p, INT n)
{
    if (!m || !p || n <= 0) return InvalidParameter;
    for (int i = 0; i < n; i++) plutovg_matrix_map(m, p[i].X, p[i].Y, &p[i].X, &p[i].Y);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipTransformMatrixPointsI(GpMatrix *m, GpPoint *p, INT n)
{
    if (!m || !p || n <= 0) return InvalidParameter;
    for (int i = 0; i < n; i++) {
        float x, y;
        plutovg_matrix_map(m, (float)p[i].X, (float)p[i].Y, &x, &y);
        p[i].X = (INT)floorf(x + 0.5f);
        p[i].Y = (INT)floorf(y + 0.5f);
    }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipVectorTransformMatrixPoints(GpMatrix *m, GpPointF *p, INT n)
{
    if (!m || !p || n <= 0) return InvalidParameter;
    for (int i = 0; i < n; i++) {
        REAL x = p[i].X, y = p[i].Y;
        p[i].X = x * m->a + y * m->c;
        p[i].Y = x * m->b + y * m->d;
    }
    return Ok;
}

/* ---- world transform ---------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipResetWorldTransform(GpGraphics *g) { if (!g) return InvalidParameter; plutovg_matrix_init_identity(&g->world); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetWorldTransform(GpGraphics *g, const GpMatrix *m) { if (!g || !m) return InvalidParameter; g->world = *m; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetWorldTransform(GpGraphics *g, GpMatrix *m) { if (!g || !m) return InvalidParameter; *m = g->world; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipMultiplyWorldTransform(GpGraphics *g, const GpMatrix *m, INT order)
{ if (!g || !m) return InvalidParameter; combine(&g->world, m, order); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipTranslateWorldTransform(GpGraphics *g, REAL dx, REAL dy, INT order)
{ if (!g) return InvalidParameter; return GdipTranslateMatrix(&g->world, dx, dy, order); }
GDIPAPI GpStatus GDIPCALL GdipScaleWorldTransform(GpGraphics *g, REAL sx, REAL sy, INT order)
{ if (!g) return InvalidParameter; return GdipScaleMatrix(&g->world, sx, sy, order); }
GDIPAPI GpStatus GDIPCALL GdipRotateWorldTransform(GpGraphics *g, REAL deg, INT order)
{ if (!g) return InvalidParameter; return GdipRotateMatrix(&g->world, deg, order); }

/* points between world (0), page (1) and device (2) coordinates */
static GpStatus transform_points(GpGraphics *g, INT dst, INT src, GpPointF *p, INT n)
{
    if (!g || !p || n <= 0 || dst < 0 || dst > 2 || src < 0 || src > 2) return InvalidParameter;
    if (dst == src) return Ok;
    plutovg_matrix_t to_dev[3], m;
    world_to_device(g, &to_dev[0]);                 /* world -> device */
    REAL s = gdip_unit_to_pixels(g->page_unit, g->page_scale);
    plutovg_matrix_init_scale(&to_dev[1], s, s);    /* page -> device */
    plutovg_matrix_init_identity(&to_dev[2]);
    plutovg_matrix_t inv;
    if (!plutovg_matrix_invert(&to_dev[dst], &inv)) return InvalidParameter;
    plutovg_matrix_multiply(&m, &to_dev[src], &inv);
    for (int i = 0; i < n; i++) plutovg_matrix_map(&m, p[i].X, p[i].Y, &p[i].X, &p[i].Y);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipTransformPoints(GpGraphics *g, INT dst, INT src, GpPointF *p, INT n) { return transform_points(g, dst, src, p, n); }
GDIPAPI GpStatus GDIPCALL GdipTransformPointsI(GpGraphics *g, INT dst, INT src, GpPoint *p, INT n)
{
    if (!p || n <= 0) return InvalidParameter;
    GpPointF *f = xalloc(sizeof *f * n);
    if (!f) return OutOfMemory;
    for (int i = 0; i < n; i++) f[i] = (GpPointF){ (REAL)p[i].X, (REAL)p[i].Y };
    GpStatus st = transform_points(g, dst, src, f, n);
    if (st == Ok) for (int i = 0; i < n; i++) { p[i].X = (INT)floorf(f[i].X + 0.5f); p[i].Y = (INT)floorf(f[i].Y + 0.5f); }
    xfree(f);
    return st;
}

/* ---- regions ------------------------------------------------------------ */

GDIPAPI GpStatus GDIPCALL GdipCreateRegion(GpRegion **out)
{
    if (!out) return InvalidParameter;
    *out = xalloc(sizeof **out);
    return *out ? Ok : OutOfMemory;                 /* RgnInfinite */
}
GDIPAPI GpStatus GDIPCALL GdipCreateRegionRect(const GpRectF *r, GpRegion **out)
{
    if (!r) return InvalidParameter;
    GpStatus st = GdipCreateRegion(out);
    if (st == Ok) { (*out)->kind = RgnRect; (*out)->r = *r; }
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipCreateRegionRectI(const GpRect *r, GpRegion **out)
{
    if (!r) return InvalidParameter;
    GpRectF f = { (REAL)r->X, (REAL)r->Y, (REAL)r->Width, (REAL)r->Height };
    return GdipCreateRegionRect(&f, out);
}
GDIPAPI GpStatus GDIPCALL GdipCreateRegionPath(GpPath *p, GpRegion **out)
{
    if (!p) return InvalidParameter;
    GpStatus st = GdipCreateRegion(out);
    if (st != Ok) return st;
    (*out)->kind = RgnPath;
    st = GdipClonePath(p, &(*out)->path);
    if (st != Ok) { xfree(*out); *out = NULL; }
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipCreateRegionHrgn(HRGN h, GpRegion **out)
{
    RECT r;
    if (!h || GetRgnBox(h, &r) == ERROR) return InvalidParameter;
    GpRectF f = { (REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top) };
    return GdipCreateRegionRect(&f, out);
}
GDIPAPI GpStatus GDIPCALL GdipCloneRegion(GpRegion *r, GpRegion **out)
{
    if (!r) return InvalidParameter;
    GpStatus st = GdipCreateRegion(out);
    if (st == Ok && (st = gdip_region_copy(*out, r)) != Ok) { xfree(*out); *out = NULL; }
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipDeleteRegion(GpRegion *r) { if (!r) return InvalidParameter; gdip_region_reset(r); xfree(r); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetInfinite(GpRegion *r) { if (!r) return InvalidParameter; gdip_region_reset(r); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetEmpty(GpRegion *r) { if (!r) return InvalidParameter; gdip_region_reset(r); r->kind = RgnEmpty; return Ok; }

static GpRectF region_bounds(const GpRegion *r)
{
    switch (r->kind) {
    case RgnRect: return r->r;
    case RgnPath: return gdip_path_bounds(r->path);
    case RgnEmpty: return (GpRectF){ 0, 0, 0, 0 };
    default: return (GpRectF){ -4194304.f, -4194304.f, 8388608.f, 8388608.f };   /* GDI+'s "infinite" */
    }
}
static BOOL intersect_f(const GpRectF *a, const GpRectF *b, GpRectF *out)
{
    REAL x0 = a->X > b->X ? a->X : b->X, y0 = a->Y > b->Y ? a->Y : b->Y;
    REAL x1 = a->X + a->Width < b->X + b->Width ? a->X + a->Width : b->X + b->Width;
    REAL y1 = a->Y + a->Height < b->Y + b->Height ? a->Y + a->Height : b->Y + b->Height;
    if (x1 <= x0 || y1 <= y0) { *out = (GpRectF){ 0, 0, 0, 0 }; return FALSE; }
    *out = (GpRectF){ x0, y0, x1 - x0, y1 - y0 };
    return TRUE;
}

/* combining: replace, and intersect (exactly for rectangles; with a path, the
 * path is kept and the rectangles bound it); union and the rest by bounds */
static GpStatus combine_region(GpRegion *dst, const GpRegion *src, INT mode)
{
    if (mode == 0 || dst->kind == RgnInfinite && mode == 1) return gdip_region_copy(dst, src);   /* Replace */
    if (mode == 1) {                                                                            /* Intersect */
        if (dst->kind == RgnEmpty || src->kind == RgnInfinite) return Ok;
        if (src->kind == RgnEmpty) { gdip_region_reset(dst); dst->kind = RgnEmpty; return Ok; }
        if (dst->kind == RgnRect && src->kind == RgnRect) {
            if (!intersect_f(&dst->r, &src->r, &dst->r)) dst->kind = RgnEmpty;
            return Ok;
        }
        if (dst->kind == RgnRect) return gdip_region_copy(dst, src);
        return Ok;
    }
    if (mode == 2 || mode == 3) {                                                               /* Union, Xor */
        if (src->kind == RgnEmpty) return Ok;
        if (dst->kind == RgnEmpty) return gdip_region_copy(dst, src);
        if (dst->kind == RgnInfinite || src->kind == RgnInfinite) { gdip_region_reset(dst); return Ok; }
        GpRectF a = region_bounds(dst), b = region_bounds(src);
        REAL x0 = a.X < b.X ? a.X : b.X, y0 = a.Y < b.Y ? a.Y : b.Y;
        REAL x1 = a.X + a.Width > b.X + b.Width ? a.X + a.Width : b.X + b.Width;
        REAL y1 = a.Y + a.Height > b.Y + b.Height ? a.Y + a.Height : b.Y + b.Height;
        gdip_region_reset(dst);
        dst->kind = RgnRect;
        dst->r = (GpRectF){ x0, y0, x1 - x0, y1 - y0 };
        return Ok;
    }
    if (mode == 4) return Ok;                                                                   /* Exclude: kept whole */
    if (mode == 5) return gdip_region_copy(dst, src);                                           /* Complement */
    return InvalidParameter;
}
GDIPAPI GpStatus GDIPCALL GdipCombineRegionRect(GpRegion *r, const GpRectF *rect, INT mode)
{
    if (!r || !rect) return InvalidParameter;
    GpRegion t = { RgnRect, *rect, NULL };
    return combine_region(r, &t, mode);
}
GDIPAPI GpStatus GDIPCALL GdipCombineRegionRectI(GpRegion *r, const GpRect *rect, INT mode)
{
    if (!rect) return InvalidParameter;
    GpRectF f = { (REAL)rect->X, (REAL)rect->Y, (REAL)rect->Width, (REAL)rect->Height };
    return GdipCombineRegionRect(r, &f, mode);
}
GDIPAPI GpStatus GDIPCALL GdipCombineRegionPath(GpRegion *r, GpPath *p, INT mode)
{
    if (!r || !p) return InvalidParameter;
    GpRegion t = { RgnPath, { 0 }, p };
    return combine_region(r, &t, mode);
}
GDIPAPI GpStatus GDIPCALL GdipCombineRegionRegion(GpRegion *r, GpRegion *src, INT mode)
{
    if (!r || !src) return InvalidParameter;
    return combine_region(r, src, mode);
}
GDIPAPI GpStatus GDIPCALL GdipTranslateRegion(GpRegion *r, REAL dx, REAL dy)
{
    if (!r) return InvalidParameter;
    if (r->kind == RgnRect) { r->r.X += dx; r->r.Y += dy; }
    if (r->kind == RgnPath) for (int i = 0; i < r->path->n; i++) { r->path->pts[i].X += dx; r->path->pts[i].Y += dy; }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipTranslateRegionI(GpRegion *r, INT dx, INT dy) { return GdipTranslateRegion(r, (REAL)dx, (REAL)dy); }
GDIPAPI GpStatus GDIPCALL GdipGetRegionBounds(GpRegion *r, GpGraphics *g, GpRectF *out)
{
    if (!r || !g || !out) return InvalidParameter;
    *out = region_bounds(r);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetRegionBoundsI(GpRegion *r, GpGraphics *g, GpRect *out)
{
    if (!out) return InvalidParameter;
    GpRectF f;
    GpStatus st = GdipGetRegionBounds(r, g, &f);
    if (st == Ok) *out = (GpRect){ (INT)floorf(f.X), (INT)floorf(f.Y), (INT)ceilf(f.X + f.Width) - (INT)floorf(f.X),
                                   (INT)ceilf(f.Y + f.Height) - (INT)floorf(f.Y) };
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipIsEmptyRegion(GpRegion *r, GpGraphics *g, BOOL *out)
{
    if (!r || !g || !out) return InvalidParameter;
    GpRectF b = region_bounds(r);
    *out = r->kind == RgnEmpty || (r->kind != RgnInfinite && (b.Width <= 0 || b.Height <= 0));
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipIsInfiniteRegion(GpRegion *r, GpGraphics *g, BOOL *out)
{ if (!r || !g || !out) return InvalidParameter; *out = r->kind == RgnInfinite; return Ok; }
/* an HRGN: the infinite region is NULL, as in GDI+; a path gives its bounds */
GDIPAPI GpStatus GDIPCALL GdipGetRegionHRgn(GpRegion *r, GpGraphics *g, HRGN *out)
{
    if (!r || !out) return InvalidParameter;
    (void)g;
    if (r->kind == RgnInfinite) { *out = NULL; return Ok; }
    GpRectF b = region_bounds(r);
    *out = CreateRectRgn((int)floorf(b.X), (int)floorf(b.Y), (int)ceilf(b.X + b.Width), (int)ceilf(b.Y + b.Height));
    return *out ? Ok : OutOfMemory;
}
GDIPAPI GpStatus GDIPCALL GdipIsVisibleRegionPoint(GpRegion *r, REAL x, REAL y, GpGraphics *g, BOOL *out)
{
    (void)g;
    if (!r || !out) return InvalidParameter;
    GpRectF b = region_bounds(r);
    *out = r->kind == RgnInfinite || (r->kind != RgnEmpty && x >= b.X && y >= b.Y && x < b.X + b.Width && y < b.Y + b.Height);
    return Ok;
}

/* ---- clipping (kept in device coordinates, as GDI+ does) ---------------- */

static void region_to_device(GpGraphics *g, GpRegion *r)
{
    plutovg_matrix_t m;
    world_to_device(g, &m);
    if (r->kind == RgnRect) {
        plutovg_rect_t a = { r->r.X, r->r.Y, r->r.Width, r->r.Height }, d;
        plutovg_matrix_map_rect(&m, &a, &d);
        r->r = (GpRectF){ d.x, d.y, d.w, d.h };
    } else if (r->kind == RgnPath) {
        for (int i = 0; i < r->path->n; i++) plutovg_matrix_map(&m, r->path->pts[i].X, r->path->pts[i].Y, &r->path->pts[i].X, &r->path->pts[i].Y);
    }
}
static void region_from_device(GpGraphics *g, GpRegion *r)
{
    plutovg_matrix_t m, inv;
    world_to_device(g, &m);
    if (!plutovg_matrix_invert(&m, &inv)) return;
    if (r->kind == RgnRect) {
        plutovg_rect_t a = { r->r.X, r->r.Y, r->r.Width, r->r.Height }, d;
        plutovg_matrix_map_rect(&inv, &a, &d);
        r->r = (GpRectF){ d.x, d.y, d.w, d.h };
    } else if (r->kind == RgnPath) {
        for (int i = 0; i < r->path->n; i++) plutovg_matrix_map(&inv, r->path->pts[i].X, r->path->pts[i].Y, &r->path->pts[i].X, &r->path->pts[i].Y);
    }
}
static GpStatus set_clip(GpGraphics *g, const GpRegion *src, INT mode)
{
    GpRegion t = { 0 };
    GpStatus st = gdip_region_copy(&t, src);
    if (st != Ok) return st;
    region_to_device(g, &t);
    st = combine_region(&g->clip, &t, mode);
    gdip_region_reset(&t);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipSetClipRect(GpGraphics *g, REAL x, REAL y, REAL w, REAL h, INT mode)
{
    if (!g) return InvalidParameter;
    GpRegion r = { RgnRect, { x, y, w, h }, NULL };
    return set_clip(g, &r, mode);
}
GDIPAPI GpStatus GDIPCALL GdipSetClipRectI(GpGraphics *g, INT x, INT y, INT w, INT h, INT mode)
{ return GdipSetClipRect(g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, mode); }
GDIPAPI GpStatus GDIPCALL GdipSetClipRegion(GpGraphics *g, GpRegion *r, INT mode) { if (!g || !r) return InvalidParameter; return set_clip(g, r, mode); }
GDIPAPI GpStatus GDIPCALL GdipSetClipPath(GpGraphics *g, GpPath *p, INT mode)
{
    if (!g || !p) return InvalidParameter;
    GpRegion r = { RgnPath, { 0 }, p };
    return set_clip(g, &r, mode);
}
GDIPAPI GpStatus GDIPCALL GdipSetClipHrgn(GpGraphics *g, HRGN h, INT mode)
{
    RECT b;
    if (!g || !h || GetRgnBox(h, &b) == ERROR) return InvalidParameter;
    GpRegion r = { RgnRect, { (REAL)b.left, (REAL)b.top, (REAL)(b.right - b.left), (REAL)(b.bottom - b.top) }, NULL };
    GpStatus st = combine_region(&g->clip, &r, mode);       /* an HRGN is in device units already */
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipSetClipGraphics(GpGraphics *g, GpGraphics *src, INT mode)
{ if (!g || !src) return InvalidParameter; return combine_region(&g->clip, &src->clip, mode); }
GDIPAPI GpStatus GDIPCALL GdipResetClip(GpGraphics *g) { if (!g) return InvalidParameter; gdip_region_reset(&g->clip); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipTranslateClip(GpGraphics *g, REAL dx, REAL dy) { if (!g) return InvalidParameter; return GdipTranslateRegion(&g->clip, dx, dy); }
GDIPAPI GpStatus GDIPCALL GdipTranslateClipI(GpGraphics *g, INT dx, INT dy) { return GdipTranslateClip(g, (REAL)dx, (REAL)dy); }
GDIPAPI GpStatus GDIPCALL GdipGetClip(GpGraphics *g, GpRegion *out)
{
    if (!g || !out) return InvalidParameter;
    GpStatus st = gdip_region_copy(out, &g->clip);
    if (st == Ok) region_from_device(g, out);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipGetClipBounds(GpGraphics *g, GpRectF *out)
{
    if (!g || !out) return InvalidParameter;
    GpRegion r = { 0 };
    GpStatus st = GdipGetClip(g, &r);
    if (st == Ok) *out = region_bounds(&r);
    gdip_region_reset(&r);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipGetClipBoundsI(GpGraphics *g, GpRect *out)
{
    if (!out) return InvalidParameter;
    GpRectF f;
    GpStatus st = GdipGetClipBounds(g, &f);
    if (st == Ok) *out = (GpRect){ (INT)f.X, (INT)f.Y, (INT)f.Width, (INT)f.Height };
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipIsClipEmpty(GpGraphics *g, BOOL *out) { if (!g || !out) return InvalidParameter; *out = g->clip.kind == RgnEmpty; return Ok; }

/* what a graphics object can draw on, in device coordinates */
GpRectF gdip_region_bounds(const GpRegion *r) { return region_bounds(r); }

static GpRectF device_bounds(GpGraphics *g)
{
    if (g->img) return (GpRectF){ 0, 0, (REAL)plutovg_surface_get_width(g->img->s), (REAL)plutovg_surface_get_height(g->img->s) };
    RECT r;
    if (GetClipBox(g->hdc, &r) == ERROR) return (GpRectF){ 0, 0, 0, 0 };
    return (GpRectF){ (REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top) };
}
GDIPAPI GpStatus GDIPCALL GdipGetVisibleClipBounds(GpGraphics *g, GpRectF *out)
{
    if (!g || !out) return InvalidParameter;
    GpRectF d = device_bounds(g), c = g->clip.kind == RgnInfinite ? d : region_bounds(&g->clip);
    GpRegion r = { RgnRect, { 0 }, NULL };
    intersect_f(&d, &c, &r.r);
    region_from_device(g, &r);
    *out = r.r;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetVisibleClipBoundsI(GpGraphics *g, GpRect *out)
{
    if (!out) return InvalidParameter;
    GpRectF f;
    GpStatus st = GdipGetVisibleClipBounds(g, &f);
    if (st == Ok) *out = (GpRect){ (INT)f.X, (INT)f.Y, (INT)f.Width, (INT)f.Height };
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipIsVisibleRect(GpGraphics *g, REAL x, REAL y, REAL w, REAL h, BOOL *out)
{
    if (!g || !out) return InvalidParameter;
    GpRectF v, r = { x, y, w, h }, t;
    GdipGetVisibleClipBounds(g, &v);
    *out = intersect_f(&v, &r, &t);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipIsVisibleRectI(GpGraphics *g, INT x, INT y, INT w, INT h, BOOL *out)
{ return GdipIsVisibleRect(g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, out); }
GDIPAPI GpStatus GDIPCALL GdipIsVisiblePoint(GpGraphics *g, REAL x, REAL y, BOOL *out)
{
    if (!g || !out) return InvalidParameter;
    GpRectF v;
    GdipGetVisibleClipBounds(g, &v);
    *out = x >= v.X && y >= v.Y && x < v.X + v.Width && y < v.Y + v.Height;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipIsVisiblePointI(GpGraphics *g, INT x, INT y, BOOL *out) { return GdipIsVisiblePoint(g, (REAL)x, (REAL)y, out); }

/* saving and restoring the graphics state: a small stack */
#define MAX_SAVED 32
typedef struct { GpGraphics *g; UINT id; GpGraphics state; } Saved;
static Saved g_saved[MAX_SAVED];
static int g_nsaved;
static UINT g_next_state = 1;
GDIPAPI GpStatus GDIPCALL GdipSaveGraphics(GpGraphics *g, UINT *state)
{
    if (!g || !state) return InvalidParameter;
    if (g_nsaved == MAX_SAVED) return OutOfMemory;
    Saved *s = &g_saved[g_nsaved];
    s->g = g;
    s->id = g_next_state++;
    s->state = *g;
    s->state.clip.path = NULL;
    if (gdip_region_copy(&s->state.clip, &g->clip) != Ok) return OutOfMemory;
    g_nsaved++;
    *state = s->id;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipRestoreGraphics(GpGraphics *g, UINT state)
{
    if (!g) return InvalidParameter;
    for (int i = g_nsaved - 1; i >= 0; i--) {
        if (g_saved[i].g != g || g_saved[i].id != state) continue;
        GpGraphics *s = &g_saved[i].state;
        gdip_region_reset(&g->clip);
        g->clip = s->clip;
        g->smoothing = s->smoothing; g->pixel_offset = s->pixel_offset; g->interpolation = s->interpolation;
        g->compositing = s->compositing; g->compositing_quality = s->compositing_quality; g->text_hint = s->text_hint;
        g->page_unit = s->page_unit; g->page_scale = s->page_scale; g->world = s->world;
        for (int k = i + 1; k < g_nsaved; k++) gdip_region_reset(&g_saved[k].state.clip);   /* the later saves go too */
        g_nsaved = i;
        return Ok;
    }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipBeginContainer2(GpGraphics *g, UINT *state) { return GdipSaveGraphics(g, state); }
GDIPAPI GpStatus GDIPCALL GdipEndContainer(GpGraphics *g, UINT state) { return GdipRestoreGraphics(g, state); }
GDIPAPI GpStatus GDIPCALL GdipFlush(GpGraphics *g, INT intent) { (void)intent; return g ? Ok : InvalidParameter; }

/* A drawing operation: a canvas set up for the target with the world
 * transform and clip applied.  On a DC, the part of the DC the operation
 * can touch (`area`, world coordinates, NULL: all of it) is read into a DIB
 * first and copied back afterwards. */

BOOL gdip_op_begin(GdipOp *op, GpGraphics *g, const GpRectF *area, BOOL stroke)
{
    memset(op, 0, sizeof *op);
    op->g = g;
    if (g->img) {
        op->c = g->c;
        plutovg_canvas_save(op->c);
    } else {
        RECT clip;
        if (GetClipBox(g->hdc, &clip) == ERROR || clip.right <= clip.left || clip.bottom <= clip.top) return FALSE;
        RECT box = clip;
        if (area) {
            /* the area in device space (world transform applied), widened for anti-aliasing */
            plutovg_rect_t r = { area->X, area->Y, area->Width, area->Height }, d;
            plutovg_matrix_t m;
            world_to_device(g, &m);
            plutovg_matrix_map_rect(&m, &r, &d);
            RECT a = { (LONG)floorf(d.x) - 2, (LONG)floorf(d.y) - 2, (LONG)ceilf(d.x + d.w) + 3, (LONG)ceilf(d.y + d.h) + 3 };
            if (!IntersectRect(&box, &clip, &a)) return FALSE;
        }
        int w = box.right - box.left, h = box.bottom - box.top;
        DWORD *bits;
        op->bmp = gdip_dib_section(w, h, &bits);
        if (!op->bmp) return FALSE;
        op->mem = CreateCompatibleDC(g->hdc);
        op->old = SelectObject(op->mem, op->bmp);
        BitBlt(op->mem, 0, 0, w, h, g->hdc, box.left, box.top, SRCCOPY);
        for (int k = 0; k < w * h; k++) bits[k] |= 0xFF000000u;   /* the DC's pixels are opaque */
        op->s = plutovg_surface_create_for_data((unsigned char *)bits, w, h, w * 4);
        op->c = op->s ? plutovg_canvas_create(op->s) : NULL;
        if (!op->c) {
            if (op->s) plutovg_surface_destroy(op->s);
            SelectObject(op->mem, op->old);
            DeleteDC(op->mem);
            DeleteObject(op->bmp);
            return FALSE;
        }
        op->box = box;
        plutovg_canvas_translate(op->c, (float)-box.left, (float)-box.top);
    }
    /* the clip is in device coordinates: before the transforms */
    if (g->clip.kind == RgnEmpty) { op->g = g; gdip_op_end(op); return FALSE; }
    if (g->clip.kind == RgnRect) plutovg_canvas_clip_rect(op->c, g->clip.r.X, g->clip.r.Y, g->clip.r.Width, g->clip.r.Height);
    if (g->clip.kind == RgnPath) {
        plutovg_path_t *cp = gdip_path_build(g->clip.path);
        if (cp) {
            plutovg_canvas_set_fill_rule(op->c, g->clip.path->fill_mode == FillModeWinding ? PLUTOVG_FILL_RULE_NON_ZERO : PLUTOVG_FILL_RULE_EVEN_ODD);
            plutovg_canvas_clip_path(op->c, cp);
            plutovg_path_destroy(cp);
        }
    }
    plutovg_matrix_t m;
    world_to_device(g, &m);
    plutovg_canvas_transform(op->c, &m);
    /* GDI+ puts pixel centres on whole coordinates; plutovg on halves.  With
     * anti-aliasing (or a half-pixel offset) shapes land as GDI+ draws them; without,
     * fills keep their edges on pixel edges and one-pixel lines on one row */
    BOOL aa = g->smoothing == SmoothingModeAntiAlias || g->smoothing == SmoothingModeHighQuality ||
              g->smoothing == 5 /* AntiAlias8x8 */;
    BOOL half = g->pixel_offset == PixelOffsetModeHalf || g->pixel_offset == PixelOffsetModeHighQuality;
    if ((aa && !half) || (!aa && stroke)) plutovg_canvas_translate(op->c, 0.5f, 0.5f);
    if (g->compositing == CompositingModeSourceCopy) plutovg_canvas_set_operator(op->c, PLUTOVG_OPERATOR_SRC);
    return TRUE;
}

void gdip_op_end(GdipOp *op)
{
    if (op->g->img) { plutovg_canvas_restore(op->c); return; }
    plutovg_canvas_destroy(op->c);
    plutovg_surface_destroy(op->s);
    BitBlt(op->g->hdc, op->box.left, op->box.top, op->box.right - op->box.left, op->box.bottom - op->box.top, op->mem, 0, 0, SRCCOPY);
    SelectObject(op->mem, op->old);
    DeleteDC(op->mem);
    DeleteObject(op->bmp);
}

static GpRectF rect_of_points(const GpPointF *p, int n, REAL grow)
{
    REAL x0 = p[0].X, y0 = p[0].Y, x1 = x0, y1 = y0;
    for (int i = 1; i < n; i++) {
        if (p[i].X < x0) x0 = p[i].X;
        if (p[i].X > x1) x1 = p[i].X;
        if (p[i].Y < y0) y0 = p[i].Y;
        if (p[i].Y > y1) y1 = p[i].Y;
    }
    return (GpRectF){ x0 - grow, y0 - grow, x1 - x0 + 2 * grow, y1 - y0 + 2 * grow };
}

GDIPAPI GpStatus GDIPCALL GdipGraphicsClear(GpGraphics *g, ARGB color)
{
    if (!g) return InvalidParameter;
    GdipOp op;
    if (!gdip_op_begin(&op, g, NULL, FALSE)) return Ok;
    plutovg_canvas_set_operator(op.c, PLUTOVG_OPERATOR_SRC);
    set_argb(op.c, color);
    plutovg_canvas_paint(op.c);
    gdip_op_end(&op);
    return Ok;
}

/* ---- brushes and pens ------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipCreateSolidFill(ARGB color, GpBrush **out)
{
    if (!out) return InvalidParameter;
    GpBrush *b = xalloc(sizeof *b);
    if (!b) return OutOfMemory;
    b->type = BrushTypeSolidColor;
    b->color = color;
    plutovg_matrix_init_identity(&b->xform);
    *out = b;
    return Ok;
}
GpStatus gdip_brush_free(GpBrush *b)
{
    xfree(b->pts);
    xfree(b->surround);
    if (b->img) GdipDisposeImage(b->img);
    b->pts = NULL; b->surround = NULL; b->img = NULL;
    b->npts = b->nsurround = 0;
    return Ok;
}
GpStatus gdip_brush_copy(GpBrush *dst, const GpBrush *src)
{
    *dst = *src;
    dst->pts = NULL; dst->surround = NULL; dst->img = NULL;
    if (src->npts) {
        dst->pts = xalloc(src->npts * sizeof *dst->pts);
        if (!dst->pts) return OutOfMemory;
        memcpy(dst->pts, src->pts, src->npts * sizeof *dst->pts);
    }
    if (src->nsurround) {
        dst->surround = xalloc(src->nsurround * sizeof *dst->surround);
        if (!dst->surround) { gdip_brush_free(dst); return OutOfMemory; }
        memcpy(dst->surround, src->surround, src->nsurround * sizeof *dst->surround);
    }
    if (src->img && GdipCloneImage(src->img, &dst->img) != Ok) { gdip_brush_free(dst); return OutOfMemory; }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCloneBrush(GpBrush *b, GpBrush **out)
{
    if (!b || !out) return InvalidParameter;
    GpBrush *c = xalloc(sizeof *c);
    if (!c) return OutOfMemory;
    GpStatus st = gdip_brush_copy(c, b);
    if (st != Ok) { xfree(c); return st; }
    *out = c;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDeleteBrush(GpBrush *b) { if (!b) return InvalidParameter; gdip_brush_free(b); xfree(b); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetSolidFillColor(GpBrush *b, ARGB *c) { if (!b || !c) return InvalidParameter; *c = b->color; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetSolidFillColor(GpBrush *b, ARGB c) { if (!b) return InvalidParameter; b->color = c; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetBrushType(GpBrush *b, INT *t) { if (!b || !t) return InvalidParameter; *t = b->type; return Ok; }

static plutovg_color_t argb_color(ARGB a)
{
    plutovg_color_t c = { ((a >> 16) & 255) / 255.f, ((a >> 8) & 255) / 255.f, (a & 255) / 255.f, (a >> 24) / 255.f };
    return c;
}
/* a colour between two at t, 0..1 */
static ARGB mix(ARGB a, ARGB b, REAL t)
{
    ARGB r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 255, cb = (b >> sh) & 255;
        r |= (ARGB)(int)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return r;
}
/* the stops of a gradient from colour `c1` to `c2`, with the brush's blend
 * factors or preset colours applied */
static int gradient_stops(const GpBrush *b, ARGB c1, ARGB c2, plutovg_gradient_stop_t *st)
{
    int n = 0;
    if (b->npreset >= 2) {
        for (int i = 0; i < b->npreset; i++) { st[n].offset = b->preset_p[i]; st[n].color = argb_color(b->preset_c[i]); n++; }
    } else if (b->nblend >= 2) {
        for (int i = 0; i < b->nblend; i++) { st[n].offset = b->blend_p[i]; st[n].color = argb_color(mix(c1, c2, b->blend_f[i])); n++; }
    } else {
        st[0].offset = 0; st[0].color = argb_color(c1);
        st[1].offset = 1; st[1].color = argb_color(c2);
        n = 2;
    }
    return n;
}
static plutovg_spread_method_t spread_of(int wrap)
{
    switch (wrap) {
    case 0: return PLUTOVG_SPREAD_METHOD_REPEAT;       /* WrapModeTile */
    case 4: return PLUTOVG_SPREAD_METHOD_PAD;          /* WrapModeClamp */
    default: return PLUTOVG_SPREAD_METHOD_REFLECT;     /* the flips */
    }
}
void gdip_use_brush(plutovg_canvas_t *c, const GpBrush *b)
{
    plutovg_gradient_stop_t st[MAX_BLEND];
    switch (b->type) {
    case BrushTypeLinearGradient: {
        /* the gradient runs along `angle` across the rectangle: its length
         * is the rectangle's projection onto that direction */
        REAL a = b->angle * 3.14159265f / 180.f, ca = cosf(a), sa = sinf(a);
        REAL w = b->rect.Width, h = b->rect.Height;
        REAL len = fabsf(w * ca) + fabsf(h * sa);
        REAL cx = b->rect.X + w / 2, cy = b->rect.Y + h / 2;
        if (b->angle_scalable && w > 0 && h > 0) { REAL s = sqrtf(ca * ca * w * w + sa * sa * h * h); if (s > 0) { ca = ca * w / s; sa = sa * h / s; } }
        REAL x1 = cx - ca * len / 2, y1 = cy - sa * len / 2, x2 = cx + ca * len / 2, y2 = cy + sa * len / 2;
        int n = gradient_stops(b, b->color, b->color2, st);
        plutovg_canvas_set_linear_gradient(c, x1, y1, x2, y2, spread_of(b->wrap), st, n, &b->xform);
        return;
    }
    case BrushTypePathGradient: {
        /* a radial gradient from the centre colour out to the first surround colour */
        GpRectF r = b->rect;
        REAL rad = sqrtf(r.Width * r.Width + r.Height * r.Height) / 2;
        if (rad <= 0) rad = 1;
        ARGB edge = b->nsurround ? b->surround[0] : b->color2;
        plutovg_gradient_stop_t rs[MAX_BLEND];
        int n;
        if (b->npreset >= 2) {
            /* preset blend: position 0 is the boundary, 1 the centre */
            for (n = 0; n < b->npreset; n++) { rs[n].offset = 1 - b->preset_p[b->npreset - 1 - n]; rs[n].color = argb_color(b->preset_c[b->npreset - 1 - n]); }
        } else if (b->nblend >= 2) {
            for (n = 0; n < b->nblend; n++) { rs[n].offset = 1 - b->blend_p[b->nblend - 1 - n]; rs[n].color = argb_color(mix(edge, b->color, b->blend_f[b->nblend - 1 - n])); }
        } else {
            rs[0].offset = 0; rs[0].color = argb_color(b->color);
            rs[1].offset = 1; rs[1].color = argb_color(edge);
            n = 2;
        }
        REAL fr = rad * (b->focus.X > b->focus.Y ? b->focus.X : b->focus.Y);
        plutovg_canvas_set_radial_gradient(c, b->centre.X, b->centre.Y, rad, b->centre.X, b->centre.Y, fr < 0 ? 0 : fr,
                                           PLUTOVG_SPREAD_METHOD_PAD, rs, n, &b->xform);
        return;
    }
    case BrushTypeTextureFill:
        if (b->img && b->img->s) {
            plutovg_canvas_set_texture(c, b->img->s, b->wrap == 4 ? PLUTOVG_TEXTURE_TYPE_PLAIN : PLUTOVG_TEXTURE_TYPE_TILED, 1.f, &b->xform);
            return;
        }
        break;
    case BrushTypeHatchFill: {
        /* hatches: the two colours mixed by the style's density */
        static const BYTE density[] = { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50,
                                        5, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90 };
        int d = b->hatch >= 0 && b->hatch < (int)sizeof density ? density[b->hatch] : 50;
        set_argb(c, mix(b->color2, b->color, d / 100.f));
        return;
    }
    }
    set_argb(c, b->color);
}

static void pen_init(GpPen *p, ARGB color, REAL width, INT unit)
{
    p->color = color;
    p->width = width;
    p->unit = unit;
    p->miter = 10.f;
    p->mode = 0;
    plutovg_matrix_init_identity(&p->xform);
}
GDIPAPI GpStatus GDIPCALL GdipCreatePen1(ARGB color, REAL width, INT unit, GpPen **out)
{
    if (!out) return InvalidParameter;
    GpPen *p = xalloc(sizeof *p);
    if (!p) return OutOfMemory;
    pen_init(p, color, width, unit);
    *out = p;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreatePen2(GpBrush *b, REAL width, INT unit, GpPen **out)
{
    if (!b || !out) return InvalidParameter;
    GpPen *p = xalloc(sizeof *p);
    if (!p) return OutOfMemory;
    pen_init(p, b->color, width, unit);
    if (b->type != BrushTypeSolidColor && GdipCloneBrush(b, &p->brush) != Ok) { xfree(p); return OutOfMemory; }
    *out = p;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipClonePen(GpPen *p, GpPen **out)
{
    if (!p || !out) return InvalidParameter;
    GpPen *c = xalloc(sizeof *c);
    if (!c) return OutOfMemory;
    *c = *p;
    c->dashes = NULL; c->compound = NULL; c->brush = NULL;
    if (p->ndashes) {
        c->dashes = xalloc(p->ndashes * sizeof *c->dashes);
        if (!c->dashes) { xfree(c); return OutOfMemory; }
        memcpy(c->dashes, p->dashes, p->ndashes * sizeof *c->dashes);
    }
    if (p->ncompound) {
        c->compound = xalloc(p->ncompound * sizeof *c->compound);
        if (!c->compound) { xfree(c->dashes); xfree(c); return OutOfMemory; }
        memcpy(c->compound, p->compound, p->ncompound * sizeof *c->compound);
    }
    if (p->brush && GdipCloneBrush(p->brush, &c->brush) != Ok) { xfree(c->dashes); xfree(c->compound); xfree(c); return OutOfMemory; }
    *out = c;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDeletePen(GpPen *p)
{
    if (!p) return InvalidParameter;
    xfree(p->dashes);
    xfree(p->compound);
    if (p->brush) GdipDeleteBrush(p->brush);
    xfree(p);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipSetPenColor(GpPen *p, ARGB c)
{
    if (!p) return InvalidParameter;
    p->color = c;
    if (p->brush) { GdipDeleteBrush(p->brush); p->brush = NULL; }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPenColor(GpPen *p, ARGB *c) { if (!p || !c) return InvalidParameter; *c = p->color; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenWidth(GpPen *p, REAL w) { if (!p) return InvalidParameter; p->width = w; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenWidth(GpPen *p, REAL *w) { if (!p || !w) return InvalidParameter; *w = p->width; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenLineJoin(GpPen *p, INT j) { if (!p) return InvalidParameter; p->line_join = j; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenStartCap(GpPen *p, INT c) { if (!p) return InvalidParameter; p->start_cap = c; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenEndCap(GpPen *p, INT c) { if (!p) return InvalidParameter; p->end_cap = c; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenLineCap197819(GpPen *p, INT s, INT e, INT dash)
{ if (!p) return InvalidParameter; p->start_cap = s; p->end_cap = e; p->dash_cap = dash; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenDashStyle(GpPen *p, INT d)
{
    if (!p) return InvalidParameter;
    p->dash = d;
    if (d != 5) { xfree(p->dashes); p->dashes = NULL; p->ndashes = 0; }   /* not DashStyleCustom: the pattern goes */
    return Ok;
}

void gdip_use_pen(plutovg_canvas_t *c, const GpPen *p)
{
    if (p->brush) gdip_use_brush(c, p->brush);
    else set_argb(c, p->color);
    float w = p->width > 0 ? p->width : 1.f;                            /* width 0: a one-pixel line */
    plutovg_canvas_set_line_width(c, w);
    plutovg_canvas_set_line_join(c, p->line_join == 1 ? PLUTOVG_LINE_JOIN_BEVEL :
                                    p->line_join == 2 ? PLUTOVG_LINE_JOIN_ROUND : PLUTOVG_LINE_JOIN_MITER);
    plutovg_canvas_set_miter_limit(c, p->miter > 1 ? p->miter : 1.f);
    int cap = p->start_cap;
    if (p->dash && p->dash_cap) cap = p->dash_cap;
    plutovg_canvas_set_line_cap(c, cap == 1 ? PLUTOVG_LINE_CAP_SQUARE :
                                   cap == 2 ? PLUTOVG_LINE_CAP_ROUND : PLUTOVG_LINE_CAP_BUTT);
    static const float dot[] = { 1, 1 }, dash[] = { 3, 1 }, dashdot[] = { 3, 1, 1, 1 }, dashdotdot[] = { 3, 1, 1, 1, 1, 1 };
    float d[64];
    const float *pat = NULL;
    int n = 0;
    switch (p->dash) {
    case 1: pat = dash; n = 2; break;
    case 2: pat = dot; n = 2; break;
    case 3: pat = dashdot; n = 4; break;
    case 4: pat = dashdotdot; n = 6; break;
    case 5: pat = p->dashes; n = p->ndashes > 64 ? 64 : p->ndashes; break;     /* DashStyleCustom: in pen widths */
    }
    for (int i = 0; i < n; i++) d[i] = pat[i] * w;
    plutovg_canvas_set_dash_array(c, n ? d : NULL, n);
    plutovg_canvas_set_dash_offset(c, n ? p->dash_offset * w : 0);
}

/* ---- shapes ----------------------------------------------------------- */

static GpStatus stroke_points(GpGraphics *g, GpPen *pen, const GpPointF *pts, int n, BOOL bezier, BOOL close)
{
    if (!g || !pen || !pts || n < 2 || (bezier && (n - 1) % 3)) return InvalidParameter;
    GpRectF a = rect_of_points(pts, n, pen->width + 2);
    GdipOp op;
    if (!gdip_op_begin(&op, g, &a, TRUE)) return Ok;
    gdip_use_pen(op.c, pen);
    plutovg_path_t *path = plutovg_path_create();
    plutovg_path_move_to(path, pts[0].X, pts[0].Y);
    if (bezier)
        for (int i = 1; i + 2 < n; i += 3)
            plutovg_path_cubic_to(path, pts[i].X, pts[i].Y, pts[i + 1].X, pts[i + 1].Y, pts[i + 2].X, pts[i + 2].Y);
    else
        for (int i = 1; i < n; i++) plutovg_path_line_to(path, pts[i].X, pts[i].Y);
    if (close) plutovg_path_close(path);
    plutovg_canvas_stroke_path(op.c, path);
    plutovg_path_destroy(path);
    gdip_op_end(&op);
    return Ok;
}

static GpPointF *points_f(const GpPoint *p, int n)
{
    GpPointF *f = xalloc(sizeof *f * (n > 0 ? n : 1));
    if (f) for (int i = 0; i < n; i++) f[i] = (GpPointF){ (REAL)p[i].X, (REAL)p[i].Y };
    return f;
}

GDIPAPI GpStatus GDIPCALL GdipDrawLine(GpGraphics *g, GpPen *pen, REAL x1, REAL y1, REAL x2, REAL y2)
{
    GpPointF p[2] = { { x1, y1 }, { x2, y2 } };
    return stroke_points(g, pen, p, 2, FALSE, FALSE);
}
GDIPAPI GpStatus GDIPCALL GdipDrawLineI(GpGraphics *g, GpPen *pen, INT x1, INT y1, INT x2, INT y2)
{ return GdipDrawLine(g, pen, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2); }
GDIPAPI GpStatus GDIPCALL GdipDrawLines(GpGraphics *g, GpPen *pen, const GpPointF *p, INT n) { return stroke_points(g, pen, p, n, FALSE, FALSE); }
GDIPAPI GpStatus GDIPCALL GdipDrawLinesI(GpGraphics *g, GpPen *pen, const GpPoint *p, INT n)
{
    if (!p || n < 2) return InvalidParameter;
    GpPointF *f = points_f(p, n);
    if (!f) return OutOfMemory;
    GpStatus st = stroke_points(g, pen, f, n, FALSE, FALSE);
    xfree(f);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipDrawPolygonI(GpGraphics *g, GpPen *pen, const GpPoint *p, INT n)
{
    if (!p || n < 2) return InvalidParameter;
    GpPointF *f = points_f(p, n);
    if (!f) return OutOfMemory;
    GpStatus st = stroke_points(g, pen, f, n, FALSE, TRUE);
    xfree(f);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipDrawBezier(GpGraphics *g, GpPen *pen, REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4)
{
    GpPointF p[4] = { { x1, y1 }, { x2, y2 }, { x3, y3 }, { x4, y4 } };
    return stroke_points(g, pen, p, 4, TRUE, FALSE);
}
GDIPAPI GpStatus GDIPCALL GdipDrawBezierI(GpGraphics *g, GpPen *pen, INT x1, INT y1, INT x2, INT y2, INT x3, INT y3, INT x4, INT y4)
{ return GdipDrawBezier(g, pen, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2, (REAL)x3, (REAL)y3, (REAL)x4, (REAL)y4); }
GDIPAPI GpStatus GDIPCALL GdipDrawBeziersI(GpGraphics *g, GpPen *pen, const GpPoint *p, INT n)
{
    if (!p || n < 4) return InvalidParameter;
    GpPointF *f = points_f(p, n);
    if (!f) return OutOfMemory;
    GpStatus st = stroke_points(g, pen, f, n, TRUE, FALSE);
    xfree(f);
    return st;
}

GDIPAPI GpStatus GDIPCALL GdipDrawRectangle(GpGraphics *g, GpPen *pen, REAL x, REAL y, REAL w, REAL h)
{
    GpPointF p[4] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
    return stroke_points(g, pen, p, 4, FALSE, TRUE);
}
GDIPAPI GpStatus GDIPCALL GdipDrawRectangleI(GpGraphics *g, GpPen *pen, INT x, INT y, INT w, INT h)
{ return GdipDrawRectangle(g, pen, (REAL)x, (REAL)y, (REAL)w, (REAL)h); }

static GpStatus fill_shape(GpGraphics *g, GpBrush *b, GpPath *path)
{
    GpStatus st = GdipFillPath(g, b, path);
    GdipDeletePath(path);
    return st;
}
static GpStatus stroke_shape(GpGraphics *g, GpPen *pen, GpPath *path)
{
    GpStatus st = GdipDrawPath(g, pen, path);
    GdipDeletePath(path);
    return st;
}

GDIPAPI GpStatus GDIPCALL GdipFillRectangle(GpGraphics *g, GpBrush *b, REAL x, REAL y, REAL w, REAL h)
{
    if (!g || !b) return InvalidParameter;
    GpRectF a = { x, y, w, h };
    GdipOp op;
    if (!gdip_op_begin(&op, g, &a, FALSE)) return Ok;
    gdip_use_brush(op.c, b);
    plutovg_canvas_fill_rect(op.c, x, y, w, h);
    gdip_op_end(&op);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipFillRectangleI(GpGraphics *g, GpBrush *b, INT x, INT y, INT w, INT h)
{ return GdipFillRectangle(g, b, (REAL)x, (REAL)y, (REAL)w, (REAL)h); }
GDIPAPI GpStatus GDIPCALL GdipFillRectanglesI(GpGraphics *g, GpBrush *b, const GpRect *r, INT n)
{
    if (!r || n < 0) return InvalidParameter;
    for (int i = 0; i < n; i++) {
        GpStatus st = GdipFillRectangleI(g, b, r[i].X, r[i].Y, r[i].Width, r[i].Height);
        if (st != Ok) return st;
    }
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipFillEllipse(GpGraphics *g, GpBrush *b, REAL x, REAL y, REAL w, REAL h)
{
    GpPath *p;
    if (!g || !b) return InvalidParameter;
    if (GdipCreatePath(0, &p) != Ok) return OutOfMemory;
    GdipAddPathEllipse(p, x, y, w, h);
    return fill_shape(g, b, p);
}
GDIPAPI GpStatus GDIPCALL GdipFillEllipseI(GpGraphics *g, GpBrush *b, INT x, INT y, INT w, INT h)
{ return GdipFillEllipse(g, b, (REAL)x, (REAL)y, (REAL)w, (REAL)h); }
GDIPAPI GpStatus GDIPCALL GdipDrawEllipse(GpGraphics *g, GpPen *pen, REAL x, REAL y, REAL w, REAL h)
{
    GpPath *p;
    if (!g || !pen) return InvalidParameter;
    if (GdipCreatePath(0, &p) != Ok) return OutOfMemory;
    GdipAddPathEllipse(p, x, y, w, h);
    return stroke_shape(g, pen, p);
}
GDIPAPI GpStatus GDIPCALL GdipDrawEllipseI(GpGraphics *g, GpPen *pen, INT x, INT y, INT w, INT h)
{ return GdipDrawEllipse(g, pen, (REAL)x, (REAL)y, (REAL)w, (REAL)h); }
GDIPAPI GpStatus GDIPCALL GdipDrawArc(GpGraphics *g, GpPen *pen, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep)
{
    GpPath *p;
    if (!g || !pen) return InvalidParameter;
    if (GdipCreatePath(0, &p) != Ok) return OutOfMemory;
    GdipAddPathArc(p, x, y, w, h, start, sweep);
    return stroke_shape(g, pen, p);
}
GDIPAPI GpStatus GDIPCALL GdipDrawArcI(GpGraphics *g, GpPen *pen, INT x, INT y, INT w, INT h, REAL start, REAL sweep)
{ return GdipDrawArc(g, pen, (REAL)x, (REAL)y, (REAL)w, (REAL)h, start, sweep); }
GDIPAPI GpStatus GDIPCALL GdipFillPolygonI(GpGraphics *g, GpBrush *b, const GpPoint *pts, INT n, INT mode)
{
    GpPath *p;
    if (!g || !b || !pts || n < 3) return InvalidParameter;
    if (GdipCreatePath(mode, &p) != Ok) return OutOfMemory;
    GdipAddPathLine2I(p, pts, n);
    GdipClosePathFigure(p);
    return fill_shape(g, b, p);
}

/* ---- drawing images --------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipDrawImageRectRect(GpGraphics *g, GpImage *i, REAL dx, REAL dy, REAL dw, REAL dh,
                                                REAL sx, REAL sy, REAL sw, REAL sh, INT unit, const void *attrs,
                                                void *cb, void *cbdata)
{
    (void)unit; (void)attrs; (void)cb; (void)cbdata;
    if (!g || !i) return InvalidParameter;
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return Ok;
    if (i->locked) return WrongState;
    GpRectF a = { dw < 0 ? dx + dw : dx, dh < 0 ? dy + dh : dy, fabsf(dw), fabsf(dh) };
    GdipOp op;
    /* images are placed on whole pixels: no stroke or anti-aliasing offset */
    int smoothing = g->smoothing;
    g->smoothing = 0;
    BOOL ok = gdip_op_begin(&op, g, &a, FALSE);
    g->smoothing = smoothing;
    if (!ok) return Ok;
    plutovg_matrix_t m;
    plutovg_matrix_init_translate(&m, dx, dy);
    plutovg_matrix_scale(&m, dw / sw, dh / sh);
    plutovg_matrix_translate(&m, -sx, -sy);
    plutovg_canvas_set_texture(op.c, i->s, PLUTOVG_TEXTURE_TYPE_PLAIN, 1.f, &m);
    plutovg_canvas_fill_rect(op.c, a.X, a.Y, a.Width, a.Height);
    gdip_op_end(&op);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDrawImageRectRectI(GpGraphics *g, GpImage *i, INT dx, INT dy, INT dw, INT dh,
                                                 INT sx, INT sy, INT sw, INT sh, INT unit, const void *attrs,
                                                 void *cb, void *cbdata)
{
    return GdipDrawImageRectRect(g, i, (REAL)dx, (REAL)dy, (REAL)dw, (REAL)dh, (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh,
                                 unit, attrs, cb, cbdata);
}
GDIPAPI GpStatus GDIPCALL GdipDrawImageRect(GpGraphics *g, GpImage *i, REAL x, REAL y, REAL w, REAL h)
{
    if (!g || !i) return InvalidParameter;
    return GdipDrawImageRectRect(g, i, x, y, w, h, 0, 0, (REAL)plutovg_surface_get_width(i->s),
                                 (REAL)plutovg_surface_get_height(i->s), UnitPixel, NULL, NULL, NULL);
}
GDIPAPI GpStatus GDIPCALL GdipDrawImageRectI(GpGraphics *g, GpImage *i, INT x, INT y, INT w, INT h)
{ return GdipDrawImageRect(g, i, (REAL)x, (REAL)y, (REAL)w, (REAL)h); }
GDIPAPI GpStatus GDIPCALL GdipDrawImage(GpGraphics *g, GpImage *i, REAL x, REAL y)
{
    if (!g || !i) return InvalidParameter;
    return GdipDrawImageRect(g, i, x, y, (REAL)plutovg_surface_get_width(i->s), (REAL)plutovg_surface_get_height(i->s));
}
GDIPAPI GpStatus GDIPCALL GdipDrawImageI(GpGraphics *g, GpImage *i, INT x, INT y) { return GdipDrawImage(g, i, (REAL)x, (REAL)y); }
GDIPAPI GpStatus GDIPCALL GdipDrawImagePointRectI(GpGraphics *g, GpImage *i, INT x, INT y, INT sx, INT sy, INT sw, INT sh, INT unit)
{ return GdipDrawImageRectRectI(g, i, x, y, sw, sh, sx, sy, sw, sh, unit, NULL, NULL, NULL); }

GDIPAPI GpStatus GDIPCALL GdipGetImageThumbnail(GpImage *i, UINT w, UINT h, GpImage **out, void *cb, void *data)
{
    (void)cb; (void)data;
    if (!i || !out) return InvalidParameter;
    if (!w) w = 120;
    if (!h) h = 120;
    GpBitmap *t;
    GpStatus st = GdipCreateBitmapFromScan0((INT)w, (INT)h, 0, PixelFormat32bppARGB, NULL, &t);
    if (st != Ok) return st;
    GpGraphics *g;
    GdipGetImageGraphicsContext(t, &g);
    GdipDrawImageRect(g, i, 0, 0, (REAL)w, (REAL)h);
    GdipDeleteGraphics(g);
    *out = t;
    return Ok;
}

/* ---- filling and stroking paths ---------------------------------------- */

static GpRectF grow(GpRectF r, REAL d) { return (GpRectF){ r.X - d, r.Y - d, r.Width + 2 * d, r.Height + 2 * d }; }

GDIPAPI GpStatus GDIPCALL GdipFillPath(GpGraphics *g, GpBrush *b, GpPath *p)
{
    if (!g || !b || !p) return InvalidParameter;
    if (!p->n) return Ok;
    GpRectF a = grow(gdip_path_bounds(p), 1);
    plutovg_path_t *pp = gdip_path_build(p);
    if (!pp) return OutOfMemory;
    GdipOp op;
    if (gdip_op_begin(&op, g, &a, FALSE)) {
        gdip_use_brush(op.c, b);
        plutovg_canvas_set_fill_rule(op.c, p->fill_mode == FillModeWinding ? PLUTOVG_FILL_RULE_NON_ZERO : PLUTOVG_FILL_RULE_EVEN_ODD);
        plutovg_canvas_fill_path(op.c, pp);
        gdip_op_end(&op);
    }
    plutovg_path_destroy(pp);
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *p)
{
    if (!g || !pen || !p) return InvalidParameter;
    if (!p->n) return Ok;
    GpRectF a = grow(gdip_path_bounds(p), pen->width + 2);
    plutovg_path_t *pp = gdip_path_build(p);
    if (!pp) return OutOfMemory;
    GdipOp op;
    if (gdip_op_begin(&op, g, &a, TRUE)) {
        gdip_use_pen(op.c, pen);
        plutovg_canvas_stroke_path(op.c, pp);
        gdip_op_end(&op);
    }
    plutovg_path_destroy(pp);
    return Ok;
}
