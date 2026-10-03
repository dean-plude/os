/*
 * more.c — the rest of GDI+'s flat API that wxWidgets, .NET and WPF-free
 * programs reach for: cardinal-spline curves, pies and polygons, images on
 * parallelograms, containers, region data and scans, the gradient, hatch
 * and texture brushes with their blends and transforms, every pen
 * property, custom line caps, and the metafile entry points (recorded
 * metafiles are not supported: those return NotImplemented).
 *
 * wx's GDI+ renderer resolves all six hundred names it uses at start-up
 * and disables itself if one is missing, so every name is here even where
 * the implementation is a stub.
 */
#include "gdip.h"

static GpPointF *to_f(const GpPoint *p, int n)
{
    GpPointF *f = xalloc(sizeof *f * (n > 0 ? n : 1));
    if (f) for (int i = 0; i < n; i++) f[i] = (GpPointF){ (REAL)p[i].X, (REAL)p[i].Y };
    return f;
}
static GpRectF rect_f(const GpRect *r) { return (GpRectF){ (REAL)r->X, (REAL)r->Y, (REAL)r->Width, (REAL)r->Height }; }
static GpRect rect_i(GpRectF r)
{
    GpRect o = { (INT)floorf(r.X), (INT)floorf(r.Y), 0, 0 };
    o.Width = (INT)ceilf(r.X + r.Width) - o.X;
    o.Height = (INT)ceilf(r.Y + r.Height) - o.Y;
    return o;
}

/* ---- cardinal splines ---------------------------------------------------- */

/* The curve through pts[offset..offset+nseg] as Béziers, GDI+'s way: the
 * tangent at a point is `tension` times the chord between its neighbours;
 * an open curve repeats its end points, a closed one wraps around. */
