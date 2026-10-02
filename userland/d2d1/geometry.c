/*
 * geometry.c — ID2D1Geometry and its kinds (rectangle, rounded rectangle,
 * ellipse, path, transformed, group), and geometry sinks
 */
#include "d2d_int.h"
#include <windows.h>

extern const void *const g_vtbl_rect[], *const g_vtbl_rrect[], *const g_vtbl_ellipse[], *const g_vtbl_path[],
    *const g_vtbl_transformed[], *const g_vtbl_group[];

Geometry *geometry_of(void *iface)
{
    if (!iface) return NULL;
    const void *v = *(const void **)iface;
    if (v == g_vtbl_rect || v == g_vtbl_rrect || v == g_vtbl_ellipse || v == g_vtbl_path ||
        v == g_vtbl_transformed || v == g_vtbl_group)
        return iface;
    return NULL;
}

const Path *geometry_path(Geometry *g) { return &g->path; }

static const GUID *kind_iid(int kind)
{
    switch (kind) {
    case G_RECT: return &IID_ID2D1RectangleGeometry;
    case G_RRECT: return &IID_ID2D1RoundedRectangleGeometry;
    case G_ELLIPSE: return &IID_ID2D1EllipseGeometry;
    case G_PATH: return &IID_ID2D1PathGeometry;
    case G_TRANSFORMED: return &IID_ID2D1TransformedGeometry;
    default: return &IID_ID2D1GeometryGroup;
    }
}

