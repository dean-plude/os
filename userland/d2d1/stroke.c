/*
 * stroke.c — ID2D1StrokeStyle and the stroker: a figure is flattened, cut
 * into dashes, and each piece becomes the outline of its stroke (left side
 * forwards, end cap, right side backwards, start cap), joins drawn on the
 * outer side of each turn and pivoted through the vertex on the inner side
 * so that the nonzero fill rule unites everything.
 */
#include "d2d_int.h"
#include <windows.h>

#define PI_F 3.14159265358979f

/* ---- ID2D1StrokeStyle ---- */
extern const void *const ss_vtbl[];

StrokeStyle *stroke_style_of(void *iface)
{
    return iface && *(const void **)iface == (const void *)ss_vtbl ? iface : NULL;
}

static HRESULT STDMETHODCALLTYPE ss_qi(StrokeStyle *s, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID2D1Resource) ||
        IsEqualIID(riid, &IID_ID2D1StrokeStyle)) {
        *out = s;
        InterlockedIncrement(&s->ref);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ss_addref(StrokeStyle *s) { return InterlockedIncrement(&s->ref); }
static ULONG STDMETHODCALLTYPE ss_release(StrokeStyle *s)
{
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        factory_release(s->factory);
        d_free(s->dashes);
        d_free(s);
    }
    return r;
}
static void STDMETHODCALLTYPE ss_factory(StrokeStyle *s, void **f) { *f = s->factory; factory_addref(s->factory); }
static UINT32 STDMETHODCALLTYPE ss_start_cap(StrokeStyle *s) { return s->p.startCap; }
static UINT32 STDMETHODCALLTYPE ss_end_cap(StrokeStyle *s) { return s->p.endCap; }
static UINT32 STDMETHODCALLTYPE ss_dash_cap(StrokeStyle *s) { return s->p.dashCap; }
static float STDMETHODCALLTYPE ss_miter(StrokeStyle *s) { return s->p.miterLimit; }
static UINT32 STDMETHODCALLTYPE ss_join(StrokeStyle *s) { return s->p.lineJoin; }
static float STDMETHODCALLTYPE ss_offset(StrokeStyle *s) { return s->p.dashOffset; }
static UINT32 STDMETHODCALLTYPE ss_style(StrokeStyle *s) { return s->p.dashStyle; }
static UINT32 STDMETHODCALLTYPE ss_count(StrokeStyle *s) { return s->p.dashStyle == DASH_CUSTOM ? s->ndashes : 0; }
static void STDMETHODCALLTYPE ss_dashes(StrokeStyle *s, float *d, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++) d[i] = s->p.dashStyle == DASH_CUSTOM && i < s->ndashes ? s->dashes[i] : 0;
}

const void *const ss_vtbl[] = { ss_qi, ss_addref, ss_release, ss_factory, ss_start_cap, ss_end_cap, ss_dash_cap,
                                ss_miter, ss_join, ss_offset, ss_style, ss_count, ss_dashes };

HRESULT stroke_style_create(void *factory, const STROKEPROPS *p, const float *dashes, UINT32 n, void **out)
{
    *out = 0;
    if (!p) return E_INVALIDARG;
    if (p->dashStyle == DASH_CUSTOM && (!dashes || !n)) return E_INVALIDARG;
    if (p->dashStyle != DASH_CUSTOM && dashes) return E_INVALIDARG;
    StrokeStyle *s = d_alloc(sizeof(*s));
    if (!s) return E_OUTOFMEMORY;
    s->vtbl = ss_vtbl;
    s->ref = 1;
    s->factory = factory;
    factory_addref(factory);
    s->p = *p;
    if (p->dashStyle == DASH_CUSTOM) {
        s->dashes = d_alloc(sizeof(float) * n);
        if (!s->dashes) { ss_release(s); return E_OUTOFMEMORY; }
        memcpy(s->dashes, dashes, sizeof(float) * n);
        s->ndashes = n;
    }
    *out = s;
    return S_OK;
}

