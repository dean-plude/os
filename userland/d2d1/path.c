/*
 * path.c — path data: figures of lines and cubic Béziers, the shapes and
 * arcs that become them, flattening, exact bounds, hit testing, and the
 * slab decomposition behind Tessellate, ComputeArea, CompareWithGeometry,
 * CombineWithGeometry and Outline
 */
#include "d2d_int.h"
#include <windows.h>
#include <stdlib.h>

#define PI_F 3.14159265358979f

void path_init(Path *p) { memset(p, 0, sizeof(*p)); }

void path_free(Path *p)
{
    for (int i = 0; i < p->n; i++) {
        d_free(p->f[i].kind);
        d_free(p->f[i].pts);
    }
    d_free(p->f);
    memset(p, 0, sizeof(*p));
}

static Figure *cur_fig(Path *p) { return p->n ? &p->f[p->n - 1] : NULL; }

static PT cur_point(Path *p)
{
    Figure *f = cur_fig(p);
    if (!f) { PT z = { 0, 0 }; return z; }
    return f->n ? f->pts[f->n * 3 - 1] : f->start;
}

Figure *path_begin(Path *p, PT start, int filled)
{
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 4;
        Figure *nf = d_realloc(p->f, sizeof(Figure) * nc);
        if (!nf) return NULL;
        p->f = nf;
        p->cap = nc;
    }
    Figure *f = &p->f[p->n++];
    memset(f, 0, sizeof(*f));
    f->start = start;
    f->filled = filled;
    return f;
}

static PT *add_seg(Path *p, int kind)
{
    Figure *f = cur_fig(p);
    if (!f) return NULL;
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 8;
        unsigned char *k = d_realloc(f->kind, nc);
        if (!k) return NULL;
        f->kind = k;
        PT *pp = d_realloc(f->pts, sizeof(PT) * 3 * nc);
        if (!pp) return NULL;
        f->pts = pp;
        f->cap = nc;
    }
    f->kind[f->n] = (unsigned char)kind;
    return &f->pts[3 * f->n++];
}

void path_line(Path *p, PT to)
{
    PT *s = add_seg(p, SEG_LINE);
    if (s) s[0] = s[1] = s[2] = to;
}

void path_cubic(Path *p, PT c1, PT c2, PT to)
{
    PT *s = add_seg(p, SEG_CUBIC);
    if (s) { s[0] = c1; s[1] = c2; s[2] = to; }
}

void path_quad(Path *p, PT c, PT to)
{
    PT f = cur_point(p);
    PT c1 = { f.x + (c.x - f.x) * (2.0f / 3), f.y + (c.y - f.y) * (2.0f / 3) };
    PT c2 = { to.x + (c.x - to.x) * (2.0f / 3), to.y + (c.y - to.y) * (2.0f / 3) };
    path_cubic(p, c1, c2, to);
}

void path_end(Path *p, int closed)
{
    Figure *f = cur_fig(p);
    if (f) f->closed = closed;
}

/* cubics for the elliptical arc of centre (cx, cy), radii (rx, ry) rotated by
   (cs, sn), from angle t0 through dt */
static void arc_cubics(Path *p, float cx, float cy, float rx, float ry, float cs, float sn, float t0, float dt)
{
    int n = (int)ceilf(fabsf(dt) / (PI_F / 2) - 1e-4f);
    if (n < 1) n = 1;
    float d = dt / n, k = 4.0f / 3 * tanf(d / 4);
    for (int i = 0; i < n; i++) {
        float a = t0 + d * i, b = a + d;
        float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        PT pa = { cx + rx * ca * cs - ry * sa * sn, cy + rx * ca * sn + ry * sa * cs };
        PT pb = { cx + rx * cb * cs - ry * sb * sn, cy + rx * cb * sn + ry * sb * cs };
        PT da = { -rx * sa * cs - ry * ca * sn, -rx * sa * sn + ry * ca * cs };
        PT db = { -rx * sb * cs - ry * cb * sn, -rx * sb * sn + ry * cb * cs };
        PT c1 = { pa.x + k * da.x, pa.y + k * da.y }, c2 = { pb.x - k * db.x, pb.y - k * db.y };
        path_cubic(p, c1, c2, pb);
    }
}

