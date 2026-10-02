/*
 * raster.c — the scanline rasterizer and compositing
 *
 * Polygons are rasterized by exact area: each edge adds, to the cells it
 * crosses, the signed area it covers to its right, and a running sum
 * along each row turns that into the winding coverage of every pixel,
 * folded by the fill rule (the method of libart, FreeType's gray raster
 * and font-rs).  Rows are done in bands to bound memory.  Coverage is
 * multiplied by layer masks and opacity masks, and the brush's colour is
 * blended source-over into the premultiplied BGRA surface.
 */
#include "d2d_int.h"
#include <windows.h>

#define BAND 128

/* ---- paint ---- */
static UINT32 scale_px(UINT32 c, unsigned a)      /* a in 0..256 */
{
    UINT32 rb = ((c & 0x00FF00FF) * a >> 8) & 0x00FF00FF;
    UINT32 ag = (((c >> 8) & 0x00FF00FF) * a) & 0xFF00FF00;
    return rb | ag;
}

static float extend_t(float t, int mode)
{
    if (!isfinite(t)) return 0;
    switch (mode) {
    case EXTEND_WRAP: t -= floorf(t); break;
    case EXTEND_MIRROR: t = fabsf(t - 2 * floorf(t / 2)); if (t > 1) t = 2 - t; break;
    default: t = t < 0 ? 0 : t > 1 ? 1 : t; break;
    }
    return t;
}

static int extend_i(int i, int n, int mode)
{
    if (i >= 0 && i < n) return i;
    switch (mode) {
    case EXTEND_WRAP: i %= n; return i < 0 ? i + n : i;
    case EXTEND_MIRROR: {
        int p = 2 * n;
        i %= p;
        if (i < 0) i += p;
        return i < n ? i : p - 1 - i;
    }
    default: return i < 0 ? 0 : n - 1;
    }
}

static UINT32 lerp_px(UINT32 a, UINT32 b, unsigned f)  /* f in 0..256 */
{
    return scale_px(a, 256 - f) + scale_px(b, f);
}

static UINT32 sample_bitmap(const Paint *p, float u, float v)
{
    const Surface *s = p->bmp;
    if (!p->linear_filter) {
        int x = extend_i((int)floorf(u), s->w, p->extend), y = extend_i((int)floorf(v), s->h, p->extend_y);
        return s->px[y * s->stride + x];
    }
    u -= 0.5f;
    v -= 0.5f;
    float fu = floorf(u), fv = floorf(v);
    int x0 = (int)fu, y0 = (int)fv;
    unsigned ax = (unsigned)((u - fu) * 256), ay = (unsigned)((v - fv) * 256);
    int xa = extend_i(x0, s->w, p->extend), xb = extend_i(x0 + 1, s->w, p->extend);
    int ya = extend_i(y0, s->h, p->extend_y), yb = extend_i(y0 + 1, s->h, p->extend_y);
    UINT32 top = lerp_px(s->px[ya * s->stride + xa], s->px[ya * s->stride + xb], ax);
    UINT32 bot = lerp_px(s->px[yb * s->stride + xa], s->px[yb * s->stride + xb], ax);
    return lerp_px(top, bot, ay);
}

void paint_span(const Paint *p, int x, int y, int n, UINT32 *out)
{
    unsigned op = (unsigned)(p->opacity * 256 + 0.5f);
    if (op > 256) op = 256;
    if (p->kind == PAINT_SOLID) {
        for (int i = 0; i < n; i++) out[i] = p->color;
        return;
    }
    const MAT *m = &p->inv;
    float fy = y + 0.5f;
    for (int i = 0; i < n; i++) {
        float fx = x + i + 0.5f;
        float u = fx * m->m11 + fy * m->m21 + m->dx, v = fx * m->m12 + fy * m->m22 + m->dy;
        UINT32 c;
        if (p->kind == PAINT_LINEAR) {
            float dx = p->p1.x - p->p0.x, dy = p->p1.y - p->p0.y, l2 = dx * dx + dy * dy;
            float t = l2 > 0 ? ((u - p->p0.x) * dx + (v - p->p0.y) * dy) / l2 : 1;
            c = p->lut[(int)(extend_t(t, p->extend) * 255 + 0.5f)];
        } else if (p->kind == PAINT_RADIAL) {
            float rx = p->rx, ry = p->ry;
            if (rx == 0 || ry == 0) { c = p->lut[255]; goto done; }
            float wx = (u - p->p0.x) / rx, wy = (v - p->p0.y) / ry;   /* unit-circle space */
            float fx_ = p->p1.x / rx, fy_ = p->p1.y / ry;             /* the focus */
            float f2 = fx_ * fx_ + fy_ * fy_;
            if (f2 > 0.998f) { float k = sqrtf(0.998f / f2); fx_ *= k; fy_ *= k; f2 = 0.998f; }
            float qx = wx - fx_, qy = wy - fy_;
            float a = 1 - f2, b = qx * fx_ + qy * fy_, cc = qx * qx + qy * qy;
            float t = (b + sqrtf(b * b + a * cc)) / a;
            c = p->lut[(int)(extend_t(t, p->extend) * 255 + 0.5f)];
        } else c = sample_bitmap(p, u, v);
    done:
        out[i] = op >= 256 ? c : scale_px(c, op);
    }
}