/* ---- the stroker ---- */
typedef struct {
    Path *out;
    float hw, tol;
    int join;
    float miter;
    BOOL begun;
} Stroker;

static PT pt(float x, float y) { PT p = { x, y }; return p; }
static PT nrm(PT d) { return pt(-d.y, d.x); }

static void emit(Stroker *k, PT p)
{
    if (!k->begun) {
        path_begin(k->out, p, 1);
        k->begun = TRUE;
    } else path_line(k->out, p);
}

static void close_fig(Stroker *k)
{
    if (k->begun) path_end(k->out, 1);
    k->begun = FALSE;
}

static int arc_steps(Stroker *k, float sweep)
{
    float r = k->hw, c = 1 - k->tol / (r > 0 ? r : 1);
    float step = c > -1 && c < 1 ? 2 * acosf(c) : PI_F / 2;
    if (step < 0.02f) step = 0.02f;
    int n = (int)ceilf(fabsf(sweep) / step);
    return n < 1 ? 1 : n > 256 ? 256 : n;
}

/* points around @c at radius hw from angle @a0 through @sweep, both ends excluded */
static void arc(Stroker *k, PT c, float a0, float sweep)
{
    int n = arc_steps(k, sweep);
    for (int i = 1; i < n; i++) {
        float a = a0 + sweep * i / n;
        emit(k, pt(c.x + k->hw * cosf(a), c.y + k->hw * sinf(a)));
    }
}

/* the join at @v from direction @a to @b, on the side of nrm() */
static void join(Stroker *k, PT v, PT a, PT b)
{
    PT na = nrm(a), nb = nrm(b);
    PT p1 = pt(v.x + k->hw * na.x, v.y + k->hw * na.y), p2 = pt(v.x + k->hw * nb.x, v.y + k->hw * nb.y);
    float cross = a.x * b.y - a.y * b.x, dot = a.x * b.x + a.y * b.y;
    if (cross > 1e-6f || (fabsf(cross) <= 1e-6f && dot > 0)) {
        /* the inner side, or straight on */
        emit(k, p1);
        if (cross > 1e-6f) emit(k, v);
        emit(k, p2);
        return;
    }
    emit(k, p1);
    int j = k->join;
    if (j == JOIN_ROUND) {
        float a0 = atan2f(na.y, na.x), a1 = atan2f(nb.y, nb.x), d = a1 - a0;
        while (d > PI_F) d -= 2 * PI_F;
        while (d <= -PI_F) d += 2 * PI_F;
        if (d > 0) d -= 2 * PI_F;
        arc(k, v, a0, d);
    } else if ((j == JOIN_MITER || j == JOIN_MITER_OR_BEVEL) && dot > -0.9999f) {
        float c = na.x * nb.x + na.y * nb.y;                 /* cos of the turn */
        float ratio = sqrtf(2 / (1 + c));                    /* miter length / half width */
        float limit = k->miter < 1 ? 1 : k->miter;
        if (ratio <= limit) {
            float s = k->hw / (1 + c);
            emit(k, pt(v.x + (na.x + nb.x) * s, v.y + (na.y + nb.y) * s));
        } else if (j == JOIN_MITER) {
            /* clipped at the limit */
            PT u = pt(na.x + nb.x, na.y + nb.y);
            float ul = sqrtf(u.x * u.x + u.y * u.y);
            if (ul > 1e-6f) {
                u.x /= ul; u.y /= ul;
                float L = limit * k->hw, h = k->hw * (na.x * u.x + na.y * u.y);
                float au = a.x * u.x + a.y * u.y, bu = -(b.x * u.x + b.y * u.y);
                if (au > 1e-6f && bu > 1e-6f) {
                    float t1 = (L - h) / au, t2 = (L - h) / bu;
                    emit(k, pt(p1.x + a.x * t1, p1.y + a.y * t1));
                    emit(k, pt(p2.x - b.x * t2, p2.y - b.y * t2));
                }
            }
        }
    }
    emit(k, p2);
}

