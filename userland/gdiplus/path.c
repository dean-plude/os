/*
 * path.c — GDI+ paths, kept as GDI+ keeps them: an array of points and an
 * array of point types (start, line, Bézier; close-subpath and marker
 * flags), so GetPathData, markers and path iterators see what programs
 * expect.  Drawing turns a path into a plutovg path.
 */
#include "gdip.h"

typedef struct { INT Count; GpPointF *Points; BYTE *Types; } PathData;

static BOOL reserve(GpPath *p, int more)
{
    if (p->n + more <= p->cap) return TRUE;
    int cap = p->cap ? p->cap * 2 : 16;
    while (cap < p->n + more) cap *= 2;
    GpPointF *pts = xalloc(sizeof *pts * cap);
    BYTE *types = xalloc(cap);
    if (!pts || !types) { xfree(pts); xfree(types); return FALSE; }
    if (p->n) {
        memcpy(pts, p->pts, sizeof *pts * p->n);
        memcpy(types, p->types, p->n);
    }
    xfree(p->pts);
    xfree(p->types);
    p->pts = pts;
    p->types = types;
    p->cap = cap;
    return TRUE;
}

static void add(GpPath *p, REAL x, REAL y, BYTE type)
{
    p->pts[p->n] = (GpPointF){ x, y };
    p->types[p->n] = type;
    p->n++;
}

/* the first point of a segment: it starts a figure, joins the open one
 * with a line, or is the open figure's last point already */
static void start_at(GpPath *p, REAL x, REAL y)
{
    if (p->new_figure || !p->n) {
        add(p, x, y, PathPointTypeStart);
        p->new_figure = FALSE;
    } else if (p->pts[p->n - 1].X != x || p->pts[p->n - 1].Y != y) {
        add(p, x, y, PathPointTypeLine);
    }
}