static HRESULT STDMETHODCALLTYPE g_qi(Geometry *g, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Resource) ||
        IsEqualIID(riid, &IID_ID2D1Geometry) || IsEqualIID(riid, kind_iid(g->kind))) {
        *out = g;
        InterlockedIncrement(&g->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE g_addref(Geometry *g) { return InterlockedIncrement(&g->ref); }
static ULONG STDMETHODCALLTYPE g_release(Geometry *g)
{
    LONG r = InterlockedDecrement(&g->ref);
    if (!r) {
        path_free(&g->path);
        if (g->source) COM_RELEASE(g->source);
        for (UINT32 i = 0; i < g->nchildren; i++) COM_RELEASE(g->children[i]);
        d_free(g->children);
        factory_release(g->factory);
        d_free(g);
    }
    return r;
}
static void STDMETHODCALLTYPE g_factory(Geometry *g, void **f) { *f = g->factory; factory_addref(g->factory); }

static const MAT *or_identity(const MAT *m) { return m ? m : &MAT_IDENTITY; }
static float tol_of(float t) { return t > 0 && isfinite(t) ? t : 0.25f; }

static HRESULT STDMETHODCALLTYPE g_bounds(Geometry *g, const MAT *m, RCF *r)
{
    if (!r) return E_POINTER;
    path_bounds(&g->path, or_identity(m), r);
    return S_OK;
}

/* @g's stroke outline in its own space */
static void widen(Geometry *g, float w, void *style, const MAT *m, float tol, Path *out)
{
    float s = mat_scale(or_identity(m));
    stroke_to_path(&g->path, w, stroke_style_of(style), tol_of(tol) / (s > 0 ? s : 1), out);
}

static HRESULT STDMETHODCALLTYPE g_widened_bounds(Geometry *g, float w, void *style, const MAT *m, float tol, RCF *r)
{
    if (!r) return E_POINTER;
    Path p;
    path_init(&p);
    widen(g, w, style, m, tol, &p);
    path_bounds(&p, or_identity(m), r);
    path_free(&p);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE g_stroke_contains(Geometry *g, PT pt, float w, void *style, const MAT *m, float tol,
                                                   BOOL *out)
{
    if (!out) return E_POINTER;
    Path p;
    path_init(&p);
    widen(g, w, style, m, tol, &p);
    Poly o = { 0 };
    path_flatten(&p, or_identity(m), tol_of(tol), &o);
    for (int i = 0; i < o.nfig; i++) o.filled[i] = 1;
    *out = poly_contains(&o, FILL_WINDING, pt);
    poly_free(&o);
    path_free(&p);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE g_fill_contains(Geometry *g, PT pt, const MAT *m, float tol, BOOL *out)
{
    if (!out) return E_POINTER;
    Poly o = { 0 };
    path_flatten(&g->path, or_identity(m), tol_of(tol), &o);
    *out = poly_contains(&o, g->path.fill_mode, pt);
    poly_free(&o);
    return S_OK;
}

static void add_area(void *ctx, float ya, float yb, float la, float lb, float ra, float rb)
{
    *(double *)ctx += (double)(yb - ya) * ((ra - la) + (rb - lb)) / 2;
}

static double poly_area(const Poly *a, int fa, const Poly *b, int fb, int op)
{
    double area = 0;
    slabs(a, fa, b, fb, op, add_area, &area);
    return area;
}

static HRESULT STDMETHODCALLTYPE g_compare(Geometry *g, void *input, const MAT *m, float tol, UINT32 *rel)
{
    Geometry *in = geometry_of(input);
    if (!rel) return E_POINTER;
    if (!in) return E_INVALIDARG;
    Poly a = { 0 }, b = { 0 };
    path_flatten(&g->path, &MAT_IDENTITY, tol_of(tol), &a);
    path_flatten(&in->path, or_identity(m), tol_of(tol), &b);
    double aa = poly_area(&a, g->path.fill_mode, NULL, 0, OP_ONLY_A);
    double ab = poly_area(&b, in->path.fill_mode, NULL, 0, OP_ONLY_A);
    double ai = poly_area(&a, g->path.fill_mode, &b, in->path.fill_mode, OP_INTERSECT);
    double eps = 1e-4 * (aa > ab ? aa : ab) + 1e-9;
    if (ai <= eps) *rel = 1;                    /* disjoint */
    else if (aa - ai <= eps) *rel = 2;          /* this is contained in the input */
    else if (ab - ai <= eps) *rel = 3;          /* this contains the input */
    else *rel = 4;                              /* overlap */
    poly_free(&a);
    poly_free(&b);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE g_simplify(Geometry *g, UINT32 option, const MAT *m, float tol, void *sink)
{
    if (!sink) return E_POINTER;
    path_emit(&g->path, or_identity(m), option == 1, tol_of(tol), sink);
    return S_OK;
}

typedef struct { void *sink; } TessCtx;
typedef void (STDMETHODCALLTYPE *AddTriangles)(void *, const TRIANGLE *, UINT32);
static void tess_trap(void *ctx, float ya, float yb, float la, float lb, float ra, float rb)
{
    void *sink = ((TessCtx *)ctx)->sink;
    TRIANGLE t[2];
    int n = 0;
    PT tl = { la, ya }, tr = { ra, ya }, br = { rb, yb }, bl = { lb, yb };
    if (ra > la) { t[n].point1 = tl; t[n].point2 = tr; t[n].point3 = br; n++; }
    if (rb > lb) { t[n].point1 = tl; t[n].point2 = br; t[n].point3 = bl; n++; }
    if (n) VSLOT(sink, 3, AddTriangles)(sink, t, n);
}

static HRESULT STDMETHODCALLTYPE g_tessellate(Geometry *g, const MAT *m, float tol, void *sink)
{
    if (!sink) return E_POINTER;
    Poly a = { 0 };
    path_flatten(&g->path, or_identity(m), tol_of(tol), &a);
    TessCtx c = { sink };
    slabs(&a, g->path.fill_mode, NULL, 0, OP_ONLY_A, tess_trap, &c);
    poly_free(&a);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE g_combine(Geometry *g, void *input, UINT32 mode, const MAT *m, float tol, void *sink)
{
    Geometry *in = geometry_of(input);
    if (!sink) return E_POINTER;
    if (!in || mode > 3) return E_INVALIDARG;
    Poly a = { 0 }, b = { 0 };
    path_flatten(&g->path, &MAT_IDENTITY, tol_of(tol), &a);
    path_flatten(&in->path, or_identity(m), tol_of(tol), &b);
    Path out;
    path_init(&out);
    slabs_outline(&a, g->path.fill_mode, &b, in->path.fill_mode, (int)mode, &out);
    path_emit(&out, &MAT_IDENTITY, 0, 0, sink);
    path_free(&out);
    poly_free(&a);
    poly_free(&b);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE g_outline(Geometry *g, const MAT *m, float tol, void *sink)
{
    if (!sink) return E_POINTER;
    Poly a = { 0 };
    path_flatten(&g->path, or_identity(m), tol_of(tol), &a);
    Path out;
    path_init(&out);
    slabs_outline(&a, g->path.fill_mode, NULL, 0, OP_ONLY_A, &out);
    path_emit(&out, &MAT_IDENTITY, 0, 0, sink);
    path_free(&out);
    poly_free(&a);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE g_area(Geometry *g, const MAT *m, float tol, float *area)
{
    if (!area) return E_POINTER;
    Poly a = { 0 };
    path_flatten(&g->path, or_identity(m), tol_of(tol), &a);
    *area = (float)poly_area(&a, g->path.fill_mode, NULL, 0, OP_ONLY_A);
    poly_free(&a);
    return S_OK;
}

/* walks the flattened outline, closed figures back to their start */
static float walk(const Poly *o, float stop, PT *at, PT *tangent)
{
    float total = 0;
    int start = 0;
    PT last = { 0, 0 }, dir = { 1, 0 };
    BOOL any = FALSE;
    for (int f = 0; f < o->nfig; f++) {
        int end = o->fig_end[f];
        int nseg = end - start - 1 + (o->closed[f] && end - start > 1);
        for (int i = 0; i < nseg; i++) {
            PT a = o->pt[start + i], b = o->pt[start + (i + 1) % (end - start)];
            float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
            if (len <= 0) continue;
            if (at && total + len >= stop) {
                float t = (stop - total) / len;
                if (t < 0) t = 0;
                at->x = a.x + dx * t;
                at->y = a.y + dy * t;
                tangent->x = dx / len;
                tangent->y = dy / len;
                return stop;
            }
            total += len;
            last = b;
            dir.x = dx / len;
            dir.y = dy / len;
            any = TRUE;
        }
        if (!any && end > start) last = o->pt[start];
        start = end;
    }
    if (at) { *at = last; *tangent = dir; }
    return total;
}

static HRESULT STDMETHODCALLTYPE g_length(Geometry *g, const MAT *m, float tol, float *len)
{
    if (!len) return E_POINTER;
    Poly a = { 0 };
    path_flatten(&g->path, or_identity(m), tol_of(tol), &a);
    *len = walk(&a, 0, NULL, NULL);
    poly_free(&a);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE g_point_at(Geometry *g, float length, const MAT *m, float tol, PT *pt, PT *tangent)
{
    Poly a = { 0 };
    PT p, t;
    path_flatten(&g->path, or_identity(m), tol_of(tol), &a);
    walk(&a, length < 0 ? 0 : length, &p, &t);
    poly_free(&a);
    if (pt) *pt = p;
    if (tangent) *tangent = t;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE g_widen(Geometry *g, float w, void *style, const MAT *m, float tol, void *sink)
{
    if (!sink) return E_POINTER;
    Path p;
    path_init(&p);
    widen(g, w, style, m, tol, &p);
    p.fill_mode = FILL_WINDING;
    path_emit(&p, or_identity(m), 0, tol_of(tol), sink);
    path_free(&p);
    return S_OK;
}

#define GEOMETRY_METHODS \
    g_qi, g_addref, g_release, g_factory, g_bounds, g_widened_bounds, g_stroke_contains, g_fill_contains, \
    g_compare, g_simplify, g_tessellate, g_combine, g_outline, g_area, g_length, g_point_at, g_widen

/* ---- the kinds ---- */
static void STDMETHODCALLTYPE g_get_rect(Geometry *g, RCF *r) { *r = g->rect; }
static void STDMETHODCALLTYPE g_get_rrect(Geometry *g, RRECT *r) { *r = g->rrect; }
static void STDMETHODCALLTYPE g_get_ellipse(Geometry *g, ELLIPSE_ *e) { *e = g->ell; }
static void STDMETHODCALLTYPE g_get_source(Geometry *g, void **src)
{
    *src = g->source;
    COM_ADDREF(g->source);
}
static void STDMETHODCALLTYPE g_get_transform(Geometry *g, MAT *m) { *m = g->transform; }
static UINT32 STDMETHODCALLTYPE g_group_fill(Geometry *g) { return g->path.fill_mode; }
static UINT32 STDMETHODCALLTYPE g_group_count(Geometry *g) { return g->nchildren; }
static void STDMETHODCALLTYPE g_group_get(Geometry *g, void **out, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++) {
        out[i] = i < g->nchildren ? g->children[i] : NULL;
        if (out[i]) COM_ADDREF(out[i]);
    }
}

/* path geometries */
typedef struct {
    const void *vtbl;
    LONG ref;
    Path *path;
    Geometry *owner;
    int in_figure;
    int closed;
} Sink;

extern const void *const sink_vtbl[];

static HRESULT STDMETHODCALLTYPE p_open(Geometry *g, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    if (g->sealed || g->kind != G_PATH || g->nsegments < 0) return D2DERR_WRONG_STATE;
    Sink *s = geometry_sink_new(&g->path);
    if (!s) return E_OUTOFMEMORY;
    s->owner = g;
    g_addref(g);
    g->nsegments = -1;      /* opened */
    *out = s;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE p_stream(Geometry *g, void *sink)
{
    if (!sink) return E_POINTER;
    if (!g->sealed) return D2DERR_WRONG_STATE;
    path_emit(&g->path, &MAT_IDENTITY, 0, 0, sink);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE p_segments(Geometry *g, UINT32 *n)
{
    if (!n) return E_POINTER;
    if (!g->sealed) return D2DERR_WRONG_STATE;
    UINT32 c = 0;
    for (int i = 0; i < g->path.n; i++) c += g->path.f[i].n;
    *n = c;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE p_figures(Geometry *g, UINT32 *n)
{
    if (!n) return E_POINTER;
    if (!g->sealed) return D2DERR_WRONG_STATE;
    *n = g->path.n;
    return S_OK;
}

const void *const g_vtbl_rect[] = { GEOMETRY_METHODS, g_get_rect };
const void *const g_vtbl_rrect[] = { GEOMETRY_METHODS, g_get_rrect };
const void *const g_vtbl_ellipse[] = { GEOMETRY_METHODS, g_get_ellipse };
const void *const g_vtbl_path[] = { GEOMETRY_METHODS, p_open, p_stream, p_segments, p_figures };
const void *const g_vtbl_transformed[] = { GEOMETRY_METHODS, g_get_source, g_get_transform };
const void *const g_vtbl_group[] = { GEOMETRY_METHODS, g_group_fill, g_group_count, g_group_get };

HRESULT geometry_create(void *factory, int kind, Geometry **out)
{
    static const void *const *vt[] = { g_vtbl_rect, g_vtbl_rrect, g_vtbl_ellipse, g_vtbl_path, g_vtbl_transformed,
                                       g_vtbl_group };
    Geometry *g = d_alloc(sizeof(*g));
    if (!g) return E_OUTOFMEMORY;
    g->vtbl = vt[kind];
    g->ref = 1;
    g->kind = kind;
    g->factory = factory;
    factory_addref(factory);
    path_init(&g->path);
    g->transform = MAT_IDENTITY;
    if (kind != G_PATH) g->sealed = 1;
    *out = g;
    return S_OK;
}

/* ---- ID2D1GeometrySink ---- */
static HRESULT STDMETHODCALLTYPE s_qi(Sink *s, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1SimplifiedGeometrySink) ||
        IsEqualIID(riid, &IID_ID2D1GeometrySink)) {
        *out = s;
        InterlockedIncrement(&s->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE s_addref(Sink *s) { return InterlockedIncrement(&s->ref); }
static ULONG STDMETHODCALLTYPE s_release(Sink *s)
{
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        if (s->owner) g_release(s->owner);
        d_free(s);
    }
    return r;
}
static void STDMETHODCALLTYPE s_fill_mode(Sink *s, UINT32 mode) { s->path->fill_mode = mode ? FILL_WINDING : FILL_ALTERNATE; }
static void STDMETHODCALLTYPE s_flags(Sink *s, UINT32 flags) { (void)s; (void)flags; }
static void STDMETHODCALLTYPE s_begin(Sink *s, PT p, UINT32 begin)
{
    if (s->closed) return;
    if (s->in_figure) path_end(s->path, 0);
    path_begin(s->path, p, begin == 0);
    s->in_figure = 1;
}
static void STDMETHODCALLTYPE s_lines(Sink *s, const PT *p, UINT32 n)
{
    if (!s->in_figure) return;
    for (UINT32 i = 0; i < n; i++) path_line(s->path, p[i]);
}
static void STDMETHODCALLTYPE s_beziers(Sink *s, const BEZIER *b, UINT32 n)
{
    if (!s->in_figure) return;
    for (UINT32 i = 0; i < n; i++) path_cubic(s->path, b[i].point1, b[i].point2, b[i].point3);
}
static void STDMETHODCALLTYPE s_end(Sink *s, UINT32 end)
{
    if (!s->in_figure) return;
    path_end(s->path, end == 1);
    s->in_figure = 0;
}
static HRESULT STDMETHODCALLTYPE s_close(Sink *s)
{
    if (s->closed) return D2DERR_WRONG_STATE;
    if (s->in_figure) { path_end(s->path, 0); s->in_figure = 0; }
    s->closed = 1;
    if (s->owner) {
        s->owner->sealed = 1;
        s->owner->nsegments = 0;
    }
    return S_OK;
}
static void STDMETHODCALLTYPE s_line(Sink *s, PT p) { s_lines(s, &p, 1); }
static void STDMETHODCALLTYPE s_bezier(Sink *s, const BEZIER *b) { s_beziers(s, b, 1); }
static void STDMETHODCALLTYPE s_quads(Sink *s, const QBEZIER *q, UINT32 n)
{
    if (!s->in_figure) return;
    for (UINT32 i = 0; i < n; i++) path_quad(s->path, q[i].point1, q[i].point2);
}
static void STDMETHODCALLTYPE s_quad(Sink *s, const QBEZIER *q) { s_quads(s, q, 1); }
static void STDMETHODCALLTYPE s_arc(Sink *s, const ARCSEG *a)
{
    if (s->in_figure) path_arc(s->path, a);
}

const void *const sink_vtbl[] = { s_qi, s_addref, s_release, s_fill_mode, s_flags, s_begin, s_lines, s_beziers,
                                  s_end, s_close, s_line, s_bezier, s_quad, s_quads, s_arc };

void *geometry_sink_new(Path *path)
{
    Sink *s = d_alloc(sizeof(*s));
    if (!s) return NULL;
    s->vtbl = sink_vtbl;
    s->ref = 1;
    s->path = path;
    return s;
}