static float vec_angle(float ux, float uy, float vx, float vy)
{
    return atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
}

void path_arc(Path *p, const ARCSEG *a)
{
    PT p0 = cur_point(p), p1 = a->point;
    float rx = fabsf(a->size.width), ry = fabsf(a->size.height);
    if (p0.x == p1.x && p0.y == p1.y) return;
    if (rx == 0 || ry == 0) { path_line(p, p1); return; }
    float phi = a->rotationAngle * PI_F / 180, cs = cosf(phi), sn = sinf(phi);
    float dx2 = (p0.x - p1.x) / 2, dy2 = (p0.y - p1.y) / 2;
    float x1 = cs * dx2 + sn * dy2, y1 = -sn * dx2 + cs * dy2;
    float lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lam > 1) { float s = sqrtf(lam); rx *= s; ry *= s; }
    float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float coef = den > 0 && num > 0 ? sqrtf(num / den) : 0;
    int sweep = a->sweepDirection == 1, large = a->arcSize == 1;
    if (large == sweep) coef = -coef;
    float cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
    float cx = cs * cxp - sn * cyp + (p0.x + p1.x) / 2, cy = sn * cxp + cs * cyp + (p0.y + p1.y) / 2;
    float ux = (x1 - cxp) / rx, uy = (y1 - cyp) / ry, vx = (-x1 - cxp) / rx, vy = (-y1 - cyp) / ry;
    float t0 = vec_angle(1, 0, ux, uy), dt = vec_angle(ux, uy, vx, vy);
    if (!sweep && dt > 0) dt -= 2 * PI_F;
    if (sweep && dt < 0) dt += 2 * PI_F;
    int before = cur_fig(p) ? cur_fig(p)->n : 0;
    arc_cubics(p, cx, cy, rx, ry, cs, sn, t0, dt);
    /* land exactly on the end point */
    Figure *f = cur_fig(p);
    if (f && f->n > before) f->pts[f->n * 3 - 1] = p1;
}

void path_rect(Path *p, const RCF *r)
{
    PT a = { r->left, r->top }, b = { r->right, r->top }, c = { r->right, r->bottom }, d = { r->left, r->bottom };
    path_begin(p, a, 1);
    path_line(p, b);
    path_line(p, c);
    path_line(p, d);
    path_end(p, 1);
}

void path_rrect(Path *p, const RRECT *rr)
{
    const RCF *r = &rr->rect;
    float l = fminf(r->left, r->right), t = fminf(r->top, r->bottom);
    float R = fmaxf(r->left, r->right), B = fmaxf(r->top, r->bottom);
    float rx = fminf(fabsf(rr->radiusX), (R - l) / 2), ry = fminf(fabsf(rr->radiusY), (B - t) / 2);
    if (rx <= 0 || ry <= 0) {
        RCF q = { l, t, R, B };
        path_rect(p, &q);
        return;
    }
    float k = 0.5522847f;
    PT s = { l + rx, t };
    path_begin(p, s, 1);
#define P(x, y) ((PT){ (x), (y) })
    path_line(p, P(R - rx, t));
    path_cubic(p, P(R - rx + rx * k, t), P(R, t + ry - ry * k), P(R, t + ry));
    path_line(p, P(R, B - ry));
    path_cubic(p, P(R, B - ry + ry * k), P(R - rx + rx * k, B), P(R - rx, B));
    path_line(p, P(l + rx, B));
    path_cubic(p, P(l + rx - rx * k, B), P(l, B - ry + ry * k), P(l, B - ry));
    path_line(p, P(l, t + ry));
    path_cubic(p, P(l, t + ry - ry * k), P(l + rx - rx * k, t), P(l + rx, t));
#undef P
    path_end(p, 1);
}

void path_ellipse(Path *p, const ELLIPSE_ *e)
{
    float rx = fabsf(e->radiusX), ry = fabsf(e->radiusY);
    PT s = { e->point.x + rx, e->point.y };
    path_begin(p, s, 1);
    arc_cubics(p, e->point.x, e->point.y, rx, ry, 1, 0, 0, 2 * PI_F);
    Figure *f = cur_fig(p);
    if (f && f->n) f->pts[f->n * 3 - 1] = s;
    path_end(p, 1);
}