/* ---- compositing a row ---- */
typedef struct {
    Surface *s;
    const Paint *paint;
    const Mask *mask;
    UINT32 *colors, *alphas;
} Comp;

static void composite_row(Comp *c, int y, int x0, int n, const float *cov)
{
    UINT32 *dst = c->s->px + (size_t)y * c->s->stride + x0;
    int i = 0;
    while (i < n) {
        if (cov[i] <= 0.0f) { i++; continue; }
        int j = i;
        while (j < n && cov[j] > 0.0f) j++;
        int len = j - i;
        if (c->paint->kind != PAINT_SOLID) paint_span(c->paint, x0 + i, y, len, c->colors);
        if (c->paint->alpha) paint_span(c->paint->alpha, x0 + i, y, len, c->alphas);
        for (int k = 0; k < len; k++) {
            float a = cov[i + k];
            if (c->mask) {
                int mx = x0 + i + k - c->mask->x0, my = y - c->mask->y0;
                a *= (mx >= 0 && my >= 0 && mx < c->mask->w && my < c->mask->h) ?
                         c->mask->a[(size_t)my * c->mask->w + mx] * (1.0f / 255) : 0;
            }
            if (c->paint->alpha) a *= (c->alphas[k] >> 24) * (1.0f / 255);
            unsigned cv = (unsigned)(a * 256 + 0.5f);
            if (!cv) continue;
            UINT32 src = c->paint->kind == PAINT_SOLID ? c->paint->color : c->colors[k];
            if (cv < 256) src = scale_px(src, cv);
            unsigned sa = src >> 24;
            UINT32 *d = &dst[i + k];
            *d = sa == 255 ? src : src + scale_px(*d, 256 - (sa + (sa >> 7)));
        }
        i = j;
    }
}