/* the cap at @e, leaving along @d: from the left offset to the right one */
static void cap(Stroker *k, PT e, PT d, int kind)
{
    PT n = nrm(d);
    float h = k->hw;
    switch (kind) {
    case CAP_SQUARE:
        emit(k, pt(e.x + h * n.x + h * d.x, e.y + h * n.y + h * d.y));
        emit(k, pt(e.x - h * n.x + h * d.x, e.y - h * n.y + h * d.y));
        break;
    case CAP_TRIANGLE:
        emit(k, pt(e.x + h * d.x, e.y + h * d.y));
        break;
    case CAP_ROUND:
        arc(k, e, atan2f(n.y, n.x), -PI_F);
        break;
    }
}

static PT dir(PT a, PT b)
{
    float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
    return l > 0 ? pt(dx / l, dy / l) : pt(1, 0);
}

/* one side of an open polyline, walking forwards */
static void side(Stroker *k, const PT *p, int n, int step)
{
    /* p[i * step] for i in 0..n-1 */
#define P(i) p[(i) * step]
    PT d0 = dir(P(0), P(1)), n0 = nrm(d0);
    emit(k, pt(P(0).x + k->hw * n0.x, P(0).y + k->hw * n0.y));
    for (int i = 1; i < n - 1; i++) join(k, P(i), dir(P(i - 1), P(i)), dir(P(i), P(i + 1)));
    PT dl = dir(P(n - 2), P(n - 1)), nl = nrm(dl);
    emit(k, pt(P(n - 1).x + k->hw * nl.x, P(n - 1).y + k->hw * nl.y));
#undef P
}

static void stroke_open(Stroker *k, const PT *p, int n, PT hint, int cap0, int cap1)
{
    if (n == 1) {
        /* a dot: only caps make it visible */
        if (cap0 == CAP_FLAT && cap1 == CAP_FLAT) return;
        PT nn = nrm(hint), e = p[0];
        emit(k, pt(e.x + k->hw * nn.x, e.y + k->hw * nn.y));
        cap(k, e, hint, cap1);
        emit(k, pt(e.x - k->hw * nn.x, e.y - k->hw * nn.y));
        cap(k, e, pt(-hint.x, -hint.y), cap0);
        close_fig(k);
        return;
    }
    side(k, p, n, 1);
    cap(k, p[n - 1], dir(p[n - 2], p[n - 1]), cap1);
    side(k, p + n - 1, n, -1);
    cap(k, p[0], dir(p[1], p[0]), cap0);
    close_fig(k);
}

static void stroke_closed(Stroker *k, const PT *p, int n)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < n; j++) {
            int i = pass ? n - 1 - j : j;
            int prev = pass ? (i + 1) % n : (i + n - 1) % n, next = pass ? (i + n - 1) % n : (i + 1) % n;
            join(k, p[i], dir(p[prev], p[i]), dir(p[i], p[next]));
        }
        close_fig(k);
    }
}

/* a growing polyline */
typedef struct { PT *p; int n, cap; } Line;
static void line_add(Line *l, PT q)
{
    if (l->n && l->p[l->n - 1].x == q.x && l->p[l->n - 1].y == q.y) return;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        PT *np = d_realloc(l->p, sizeof(PT) * nc);
        if (!np) return;
        l->p = np;
        l->cap = nc;
    }
    l->p[l->n++] = q;
}