void path_copy(Path *dst, const Path *src, const MAT *m)
{
    for (int i = 0; i < src->n; i++) {
        const Figure *f = &src->f[i];
        if (!path_begin(dst, mat_apply(m, f->start), f->filled)) return;
        for (int s = 0; s < f->n; s++) {
            const PT *q = &f->pts[3 * s];
            if (f->kind[s] == SEG_LINE) path_line(dst, mat_apply(m, q[2]));
            else path_cubic(dst, mat_apply(m, q[0]), mat_apply(m, q[1]), mat_apply(m, q[2]));
        }
        path_end(dst, f->closed);
    }
}

float mat_scale(const MAT *m)
{
    float a = m->m11 * m->m11 + m->m12 * m->m12, b = m->m11 * m->m21 + m->m12 * m->m22;
    float d = m->m21 * m->m21 + m->m22 * m->m22;
    float t = (a + d) / 2, q = sqrtf((a - d) * (a - d) / 4 + b * b);
    return sqrtf(t + q);
}

/* ---- flattening ---- */
static void poly_push(Poly *o, PT p)
{
    if (o->n == o->cap) {
        int nc = o->cap ? o->cap * 2 : 64;
        PT *np = d_realloc(o->pt, sizeof(PT) * nc);
        if (!np) return;
        o->pt = np;
        o->cap = nc;
    }
    o->pt[o->n++] = p;
}

/* append @p unless it repeats the current figure's last point */
static void poly_add(Poly *o, PT p)
{
    int start = o->nfig ? o->fig_end[o->nfig - 1] : 0;
    if (o->n > start && o->pt[o->n - 1].x == p.x && o->pt[o->n - 1].y == p.y) return;
    poly_push(o, p);
}

static void poly_fig(Poly *o, int closed, int filled)
{
    if (o->nfig == o->figcap) {
        int nc = o->figcap ? o->figcap * 2 : 8;
        o->fig_end = d_realloc(o->fig_end, sizeof(int) * nc);
        o->closed = d_realloc(o->closed, nc);
        o->filled = d_realloc(o->filled, nc);
        o->figcap = nc;
    }
    o->fig_end[o->nfig] = o->n;
    o->closed[o->nfig] = (unsigned char)closed;
    o->filled[o->nfig] = (unsigned char)filled;
    o->nfig++;
}

static void flatten_cubic(Poly *o, PT p0, PT p1, PT p2, PT p3, float tol)
{
    float ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
    float bx = p1.x - 2 * p2.x + p3.x, by = p1.y - 2 * p2.y + p3.y;
    float m = sqrtf(fmaxf(ax * ax + ay * ay, bx * bx + by * by)) * 6;
    int n = (int)ceilf(sqrtf(m / (8 * tol)));
    if (!(n >= 1)) n = 1;
    if (n > 1000) n = 1000;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        PT q = { a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y };
        if (i == n) q = p3;
        poly_add(o, q);
    }
}

void path_flatten(const Path *p, const MAT *m, float tol, Poly *out)
{
    if (!(tol > 0)) tol = 0.25f;
    for (int i = 0; i < p->n; i++) {
        const Figure *f = &p->f[i];
        PT cur = mat_apply(m, f->start);
        poly_push(out, cur);
        for (int s = 0; s < f->n; s++) {
            const PT *q = &f->pts[3 * s];
            PT e = mat_apply(m, q[2]);
            if (f->kind[s] == SEG_LINE) {
                if (e.x != cur.x || e.y != cur.y) poly_add(out, e);
            } else flatten_cubic(out, cur, mat_apply(m, q[0]), mat_apply(m, q[1]), e, tol);
            cur = e;
        }
        poly_fig(out, f->closed, f->filled);
    }
}

void poly_free(Poly *p)
{
    d_free(p->pt);
    d_free(p->fig_end);
    d_free(p->closed);
    d_free(p->filled);
    memset(p, 0, sizeof(*p));
}