GDIPAPI GpStatus GDIPCALL GdipCreatePath(INT fill_mode, GpPath **out)
{
    if (!out) return InvalidParameter;
    GpPath *p = xalloc(sizeof *p);
    if (!p) return OutOfMemory;
    p->fill_mode = fill_mode;
    p->new_figure = TRUE;
    *out = p;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreatePath2(const GpPointF *pts, const BYTE *types, INT n, INT fill_mode, GpPath **out)
{
    if (!pts || !types || n < 0) return InvalidParameter;
    GpStatus st = GdipCreatePath(fill_mode, out);
    if (st != Ok) return st;
    if (n && !reserve(*out, n)) { GdipDeletePath(*out); *out = NULL; return OutOfMemory; }
    for (int i = 0; i < n; i++) add(*out, pts[i].X, pts[i].Y, types[i]);
    (*out)->new_figure = !n || (types[n - 1] & PathPointTypeCloseSubpath);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCreatePath2I(const GpPoint *pts, const BYTE *types, INT n, INT fill_mode, GpPath **out)
{
    if (!pts || n < 0) return InvalidParameter;
    GpPointF *f = xalloc(sizeof *f * (n ? n : 1));
    if (!f) return OutOfMemory;
    for (int i = 0; i < n; i++) f[i] = (GpPointF){ (REAL)pts[i].X, (REAL)pts[i].Y };
    GpStatus st = GdipCreatePath2(f, types, n, fill_mode, out);
    xfree(f);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipClonePath(GpPath *p, GpPath **out)
{
    if (!p || !out) return InvalidParameter;
    GpStatus st = GdipCreatePath2(p->pts ? p->pts : (GpPointF *)p, p->types ? p->types : (BYTE *)p, p->n, p->fill_mode, out);
    if (st == Ok) (*out)->new_figure = p->new_figure;
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipDeletePath(GpPath *p)
{
    if (!p) return InvalidParameter;
    xfree(p->pts);
    xfree(p->types);
    xfree(p);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipResetPath(GpPath *p)
{
    if (!p) return InvalidParameter;
    p->n = 0;
    p->new_figure = TRUE;
    p->fill_mode = 0;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipSetPathFillMode(GpPath *p, INT m) { if (!p) return InvalidParameter; p->fill_mode = m; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathFillMode(GpPath *p, INT *m) { if (!p || !m) return InvalidParameter; *m = p->fill_mode; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipStartPathFigure(GpPath *p) { if (!p) return InvalidParameter; p->new_figure = TRUE; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipClosePathFigure(GpPath *p)
{
    if (!p) return InvalidParameter;
    if (p->n && !p->new_figure) p->types[p->n - 1] |= PathPointTypeCloseSubpath;
    p->new_figure = TRUE;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipClosePathFigures(GpPath *p)
{
    if (!p) return InvalidParameter;
    for (int i = 1; i <= p->n; i++)
        if (i == p->n || (p->types[i] & PathPointTypePathTypeMask) == PathPointTypeStart) p->types[i - 1] |= PathPointTypeCloseSubpath;
    p->new_figure = TRUE;
    return Ok;
}
/* a marker after the last point: path iterators split the path there */
GDIPAPI GpStatus GDIPCALL GdipSetPathMarker(GpPath *p) { if (!p) return InvalidParameter; if (p->n) p->types[p->n - 1] |= PathPointTypePathMarker; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipClearPathMarkers(GpPath *p)
{
    if (!p) return InvalidParameter;
    for (int i = 0; i < p->n; i++) p->types[i] &= ~PathPointTypePathMarker;
    return Ok;
}

/* ---- adding to a path ---------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipAddPathLine(GpPath *p, REAL x1, REAL y1, REAL x2, REAL y2)
{
    if (!p) return InvalidParameter;
    if (!reserve(p, 2)) return OutOfMemory;
    start_at(p, x1, y1);
    add(p, x2, y2, PathPointTypeLine);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathLineI(GpPath *p, INT x1, INT y1, INT x2, INT y2)
{ return GdipAddPathLine(p, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2); }

GDIPAPI GpStatus GDIPCALL GdipAddPathLine2(GpPath *p, const GpPointF *pts, INT n)
{
    if (!p || !pts || n < 1) return InvalidParameter;
    if (!reserve(p, n)) return OutOfMemory;
    start_at(p, pts[0].X, pts[0].Y);
    for (int i = 1; i < n; i++) add(p, pts[i].X, pts[i].Y, PathPointTypeLine);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathLine2I(GpPath *p, const GpPoint *pts, INT n)
{
    if (!p || !pts || n < 1) return InvalidParameter;
    if (!reserve(p, n)) return OutOfMemory;
    start_at(p, (REAL)pts[0].X, (REAL)pts[0].Y);
    for (int i = 1; i < n; i++) add(p, (REAL)pts[i].X, (REAL)pts[i].Y, PathPointTypeLine);
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipAddPathBezier(GpPath *p, REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4)
{
    if (!p) return InvalidParameter;
    if (!reserve(p, 4)) return OutOfMemory;
    start_at(p, x1, y1);
    add(p, x2, y2, PathPointTypeBezier);
    add(p, x3, y3, PathPointTypeBezier);
    add(p, x4, y4, PathPointTypeBezier);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathBezierI(GpPath *p, INT x1, INT y1, INT x2, INT y2, INT x3, INT y3, INT x4, INT y4)
{ return GdipAddPathBezier(p, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2, (REAL)x3, (REAL)y3, (REAL)x4, (REAL)y4); }
GDIPAPI GpStatus GDIPCALL GdipAddPathBeziers(GpPath *p, const GpPointF *pts, INT n)
{
    if (!p || !pts || n < 4 || (n - 1) % 3) return InvalidParameter;
    if (!reserve(p, n)) return OutOfMemory;
    start_at(p, pts[0].X, pts[0].Y);
    for (int i = 1; i < n; i++) add(p, pts[i].X, pts[i].Y, PathPointTypeBezier);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathBeziersI(GpPath *p, const GpPoint *pts, INT n)
{
    if (!p || !pts || n < 4 || (n - 1) % 3) return InvalidParameter;
    if (!reserve(p, n)) return OutOfMemory;
    start_at(p, (REAL)pts[0].X, (REAL)pts[0].Y);
    for (int i = 1; i < n; i++) add(p, (REAL)pts[i].X, (REAL)pts[i].Y, PathPointTypeBezier);
    return Ok;
}

/* an arc of the ellipse in the rectangle, from `start` sweeping `sweep`
 * degrees (clockwise on screen), as Béziers of at most 90 degrees */
static GpStatus arc(GpPath *p, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep)
{
    REAL rx = w / 2, ry = h / 2, cx = x + rx, cy = y + ry;
    if (sweep > 360) sweep = 360;
    if (sweep < -360) sweep = -360;
    int n = (int)ceilf(fabsf(sweep) / 90.f - 0.001f);
    if (n < 1) n = 1;
    if (!reserve(p, 1 + 3 * n)) return OutOfMemory;
    REAL step = sweep / n * 3.14159265f / 180.f, a = start * 3.14159265f / 180.f;
    REAL k = 4.f / 3.f * tanf(step / 4);
    start_at(p, cx + rx * cosf(a), cy + ry * sinf(a));
    for (int i = 0; i < n; i++) {
        REAL a1 = a + step, c0 = cosf(a), s0 = sinf(a), c1 = cosf(a1), s1 = sinf(a1);
        add(p, cx + rx * (c0 - k * s0), cy + ry * (s0 + k * c0), PathPointTypeBezier);
        add(p, cx + rx * (c1 + k * s1), cy + ry * (s1 - k * c1), PathPointTypeBezier);
        add(p, cx + rx * c1, cy + ry * s1, PathPointTypeBezier);
        a = a1;
    }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathArc(GpPath *p, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep)
{
    if (!p || w <= 0 || h <= 0) return InvalidParameter;
    return arc(p, x, y, w, h, start, sweep);
}
GDIPAPI GpStatus GDIPCALL GdipAddPathArcI(GpPath *p, INT x, INT y, INT w, INT h, REAL start, REAL sweep)
{ return GdipAddPathArc(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h, start, sweep); }

GDIPAPI GpStatus GDIPCALL GdipAddPathRectangle(GpPath *p, REAL x, REAL y, REAL w, REAL h)
{
    if (!p) return InvalidParameter;
    if (!reserve(p, 4)) return OutOfMemory;
    add(p, x, y, PathPointTypeStart);
    add(p, x + w, y, PathPointTypeLine);
    add(p, x + w, y + h, PathPointTypeLine);
    add(p, x, y + h, PathPointTypeLine | PathPointTypeCloseSubpath);
    p->new_figure = TRUE;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathRectangleI(GpPath *p, INT x, INT y, INT w, INT h)
{ return GdipAddPathRectangle(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h); }
GDIPAPI GpStatus GDIPCALL GdipAddPathRectangles(GpPath *p, const GpRectF *r, INT n)
{
    if (!p || !r || n < 0) return InvalidParameter;
    for (int i = 0; i < n; i++) {
        GpStatus st = GdipAddPathRectangle(p, r[i].X, r[i].Y, r[i].Width, r[i].Height);
        if (st != Ok) return st;
    }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathRectanglesI(GpPath *p, const GpRect *r, INT n)
{
    if (!p || !r || n < 0) return InvalidParameter;
    for (int i = 0; i < n; i++) {
        GpStatus st = GdipAddPathRectangleI(p, r[i].X, r[i].Y, r[i].Width, r[i].Height);
        if (st != Ok) return st;
    }
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipAddPathEllipse(GpPath *p, REAL x, REAL y, REAL w, REAL h)
{
    if (!p) return InvalidParameter;
    p->new_figure = TRUE;
    GpStatus st = arc(p, x, y, w, h, 0, 360);
    if (st == Ok) GdipClosePathFigure(p);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathEllipseI(GpPath *p, INT x, INT y, INT w, INT h)
{ return GdipAddPathEllipse(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h); }

GDIPAPI GpStatus GDIPCALL GdipAddPathPie(GpPath *p, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep)
{
    if (!p) return InvalidParameter;
    if (!reserve(p, 1)) return OutOfMemory;
    p->new_figure = TRUE;
    start_at(p, x + w / 2, y + h / 2);
    GpStatus st = arc(p, x, y, w, h, start, sweep);
    if (st == Ok) GdipClosePathFigure(p);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathPolygon(GpPath *p, const GpPointF *pts, INT n)
{
    if (!p || !pts || n < 3) return InvalidParameter;
    p->new_figure = TRUE;
    GpStatus st = GdipAddPathLine2(p, pts, n);
    if (st == Ok) GdipClosePathFigure(p);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathPolygonI(GpPath *p, const GpPoint *pts, INT n)
{
    if (!p || !pts || n < 3) return InvalidParameter;
    p->new_figure = TRUE;
    GpStatus st = GdipAddPathLine2I(p, pts, n);
    if (st == Ok) GdipClosePathFigure(p);
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipAddPathPath(GpPath *p, const GpPath *add_p, BOOL connect)
{
    if (!p || !add_p) return InvalidParameter;
    if (!add_p->n) return Ok;
    if (!reserve(p, add_p->n + 1)) return OutOfMemory;
    int i = 0;
    if (connect && !p->new_figure && p->n) { start_at(p, add_p->pts[0].X, add_p->pts[0].Y); i = 1; }
    for (; i < add_p->n; i++) add(p, add_p->pts[i].X, add_p->pts[i].Y, add_p->types[i]);
    p->new_figure = add_p->new_figure;
    return Ok;
}

/* ---- reading a path ------------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipGetPointCount(GpPath *p, INT *n) { if (!p || !n) return InvalidParameter; *n = p->n; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPathPoints(GpPath *p, GpPointF *out, INT n)
{
    if (!p || !out || n < p->n) return InvalidParameter;
    memcpy(out, p->pts, sizeof *out * p->n);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPathPointsI(GpPath *p, GpPoint *out, INT n)
{
    if (!p || !out || n < p->n) return InvalidParameter;
    for (int i = 0; i < p->n; i++) out[i] = (GpPoint){ (INT)floorf(p->pts[i].X + 0.5f), (INT)floorf(p->pts[i].Y + 0.5f) };
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPathTypes(GpPath *p, BYTE *out, INT n)
{
    if (!p || !out || n < p->n) return InvalidParameter;
    memcpy(out, p->types, p->n);
    return Ok;
}
/* the caller allocates Points and Types for Count entries */
GDIPAPI GpStatus GDIPCALL GdipGetPathData(GpPath *p, PathData *d)
{
    if (!p || !d || !d->Points || !d->Types || d->Count < p->n) return InvalidParameter;
    memcpy(d->Points, p->pts, sizeof *d->Points * p->n);
    memcpy(d->Types, p->types, p->n);
    d->Count = p->n;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPathLastPoint(GpPath *p, GpPointF *out)
{
    if (!p || !out || !p->n) return InvalidParameter;
    *out = p->pts[p->n - 1];
    return Ok;
}

GpRectF gdip_path_bounds(const GpPath *p)
{
    if (!p->n) return (GpRectF){ 0, 0, 0, 0 };
    REAL x0 = p->pts[0].X, y0 = p->pts[0].Y, x1 = x0, y1 = y0;
    for (int i = 1; i < p->n; i++) {
        if (p->pts[i].X < x0) x0 = p->pts[i].X;
        if (p->pts[i].X > x1) x1 = p->pts[i].X;
        if (p->pts[i].Y < y0) y0 = p->pts[i].Y;
        if (p->pts[i].Y > y1) y1 = p->pts[i].Y;
    }
    return (GpRectF){ x0, y0, x1 - x0, y1 - y0 };
}

GDIPAPI GpStatus GDIPCALL GdipGetPathWorldBounds(GpPath *p, GpRectF *r, const GpMatrix *m, const GpPen *pen)
{
    if (!p || !r) return InvalidParameter;
    GpRectF b = gdip_path_bounds(p);
    if (m) {
        plutovg_rect_t a = { b.X, b.Y, b.Width, b.Height }, d;
        plutovg_matrix_map_rect(m, &a, &d);
        b = (GpRectF){ d.x, d.y, d.w, d.h };
    }
    REAL g = pen ? pen->width / 2 : 0;
    *r = (GpRectF){ b.X - g, b.Y - g, b.Width + 2 * g, b.Height + 2 * g };
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetPathWorldBoundsI(GpPath *p, GpRect *r, const GpMatrix *m, const GpPen *pen)
{
    if (!r) return InvalidParameter;
    GpRectF f;
    GpStatus st = GdipGetPathWorldBounds(p, &f, m, pen);
    if (st == Ok) *r = (GpRect){ (INT)floorf(f.X), (INT)floorf(f.Y), (INT)ceilf(f.Width), (INT)ceilf(f.Height) };
    return st;
}

GDIPAPI GpStatus GDIPCALL GdipTransformPath(GpPath *p, GpMatrix *m)
{
    if (!p) return InvalidParameter;
    if (m) for (int i = 0; i < p->n; i++) plutovg_matrix_map(m, p->pts[i].X, p->pts[i].Y, &p->pts[i].X, &p->pts[i].Y);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipReversePath(GpPath *p)
{
    if (!p) return InvalidParameter;
    for (int i = 0, j = p->n - 1; i < j; i++, j--) {
        GpPointF t = p->pts[i]; p->pts[i] = p->pts[j]; p->pts[j] = t;
    }
    return Ok;
}
/* the outline of overlapping figures as one: drawing fills by winding already */
GDIPAPI GpStatus GDIPCALL GdipWindingModeOutline(GpPath *p, GpMatrix *m, REAL flatness)
{
    (void)flatness;
    if (!p) return InvalidParameter;
    GdipTransformPath(p, m);
    p->fill_mode = FillModeWinding;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipFlattenPath(GpPath *p, GpMatrix *m, REAL flatness) { (void)flatness; return GdipTransformPath(p, m); }

plutovg_path_t *gdip_path_build(const GpPath *p)
{
    plutovg_path_t *pp = plutovg_path_create();
    if (!pp) return NULL;
    for (int i = 0; i < p->n; i++) {
        BYTE t = p->types[i];
        switch (t & PathPointTypePathTypeMask) {
        case PathPointTypeStart: plutovg_path_move_to(pp, p->pts[i].X, p->pts[i].Y); break;
        case PathPointTypeBezier:
            if (i + 2 < p->n) {
                plutovg_path_cubic_to(pp, p->pts[i].X, p->pts[i].Y, p->pts[i + 1].X, p->pts[i + 1].Y, p->pts[i + 2].X, p->pts[i + 2].Y);
                i += 2;
                t = p->types[i];
            }
            break;
        default: plutovg_path_line_to(pp, p->pts[i].X, p->pts[i].Y); break;
        }
        if (t & PathPointTypeCloseSubpath) plutovg_path_close(pp);
    }
    return pp;
}

/* is a point inside (filled) or on (stroked with the pen) the path */
static GpStatus point_test(GpPath *p, REAL x, REAL y, const GpPen *pen, BOOL *out)
{
    if (!p || !out) return InvalidParameter;
    *out = FALSE;
    plutovg_surface_t *s = plutovg_surface_create(1, 1);
    plutovg_canvas_t *c = s ? plutovg_canvas_create(s) : NULL;
    plutovg_path_t *pp = gdip_path_build(p);
    if (c && pp) {
        plutovg_canvas_add_path(c, pp);
        plutovg_canvas_set_fill_rule(c, p->fill_mode == FillModeWinding ? PLUTOVG_FILL_RULE_NON_ZERO : PLUTOVG_FILL_RULE_EVEN_ODD);
        if (pen) {
            plutovg_canvas_set_line_width(c, pen->width > 0 ? pen->width : 1.f);
            *out = plutovg_canvas_stroke_contains(c, x, y);
        } else {
            *out = plutovg_canvas_fill_contains(c, x, y);
        }
    }
    if (pp) plutovg_path_destroy(pp);
    if (c) plutovg_canvas_destroy(c);
    if (s) plutovg_surface_destroy(s);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipIsVisiblePathPoint(GpPath *p, REAL x, REAL y, GpGraphics *g, BOOL *out) { (void)g; return point_test(p, x, y, NULL, out); }
GDIPAPI GpStatus GDIPCALL GdipIsVisiblePathPointI(GpPath *p, INT x, INT y, GpGraphics *g, BOOL *out) { (void)g; return point_test(p, (REAL)x, (REAL)y, NULL, out); }
GDIPAPI GpStatus GDIPCALL GdipIsOutlineVisiblePathPoint(GpPath *p, REAL x, REAL y, GpPen *pen, GpGraphics *g, BOOL *out)
{ (void)g; if (!pen) return InvalidParameter; return point_test(p, x, y, pen, out); }
GDIPAPI GpStatus GDIPCALL GdipIsOutlineVisiblePathPointI(GpPath *p, INT x, INT y, GpPen *pen, GpGraphics *g, BOOL *out)
{ (void)g; if (!pen) return InvalidParameter; return point_test(p, (REAL)x, (REAL)y, pen, out); }

/* ---- regions: copying and freeing ------------------------------------------ */

void gdip_region_reset(GpRegion *r)
{
    if (r->path) GdipDeletePath(r->path);
    r->path = NULL;
    r->kind = RgnInfinite;
}
GpStatus gdip_region_copy(GpRegion *dst, const GpRegion *src)
{
    if (dst == src) return Ok;
    GpPath *p = NULL;
    if (src->kind == RgnPath && GdipClonePath(src->path, &p) != Ok) return OutOfMemory;
    gdip_region_reset(dst);
    dst->kind = src->kind;
    dst->r = src->r;
    dst->path = p;
    return Ok;
}

/* ---- path iterators -------------------------------------------------------- */

typedef struct {
    GpPath *p;              /* a copy, as GDI+ takes */
    int sub;                /* the next subpath's first point */
    int marker;             /* the next marker section's first point */
} GpPathIterator;

GDIPAPI GpStatus GDIPCALL GdipCreatePathIter(GpPathIterator **out, GpPath *p)
{
    if (!out) return InvalidParameter;
    GpPathIterator *it = xalloc(sizeof *it);
    if (!it) return OutOfMemory;
    if (p && GdipClonePath(p, &it->p) != Ok) { xfree(it); return OutOfMemory; }
    if (!p) GdipCreatePath(0, &it->p);
    *out = it;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDeletePathIter(GpPathIterator *it)
{
    if (!it) return InvalidParameter;
    GdipDeletePath(it->p);
    xfree(it);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipPathIterRewind(GpPathIterator *it) { if (!it) return InvalidParameter; it->sub = it->marker = 0; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipPathIterGetCount(GpPathIterator *it, INT *n) { if (!it || !n) return InvalidParameter; *n = it->p->n; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipPathIterGetSubpathCount(GpPathIterator *it, INT *n)
{
    if (!it || !n) return InvalidParameter;
    *n = 0;
    for (int i = 0; i < it->p->n; i++) if ((it->p->types[i] & PathPointTypePathTypeMask) == PathPointTypeStart) (*n)++;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipPathIterHasCurve(GpPathIterator *it, BOOL *r)
{
    if (!it || !r) return InvalidParameter;
    *r = FALSE;
    for (int i = 0; i < it->p->n; i++) if ((it->p->types[i] & PathPointTypePathTypeMask) == PathPointTypeBezier) *r = TRUE;
    return Ok;
}

/* the next section up to a marker (or the end): its points as a path */
static GpStatus copy_range(GpPath *src, int from, int to, GpPath *dst)
{
    GdipResetPath(dst);
    if (to <= from) return Ok;
    if (!reserve(dst, to - from)) return OutOfMemory;
    for (int i = from; i < to; i++) add(dst, src->pts[i].X, src->pts[i].Y, src->types[i]);
    dst->types[0] = (dst->types[0] & ~PathPointTypePathTypeMask) | PathPointTypeStart;
    dst->fill_mode = src->fill_mode;
    dst->new_figure = TRUE;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipPathIterNextMarkerPath(GpPathIterator *it, INT *count, GpPath *out)
{
    if (!it || !count) return InvalidParameter;
    int n = it->p->n, from = it->marker, to = from;
    while (to < n && !(it->p->types[to] & PathPointTypePathMarker)) to++;
    if (to < n) to++;                                    /* the marked point ends the section */
    *count = to - from;
    it->marker = to;
    return out ? copy_range(it->p, from, to, out) : Ok;
}
GDIPAPI GpStatus GDIPCALL GdipPathIterNextMarker(GpPathIterator *it, INT *count, INT *start, INT *end)
{
    if (!it || !count || !start || !end) return InvalidParameter;
    int n = it->p->n, from = it->marker, to = from;
    while (to < n && !(it->p->types[to] & PathPointTypePathMarker)) to++;
    if (to < n) to++;
    *count = to - from;
    *start = from;
    *end = to - 1;
    it->marker = to;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipPathIterNextSubpathPath(GpPathIterator *it, INT *count, GpPath *out, BOOL *closed)
{
    if (!it || !count || !closed) return InvalidParameter;
    int n = it->p->n, from = it->sub, to = from + 1;
    if (from >= n) { *count = 0; *closed = FALSE; if (out) GdipResetPath(out); return Ok; }
    while (to < n && (it->p->types[to] & PathPointTypePathTypeMask) != PathPointTypeStart) to++;
    *count = to - from;
    *closed = (it->p->types[to - 1] & PathPointTypeCloseSubpath) != 0;
    it->sub = to;
    return out ? copy_range(it->p, from, to, out) : Ok;
}
GDIPAPI GpStatus GDIPCALL GdipPathIterNextSubpath(GpPathIterator *it, INT *count, INT *start, INT *end, BOOL *closed)
{
    if (!it || !count || !start || !end || !closed) return InvalidParameter;
    int n = it->p->n, from = it->sub, to = from + 1;
    if (from >= n) { *count = 0; *start = *end = 0; *closed = FALSE; return Ok; }
    while (to < n && (it->p->types[to] & PathPointTypePathTypeMask) != PathPointTypeStart) to++;
    *count = to - from;
    *start = from;
    *end = to - 1;
    *closed = (it->p->types[to - 1] & PathPointTypeCloseSubpath) != 0;
    it->sub = to;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipPathIterCopyData(GpPathIterator *it, INT *count, GpPointF *pts, BYTE *types, INT start, INT end)
{
    if (!it || !count || !pts || !types) return InvalidParameter;
    if (start < 0 || end >= it->p->n || end < start) { *count = 0; return Ok; }
    *count = end - start + 1;
    memcpy(pts, it->p->pts + start, sizeof *pts * *count);
    memcpy(types, it->p->types + start, *count);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipPathIterEnumerate(GpPathIterator *it, INT *count, GpPointF *pts, BYTE *types, INT n)
{
    if (!it || !count) return InvalidParameter;
    int k = it->p->n < n ? it->p->n : n;
    return GdipPathIterCopyData(it, count, pts, types, 0, k - 1);
}