/* ---- the rasterizer ---- */
static void add_line(float *acc, int w, int h, PT p0, PT p1)
{
    if (p0.y == p1.y) return;
    float dir = 1;
    if (p0.y > p1.y) { PT t = p0; p0 = p1; p1 = t; dir = -1; }
    if (p1.y <= 0 || p0.y >= h) return;
    float dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    float ys = p0.y < 0 ? 0 : p0.y, ye = p1.y > h ? h : p1.y;
    float x = p0.x + (ys - p0.y) * dxdy;
    int yi = (int)ys;
    for (; yi < h && yi < ye; yi++) {
        float top = yi > ys ? (float)yi : ys, bot = yi + 1 < ye ? (float)(yi + 1) : ye;
        float dy = bot - top;
        if (dy <= 0) continue;
        float xn = x + dxdy * dy;
        float d = dy * dir;
        float xa = x < xn ? x : xn, xb = x < xn ? xn : x;
        if (xa < 0) xa = 0;
        if (xb < 0) xb = 0;
        if (xa > w) xa = (float)w;
        if (xb > w) xb = (float)w;
        float *row = acc + (size_t)yi * (w + 2);
        float xaf = floorf(xa);
        int xai = (int)xaf, xbi = (int)ceilf(xb);
        if (xbi <= xai + 1) {
            /* within one cell: split by the mean position */
            float xm = 0.5f * (xa + xb) - xaf;
            row[xai] += d - d * xm;
            row[xai + 1] += d * xm;
        } else {
            float s = 1.0f / (xb - xa);
            float fa = xa - xaf;
            float a0 = 0.5f * s * (1 - fa) * (1 - fa);
            float fb = xb - ceilf(xb) + 1;
            float am = 0.5f * s * fb * fb;
            row[xai] += d * a0;
            if (xbi == xai + 2) row[xai + 1] += d * (1 - a0 - am);
            else {
                float a1 = s * (1.5f - fa);
                row[xai + 1] += d * (a1 - a0);
                for (int k = xai + 2; k < xbi - 1; k++) row[k] += d * s;
                float a2 = a1 + (xbi - xai - 3) * s;
                row[xbi - 1] += d * (1 - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xn;
    }
}

typedef void (*row_fn)(void *ctx, int y, int x0, int n, const float *cov);

static void rasterize(const Poly *o, int fill, int aliased, const Clip *c, row_fn fn, void *ctx)
{
    float minx = INFINITY, miny = INFINITY, maxx = -INFINITY, maxy = -INFINITY;
    int start = 0;
    for (int f = 0; f < o->nfig; f++) {
        int end = o->fig_end[f];
        if (o->filled[f])
            for (int i = start; i < end; i++) {
                PT p = o->pt[i];
                if (!isfinite(p.x) || !isfinite(p.y)) return;
                minx = fminf(minx, p.x); maxx = fmaxf(maxx, p.x);
                miny = fminf(miny, p.y); maxy = fmaxf(maxy, p.y);
            }
        start = end;
    }
    if (minx > maxx) return;
    int bx0 = c->x0, by0 = (int)floorf(miny), by1 = (int)ceilf(maxy), bx1 = (int)ceilf(maxx);
    if (by0 < c->y0) by0 = c->y0;
    if (by1 > c->y1) by1 = c->y1;
    if (bx1 > c->x1) bx1 = c->x1;
    if ((int)floorf(minx) > bx0) bx0 = (int)floorf(minx);
    if (bx0 >= bx1 || by0 >= by1) return;
    int w = bx1 - bx0;
    float *acc = d_alloc(sizeof(float) * (size_t)(w + 2) * BAND);
    float *cov = d_alloc(sizeof(float) * (size_t)w);
    if (!acc || !cov) { d_free(acc); d_free(cov); return; }
    for (int y0 = by0; y0 < by1; y0 += BAND) {
        int h = by1 - y0 < BAND ? by1 - y0 : BAND;
        memset(acc, 0, sizeof(float) * (size_t)(w + 2) * h);
        start = 0;
        for (int f = 0; f < o->nfig; f++) {
            int end = o->fig_end[f];
            if (o->filled[f] && end - start >= 2)
                for (int i = start; i < end; i++) {
                    PT a = o->pt[i], b = o->pt[i + 1 < end ? i + 1 : start];
                    a.x -= bx0; a.y -= y0;
                    b.x -= bx0; b.y -= y0;
                    add_line(acc, w, h, a, b);
                }
            start = end;
        }
        for (int r = 0; r < h; r++) {
            float *row = acc + (size_t)r * (w + 2), sum = 0;
            int any = 0;
            for (int x = 0; x < w; x++) {
                sum += row[x];
                float v = fabsf(sum);
                if (fill == FILL_ALTERNATE) {
                    v = fmodf(v, 2);
                    if (v > 1) v = 2 - v;
                } else if (v > 1) v = 1;
                if (aliased) v = v >= 0.5f ? 1 : 0;
                else if (v < 1.0f / 512) v = 0;
                cov[x] = v;
                any |= v > 0;
            }
            if (any) fn(ctx, y0 + r, bx0, w, cov);
        }
    }
    d_free(acc);
    d_free(cov);
}

static void fill_poly(Surface *s, const Clip *c, const Mask *mask, const Poly *o, int fill, int aliased,
                      const Paint *paint)
{
    Comp comp = { s, paint, mask, NULL, NULL };
    int w = c->x1 - c->x0;
    if (w <= 0) return;
    comp.colors = d_alloc(sizeof(UINT32) * w);
    comp.alphas = d_alloc(sizeof(UINT32) * w);
    if (comp.colors && comp.alphas) rasterize(o, fill, aliased, c, (row_fn)composite_row, &comp);
    d_free(comp.colors);
    d_free(comp.alphas);
}

static float device_tol(const MAT *m) { (void)m; return 0.2f; }

void raster_fill(Surface *s, const Clip *c, const Mask *mask, const Path *path, const MAT *m, int aliased,
                 const Paint *paint)
{
    Poly o = { 0 };
    path_flatten(path, m, device_tol(m), &o);
    fill_poly(s, c, mask, &o, path->fill_mode, aliased, paint);
    poly_free(&o);
}

void raster_stroke(Surface *s, const Clip *c, const Mask *mask, const Path *path, const MAT *m, float width,
                   const StrokeStyle *st, int aliased, const Paint *paint)
{
    float sc = mat_scale(m);
    Path out;
    path_init(&out);
    stroke_to_path(path, width, st, 0.2f / (sc > 0 ? sc : 1), &out);
    Poly o = { 0 };
    path_flatten(&out, m, device_tol(m), &o);
    for (int i = 0; i < o.nfig; i++) o.filled[i] = 1;
    fill_poly(s, c, mask, &o, FILL_WINDING, aliased, paint);
    poly_free(&o);
    path_free(&out);
}

static void mask_row(void *ctx, int y, int x0, int n, const float *cov)
{
    Mask *m = ctx;
    unsigned char *row = m->a + (size_t)(y - m->y0) * m->w + (x0 - m->x0);
    for (int i = 0; i < n; i++) row[i] = (unsigned char)(cov[i] * 255 + 0.5f);
}

void raster_mask(Mask *mask, const Clip *c, const Path *path, const MAT *m, int aliased)
{
    mask->x0 = c->x0;
    mask->y0 = c->y0;
    mask->w = c->x1 > c->x0 ? c->x1 - c->x0 : 0;
    mask->h = c->y1 > c->y0 ? c->y1 - c->y0 : 0;
    mask->a = d_alloc((size_t)mask->w * mask->h + 1);
    if (!mask->a) return;
    Poly o = { 0 };
    path_flatten(path, m, 0.2f, &o);
    rasterize(&o, path->fill_mode, aliased, c, mask_row, mask);
    poly_free(&o);
}