/* ---- exact bounds ---- */
static void grow(RCF *r, PT p)
{
    if (p.x < r->left) r->left = p.x;
    if (p.x > r->right) r->right = p.x;
    if (p.y < r->top) r->top = p.y;
    if (p.y > r->bottom) r->bottom = p.y;
}

static float cub(float a, float b, float c, float d, float t)
{
    float u = 1 - t;
    return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
}

/* the parameters in (0, 1) where one coordinate of a cubic peaks */
static int extrema(float a, float b, float c, float d, float *t)
{
    float qa = -a + 3 * b - 3 * c + d, qb = 2 * (a - 2 * b + c), qc = b - a;
    int n = 0;
    if (fabsf(qa) < 1e-12f) {
        if (fabsf(qb) > 1e-12f) t[n++] = -qc / qb;
    } else {
        float disc = qb * qb - 4 * qa * qc;
        if (disc >= 0) {
            float s = sqrtf(disc);
            t[n++] = (-qb + s) / (2 * qa);
            t[n++] = (-qb - s) / (2 * qa);
        }
    }
    int k = 0;
    for (int i = 0; i < n; i++)
        if (t[i] > 0 && t[i] < 1) t[k++] = t[i];
    return k;
}

BOOL path_bounds(const Path *p, const MAT *m, RCF *r)
{
    r->left = r->top = INFINITY;
    r->right = r->bottom = -INFINITY;
    BOOL any = FALSE;
    for (int i = 0; i < p->n; i++) {
        const Figure *f = &p->f[i];
        PT cur = mat_apply(m, f->start);
        grow(r, cur);
        any = TRUE;
        for (int s = 0; s < f->n; s++) {
            const PT *q = &f->pts[3 * s];
            PT e = mat_apply(m, q[2]);
            if (f->kind[s] == SEG_CUBIC) {
                PT c1 = mat_apply(m, q[0]), c2 = mat_apply(m, q[1]);
                float t[4];
                int n = extrema(cur.x, c1.x, c2.x, e.x, t);
                n += extrema(cur.y, c1.y, c2.y, e.y, t + n);
                for (int k = 0; k < n; k++) {
                    PT x = { cub(cur.x, c1.x, c2.x, e.x, t[k]), cub(cur.y, c1.y, c2.y, e.y, t[k]) };
                    grow(r, x);
                }
            }
            grow(r, e);
            cur = e;
        }
    }
    return any;
}

/* ---- hit testing ---- */
BOOL poly_contains(const Poly *p, int fill_mode, PT pt)
{
    int w = 0, start = 0;
    for (int f = 0; f < p->nfig; f++) {
        int end = p->fig_end[f];
        if (p->filled[f] && end - start >= 2) {
            for (int i = start; i < end; i++) {
                PT a = p->pt[i], b = p->pt[i + 1 < end ? i + 1 : start];
                if (a.y <= pt.y) {
                    if (b.y > pt.y && (b.x - a.x) * (pt.y - a.y) - (pt.x - a.x) * (b.y - a.y) > 0) w++;
                } else if (b.y <= pt.y && (b.x - a.x) * (pt.y - a.y) - (pt.x - a.x) * (b.y - a.y) < 0) w--;
            }
        }
        start = end;
    }
    return fill_mode == FILL_ALTERNATE ? (w & 1) : w != 0;
}

/* ---- emitting into a sink ---- */
typedef void (STDMETHODCALLTYPE *SinkFill)(void *, UINT32);
typedef void (STDMETHODCALLTYPE *SinkBegin)(void *, PT, UINT32);
typedef void (STDMETHODCALLTYPE *SinkLines)(void *, const PT *, UINT32);
typedef void (STDMETHODCALLTYPE *SinkBeziers)(void *, const BEZIER *, UINT32);
typedef void (STDMETHODCALLTYPE *SinkEnd)(void *, UINT32);