static GpStatus add_curve(GpPath *p, const GpPointF *pts, int n, int offset, int nseg, REAL tension, BOOL closed)
{
    if (!p || !pts || n < 2 || offset < 0 || nseg < 1 || offset + nseg >= n + (closed ? 1 : 0)) return InvalidParameter;
    if (closed && n < 3) return InvalidParameter;
    int total = closed ? n : nseg;
    GpPointF *b = xalloc(sizeof *b * (3 * total + 1));
    if (!b) return OutOfMemory;
    #define PT(i) pts[closed ? ((i) % n + n) % n : ((i) < 0 ? 0 : (i) >= n ? n - 1 : (i))]
    int k = 0;
    b[k++] = PT(offset);
    for (int s = 0; s < total; s++) {
        int i = offset + s;
        GpPointF p0 = PT(i - 1), p1 = PT(i), p2 = PT(i + 1), p3 = PT(i + 2);
        b[k++] = (GpPointF){ p1.X + (p2.X - p0.X) * tension / 3, p1.Y + (p2.Y - p0.Y) * tension / 3 };
        b[k++] = (GpPointF){ p2.X - (p3.X - p1.X) * tension / 3, p2.Y - (p3.Y - p1.Y) * tension / 3 };
        b[k++] = p2;
    }
    #undef PT
    if (closed) p->new_figure = TRUE;
    GpStatus st = GdipAddPathBeziers(p, b, k);
    if (st == Ok && closed) GdipClosePathFigure(p);
    xfree(b);
    return st;
}
static GpStatus add_curve_i(GpPath *p, const GpPoint *pts, int n, int offset, int nseg, REAL tension, BOOL closed)
{
    if (!pts || n < 1) return InvalidParameter;
    GpPointF *f = to_f(pts, n);
    if (!f) return OutOfMemory;
    GpStatus st = add_curve(p, f, n, offset, nseg, tension, closed);
    xfree(f);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathCurve(GpPath *p, const GpPointF *pts, INT n) { return add_curve(p, pts, n, 0, n - 1, 0.5f, FALSE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathCurveI(GpPath *p, const GpPoint *pts, INT n) { return add_curve_i(p, pts, n, 0, n - 1, 0.5f, FALSE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathCurve2(GpPath *p, const GpPointF *pts, INT n, REAL t) { return add_curve(p, pts, n, 0, n - 1, t, FALSE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathCurve2I(GpPath *p, const GpPoint *pts, INT n, REAL t) { return add_curve_i(p, pts, n, 0, n - 1, t, FALSE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathCurve3(GpPath *p, const GpPointF *pts, INT n, INT off, INT nseg, REAL t) { return add_curve(p, pts, n, off, nseg, t, FALSE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathCurve3I(GpPath *p, const GpPoint *pts, INT n, INT off, INT nseg, REAL t) { return add_curve_i(p, pts, n, off, nseg, t, FALSE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathClosedCurve(GpPath *p, const GpPointF *pts, INT n) { return add_curve(p, pts, n, 0, n, 0.5f, TRUE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathClosedCurveI(GpPath *p, const GpPoint *pts, INT n) { return add_curve_i(p, pts, n, 0, n, 0.5f, TRUE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathClosedCurve2(GpPath *p, const GpPointF *pts, INT n, REAL t) { return add_curve(p, pts, n, 0, n, t, TRUE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathClosedCurve2I(GpPath *p, const GpPoint *pts, INT n, REAL t) { return add_curve_i(p, pts, n, 0, n, t, TRUE); }
GDIPAPI GpStatus GDIPCALL GdipAddPathPieI(GpPath *p, INT x, INT y, INT w, INT h, REAL start, REAL sweep)
{ return GdipAddPathPie(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h, start, sweep); }

/* ---- drawing with curves, pies, polygons and rectangles -------------------- */

static GpStatus draw_path(GpGraphics *g, GpPen *pen, GpPath *p, GpStatus st)
{
    if (st == Ok) st = GdipDrawPath(g, pen, p);
    GdipDeletePath(p);
    return st;
}
static GpStatus fill_path(GpGraphics *g, GpBrush *b, GpPath *p, GpStatus st)
{
    if (st == Ok) st = GdipFillPath(g, b, p);
    GdipDeletePath(p);
    return st;
}
static GpStatus curve_op(GpGraphics *g, GpPen *pen, GpBrush *b, const GpPointF *pts, int n, int off, int nseg, REAL t, BOOL closed, int fill_mode)
{
    GpPath *p;
    if (!g || (!pen && !b) || !pts) return InvalidParameter;
    if (GdipCreatePath(fill_mode, &p) != Ok) return OutOfMemory;
    GpStatus st = add_curve(p, pts, n, off, nseg, t, closed);
    return pen ? draw_path(g, pen, p, st) : fill_path(g, b, p, st);
}
static GpStatus curve_op_i(GpGraphics *g, GpPen *pen, GpBrush *b, const GpPoint *pts, int n, int off, int nseg, REAL t, BOOL closed, int fill_mode)
{
    if (!pts || n < 1) return InvalidParameter;
    GpPointF *f = to_f(pts, n);
    if (!f) return OutOfMemory;
    GpStatus st = curve_op(g, pen, b, f, n, off, nseg, t, closed, fill_mode);
    xfree(f);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipDrawCurve(GpGraphics *g, GpPen *pen, const GpPointF *pts, INT n) { return curve_op(g, pen, NULL, pts, n, 0, n - 1, 0.5f, FALSE, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawCurveI(GpGraphics *g, GpPen *pen, const GpPoint *pts, INT n) { return curve_op_i(g, pen, NULL, pts, n, 0, n - 1, 0.5f, FALSE, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawCurve2(GpGraphics *g, GpPen *pen, const GpPointF *pts, INT n, REAL t) { return curve_op(g, pen, NULL, pts, n, 0, n - 1, t, FALSE, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawCurve2I(GpGraphics *g, GpPen *pen, const GpPoint *pts, INT n, REAL t) { return curve_op_i(g, pen, NULL, pts, n, 0, n - 1, t, FALSE, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawCurve3(GpGraphics *g, GpPen *pen, const GpPointF *pts, INT n, INT off, INT nseg, REAL t) { return curve_op(g, pen, NULL, pts, n, off, nseg, t, FALSE, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawCurve3I(GpGraphics *g, GpPen *pen, const GpPoint *pts, INT n, INT off, INT nseg, REAL t) { return curve_op_i(g, pen, NULL, pts, n, off, nseg, t, FALSE, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawClosedCurve(GpGraphics *g, GpPen *pen, const GpPointF *pts, INT n) { return curve_op(g, pen, NULL, pts, n, 0, n, 0.5f, TRUE, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawClosedCurveI(GpGraphics *g, GpPen *pen, const GpPoint *pts, INT n) { return curve_op_i(g, pen, NULL, pts, n, 0, n, 0.5f, TRUE, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawClosedCurve2(GpGraphics *g, GpPen *pen, const GpPointF *pts, INT n, REAL t) { return curve_op(g, pen, NULL, pts, n, 0, n, t, TRUE, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawClosedCurve2I(GpGraphics *g, GpPen *pen, const GpPoint *pts, INT n, REAL t) { return curve_op_i(g, pen, NULL, pts, n, 0, n, t, TRUE, 0); }
GDIPAPI GpStatus GDIPCALL GdipFillClosedCurve(GpGraphics *g, GpBrush *b, const GpPointF *pts, INT n) { return curve_op(g, NULL, b, pts, n, 0, n, 0.5f, TRUE, 0); }
GDIPAPI GpStatus GDIPCALL GdipFillClosedCurveI(GpGraphics *g, GpBrush *b, const GpPoint *pts, INT n) { return curve_op_i(g, NULL, b, pts, n, 0, n, 0.5f, TRUE, 0); }
GDIPAPI GpStatus GDIPCALL GdipFillClosedCurve2(GpGraphics *g, GpBrush *b, const GpPointF *pts, INT n, REAL t, INT mode) { return curve_op(g, NULL, b, pts, n, 0, n, t, TRUE, mode); }
GDIPAPI GpStatus GDIPCALL GdipFillClosedCurve2I(GpGraphics *g, GpBrush *b, const GpPoint *pts, INT n, REAL t, INT mode) { return curve_op_i(g, NULL, b, pts, n, 0, n, t, TRUE, mode); }

GDIPAPI GpStatus GDIPCALL GdipDrawPie(GpGraphics *g, GpPen *pen, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep)
{
    GpPath *p;
    if (!g || !pen) return InvalidParameter;
    if (GdipCreatePath(0, &p) != Ok) return OutOfMemory;
    return draw_path(g, pen, p, GdipAddPathPie(p, x, y, w, h, start, sweep));
}
GDIPAPI GpStatus GDIPCALL GdipDrawPieI(GpGraphics *g, GpPen *pen, INT x, INT y, INT w, INT h, REAL start, REAL sweep)
{ return GdipDrawPie(g, pen, (REAL)x, (REAL)y, (REAL)w, (REAL)h, start, sweep); }
GDIPAPI GpStatus GDIPCALL GdipFillPie(GpGraphics *g, GpBrush *b, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep)
{
    GpPath *p;
    if (!g || !b) return InvalidParameter;
    if (GdipCreatePath(0, &p) != Ok) return OutOfMemory;
    return fill_path(g, b, p, GdipAddPathPie(p, x, y, w, h, start, sweep));
}
GDIPAPI GpStatus GDIPCALL GdipFillPieI(GpGraphics *g, GpBrush *b, INT x, INT y, INT w, INT h, REAL start, REAL sweep)
{ return GdipFillPie(g, b, (REAL)x, (REAL)y, (REAL)w, (REAL)h, start, sweep); }

GDIPAPI GpStatus GDIPCALL GdipDrawPolygon(GpGraphics *g, GpPen *pen, const GpPointF *pts, INT n)
{
    GpPath *p;
    if (!g || !pen || !pts || n < 2) return InvalidParameter;
    if (GdipCreatePath(0, &p) != Ok) return OutOfMemory;
    GpStatus st = GdipAddPathLine2(p, pts, n);
    if (st == Ok) GdipClosePathFigure(p);
    return draw_path(g, pen, p, st);
}
GDIPAPI GpStatus GDIPCALL GdipFillPolygon(GpGraphics *g, GpBrush *b, const GpPointF *pts, INT n, INT mode)
{
    GpPath *p;
    if (!g || !b || !pts || n < 3) return InvalidParameter;
    if (GdipCreatePath(mode, &p) != Ok) return OutOfMemory;
    GpStatus st = GdipAddPathLine2(p, pts, n);
    if (st == Ok) GdipClosePathFigure(p);
    return fill_path(g, b, p, st);
}
GDIPAPI GpStatus GDIPCALL GdipFillPolygon2(GpGraphics *g, GpBrush *b, const GpPointF *pts, INT n) { return GdipFillPolygon(g, b, pts, n, 0); }
GDIPAPI GpStatus GDIPCALL GdipFillPolygon2I(GpGraphics *g, GpBrush *b, const GpPoint *pts, INT n) { return GdipFillPolygonI(g, b, pts, n, 0); }
GDIPAPI GpStatus GDIPCALL GdipDrawBeziers(GpGraphics *g, GpPen *pen, const GpPointF *pts, INT n)
{
    GpPath *p;
    if (!g || !pen || !pts || n < 4) return InvalidParameter;
    if (GdipCreatePath(0, &p) != Ok) return OutOfMemory;
    return draw_path(g, pen, p, GdipAddPathBeziers(p, pts, n));
}
GDIPAPI GpStatus GDIPCALL GdipDrawRectangles(GpGraphics *g, GpPen *pen, const GpRectF *r, INT n)
{
    if (!r || n < 0) return InvalidParameter;
    for (int i = 0; i < n; i++) {
        GpStatus st = GdipDrawRectangle(g, pen, r[i].X, r[i].Y, r[i].Width, r[i].Height);
        if (st != Ok) return st;
    }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDrawRectanglesI(GpGraphics *g, GpPen *pen, const GpRect *r, INT n)
{
    if (!r || n < 0) return InvalidParameter;
    for (int i = 0; i < n; i++) {
        GpStatus st = GdipDrawRectangleI(g, pen, r[i].X, r[i].Y, r[i].Width, r[i].Height);
        if (st != Ok) return st;
    }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipFillRectangles(GpGraphics *g, GpBrush *b, const GpRectF *r, INT n)
{
    if (!r || n < 0) return InvalidParameter;
    for (int i = 0; i < n; i++) {
        GpStatus st = GdipFillRectangle(g, b, r[i].X, r[i].Y, r[i].Width, r[i].Height);
        if (st != Ok) return st;
    }
    return Ok;
}

/* ---- images on parallelograms -------------------------------------------- */

/* the source rectangle of the image mapped onto the parallelogram with
 * corners dst[0] (top left), dst[1] (top right), dst[2] (bottom left) */
static GpStatus draw_image_points(GpGraphics *g, GpImage *i, const GpPointF *dst, int n, REAL sx, REAL sy, REAL sw, REAL sh)
{
    if (!g || !i || !dst || n != 3) return InvalidParameter;
    if (i->locked) return WrongState;
    if (sw <= 0 || sh <= 0) return Ok;
    GpPointF p3 = { dst[1].X + dst[2].X - dst[0].X, dst[1].Y + dst[2].Y - dst[0].Y };
    GpPointF c[4] = { dst[0], dst[1], p3, dst[2] };
    REAL x0 = c[0].X, x1 = x0, y0 = c[0].Y, y1 = y0;
    for (int k = 1; k < 4; k++) { if (c[k].X < x0) x0 = c[k].X; if (c[k].X > x1) x1 = c[k].X; if (c[k].Y < y0) y0 = c[k].Y; if (c[k].Y > y1) y1 = c[k].Y; }
    GpRectF a = { x0, y0, x1 - x0, y1 - y0 };
    /* source (sx, sy, sw, sh) -> destination: unit square first, then the parallelogram's axes */
    plutovg_matrix_t m;
    plutovg_matrix_init(&m, (dst[1].X - dst[0].X) / sw, (dst[1].Y - dst[0].Y) / sw, (dst[2].X - dst[0].X) / sh, (dst[2].Y - dst[0].Y) / sh,
                        dst[0].X, dst[0].Y);
    plutovg_matrix_translate(&m, -sx, -sy);
    GdipOp op;
    int smoothing = g->smoothing;
    g->smoothing = 0;
    BOOL ok = gdip_op_begin(&op, g, &a, FALSE);
    g->smoothing = smoothing;
    if (!ok) return Ok;
    plutovg_canvas_set_texture(op.c, i->s, PLUTOVG_TEXTURE_TYPE_PLAIN, 1.f, &m);
    plutovg_path_t *path = plutovg_path_create();
    plutovg_path_move_to(path, c[0].X, c[0].Y);
    for (int k = 1; k < 4; k++) plutovg_path_line_to(path, c[k].X, c[k].Y);
    plutovg_path_close(path);
    plutovg_canvas_fill_path(op.c, path);
    plutovg_path_destroy(path);
    gdip_op_end(&op);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDrawImagePoints(GpGraphics *g, GpImage *i, const GpPointF *dst, INT n)
{
    if (!i) return InvalidParameter;
    return draw_image_points(g, i, dst, n, 0, 0, (REAL)plutovg_surface_get_width(i->s), (REAL)plutovg_surface_get_height(i->s));
}
GDIPAPI GpStatus GDIPCALL GdipDrawImagePointsI(GpGraphics *g, GpImage *i, const GpPoint *dst, INT n)
{
    if (!dst || n != 3) return InvalidParameter;
    GpPointF f[3] = { { (REAL)dst[0].X, (REAL)dst[0].Y }, { (REAL)dst[1].X, (REAL)dst[1].Y }, { (REAL)dst[2].X, (REAL)dst[2].Y } };
    return GdipDrawImagePoints(g, i, f, 3);
}
GDIPAPI GpStatus GDIPCALL GdipDrawImagePointsRect(GpGraphics *g, GpImage *i, const GpPointF *dst, INT n, REAL sx, REAL sy, REAL sw, REAL sh,
                                                  INT unit, const void *attrs, void *cb, void *cbdata)
{
    (void)unit; (void)attrs; (void)cb; (void)cbdata;
    return draw_image_points(g, i, dst, n, sx, sy, sw, sh);
}
GDIPAPI GpStatus GDIPCALL GdipDrawImagePointsRectI(GpGraphics *g, GpImage *i, const GpPoint *dst, INT n, INT sx, INT sy, INT sw, INT sh,
                                                   INT unit, const void *attrs, void *cb, void *cbdata)
{
    if (!dst || n != 3) return InvalidParameter;
    GpPointF f[3] = { { (REAL)dst[0].X, (REAL)dst[0].Y }, { (REAL)dst[1].X, (REAL)dst[1].Y }, { (REAL)dst[2].X, (REAL)dst[2].Y } };
    return GdipDrawImagePointsRect(g, i, f, 3, (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh, unit, attrs, cb, cbdata);
}
GDIPAPI GpStatus GDIPCALL GdipDrawImagePointRect(GpGraphics *g, GpImage *i, REAL x, REAL y, REAL sx, REAL sy, REAL sw, REAL sh, INT unit)
{ return GdipDrawImageRectRect(g, i, x, y, sw, sh, sx, sy, sw, sh, unit, NULL, NULL, NULL); }

/* ---- graphics state ---------------------------------------------------------- */

/* a container: the state is saved, and the source rectangle is mapped onto
 * the destination rectangle (both in the given unit) */
GDIPAPI GpStatus GDIPCALL GdipBeginContainer(GpGraphics *g, const GpRectF *dst, const GpRectF *src, INT unit, UINT *state)
{
    if (!g || !dst || !src || !state) return InvalidParameter;
    if (src->Width == 0 || src->Height == 0) return InvalidParameter;
    GpStatus st = GdipSaveGraphics(g, state);
    if (st != Ok) return st;
    REAL k = gdip_unit_to_pixels(unit, 1);
    plutovg_matrix_t m;
    plutovg_matrix_init_translate(&m, dst->X * k, dst->Y * k);
    plutovg_matrix_scale(&m, dst->Width / src->Width, dst->Height / src->Height);
    plutovg_matrix_translate(&m, -src->X * k, -src->Y * k);
    plutovg_matrix_multiply(&g->world, &m, &g->world);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipBeginContainerI(GpGraphics *g, const GpRect *dst, const GpRect *src, INT unit, UINT *state)
{
    if (!dst || !src) return InvalidParameter;
    GpRectF d = rect_f(dst), s = rect_f(src);
    return GdipBeginContainer(g, &d, &s, unit, state);
}
GDIPAPI GpStatus GDIPCALL GdipResetPageTransform(GpGraphics *g)
{
    if (!g) return InvalidParameter;
    g->page_unit = UnitDisplay;
    g->page_scale = 1;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetRenderingOrigin(GpGraphics *g, INT *x, INT *y)
{ if (!g || !x || !y) return InvalidParameter; *x = *y = 0; return Ok; }
static int g_text_contrast = 4;
GDIPAPI GpStatus GDIPCALL GdipSetTextContrast(GpGraphics *g, UINT c) { if (!g || c > 12) return InvalidParameter; g_text_contrast = (int)c; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetTextContrast(GpGraphics *g, UINT *c) { if (!g || !c) return InvalidParameter; *c = (UINT)g_text_contrast; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipIsVisibleClipEmpty(GpGraphics *g, BOOL *out)
{
    GpRectF r;
    if (!g || !out) return InvalidParameter;
    GpStatus st = GdipGetVisibleClipBounds(g, &r);
    if (st == Ok) *out = r.Width <= 0 || r.Height <= 0;
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipComment(GpGraphics *g, UINT size, const BYTE *data) { (void)size; (void)data; return g ? Ok : InvalidParameter; }

/* ---- matrices ------------------------------------------------------------------ */

/* the matrix mapping a rectangle onto the parallelogram of three points */
GDIPAPI GpStatus GDIPCALL GdipCreateMatrix3(const GpRectF *r, const GpPointF *pts, GpMatrix **out)
{
    if (!r || !pts || !out || r->Width == 0 || r->Height == 0) return InvalidParameter;
    GpMatrix *m = xalloc(sizeof *m);
    if (!m) return OutOfMemory;
    REAL a = (pts[1].X - pts[0].X) / r->Width, b = (pts[1].Y - pts[0].Y) / r->Width;
    REAL c = (pts[2].X - pts[0].X) / r->Height, d = (pts[2].Y - pts[0].Y) / r->Height;
    plutovg_matrix_init(m, a, b, c, d, pts[0].X - a * r->X - c * r->Y, pts[0].Y - b * r->X - d * r->Y);
    *out = m;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreateMatrix3I(const GpRect *r, const GpPoint *pts, GpMatrix **out)
{
    if (!r || !pts) return InvalidParameter;
    GpRectF rf = rect_f(r);
    GpPointF f[3] = { { (REAL)pts[0].X, (REAL)pts[0].Y }, { (REAL)pts[1].X, (REAL)pts[1].Y }, { (REAL)pts[2].X, (REAL)pts[2].Y } };
    return GdipCreateMatrix3(&rf, f, out);
}
GDIPAPI GpStatus GDIPCALL GdipIsMatrixEqual(const GpMatrix *a, const GpMatrix *b, BOOL *r)
{
    if (!a || !b || !r) return InvalidParameter;
    *r = a->a == b->a && a->b == b->b && a->c == b->c && a->d == b->d && a->e == b->e && a->f == b->f;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipVectorTransformMatrixPointsI(GpMatrix *m, GpPoint *p, INT n)
{
    if (!m || !p || n <= 0) return InvalidParameter;
    GpPointF *f = to_f(p, n);
    if (!f) return OutOfMemory;
    GpStatus st = GdipVectorTransformMatrixPoints(m, f, n);
    if (st == Ok) for (int i = 0; i < n; i++) p[i] = (GpPoint){ (INT)floorf(f[i].X + .5f), (INT)floorf(f[i].Y + .5f) };
    xfree(f);
    return st;
}

/* ---- regions ---------------------------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipIsEqualRegion(GpRegion *a, GpRegion *b, GpGraphics *g, BOOL *out)
{
    (void)g;
    if (!a || !b || !out) return InvalidParameter;
    *out = FALSE;
    if (a->kind != b->kind) {
        /* a rectangle region and a path region of the same one rectangle are equal */
        GpRectF ra = gdip_region_bounds(a), rb = gdip_region_bounds(b);
        if ((a->kind == RgnRect || a->kind == RgnPath) && (b->kind == RgnRect || b->kind == RgnPath))
            *out = memcmp(&ra, &rb, sizeof ra) == 0 && (a->kind == RgnRect ? b->path->n <= 5 : a->path->n <= 5);
        return Ok;
    }
    switch (a->kind) {
    case RgnInfinite: case RgnEmpty: *out = TRUE; break;
    case RgnRect: *out = memcmp(&a->r, &b->r, sizeof a->r) == 0; break;
    case RgnPath:
        *out = a->path->n == b->path->n && memcmp(a->path->pts, b->path->pts, a->path->n * sizeof *a->path->pts) == 0 &&
               memcmp(a->path->types, b->path->types, a->path->n) == 0;
        break;
    }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipIsVisibleRegionPointI(GpRegion *r, INT x, INT y, GpGraphics *g, BOOL *out)
{ return GdipIsVisibleRegionPoint(r, (REAL)x, (REAL)y, g, out); }
GDIPAPI GpStatus GDIPCALL GdipIsVisibleRegionRect(GpRegion *r, REAL x, REAL y, REAL w, REAL h, GpGraphics *g, BOOL *out)
{
    (void)g;
    if (!r || !out) return InvalidParameter;
    *out = FALSE;
    if (w <= 0 || h <= 0) return Ok;
    if (r->kind == RgnInfinite) { *out = TRUE; return Ok; }
    if (r->kind == RgnEmpty) return Ok;
    GpRectF b = gdip_region_bounds(r);
    BOOL overlap = x < b.X + b.Width && x + w > b.X && y < b.Y + b.Height && y + h > b.Y;
    if (!overlap || r->kind == RgnRect) { *out = overlap; return Ok; }
    /* a path: any of the rectangle's corners or centre inside, or the path's bounds inside the rectangle */
    GpPointF probe[5] = { { x, y }, { x + w, y }, { x, y + h }, { x + w, y + h }, { x + w / 2, y + h / 2 } };
    for (int i = 0; i < 5 && !*out; i++) GdipIsVisibleRegionPoint(r, probe[i].X, probe[i].Y, NULL, out);
    if (!*out && b.X >= x && b.Y >= y && b.X + b.Width <= x + w && b.Y + b.Height <= y + h) *out = TRUE;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipIsVisibleRegionRectI(GpRegion *r, INT x, INT y, INT w, INT h, GpGraphics *g, BOOL *out)
{ return GdipIsVisibleRegionRect(r, (REAL)x, (REAL)y, (REAL)w, (REAL)h, g, out); }
GDIPAPI GpStatus GDIPCALL GdipTransformRegion(GpRegion *r, GpMatrix *m)
{
    if (!r || !m) return InvalidParameter;
    if (r->kind == RgnPath) return GdipTransformPath(r->path, m);
    if (r->kind != RgnRect) return Ok;
    /* a rectangle stays one under translation and scaling; otherwise it becomes a path */
    if (m->b == 0 && m->c == 0) {
        GpPointF a = { r->r.X, r->r.Y }, b = { r->r.X + r->r.Width, r->r.Y + r->r.Height };
        GdipTransformMatrixPoints(m, &a, 1);
        GdipTransformMatrixPoints(m, &b, 1);
        r->r = (GpRectF){ a.X < b.X ? a.X : b.X, a.Y < b.Y ? a.Y : b.Y, fabsf(b.X - a.X), fabsf(b.Y - a.Y) };
        return Ok;
    }
    GpPath *p;
    if (GdipCreatePath(0, &p) != Ok) return OutOfMemory;
    GdipAddPathRectangle(p, r->r.X, r->r.Y, r->r.Width, r->r.Height);
    GdipTransformPath(p, m);
    r->kind = RgnPath;
    r->path = p;
    return Ok;
}

/* Region data, in GDI+'s file format: a header (size, checksum, magic,
 * element count) and one element: an infinite or empty marker, or a
 * rectangle.  A path region is stored as its path. */
#define RGN_MAGIC 0xDBC01001u
enum { RD_RECT = 0x10000000, RD_PATH = 0x10000001, RD_EMPTY = 0x10000002, RD_INFINITE = 0x10000003 };
static UINT region_data_size(const GpRegion *r)
{
    UINT n = 16 + 4;                        /* header and the element's type */
    if (r->kind == RgnRect) n += 16;
    if (r->kind == RgnPath) n += 4 + 4 + 4 + 4 + r->path->n * 8 + ((r->path->n + 3) & ~3);   /* size, magic, count, flags, points, types */
    return n;
}
GDIPAPI GpStatus GDIPCALL GdipGetRegionDataSize(GpRegion *r, UINT *n)
{ if (!r || !n) return InvalidParameter; *n = region_data_size(r); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetRegionData(GpRegion *r, BYTE *buf, UINT size, UINT *filled)
{
    if (!r || !buf) return InvalidParameter;
    UINT need = region_data_size(r);
    if (size < need) return InsufficientBuffer;
    DWORD *d = (DWORD *)buf;
    d[0] = need - 8; d[1] = 0; d[2] = RGN_MAGIC; d[3] = 0;       /* size after the first two words, checksum, magic, combine count */
    switch (r->kind) {
    case RgnInfinite: d[4] = RD_INFINITE; break;
    case RgnEmpty: d[4] = RD_EMPTY; break;
    case RgnRect: d[4] = RD_RECT; memcpy(&d[5], &r->r, 16); break;
    case RgnPath: {
        d[4] = RD_PATH;
        int n = r->path->n;
        d[5] = 12 + n * 8 + ((n + 3) & ~3);
        d[6] = RGN_MAGIC;
        d[7] = (DWORD)n;
        d[8] = 0;                                                 /* flags: points as floats */
        memcpy(&d[9], r->path->pts, n * 8);
        memcpy((BYTE *)&d[9] + n * 8, r->path->types, n);
        break;
    }
    }
    if (filled) *filled = need;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreateRegionRgnData(const BYTE *buf, INT size, GpRegion **out)
{
    if (!buf || size < 20 || !out) return InvalidParameter;
    const DWORD *d = (const DWORD *)buf;
    if (d[2] != RGN_MAGIC) return InvalidParameter;
    GpRegion *r;
    GpStatus st = GdipCreateRegion(&r);
    if (st != Ok) return st;
    switch (d[4]) {
    case RD_INFINITE: break;
    case RD_EMPTY: r->kind = RgnEmpty; break;
    case RD_RECT:
        if (size < 36) { GdipDeleteRegion(r); return InvalidParameter; }
        r->kind = RgnRect;
        memcpy(&r->r, &d[5], 16);
        break;
    case RD_PATH: {
        if (size < 36) { GdipDeleteRegion(r); return InvalidParameter; }
        int n = (int)d[7];
        BOOL ints = (d[8] & 0x4000) != 0;                      /* points as 16-bit integers */
        int psize = ints ? 4 : 8;
        if (n < 0 || size < 36 + n * psize + n) { GdipDeleteRegion(r); return InvalidParameter; }
        GpPointF *pts = xalloc(sizeof *pts * (n ? n : 1));
        if (!pts) { GdipDeleteRegion(r); return OutOfMemory; }
        for (int i = 0; i < n; i++) {
            if (ints) { const short *s = (const short *)&d[9] + 2 * i; pts[i] = (GpPointF){ s[0], s[1] }; }
            else memcpy(&pts[i], (const BYTE *)&d[9] + 8 * i, 8);
        }
        GpPath *p;
        st = GdipCreatePath2(pts, (const BYTE *)&d[9] + n * psize, n, 0, &p);
        xfree(pts);
        if (st != Ok) { GdipDeleteRegion(r); return st; }
        r->kind = RgnPath;
        r->path = p;
        break;
    }
    default: GdipDeleteRegion(r); return InvalidParameter;
    }
    *out = r;
    return Ok;
}

/* scans: the rectangles a region is made of.  Rectangles and paths report
 * their bounds (a path's inside is not decomposed); the infinite region one
 * huge rectangle; the empty region none. */
static int region_scans(GpRegion *r, GpMatrix *m, GpRectF *out)
{
    GpRectF b;
    switch (r->kind) {
    case RgnEmpty: return 0;
    case RgnInfinite: b = (GpRectF){ -4194304.f, -4194304.f, 8388608.f, 8388608.f }; break;
    default: b = gdip_region_bounds(r); break;
    }
    if (m && r->kind != RgnInfinite) {
        GpPointF c[4] = { { b.X, b.Y }, { b.X + b.Width, b.Y }, { b.X, b.Y + b.Height }, { b.X + b.Width, b.Y + b.Height } };
        GdipTransformMatrixPoints(m, c, 4);
        REAL x0 = c[0].X, x1 = x0, y0 = c[0].Y, y1 = y0;
        for (int i = 1; i < 4; i++) { if (c[i].X < x0) x0 = c[i].X; if (c[i].X > x1) x1 = c[i].X; if (c[i].Y < y0) y0 = c[i].Y; if (c[i].Y > y1) y1 = c[i].Y; }
        b = (GpRectF){ x0, y0, x1 - x0, y1 - y0 };
    }
    if (out) *out = b;
    return 1;
}
GDIPAPI GpStatus GDIPCALL GdipGetRegionScansCount(GpRegion *r, UINT *n, GpMatrix *m)
{ if (!r || !n) return InvalidParameter; *n = (UINT)region_scans(r, m, NULL); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetRegionScans(GpRegion *r, GpRectF *rects, INT *n, GpMatrix *m)
{
    if (!r || !n) return InvalidParameter;
    GpRectF b;
    int k = region_scans(r, m, &b);
    if (rects && k) rects[0] = b;
    *n = k;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetRegionScansI(GpRegion *r, GpRect *rects, INT *n, GpMatrix *m)
{
    if (!r || !n) return InvalidParameter;
    GpRectF b;
    int k = region_scans(r, m, &b);
    if (rects && k) rects[0] = rect_i(b);
    *n = k;
    return Ok;
}

/* ---- path iterators and paths ------------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipPathIterIsValid(GpPathIterator *it, BOOL *valid)
{ if (!it || !valid) return InvalidParameter; *valid = TRUE; return Ok; }
/* the next run of points of one type (lines or Béziers) within the current subpath */
GDIPAPI GpStatus GDIPCALL GdipPathIterNextPathType(GpPathIterator *it, INT *count, BYTE *type, INT *start, INT *end)
{
    if (!it || !count || !type || !start || !end) return InvalidParameter;
    GpPath *p = it->p;
    int i = it->marker;                                         /* reused as the type cursor */
    if (i >= p->n) { *count = 0; return Ok; }
    if ((p->types[i] & PathPointTypePathTypeMask) == PathPointTypeStart) i++;
    if (i >= p->n) { *count = 0; it->marker = p->n; return Ok; }
    BYTE t = p->types[i] & PathPointTypePathTypeMask;
    int j = i;
    while (j < p->n && (p->types[j] & PathPointTypePathTypeMask) == t) j++;
    *type = t;
    *start = i - 1;
    *end = j - 1;
    *count = j - i + 1;
    it->marker = j;
    return Ok;
}
/* warping and widening a path: the path is transformed (the warp's
 * parallelogram is not applied, the outline is not widened) */
GDIPAPI GpStatus GDIPCALL GdipWarpPath(GpPath *p, GpMatrix *m, const GpPointF *pts, INT n, REAL x, REAL y, REAL w, REAL h, INT mode, REAL flatness)
{
    (void)mode; (void)flatness;
    if (!p || !pts || (n != 3 && n != 4) || w <= 0 || h <= 0) return InvalidParameter;
    GpRectF r = { x, y, w, h };
    GpMatrix *warp;
    GpStatus st = GdipCreateMatrix3(&r, pts, &warp);
    if (st != Ok) return st;
    if (m) GdipMultiplyMatrix(warp, m, MatrixOrderAppend);
    st = GdipTransformPath(p, warp);
    GdipDeleteMatrix(warp);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipWidenPath(GpPath *p, GpPen *pen, GpMatrix *m, REAL flatness)
{
    if (!p || !pen) return InvalidParameter;
    return GdipFlattenPath(p, m, flatness);
}

/* ---- hatch brushes ---------------------------------------------------------------- */

static GpBrush *brush_new(int type)
{
    GpBrush *b = xalloc(sizeof *b);
    if (!b) return NULL;
    b->type = type;
    plutovg_matrix_init_identity(&b->xform);
    return b;
}
GDIPAPI GpStatus GDIPCALL GdipCreateHatchBrush(INT style, ARGB fore, ARGB back, GpBrush **out)
{
    if (!out || style < 0 || style > 52) return InvalidParameter;
    GpBrush *b = brush_new(BrushTypeHatchFill);
    if (!b) return OutOfMemory;
    b->hatch = style; b->color = fore; b->color2 = back;
    *out = b;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetHatchStyle(GpBrush *b, INT *s) { if (!b || !s || b->type != BrushTypeHatchFill) return InvalidParameter; *s = b->hatch; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetHatchForegroundColor(GpBrush *b, ARGB *c) { if (!b || !c || b->type != BrushTypeHatchFill) return InvalidParameter; *c = b->color; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetHatchBackgroundColor(GpBrush *b, ARGB *c) { if (!b || !c || b->type != BrushTypeHatchFill) return InvalidParameter; *c = b->color2; return Ok; }

/* ---- blends, shared by the gradient brushes ---------------------------------------- */

static GpStatus set_blend(GpBrush *b, const REAL *f, const REAL *pos, INT n)
{
    if (!b || !f || !pos || n < 1 || n > MAX_BLEND) return n > MAX_BLEND ? OutOfMemory : InvalidParameter;
    if (n > 1 && (pos[0] != 0 || pos[n - 1] != 1)) return InvalidParameter;
    b->nblend = n;
    for (int i = 0; i < n; i++) { b->blend_f[i] = f[i]; b->blend_p[i] = pos[i]; }
    b->npreset = 0;
    return Ok;
}
static GpStatus get_blend(GpBrush *b, REAL *f, REAL *pos, INT n)
{
    if (!b || !f || !pos || n < 1) return InvalidParameter;
    if (b->nblend == 0) { f[0] = 1; pos[0] = 1; return Ok; }
    if (n < b->nblend) return InsufficientBuffer;
    for (int i = 0; i < b->nblend; i++) { f[i] = b->blend_f[i]; pos[i] = b->blend_p[i]; }
    return Ok;
}
static GpStatus set_preset(GpBrush *b, const ARGB *c, const REAL *pos, INT n)
{
    if (!b || !c || !pos || n < 2 || n > MAX_BLEND) return n > MAX_BLEND ? OutOfMemory : InvalidParameter;
    if (pos[0] != 0 || pos[n - 1] != 1) return InvalidParameter;
    b->npreset = n;
    for (int i = 0; i < n; i++) { b->preset_c[i] = c[i]; b->preset_p[i] = pos[i]; }
    b->nblend = 0;
    return Ok;
}
static GpStatus get_preset(GpBrush *b, ARGB *c, REAL *pos, INT n)
{
    if (!b || !c || !pos || n < 2) return InvalidParameter;
    if (b->npreset < 2) return GenericError;
    if (n < b->npreset) return InsufficientBuffer;
    for (int i = 0; i < b->npreset; i++) { c[i] = b->preset_c[i]; pos[i] = b->preset_p[i]; }
    return Ok;
}
/* a triangular blend: 0 at the ends, `scale` at `focus` */
static GpStatus set_linear_blend(GpBrush *b, REAL focus, REAL scale)
{
    if (!b || focus < 0 || focus > 1 || scale < 0 || scale > 1) return InvalidParameter;
    int n = 0;
    if (focus > 0) { b->blend_p[n] = 0; b->blend_f[n++] = 0; }
    b->blend_p[n] = focus; b->blend_f[n++] = scale;
    if (focus < 1) { b->blend_p[n] = 1; b->blend_f[n++] = 0; }
    b->nblend = n;
    b->npreset = 0;
    return Ok;
}
/* a bell-shaped blend around `focus` */
static GpStatus set_sigma_blend(GpBrush *b, REAL focus, REAL scale)
{
    if (!b || focus < 0 || focus > 1 || scale < 0 || scale > 1) return InvalidParameter;
    int n = 0;
    REAL side[] = { 0.f, 0.25f, 0.5f, 0.75f, 1.f }, bell[] = { 0.f, 0.024f, 0.5f, 0.976f, 1.f };
    if (focus > 0) for (int i = 0; i < 5; i++) { b->blend_p[n] = focus * side[i]; b->blend_f[n++] = scale * bell[i]; }
    else { b->blend_p[n] = 0; b->blend_f[n++] = scale; }
    if (focus < 1) for (int i = 1; i < 5; i++) { b->blend_p[n] = focus + (1 - focus) * side[i]; b->blend_f[n++] = scale * bell[4 - i]; }
    b->nblend = n;
    b->npreset = 0;
    return Ok;
}

/* brush transforms (the brush's own, prepended or appended) */
static GpStatus xform_combine(GpBrush *b, int type, const plutovg_matrix_t *t, INT order)
{
    if (!b || b->type != type) return InvalidParameter;
    if (order == MatrixOrderAppend) plutovg_matrix_multiply(&b->xform, &b->xform, t);
    else plutovg_matrix_multiply(&b->xform, t, &b->xform);
    return Ok;
}
#define BRUSH_TRANSFORMS(Name, kind)                                                                            \
    GDIPAPI GpStatus GDIPCALL GdipGet##Name##Transform(GpBrush *b, GpMatrix *m)                                  \
    { if (!b || !m || b->type != kind) return InvalidParameter; *m = b->xform; return Ok; }                       \
    GDIPAPI GpStatus GDIPCALL GdipSet##Name##Transform(GpBrush *b, const GpMatrix *m)                            \
    { if (!b || !m || b->type != kind) return InvalidParameter; b->xform = *m; return Ok; }                       \
    GDIPAPI GpStatus GDIPCALL GdipReset##Name##Transform(GpBrush *b)                                             \
    { if (!b || b->type != kind) return InvalidParameter; plutovg_matrix_init_identity(&b->xform); return Ok; }  \
    GDIPAPI GpStatus GDIPCALL GdipMultiply##Name##Transform(GpBrush *b, const GpMatrix *m, INT order)            \
    { if (!m) return InvalidParameter; return xform_combine(b, kind, m, order); }                                \
    GDIPAPI GpStatus GDIPCALL GdipTranslate##Name##Transform(GpBrush *b, REAL dx, REAL dy, INT order)            \
    { plutovg_matrix_t t; plutovg_matrix_init_translate(&t, dx, dy); return xform_combine(b, kind, &t, order); } \
    GDIPAPI GpStatus GDIPCALL GdipScale##Name##Transform(GpBrush *b, REAL sx, REAL sy, INT order)                \
    { plutovg_matrix_t t; plutovg_matrix_init_scale(&t, sx, sy); return xform_combine(b, kind, &t, order); }     \
    GDIPAPI GpStatus GDIPCALL GdipRotate##Name##Transform(GpBrush *b, REAL deg, INT order)                       \
    { plutovg_matrix_t t; plutovg_matrix_init_rotate(&t, deg * 3.14159265f / 180.f); return xform_combine(b, kind, &t, order); }
BRUSH_TRANSFORMS(Line, BrushTypeLinearGradient)
BRUSH_TRANSFORMS(PathGradient, BrushTypePathGradient)
BRUSH_TRANSFORMS(Texture, BrushTypeTextureFill)

/* ---- linear gradient brushes ---------------------------------------------------------- */

static GpStatus line_new(const GpRectF *r, ARGB c1, ARGB c2, REAL angle, BOOL scalable, INT wrap, GpBrush **out)
{
    if (!out || !r || wrap < 0 || wrap > 4) return InvalidParameter;
    if (wrap == 4 /* WrapModeClamp */ || r->Width == 0 || r->Height == 0) return InvalidParameter;
    GpBrush *b = brush_new(BrushTypeLinearGradient);
    if (!b) return OutOfMemory;
    b->rect = *r; b->color = c1; b->color2 = c2; b->angle = angle; b->angle_scalable = scalable; b->wrap = wrap;
    *out = b;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreateLineBrush(const GpPointF *p1, const GpPointF *p2, ARGB c1, ARGB c2, INT wrap, GpBrush **out)
{
    if (!p1 || !p2) return InvalidParameter;
    if (p1->X == p2->X && p1->Y == p2->Y) return OutOfMemory;    /* as GDI+ reports a zero-length gradient */
    /* the gradient's rectangle spans the points; a horizontal or vertical
     * line gets the other dimension from its length, as GDI+ does */
    REAL w = p2->X - p1->X, h = p2->Y - p1->Y;
    GpRectF r = { w < 0 ? p2->X : p1->X, h < 0 ? p2->Y : p1->Y, fabsf(w), fabsf(h) };
    if (r.Width == 0) { r.Width = r.Height; r.X -= r.Width / 2; }
    if (r.Height == 0) { r.Height = r.Width; r.Y -= r.Height / 2; }
    REAL angle = atan2f(h, w) * 180.f / 3.14159265f;
    return line_new(&r, c1, c2, angle, FALSE, wrap, out);
}
GDIPAPI GpStatus GDIPCALL GdipCreateLineBrushI(const GpPoint *p1, const GpPoint *p2, ARGB c1, ARGB c2, INT wrap, GpBrush **out)
{
    if (!p1 || !p2) return InvalidParameter;
    GpPointF a = { (REAL)p1->X, (REAL)p1->Y }, b = { (REAL)p2->X, (REAL)p2->Y };
    return GdipCreateLineBrush(&a, &b, c1, c2, wrap, out);
}
/* LinearGradientMode: Horizontal 0, Vertical 1, ForwardDiagonal 2, BackwardDiagonal 3 */
GDIPAPI GpStatus GDIPCALL GdipCreateLineBrushFromRect(const GpRectF *r, ARGB c1, ARGB c2, INT mode, INT wrap, GpBrush **out)
{
    if (mode < 0 || mode > 3) return InvalidParameter;
    static const REAL angles[] = { 0, 90, 45, 135 };
    return line_new(r, c1, c2, angles[mode], mode >= 2, wrap, out);
}
GDIPAPI GpStatus GDIPCALL GdipCreateLineBrushFromRectI(const GpRect *r, ARGB c1, ARGB c2, INT mode, INT wrap, GpBrush **out)
{ if (!r) return InvalidParameter; GpRectF f = rect_f(r); return GdipCreateLineBrushFromRect(&f, c1, c2, mode, wrap, out); }
GDIPAPI GpStatus GDIPCALL GdipCreateLineBrushFromRectWithAngle(const GpRectF *r, ARGB c1, ARGB c2, REAL angle, BOOL scalable, INT wrap, GpBrush **out)
{ return line_new(r, c1, c2, angle, scalable, wrap, out); }
GDIPAPI GpStatus GDIPCALL GdipCreateLineBrushFromRectWithAngleI(const GpRect *r, ARGB c1, ARGB c2, REAL angle, BOOL scalable, INT wrap, GpBrush **out)
{ if (!r) return InvalidParameter; GpRectF f = rect_f(r); return line_new(&f, c1, c2, angle, scalable, wrap, out); }

#define LINE(b) if (!(b) || (b)->type != BrushTypeLinearGradient) return InvalidParameter
GDIPAPI GpStatus GDIPCALL GdipSetLineColors(GpBrush *b, ARGB c1, ARGB c2) { LINE(b); b->color = c1; b->color2 = c2; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetLineColors(GpBrush *b, ARGB *c) { LINE(b); if (!c) return InvalidParameter; c[0] = b->color; c[1] = b->color2; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetLineRect(GpBrush *b, GpRectF *r) { LINE(b); if (!r) return InvalidParameter; *r = b->rect; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetLineRectI(GpBrush *b, GpRect *r) { LINE(b); if (!r) return InvalidParameter; *r = rect_i(b->rect); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetLineGammaCorrection(GpBrush *b, BOOL on) { LINE(b); b->gamma = on; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetLineGammaCorrection(GpBrush *b, BOOL *on) { LINE(b); if (!on) return InvalidParameter; *on = b->gamma; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetLineWrapMode(GpBrush *b, INT wrap) { LINE(b); if (wrap < 0 || wrap > 4 || wrap == 4) return InvalidParameter; b->wrap = wrap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetLineWrapMode(GpBrush *b, INT *wrap) { LINE(b); if (!wrap) return InvalidParameter; *wrap = b->wrap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetLineBlendCount(GpBrush *b, INT *n) { LINE(b); if (!n) return InvalidParameter; *n = b->nblend ? b->nblend : 1; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetLineBlend(GpBrush *b, REAL *f, REAL *pos, INT n) { LINE(b); return get_blend(b, f, pos, n); }
GDIPAPI GpStatus GDIPCALL GdipSetLineBlend(GpBrush *b, const REAL *f, const REAL *pos, INT n) { LINE(b); return set_blend(b, f, pos, n); }
GDIPAPI GpStatus GDIPCALL GdipGetLinePresetBlendCount(GpBrush *b, INT *n) { LINE(b); if (!n) return InvalidParameter; *n = b->npreset; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetLinePresetBlend(GpBrush *b, ARGB *c, REAL *pos, INT n) { LINE(b); return get_preset(b, c, pos, n); }
GDIPAPI GpStatus GDIPCALL GdipSetLinePresetBlend(GpBrush *b, const ARGB *c, const REAL *pos, INT n) { LINE(b); return set_preset(b, c, pos, n); }
GDIPAPI GpStatus GDIPCALL GdipSetLineLinearBlend(GpBrush *b, REAL focus, REAL scale) { LINE(b); return set_linear_blend(b, focus, scale); }
GDIPAPI GpStatus GDIPCALL GdipSetLineSigmaBlend(GpBrush *b, REAL focus, REAL scale) { LINE(b); return set_sigma_blend(b, focus, scale); }

/* ---- path gradient brushes ---------------------------------------------------------------- */

static GpStatus path_gradient_new(const GpPointF *pts, INT n, INT wrap, GpBrush **out)
{
    if (!pts || n < 2 || !out || wrap < 0 || wrap > 4) return InvalidParameter;
    GpBrush *b = brush_new(BrushTypePathGradient);
    if (!b) return OutOfMemory;
    b->pts = xalloc(sizeof *b->pts * n);
    b->surround = xalloc(sizeof *b->surround);
    if (!b->pts || !b->surround) { gdip_brush_free(b); xfree(b); return OutOfMemory; }
    memcpy(b->pts, pts, sizeof *pts * n);
    b->npts = n;
    b->surround[0] = 0xFFFFFFFF;
    b->nsurround = 1;
    b->color = 0xFF000000;
    b->wrap = wrap;
    REAL x0 = pts[0].X, x1 = x0, y0 = pts[0].Y, y1 = y0, cx = 0, cy = 0;
    for (int i = 0; i < n; i++) {
        if (pts[i].X < x0) x0 = pts[i].X; if (pts[i].X > x1) x1 = pts[i].X;
        if (pts[i].Y < y0) y0 = pts[i].Y; if (pts[i].Y > y1) y1 = pts[i].Y;
        cx += pts[i].X; cy += pts[i].Y;
    }
    b->rect = (GpRectF){ x0, y0, x1 - x0, y1 - y0 };
    b->centre = (GpPointF){ cx / n, cy / n };
    *out = b;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreatePathGradient(const GpPointF *pts, INT n, INT wrap, GpBrush **out) { return path_gradient_new(pts, n, wrap, out); }
GDIPAPI GpStatus GDIPCALL GdipCreatePathGradientI(const GpPoint *pts, INT n, INT wrap, GpBrush **out)
{
    if (!pts || n < 2) return InvalidParameter;
    GpPointF *f = to_f(pts, n);
    if (!f) return OutOfMemory;
    GpStatus st = path_gradient_new(f, n, wrap, out);
    xfree(f);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipCreatePathGradientFromPath(GpPath *p, GpBrush **out)
{
    if (!p || !out) return InvalidParameter;
    if (p->n < 2) return OutOfMemory;
    GpStatus st = path_gradient_new(p->pts, p->n, 4 /* WrapModeClamp */, out);
    if (st == Ok) (*out)->rect = gdip_path_bounds(p);
    return st;
}
#define PGRAD(b) if (!(b) || (b)->type != BrushTypePathGradient) return InvalidParameter
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientCenterColor(GpBrush *b, ARGB *c) { PGRAD(b); if (!c) return InvalidParameter; *c = b->color; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientCenterColor(GpBrush *b, ARGB c) { PGRAD(b); b->color = c; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientCenterPoint(GpBrush *b, GpPointF *p) { PGRAD(b); if (!p) return InvalidParameter; *p = b->centre; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientCenterPointI(GpBrush *b, GpPoint *p)
{ PGRAD(b); if (!p) return InvalidParameter; *p = (GpPoint){ (INT)floorf(b->centre.X + .5f), (INT)floorf(b->centre.Y + .5f) }; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientCenterPoint(GpBrush *b, const GpPointF *p) { PGRAD(b); if (!p) return InvalidParameter; b->centre = *p; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientCenterPointI(GpBrush *b, const GpPoint *p)
{ PGRAD(b); if (!p) return InvalidParameter; b->centre = (GpPointF){ (REAL)p->X, (REAL)p->Y }; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientFocusScales(GpBrush *b, REAL *x, REAL *y) { PGRAD(b); if (!x || !y) return InvalidParameter; *x = b->focus.X; *y = b->focus.Y; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientFocusScales(GpBrush *b, REAL x, REAL y) { PGRAD(b); b->focus = (GpPointF){ x, y }; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientGammaCorrection(GpBrush *b, BOOL *on) { PGRAD(b); if (!on) return InvalidParameter; *on = b->gamma; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientGammaCorrection(GpBrush *b, BOOL on) { PGRAD(b); b->gamma = on; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientPath(GpBrush *b, GpPath *p) { PGRAD(b); (void)p; return NotImplemented; }     /* as GDI+ */
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientPath(GpBrush *b, const GpPath *p) { PGRAD(b); (void)p; return NotImplemented; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientPointCount(GpBrush *b, INT *n) { PGRAD(b); if (!n) return InvalidParameter; *n = b->npts; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientRect(GpBrush *b, GpRectF *r) { PGRAD(b); if (!r) return InvalidParameter; *r = b->rect; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientRectI(GpBrush *b, GpRect *r) { PGRAD(b); if (!r) return InvalidParameter; *r = rect_i(b->rect); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientSurroundColorCount(GpBrush *b, INT *n) { PGRAD(b); if (!n) return InvalidParameter; *n = b->npts; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientSurroundColorsWithCount(GpBrush *b, ARGB *c, INT *n)
{
    PGRAD(b);
    if (!c || !n || *n < b->npts) return InvalidParameter;
    /* one colour per point; a shorter list repeats its last colour */
    for (int i = 0; i < b->npts; i++) c[i] = b->surround[i < b->nsurround ? i : b->nsurround - 1];
    *n = b->nsurround == 1 ? 1 : b->npts;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientSurroundColorsWithCount(GpBrush *b, const ARGB *c, INT *n)
{
    PGRAD(b);
    if (!c || !n || *n < 1 || *n > b->npts) return InvalidParameter;
    int k = *n;
    while (k > 1 && c[k - 1] == c[k - 2]) k--;                   /* trailing repeats collapse, as GDI+ reports */
    ARGB *s = xalloc(sizeof *s * k);
    if (!s) return OutOfMemory;
    memcpy(s, c, sizeof *s * k);
    xfree(b->surround);
    b->surround = s;
    b->nsurround = k;
    *n = k;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientWrapMode(GpBrush *b, INT *wrap) { PGRAD(b); if (!wrap) return InvalidParameter; *wrap = b->wrap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientWrapMode(GpBrush *b, INT wrap) { PGRAD(b); if (wrap < 0 || wrap > 4) return InvalidParameter; b->wrap = wrap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientBlendCount(GpBrush *b, INT *n) { PGRAD(b); if (!n) return InvalidParameter; *n = b->nblend ? b->nblend : 1; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientBlend(GpBrush *b, REAL *f, REAL *pos, INT n) { PGRAD(b); return get_blend(b, f, pos, n); }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientBlend(GpBrush *b, const REAL *f, const REAL *pos, INT n) { PGRAD(b); return set_blend(b, f, pos, n); }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientPresetBlendCount(GpBrush *b, INT *n) { PGRAD(b); if (!n) return InvalidParameter; *n = b->npreset; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathGradientPresetBlend(GpBrush *b, ARGB *c, REAL *pos, INT n) { PGRAD(b); return get_preset(b, c, pos, n); }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientPresetBlend(GpBrush *b, const ARGB *c, const REAL *pos, INT n) { PGRAD(b); return set_preset(b, c, pos, n); }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientLinearBlend(GpBrush *b, REAL focus, REAL scale) { PGRAD(b); return set_linear_blend(b, focus, scale); }
GDIPAPI GpStatus GDIPCALL GdipSetPathGradientSigmaBlend(GpBrush *b, REAL focus, REAL scale) { PGRAD(b); return set_sigma_blend(b, focus, scale); }

/* ---- texture brushes -------------------------------------------------------------------- */

static GpStatus texture_new(GpImage *img, INT wrap, REAL x, REAL y, REAL w, REAL h, BOOL whole, GpBrush **out)
{
    if (!img || !out || wrap < 0 || wrap > 4) return InvalidParameter;
    if (img->locked) return WrongState;
    REAL iw = (REAL)plutovg_surface_get_width(img->s), ih = (REAL)plutovg_surface_get_height(img->s);
    if (!whole && (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > iw || y + h > ih)) return OutOfMemory;   /* GDI+'s status for a bad rectangle */
    GpBrush *b = brush_new(BrushTypeTextureFill);
    if (!b) return OutOfMemory;
    GpStatus st = whole ? GdipCloneImage(img, &b->img) : GdipCloneBitmapArea(x, y, w, h, img->fmt, img, &b->img);
    if (st != Ok) { xfree(b); return st; }
    b->wrap = wrap;
    *out = b;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreateTexture(GpImage *img, INT wrap, GpBrush **out) { return texture_new(img, wrap, 0, 0, 0, 0, TRUE, out); }
GDIPAPI GpStatus GDIPCALL GdipCreateTexture2(GpImage *img, INT wrap, REAL x, REAL y, REAL w, REAL h, GpBrush **out)
{ return texture_new(img, wrap, x, y, w, h, FALSE, out); }
GDIPAPI GpStatus GDIPCALL GdipCreateTexture2I(GpImage *img, INT wrap, INT x, INT y, INT w, INT h, GpBrush **out)
{ return texture_new(img, wrap, (REAL)x, (REAL)y, (REAL)w, (REAL)h, FALSE, out); }
GDIPAPI GpStatus GDIPCALL GdipCreateTextureIA(GpImage *img, const void *attrs, REAL x, REAL y, REAL w, REAL h, GpBrush **out)
{ (void)attrs; return texture_new(img, 0, x, y, w, h, FALSE, out); }
GDIPAPI GpStatus GDIPCALL GdipCreateTextureIAI(GpImage *img, const void *attrs, INT x, INT y, INT w, INT h, GpBrush **out)
{ (void)attrs; return texture_new(img, 0, (REAL)x, (REAL)y, (REAL)w, (REAL)h, FALSE, out); }
#define TEX(b) if (!(b) || (b)->type != BrushTypeTextureFill) return InvalidParameter
GDIPAPI GpStatus GDIPCALL GdipGetTextureImage(GpBrush *b, GpImage **out) { TEX(b); if (!out) return InvalidParameter; return GdipCloneImage(b->img, out); }
GDIPAPI GpStatus GDIPCALL GdipGetTextureWrapMode(GpBrush *b, INT *wrap) { TEX(b); if (!wrap) return InvalidParameter; *wrap = b->wrap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetTextureWrapMode(GpBrush *b, INT wrap) { TEX(b); if (wrap < 0 || wrap > 4) return InvalidParameter; b->wrap = wrap; return Ok; }

/* ---- pen properties ------------------------------------------------------------------------ */

#define PEN(p) if (!(p)) return InvalidParameter
GDIPAPI GpStatus GDIPCALL GdipGetPenFillType(GpPen *p, INT *t) { PEN(p); if (!t) return InvalidParameter; *t = p->brush ? p->brush->type : BrushTypeSolidColor; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenBrushFill(GpPen *p, GpBrush **out)
{
    PEN(p);
    if (!out) return InvalidParameter;
    return p->brush ? GdipCloneBrush(p->brush, out) : GdipCreateSolidFill(p->color, out);
}
GDIPAPI GpStatus GDIPCALL GdipSetPenBrushFill(GpPen *p, GpBrush *b)
{
    PEN(p);
    if (!b) return InvalidParameter;
    GpBrush *c = NULL;
    if (b->type != BrushTypeSolidColor) { GpStatus st = GdipCloneBrush(b, &c); if (st != Ok) return st; }
    if (p->brush) GdipDeleteBrush(p->brush);
    p->brush = c;
    p->color = b->color;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPenLineJoin(GpPen *p, INT *j) { PEN(p); if (!j) return InvalidParameter; *j = p->line_join; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenStartCap(GpPen *p, INT *c) { PEN(p); if (!c) return InvalidParameter; *c = p->start_cap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenEndCap(GpPen *p, INT *c) { PEN(p); if (!c) return InvalidParameter; *c = p->end_cap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenDashCap197819(GpPen *p, INT *c) { PEN(p); if (!c) return InvalidParameter; *c = p->dash_cap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenDashCap197819(GpPen *p, INT c) { PEN(p); p->dash_cap = c; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenDashStyle(GpPen *p, INT *d) { PEN(p); if (!d) return InvalidParameter; *d = p->dash; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenDashOffset(GpPen *p, REAL *o) { PEN(p); if (!o) return InvalidParameter; *o = p->dash_offset; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenDashOffset(GpPen *p, REAL o) { PEN(p); p->dash_offset = o; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenDashCount(GpPen *p, INT *n) { PEN(p); if (!n) return InvalidParameter; *n = p->ndashes; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenDashArray(GpPen *p, REAL *d, INT n)
{
    PEN(p);
    if (!d || n <= 0 || n > p->ndashes) return InvalidParameter;
    memcpy(d, p->dashes, n * sizeof *d);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipSetPenDashArray(GpPen *p, const REAL *d, INT n)
{
    PEN(p);
    if (!d || n <= 0) return InvalidParameter;
    for (int i = 0; i < n; i++) if (d[i] <= 0) return InvalidParameter;
    REAL *c = xalloc(n * sizeof *c);
    if (!c) return OutOfMemory;
    memcpy(c, d, n * sizeof *c);
    xfree(p->dashes);
    p->dashes = c;
    p->ndashes = n;
    p->dash = 5;                                                /* DashStyleCustom */
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPenCompoundCount(GpPen *p, INT *n) { PEN(p); if (!n) return InvalidParameter; *n = p->ncompound; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenCompoundArray(GpPen *p, REAL *d, INT n)
{
    PEN(p);
    if (!d || n <= 0 || n > p->ncompound) return InvalidParameter;
    memcpy(d, p->compound, n * sizeof *d);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipSetPenCompoundArray(GpPen *p, const REAL *d, INT n)
{
    PEN(p);
    if (!d || n < 2 || n & 1) return InvalidParameter;
    for (int i = 0; i < n; i++) if (d[i] < 0 || d[i] > 1 || (i && d[i] < d[i - 1])) return InvalidParameter;
    REAL *c = xalloc(n * sizeof *c);
    if (!c) return OutOfMemory;
    memcpy(c, d, n * sizeof *c);
    xfree(p->compound);
    p->compound = c;
    p->ncompound = n;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPenMiterLimit(GpPen *p, REAL *m) { PEN(p); if (!m) return InvalidParameter; *m = p->miter; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenMiterLimit(GpPen *p, REAL m) { PEN(p); p->miter = m < 1 ? 1 : m; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenMode(GpPen *p, INT *m) { PEN(p); if (!m) return InvalidParameter; *m = p->mode; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenMode(GpPen *p, INT m) { PEN(p); if (m < 0 || m > 2) return InvalidParameter; p->mode = m; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenUnit(GpPen *p, INT *u) { PEN(p); if (!u) return InvalidParameter; *u = p->unit; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenUnit(GpPen *p, INT u) { PEN(p); if (u < 0 || u > UnitMillimeter || u == UnitDisplay) return InvalidParameter; p->unit = u; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPenTransform(GpPen *p, GpMatrix *m) { PEN(p); if (!m) return InvalidParameter; *m = p->xform; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPenTransform(GpPen *p, const GpMatrix *m) { PEN(p); if (!m) return InvalidParameter; p->xform = *m; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipResetPenTransform(GpPen *p) { PEN(p); plutovg_matrix_init_identity(&p->xform); return Ok; }
static GpStatus pen_combine(GpPen *p, const plutovg_matrix_t *t, INT order)
{
    PEN(p);
    if (order == MatrixOrderAppend) plutovg_matrix_multiply(&p->xform, &p->xform, t);
    else plutovg_matrix_multiply(&p->xform, t, &p->xform);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipMultiplyPenTransform(GpPen *p, const GpMatrix *m, INT order) { if (!m) return InvalidParameter; return pen_combine(p, m, order); }
GDIPAPI GpStatus GDIPCALL GdipTranslatePenTransform(GpPen *p, REAL dx, REAL dy, INT order)
{ plutovg_matrix_t t; plutovg_matrix_init_translate(&t, dx, dy); return pen_combine(p, &t, order); }
GDIPAPI GpStatus GDIPCALL GdipScalePenTransform(GpPen *p, REAL sx, REAL sy, INT order)
{ plutovg_matrix_t t; plutovg_matrix_init_scale(&t, sx, sy); return pen_combine(p, &t, order); }
GDIPAPI GpStatus GDIPCALL GdipRotatePenTransform(GpPen *p, REAL deg, INT order)
{ plutovg_matrix_t t; plutovg_matrix_init_rotate(&t, deg * 3.14159265f / 180.f); return pen_combine(p, &t, order); }

/* ---- custom line caps (kept; strokes use the plain caps) --------------------------------- */

/* CustomLineCapType: Default 0, AdjustableArrow 1 */
typedef struct GpCustomLineCap {
    int type;
    GpPath *fill, *stroke;
    int base_cap, start_cap, end_cap, join;
    REAL inset, scale;
    /* arrows */
    REAL height, width, middle_inset;
    BOOL filled;
} GpCustomLineCap;
GDIPAPI GpStatus GDIPCALL GdipCreateCustomLineCap(GpPath *fill, GpPath *stroke, INT base_cap, REAL inset, GpCustomLineCap **out)
{
    if (!out || (!fill && !stroke)) return InvalidParameter;
    GpCustomLineCap *c = xalloc(sizeof *c);
    if (!c) return OutOfMemory;
    if ((fill && GdipClonePath(fill, &c->fill) != Ok) || (stroke && GdipClonePath(stroke, &c->stroke) != Ok)) {
        if (c->fill) GdipDeletePath(c->fill);
        xfree(c);
        return OutOfMemory;
    }
    c->base_cap = base_cap; c->inset = inset; c->scale = 1;
    *out = c;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCloneCustomLineCap(GpCustomLineCap *c, GpCustomLineCap **out)
{
    if (!c || !out) return InvalidParameter;
    GpCustomLineCap *n = xalloc(sizeof *n);
    if (!n) return OutOfMemory;
    *n = *c;
    n->fill = n->stroke = NULL;
    if ((c->fill && GdipClonePath(c->fill, &n->fill) != Ok) || (c->stroke && GdipClonePath(c->stroke, &n->stroke) != Ok)) {
        if (n->fill) GdipDeletePath(n->fill);
        xfree(n);
        return OutOfMemory;
    }
    *out = n;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDeleteCustomLineCap(GpCustomLineCap *c)
{
    if (!c) return InvalidParameter;
    if (c->fill) GdipDeletePath(c->fill);
    if (c->stroke) GdipDeletePath(c->stroke);
    xfree(c);
    return Ok;
}
#define CAP(c) if (!(c)) return InvalidParameter
GDIPAPI GpStatus GDIPCALL GdipGetCustomLineCapType(GpCustomLineCap *c, INT *t) { CAP(c); if (!t) return InvalidParameter; *t = c->type; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetCustomLineCapBaseCap(GpCustomLineCap *c, INT *v) { CAP(c); if (!v) return InvalidParameter; *v = c->base_cap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetCustomLineCapBaseCap(GpCustomLineCap *c, INT v) { CAP(c); c->base_cap = v; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetCustomLineCapBaseInset(GpCustomLineCap *c, REAL *v) { CAP(c); if (!v) return InvalidParameter; *v = c->inset; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetCustomLineCapBaseInset(GpCustomLineCap *c, REAL v) { CAP(c); c->inset = v; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetCustomLineCapStrokeCaps(GpCustomLineCap *c, INT *s, INT *e) { CAP(c); if (!s || !e) return InvalidParameter; *s = c->start_cap; *e = c->end_cap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetCustomLineCapStrokeCaps(GpCustomLineCap *c, INT s, INT e) { CAP(c); c->start_cap = s; c->end_cap = e; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetCustomLineCapStrokeJoin(GpCustomLineCap *c, INT *j) { CAP(c); if (!j) return InvalidParameter; *j = c->join; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetCustomLineCapStrokeJoin(GpCustomLineCap *c, INT j) { CAP(c); c->join = j; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetCustomLineCapWidthScale(GpCustomLineCap *c, REAL *v) { CAP(c); if (!v) return InvalidParameter; *v = c->scale; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetCustomLineCapWidthScale(GpCustomLineCap *c, REAL v) { CAP(c); c->scale = v; return Ok; }

GDIPAPI GpStatus GDIPCALL GdipCreateAdjustableArrowCap(REAL height, REAL width, BOOL filled, GpCustomLineCap **out)
{
    if (!out) return InvalidParameter;
    GpCustomLineCap *c = xalloc(sizeof *c);
    if (!c) return OutOfMemory;
    c->type = 1; c->height = height; c->width = width; c->filled = filled; c->scale = 1;
    *out = c;
    return Ok;
}
#define ARROW(c) if (!(c) || (c)->type != 1) return InvalidParameter
GDIPAPI GpStatus GDIPCALL GdipGetAdjustableArrowCapHeight(GpCustomLineCap *c, REAL *v) { ARROW(c); if (!v) return InvalidParameter; *v = c->height; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetAdjustableArrowCapHeight(GpCustomLineCap *c, REAL v) { ARROW(c); c->height = v; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetAdjustableArrowCapWidth(GpCustomLineCap *c, REAL *v) { ARROW(c); if (!v) return InvalidParameter; *v = c->width; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetAdjustableArrowCapWidth(GpCustomLineCap *c, REAL v) { ARROW(c); c->width = v; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetAdjustableArrowCapMiddleInset(GpCustomLineCap *c, REAL *v) { ARROW(c); if (!v) return InvalidParameter; *v = c->middle_inset; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetAdjustableArrowCapMiddleInset(GpCustomLineCap *c, REAL v) { ARROW(c); c->middle_inset = v; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetAdjustableArrowCapFillState(GpCustomLineCap *c, BOOL *v) { ARROW(c); if (!v) return InvalidParameter; *v = c->filled; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetAdjustableArrowCapFillState(GpCustomLineCap *c, BOOL v) { ARROW(c); c->filled = v; return Ok; }

/* a pen's custom caps: kept and cloned, drawn as the base cap */
GDIPAPI GpStatus GDIPCALL GdipSetPenCustomStartCap(GpPen *p, GpCustomLineCap *c)
{
    PEN(p);
    if (!c) return InvalidParameter;
    GpCustomLineCap *n;
    GpStatus st = GdipCloneCustomLineCap(c, &n);
    if (st != Ok) return st;
    if (p->custom_start) GdipDeleteCustomLineCap(p->custom_start);
    p->custom_start = n;
    p->start_cap = 255;                                         /* LineCapCustom */
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipSetPenCustomEndCap(GpPen *p, GpCustomLineCap *c)
{
    PEN(p);
    if (!c) return InvalidParameter;
    GpCustomLineCap *n;
    GpStatus st = GdipCloneCustomLineCap(c, &n);
    if (st != Ok) return st;
    if (p->custom_end) GdipDeleteCustomLineCap(p->custom_end);
    p->custom_end = n;
    p->end_cap = 255;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPenCustomStartCap(GpPen *p, GpCustomLineCap **out)
{ PEN(p); if (!out) return InvalidParameter; if (!p->custom_start) { *out = NULL; return Ok; } return GdipCloneCustomLineCap(p->custom_start, out); }
GDIPAPI GpStatus GDIPCALL GdipGetPenCustomEndCap(GpPen *p, GpCustomLineCap **out)
{ PEN(p); if (!out) return InvalidParameter; if (!p->custom_end) { *out = NULL; return Ok; } return GdipCloneCustomLineCap(p->custom_end, out); }

/* ---- metafiles: not recorded or played ------------------------------------------------- */

typedef struct GpMetafile GpMetafile;
GDIPAPI GpStatus GDIPCALL GdipCreateMetafileFromEmf(HENHMETAFILE h, BOOL del, GpMetafile **out) { (void)h; (void)del; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipCreateMetafileFromWmf(HMETAFILE h, BOOL del, const void *hdr, GpMetafile **out) { (void)h; (void)del; (void)hdr; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipCreateMetafileFromFile(const WCHAR *f, GpMetafile **out) { (void)f; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipCreateMetafileFromWmfFile(const WCHAR *f, const void *hdr, GpMetafile **out) { (void)f; (void)hdr; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipCreateMetafileFromStream(IStream *s, GpMetafile **out) { (void)s; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipGetHemfFromMetafile(GpMetafile *m, HENHMETAFILE *out) { (void)m; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipGetMetafileHeaderFromEmf(HENHMETAFILE h, void *hdr) { (void)h; return hdr ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipGetMetafileHeaderFromWmf(HMETAFILE h, const void *p, void *hdr) { (void)h; (void)p; return hdr ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipGetMetafileHeaderFromFile(const WCHAR *f, void *hdr) { (void)f; return hdr ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipGetMetafileHeaderFromStream(IStream *s, void *hdr) { (void)s; return hdr ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipGetMetafileHeaderFromMetafile(GpMetafile *m, void *hdr) { (void)m; return hdr ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipGetMetafileDownLevelRasterizationLimit(GpMetafile *m, UINT *dpi) { if (!m || !dpi) return InvalidParameter; *dpi = 96; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetMetafileDownLevelRasterizationLimit(GpMetafile *m, UINT dpi) { (void)dpi; return m ? Ok : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipPlayMetafileRecord(const GpMetafile *m, INT type, UINT flags, UINT size, const BYTE *d)
{ (void)type; (void)flags; (void)size; (void)d; return m ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipRecordMetafile(HDC dc, INT type, const GpRectF *r, INT unit, const WCHAR *desc, GpMetafile **out)
{ (void)dc; (void)type; (void)r; (void)unit; (void)desc; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipRecordMetafileI(HDC dc, INT type, const GpRect *r, INT unit, const WCHAR *desc, GpMetafile **out)
{ (void)dc; (void)type; (void)r; (void)unit; (void)desc; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipRecordMetafileFileName(const WCHAR *f, HDC dc, INT type, const GpRectF *r, INT unit, const WCHAR *desc, GpMetafile **out)
{ (void)f; (void)dc; (void)type; (void)r; (void)unit; (void)desc; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipRecordMetafileFileNameI(const WCHAR *f, HDC dc, INT type, const GpRect *r, INT unit, const WCHAR *desc, GpMetafile **out)
{ (void)f; (void)dc; (void)type; (void)r; (void)unit; (void)desc; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipRecordMetafileStream(IStream *s, HDC dc, INT type, const GpRectF *r, INT unit, const WCHAR *desc, GpMetafile **out)
{ (void)s; (void)dc; (void)type; (void)r; (void)unit; (void)desc; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipRecordMetafileStreamI(IStream *s, HDC dc, INT type, const GpRect *r, INT unit, const WCHAR *desc, GpMetafile **out)
{ (void)s; (void)dc; (void)type; (void)r; (void)unit; (void)desc; return out ? NotImplemented : InvalidParameter; }
/* enumeration: the callback is never called, as there are no records */
#define ENUM(name, ...) GDIPAPI GpStatus GDIPCALL name(GpGraphics *g, GpMetafile *m, __VA_ARGS__, void *cb, void *data, const void *attrs) \
    { (void)cb; (void)data; (void)attrs; return g && m ? NotImplemented : InvalidParameter; }
ENUM(GdipEnumerateMetafileDestPoint, const GpPointF *p)
ENUM(GdipEnumerateMetafileDestPointI, const GpPoint *p)
ENUM(GdipEnumerateMetafileDestRect, const GpRectF *r)
ENUM(GdipEnumerateMetafileDestRectI, const GpRect *r)
ENUM(GdipEnumerateMetafileDestPoints, const GpPointF *p, INT n)
ENUM(GdipEnumerateMetafileDestPointsI, const GpPoint *p, INT n)
ENUM(GdipEnumerateMetafileSrcRectDestPoint, const GpPointF *p, const GpRectF *src, INT unit)
ENUM(GdipEnumerateMetafileSrcRectDestPointI, const GpPoint *p, const GpRect *src, INT unit)
ENUM(GdipEnumerateMetafileSrcRectDestRect, const GpRectF *r, const GpRectF *src, INT unit)
ENUM(GdipEnumerateMetafileSrcRectDestRectI, const GpRect *r, const GpRect *src, INT unit)
ENUM(GdipEnumerateMetafileSrcRectDestPoints, const GpPointF *p, INT n, const GpRectF *src, INT unit)
ENUM(GdipEnumerateMetafileSrcRectDestPointsI, const GpPoint *p, INT n, const GpRect *src, INT unit)

/* ---- filling a region ----------------------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipFillRegion(GpGraphics *g, GpBrush *b, GpRegion *r)
{
    if (!g || !b || !r) return InvalidParameter;
    switch (r->kind) {
    case RgnEmpty: return Ok;
    case RgnRect: return GdipFillRectangle(g, b, r->r.X, r->r.Y, r->r.Width, r->r.Height);
    case RgnPath: return GdipFillPath(g, b, r->path);
    default: {                                                  /* infinite: everything the clip lets through */
        GpRectF v;
        GpStatus st = GdipGetVisibleClipBounds(g, &v);
        if (st != Ok) return st;
        return GdipFillRectangle(g, b, v.X, v.Y, v.Width, v.Height);
    }
    }
}