static void stroke_dashed(Stroker *k, const PT *p, int n, int closed, const float *pat, int npat, float offset,
                          int cap0, int cap1, int dcap)
{
    float total = 0;
    for (int i = 0; i < npat; i++) total += pat[i];
    int idx = 0;
    float left = pat[0];
    if (total > 0) {
        float o = fmodf(offset, total);
        if (o < 0) o += total;
        while (o > 0) {
            if (o >= left) { o -= left; idx = (idx + 1) % npat; left = pat[idx]; }
            else { left -= o; o = 0; }
        }
    }
    Line cur = { 0 };
    int at_start = 1;
    int nseg = closed ? n : n - 1;
    PT last_dir = dir(p[0], p[1 % n]);
    if (!(idx & 1)) line_add(&cur, p[0]);
    for (int s = 0; s < nseg; s++) {
        PT a = p[s], b = p[(s + 1) % n];
        PT d = dir(a, b);
        float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)), pos = 0;
        last_dir = d;
        int guard = 0;
        while (len - pos > left && guard++ < 100000) {
            pos += left;
            PT q = pt(a.x + d.x * pos, a.y + d.y * pos);
            if (!(idx & 1)) {
                line_add(&cur, q);
                stroke_open(k, cur.p, cur.n, d, at_start && !closed ? cap0 : dcap, dcap);
                cur.n = 0;
            } else line_add(&cur, q);
            at_start = 0;
            idx = (idx + 1) % npat;
            left = pat[idx];
            if (idx & 1) cur.n = 0;
        }
        left -= len - pos;
        if (!(idx & 1)) line_add(&cur, b);
    }
    if (!(idx & 1) && cur.n) stroke_open(k, cur.p, cur.n, last_dir, at_start && !closed ? cap0 : dcap,
                                         closed ? dcap : cap1);
    d_free(cur.p);
}

void stroke_to_path(const Path *path, float width, const StrokeStyle *st, float tol, Path *out)
{
    out->fill_mode = FILL_WINDING;
    if (!(width > 0) || !isfinite(width)) return;
    Stroker k = { out, width / 2, tol > 0 ? tol : 0.1f, JOIN_MITER, 10, FALSE };
    int cap0 = CAP_FLAT, cap1 = CAP_FLAT, dcap = CAP_FLAT, dash = DASH_SOLID;
    float offset = 0;
    if (st) {
        k.join = st->p.lineJoin;
        k.miter = st->p.miterLimit;
        cap0 = st->p.startCap;
        cap1 = st->p.endCap;
        dcap = st->p.dashCap;
        dash = st->p.dashStyle;
        offset = st->p.dashOffset * width;
    }
    float pat[64];
    int npat = 0;
    static const float dashes[][6] = { { 0 }, { 2, 2 }, { 0, 2 }, { 2, 2, 0, 2 }, { 2, 2, 0, 2, 0, 2 } };
    static const int counts[] = { 0, 2, 2, 4, 6 };
    if (dash >= DASH_DASH && dash <= DASH_DASH_DOT_DOT) {
        npat = counts[dash];
        for (int i = 0; i < npat; i++) pat[i] = dashes[dash][i] * width;
    } else if (dash == DASH_CUSTOM && st->ndashes) {
        for (UINT32 i = 0; i < st->ndashes && npat < 32; i++) pat[npat++] = fmaxf(st->dashes[i], 0) * width;
        if (npat & 1) {
            for (int i = 0; i < npat && npat + i < 64; i++) pat[npat + i] = pat[i];
            npat *= 2;
        }
    }
    float total = 0;
    for (int i = 0; i < npat; i++) total += pat[i];
    if (total <= 0) npat = 0;

    Poly o = { 0 };
    path_flatten(path, &MAT_IDENTITY, k.tol, &o);
    int start = 0;
    for (int f = 0; f < o.nfig; f++) {
        int end = o.fig_end[f], n = end - start;
        const PT *p = o.pt + start;
        int closed = o.closed[f];
        if (closed && n > 1 && p[n - 1].x == p[0].x && p[n - 1].y == p[0].y) n--;
        if (n == 1) {
            if (!closed) stroke_open(&k, p, 1, pt(1, 0), cap0, cap1);
        } else if (n >= 2) {
            if (npat) stroke_dashed(&k, p, n, closed, pat, npat, offset, cap0, cap1, dcap);
            else if (closed && n > 2) stroke_closed(&k, p, n);
            else stroke_open(&k, p, n, pt(1, 0), cap0, cap1);
        }
        start = end;
    }
    poly_free(&o);
}