void path_emit(const Path *p, const MAT *m, int flat, float tol, void *sink)
{
    VSLOT(sink, 3, SinkFill)(sink, p->fill_mode);
    if (flat) {
        Poly o = { 0 };
        path_flatten(p, m, tol, &o);
        int start = 0;
        for (int f = 0; f < o.nfig; f++) {
            int end = o.fig_end[f];
            if (end > start) {
                VSLOT(sink, 5, SinkBegin)(sink, o.pt[start], o.filled[f] ? 0 : 1);
                if (end - start > 1) VSLOT(sink, 6, SinkLines)(sink, o.pt + start + 1, end - start - 1);
                VSLOT(sink, 8, SinkEnd)(sink, o.closed[f] ? 1 : 0);
            }
            start = end;
        }
        poly_free(&o);
        return;
    }
    for (int i = 0; i < p->n; i++) {
        const Figure *f = &p->f[i];
        VSLOT(sink, 5, SinkBegin)(sink, mat_apply(m, f->start), f->filled ? 0 : 1);
        for (int s = 0; s < f->n; s++) {
            const PT *q = &f->pts[3 * s];
            if (f->kind[s] == SEG_LINE) {
                PT e = mat_apply(m, q[2]);
                VSLOT(sink, 6, SinkLines)(sink, &e, 1);
            } else {
                BEZIER b = { mat_apply(m, q[0]), mat_apply(m, q[1]), mat_apply(m, q[2]) };
                VSLOT(sink, 7, SinkBeziers)(sink, &b, 1);
            }
        }
        VSLOT(sink, 8, SinkEnd)(sink, f->closed ? 1 : 0);
    }
}

/* ---- slab decomposition ---- */
typedef struct { float x0, y0, x1, y1; int dir, which; } Edge;
typedef struct { float xa, xb, xm; int dir, which; } Cross;

static int collect_edges(const Poly *p, int which, Edge **e, int *n, int *cap)
{
    if (!p) return 1;
    int start = 0;
    for (int f = 0; f < p->nfig; f++) {
        int end = p->fig_end[f];
        if (p->filled[f] && end - start >= 3) {
            for (int i = start; i < end; i++) {
                PT a = p->pt[i], b = p->pt[i + 1 < end ? i + 1 : start];
                if (a.y == b.y || !isfinite(a.x + a.y + b.x + b.y)) continue;
                if (*n == *cap) {
                    int nc = *cap ? *cap * 2 : 64;
                    Edge *ne = d_realloc(*e, sizeof(Edge) * nc);
                    if (!ne) return 0;
                    *e = ne;
                    *cap = nc;
                }
                Edge *d = &(*e)[(*n)++];
                if (a.y < b.y) { d->x0 = a.x; d->y0 = a.y; d->x1 = b.x; d->y1 = b.y; d->dir = 1; }
                else { d->x0 = b.x; d->y0 = b.y; d->x1 = a.x; d->y1 = a.y; d->dir = -1; }
                d->which = which;
            }
        }
        start = end;
    }
    return 1;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static int cmp_cross(const void *a, const void *b)
{
    float x = ((const Cross *)a)->xm, y = ((const Cross *)b)->xm;
    return x < y ? -1 : x > y;
}

static float edge_x(const Edge *e, float y)
{
    if (y <= e->y0) return e->x0;
    if (y >= e->y1) return e->x1;
    return e->x0 + (e->x1 - e->x0) * ((y - e->y0) / (e->y1 - e->y0));
}

static int inside(int w, int fill) { return fill == FILL_ALTERNATE ? (w & 1) : w != 0; }

static int combine(int a, int b, int op)
{
    switch (op) {
    case OP_UNION: return a || b;
    case OP_INTERSECT: return a && b;
    case OP_XOR: return a != b;
    case OP_EXCLUDE: return a && !b;
    default: return a;
    }
}

void slabs(const Poly *a, int fill_a, const Poly *b, int fill_b, int op, trap_fn fn, void *ctx)
{
    Edge *e = NULL;
    int n = 0, cap = 0;
    if (!collect_edges(a, 0, &e, &n, &cap) || !collect_edges(b, 1, &e, &n, &cap) || !n) { d_free(e); return; }
    int ny = 0, ycap = n * 2 + 16;
    float *ys = d_alloc(sizeof(float) * ycap);
    if (!ys) { d_free(e); return; }
    for (int i = 0; i < n; i++) { ys[ny++] = e[i].y0; ys[ny++] = e[i].y1; }
    /* crossings inside slabs split them too */
    if (n <= 6000) {
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                const Edge *p = &e[i], *q = &e[j];
                float lo = fmaxf(p->y0, q->y0), hi = fminf(p->y1, q->y1);
                if (lo >= hi) continue;
                float d0 = edge_x(p, lo) - edge_x(q, lo), d1 = edge_x(p, hi) - edge_x(q, hi);
                if ((d0 < 0 && d1 > 0) || (d0 > 0 && d1 < 0)) {
                    float y = lo + (hi - lo) * (d0 / (d0 - d1));
                    if (ny == ycap) {
                        ycap *= 2;
                        float *nys = d_realloc(ys, sizeof(float) * ycap);
                        if (!nys) continue;
                        ys = nys;
                    }
                    ys[ny++] = y;
                }
            }
    }
    qsort(ys, ny, sizeof(float), cmp_float);
    int k = 0;
    for (int i = 0; i < ny; i++)
        if (!k || ys[i] > ys[k - 1]) ys[k++] = ys[i];
    ny = k;
    Cross *c = d_alloc(sizeof(Cross) * n);
    for (int s = 0; c && s + 1 < ny; s++) {
        float ya = ys[s], yb = ys[s + 1], ym = (ya + yb) / 2;
        if (yb - ya < 1e-7f) continue;
        int nc = 0;
        for (int i = 0; i < n; i++) {
            if (e[i].y0 <= ym && e[i].y1 > ym) {
                c[nc].xa = edge_x(&e[i], ya);
                c[nc].xb = edge_x(&e[i], yb);
                c[nc].xm = (c[nc].xa + c[nc].xb) / 2;
                c[nc].dir = e[i].dir;
                c[nc].which = e[i].which;
                nc++;
            }
        }
        qsort(c, nc, sizeof(Cross), cmp_cross);
        int wa = 0, wb = 0, in = 0;
        float la = 0, lb = 0;
        for (int i = 0; i < nc; i++) {
            if (c[i].which) wb += c[i].dir; else wa += c[i].dir;
            int now = combine(inside(wa, fill_a), b ? inside(wb, fill_b) : 0, op);
            if (now && !in) { la = c[i].xa; lb = c[i].xb; }
            else if (!now && in) fn(ctx, ya, yb, la, lb, c[i].xa, c[i].xb);
            in = now;
        }
    }
    d_free(c);
    d_free(ys);
    d_free(e);
}

/* ---- the outline of a slab decomposition ---- */
typedef struct { PT a, b; int used; } Seg;
typedef struct { float y, x0, x1; int top; } HSpan;     /* a trapezoid's top (1) or bottom (0) */
typedef struct {
    Seg *s; int ns, scap;
    HSpan *h; int nh, hcap;
} OutlineCtx;

static void add_segment(OutlineCtx *o, PT a, PT b)
{
    if (a.x == b.x && a.y == b.y) return;
    if (o->ns == o->scap) {
        int nc = o->scap ? o->scap * 2 : 64;
        Seg *ns = d_realloc(o->s, sizeof(Seg) * nc);
        if (!ns) return;
        o->s = ns;
        o->scap = nc;
    }
    o->s[o->ns].a = a;
    o->s[o->ns].b = b;
    o->s[o->ns].used = 0;
    o->ns++;
}

static void add_hspan(OutlineCtx *o, float y, float x0, float x1, int top)
{
    if (x0 >= x1) return;
    if (o->nh == o->hcap) {
        int nc = o->hcap ? o->hcap * 2 : 64;
        HSpan *nh = d_realloc(o->h, sizeof(HSpan) * nc);
        if (!nh) return;
        o->h = nh;
        o->hcap = nc;
    }
    HSpan *h = &o->h[o->nh++];
    h->y = y; h->x0 = x0; h->x1 = x1; h->top = top;
}

static void outline_trap(void *ctx, float ya, float yb, float la, float lb, float ra, float rb)
{
    OutlineCtx *o = ctx;
    /* clockwise on screen: top rightwards, right side down, bottom leftwards, left side up */
    PT tl = { la, ya }, tr = { ra, ya }, br = { rb, yb }, bl = { lb, yb };
    add_segment(o, tr, br);
    add_segment(o, bl, tl);
    add_hspan(o, ya, la, ra, 1);
    add_hspan(o, yb, lb, rb, 0);
}

static int cmp_hspan(const void *a, const void *b)
{
    const HSpan *x = a, *y = b;
    if (x->y != y->y) return x->y < y->y ? -1 : 1;
    return x->x0 < y->x0 ? -1 : x->x0 > y->x0;
}
static int cmp_seg(const void *a, const void *b)
{
    const Seg *x = a, *y = b;
    if (x->a.y != y->a.y) return x->a.y < y->a.y ? -1 : 1;
    return x->a.x < y->a.x ? -1 : x->a.x > y->a.x;
}

/* at one height: where tops (region below) and bottoms (region above) don't
   cancel, the boundary runs rightwards (only a top) or leftwards (only a bottom) */
static void resolve_horizontal(OutlineCtx *o, HSpan *h, int n)
{
    float *xs = d_alloc(sizeof(float) * n * 2);
    if (!xs) return;
    int nx = 0;
    for (int i = 0; i < n; i++) { xs[nx++] = h[i].x0; xs[nx++] = h[i].x1; }
    qsort(xs, nx, sizeof(float), cmp_float);
    float y = h[0].y;
    for (int i = 0; i + 1 < nx; i++) {
        float a = xs[i], b = xs[i + 1], m = (a + b) / 2;
        if (b <= a) continue;
        int top = 0, bot = 0;
        for (int k = 0; k < n; k++)
            if (h[k].x0 <= m && h[k].x1 > m) { if (h[k].top) top = 1; else bot = 1; }
        PT pa = { a, y }, pb = { b, y };
        if (top && !bot) add_segment(o, pa, pb);
        else if (bot && !top) add_segment(o, pb, pa);
    }
    d_free(xs);
}

static int find_seg(Seg *s, int n, PT p)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s[mid].a.y < p.y || (s[mid].a.y == p.y && s[mid].a.x < p.x)) lo = mid + 1;
        else hi = mid;
    }
    for (int i = lo; i < n && s[i].a.y == p.y && s[i].a.x == p.x; i++)
        if (!s[i].used) return i;
    return -1;
}

void slabs_outline(const Poly *a, int fill_a, const Poly *b, int fill_b, int op, Path *out)
{
    OutlineCtx o = { 0 };
    slabs(a, fill_a, b, fill_b, op, outline_trap, &o);
    if (o.nh) {
        qsort(o.h, o.nh, sizeof(HSpan), cmp_hspan);
        int i = 0;
        while (i < o.nh) {
            int j = i;
            while (j < o.nh && o.h[j].y == o.h[i].y) j++;
            resolve_horizontal(&o, o.h + i, j - i);
            i = j;
        }
    }
    qsort(o.s, o.ns, sizeof(Seg), cmp_seg);
    for (int i = 0; i < o.ns; i++) {
        if (o.s[i].used) continue;
        o.s[i].used = 1;
        PT start = o.s[i].a, prev = o.s[i].a, cur = o.s[i].b;
        if (!path_begin(out, start, 1)) break;
        for (int guard = 0; guard < o.ns; guard++) {
            if (cur.x == start.x && cur.y == start.y) break;
            int k = find_seg(o.s, o.ns, cur);
            if (k < 0) break;
            o.s[k].used = 1;
            PT next = o.s[k].b;
            /* merge collinear runs */
            float cross = (cur.x - prev.x) * (next.y - cur.y) - (cur.y - prev.y) * (next.x - cur.x);
            float len = fabsf(cur.x - prev.x) + fabsf(cur.y - prev.y) + fabsf(next.x - cur.x) + fabsf(next.y - cur.y);
            if (fabsf(cross) > 1e-6f * len * len) {
                path_line(out, cur);
                prev = cur;
            }
            cur = next;
        }
        path_end(out, 1);
    }
    out->fill_mode = FILL_WINDING;
    d_free(o.s);
    d_free(o.h);
}
