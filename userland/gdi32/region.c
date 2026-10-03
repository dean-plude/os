/*
 * Regions and clipping.  A region is a set of rectangles that do not
 * overlap (with its bounding box beside them); CombineRgn works on them
 * exactly, through the grid their edges make.  A DC's clip region keeps
 * up to NOVA_DC_CLIP_RECTS rectangles, and drawing stays inside them:
 * GTK and cairo clip to "this window minus its children" and the like,
 * which a bounding box would paint straight over.  Round and elliptic
 * regions are their bounding boxes.
 */
#define NOVA_BUILD_GDI32
#include <windows.h>
#include "gdi_int.h"

#define RGN_AND_  1
#define RGN_OR_   2
#define RGN_XOR_  3
#define RGN_DIFF_ 4
#define RGN_COPY_ 5

static void *r_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
static void r_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static int rect_empty(const RECT *r) { return r->right <= r->left || r->bottom <= r->top; }

/* user32's rectangle helpers, which gdi32 cannot import */
static void SetRect_(RECT *r, int l, int t, int rr, int b) { r->left = l; r->top = t; r->right = rr; r->bottom = b; }
static void SetRectEmpty_(RECT *r) { SetRect_(r, 0, 0, 0, 0); }
static void OffsetRect_(RECT *r, int x, int y) { r->left += x; r->right += x; r->top += y; r->bottom += y; }
static void UnionRect_(RECT *d, const RECT *a, const RECT *b)
{
    if (rect_empty(a)) { *d = *b; return; }
    if (rect_empty(b)) { *d = *a; return; }
    RECT r = { a->left < b->left ? a->left : b->left, a->top < b->top ? a->top : b->top,
               a->right > b->right ? a->right : b->right, a->bottom > b->bottom ? a->bottom : b->bottom };
    *d = r;
}
static int IntersectRect_(RECT *d, const RECT *a, const RECT *b)
{
    RECT r = { a->left > b->left ? a->left : b->left, a->top > b->top ? a->top : b->top,
               a->right < b->right ? a->right : b->right, a->bottom < b->bottom ? a->bottom : b->bottom };
    if (rect_empty(&r)) { SetRectEmpty_(d); return 0; }
    *d = r;
    return 1;
}
/* @a minus @b, when that is a rectangle (else @a) */
static void SubtractRect_(RECT *d, const RECT *a, const RECT *b)
{
    RECT r = *a;
    if (b->top <= r.top && b->bottom >= r.bottom) {
        if (b->left <= r.left && b->right > r.left) r.left = b->right;
        else if (b->right >= r.right && b->left < r.right) r.right = b->left;
    } else if (b->left <= r.left && b->right >= r.right) {
        if (b->top <= r.top && b->bottom > r.top) r.top = b->bottom;
        else if (b->bottom >= r.bottom && b->top < r.bottom) r.bottom = b->top;
    }
    if (rect_empty(&r)) SetRectEmpty_(&r);
    *d = r;
}

/* @o's rectangles: its list, or its box as the one rectangle (none if empty) */
static const RECT *rgn_rects(const GObj *o, int *n)
{
    if (o->nrects > 1) { *n = o->nrects; return o->rects; }
    *n = rect_empty(&o->rc) ? 0 : 1;
    return &o->rc;
}

/* Make @o the region of @n rectangles @r (taken over: heap-allocated, or
 * NULL when @n is 0) */
static void rgn_set(GObj *o, RECT *r, int n)
{
    r_free(o->rects);
    o->rects = NULL;
    o->nrects = 0;
    SetRectEmpty_(&o->rc);
    if (n <= 0) { r_free(r); return; }
    RECT box = r[0];
    for (int i = 1; i < n; i++) UnionRect_(&box, &box, &r[i]);
    o->rc = box;
    if (n == 1) { r_free(r); return; }
    o->rects = r;
    o->nrects = n;
}

/* Called by DeleteObject */
void rgn_free(GObj *o)
{
    r_free(o->rects);
    o->rects = NULL;
    o->nrects = 0;
}

static int sort_unique(int *v, int n)
{
    for (int i = 1; i < n; i++) { int x = v[i], j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    int k = 0;
    for (int i = 0; i < n; i++) if (!k || v[k - 1] != v[i]) v[k++] = v[i];
    return k;
}

static int covers(const RECT *r, int n, int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < n; i++)
        if (r[i].left <= x0 && r[i].right >= x1 && r[i].top <= y0 && r[i].bottom >= y1) return 1;
    return 0;
}

/* @a op @b as rectangles (*@out, heap-allocated): their edges cut the
 * plane into cells, each wholly in or out of either region; the cells
 * kept are joined along each band, then bands with the same spans are
 * joined downwards.  -1 when it would take too long (the caller falls back
 * to bounding boxes). */
static int rgn_op(const RECT *a, int na, const RECT *b, int nb, int mode, RECT **out)
{
    *out = NULL;
    int ne = 2 * (na + nb);
    int *xs = r_alloc(sizeof(int) * (ne + 1)), *ys = r_alloc(sizeof(int) * (ne + 1));
    if (!xs || !ys) { r_free(xs); r_free(ys); return -1; }
    int nx = 0, ny = 0;
    for (int i = 0; i < na; i++) { xs[nx++] = a[i].left; xs[nx++] = a[i].right; ys[ny++] = a[i].top; ys[ny++] = a[i].bottom; }
    for (int i = 0; i < nb; i++) { xs[nx++] = b[i].left; xs[nx++] = b[i].right; ys[ny++] = b[i].top; ys[ny++] = b[i].bottom; }
    nx = sort_unique(xs, nx);
    ny = sort_unique(ys, ny);
    if ((long long)nx * ny * (na + nb + 1) > 4000000) { r_free(xs); r_free(ys); return -1; }
    int cap = 16, n = 0, prev_start = 0, prev_end = 0;
    RECT *res = r_alloc(sizeof(RECT) * cap);
    if (!res) { r_free(xs); r_free(ys); return -1; }
    for (int j = 0; j + 1 < ny; j++) {
        int y0 = ys[j], y1 = ys[j + 1], band_start = n;
        for (int i = 0; i + 1 < nx; i++) {
            int x0 = xs[i], x1 = xs[i + 1];
            int ia = covers(a, na, x0, y0, x1, y1), ib = covers(b, nb, x0, y0, x1, y1);
            int in = mode == RGN_AND_ ? ia && ib : mode == RGN_OR_ ? ia || ib : mode == RGN_XOR_ ? ia != ib :
                     mode == RGN_DIFF_ ? ia && !ib : ia;
            if (!in) continue;
            if (n > band_start && res[n - 1].right == x0) { res[n - 1].right = x1; continue; }
            if (n == cap) {
                RECT *g = r_alloc(sizeof(RECT) * cap * 2);
                if (!g) { r_free(res); r_free(xs); r_free(ys); return -1; }
                memcpy(g, res, sizeof(RECT) * n);
                r_free(res);
                res = g;
                cap *= 2;
            }
            SetRect_(&res[n++], x0, y0, x1, y1);
        }
        /* the same spans as the band just above, touching it: grow those */
        if (n - band_start == prev_end - prev_start && n > band_start && res[prev_start].bottom == y0) {
            int same = 1;
            for (int k = 0; k < n - band_start && same; k++)
                same = res[prev_start + k].left == res[band_start + k].left && res[prev_start + k].right == res[band_start + k].right;
            if (same) {
                for (int k = prev_start; k < prev_end; k++) res[k].bottom = y1;
                n = band_start;
                continue;
            }
        }
        if (n > band_start) { prev_start = band_start; prev_end = n; }
    }
    r_free(xs);
    r_free(ys);
    *out = res;
    return n;
}

/* Bounding boxes, for regions too complex to combine exactly */
static void box_op(RECT *r, const RECT *a, const RECT *b, int mode)
{
    *r = *a;
    if (mode == RGN_AND_) { if (!IntersectRect_(r, a, b)) SetRectEmpty_(r); }
    else if (mode == RGN_OR_ || mode == RGN_XOR_) UnionRect_(r, a, b);
    else if (mode == RGN_DIFF_) SubtractRect_(r, a, b);
}

/* -----------------------------------------------------------------------
 * Region objects
 * ----------------------------------------------------------------------- */
GDIAPI HRGN CreateRectRgn(int l, int t, int r, int b)
{
    GObj *o = new_obj(K_REGION);
    if (o) {
        if (l > r) { int x = l; l = r; r = x; }
        if (t > b) { int x = t; t = b; b = x; }
        o->rc.left = l; o->rc.top = t; o->rc.right = r; o->rc.bottom = b;
        if (rect_empty(&o->rc)) SetRectEmpty_(&o->rc);
    }
    return (HRGN)o;
}
GDIAPI HRGN CreateRectRgnIndirect(const RECT *r) { return CreateRectRgn(r->left, r->top, r->right, r->bottom); }
GDIAPI HRGN CreateRoundRectRgn(int l, int t, int r, int b, int w, int h) { (void)w; (void)h; return CreateRectRgn(l, t, r, b); }
GDIAPI HRGN CreateEllipticRgn(int l, int t, int r, int b) { return CreateRectRgn(l, t, r, b); }

/* A region from rectangles (moved by @x, when given: scaled and offset
 * exactly, otherwise their bounding box) */
GDIAPI HRGN ExtCreateRegion(const XFORM *x, DWORD n, const RGNDATA *data)
{
    if (!data || data->rdh.dwSize < sizeof(RGNDATAHEADER) || data->rdh.iType != 1 /* RDH_RECTANGLES */) return NULL;
    DWORD count = data->rdh.nCount;
    if (n < sizeof(RGNDATAHEADER) + count * sizeof(RECT)) count = n > sizeof(RGNDATAHEADER) ? (n - sizeof(RGNDATAHEADER)) / sizeof(RECT) : 0;
    const RECT *rc = (const RECT *)((const char *)data + data->rdh.dwSize);
    GObj *o = new_obj(K_REGION);
    if (!o) return NULL;
    RECT *list = r_alloc(sizeof(RECT) * (count ? count : 1));
    if (!list) { o->used = 0; return NULL; }
    int k = 0, skew = x && (x->eM12 || x->eM21);
    for (DWORD i = 0; i < count; i++) {
        RECT r = rc[i];
        if (rect_empty(&r)) continue;
        if (x) {
            float cx[4] = { (float)r.left, (float)r.right, (float)r.left, (float)r.right };
            float cy[4] = { (float)r.top, (float)r.top, (float)r.bottom, (float)r.bottom };
            float l = 0, t = 0, rr = 0, b = 0;
            for (int c = 0; c < 4; c++) {
                float px = cx[c] * x->eM11 + cy[c] * x->eM21 + x->eDx, py = cx[c] * x->eM12 + cy[c] * x->eM22 + x->eDy;
                if (!c || px < l) l = px;
                if (!c || px > rr) rr = px;
                if (!c || py < t) t = py;
                if (!c || py > b) b = py;
            }
            SetRect_(&r, (int)(l < 0 ? l - 0.5f : l + 0.5f), (int)(t < 0 ? t - 0.5f : t + 0.5f),
                    (int)(rr < 0 ? rr - 0.5f : rr + 0.5f), (int)(b < 0 ? b - 0.5f : b + 0.5f));
        }
        list[k++] = r;
    }
    if (skew && k > 1) {                                  /* rotated: the bounding box */
        RECT box = list[0];
        for (int i = 1; i < k; i++) UnionRect_(&box, &box, &list[i]);
        list[0] = box;
        k = 1;
    }
    if (k > 1) {                                            /* the rectangles may overlap: OR them */
        RECT *u;
        int m = rgn_op(list, k, NULL, 0, RGN_OR_, &u);
        if (m >= 0) { r_free(list); list = u; k = m; }
    }
    rgn_set(o, list, k);
    return (HRGN)o;
}

GDIAPI int GetRgnBox(HRGN h, LPRECT r)
{
    GObj *o = obj_of(h);
    if (!o || o->kind != K_REGION) return 0;
    *r = o->rc;
    return rect_empty(&o->rc) ? NULLREGION : o->nrects > 1 ? COMPLEXREGION : SIMPLEREGION;
}

static int rgn_kind(const GObj *o) { return rect_empty(&o->rc) ? NULLREGION : o->nrects > 1 ? COMPLEXREGION : SIMPLEREGION; }

GDIAPI int CombineRgn(HRGN dst, HRGN a, HRGN b, int mode)
{
    GObj *d = obj_of(dst), *x = obj_of(a), *y = obj_of(b);
    if (!d || !x || mode < RGN_AND_ || mode > RGN_COPY_ || (mode != RGN_COPY_ && !y)) return ERROR;
    int na, nb = 0;
    const RECT *ra = rgn_rects(x, &na), *rb = mode == RGN_COPY_ ? NULL : rgn_rects(y, &nb);
    RECT *out;
    int n = rgn_op(ra, na, rb, nb, mode, &out);
    if (n < 0) {
        RECT box;
        box_op(&box, &x->rc, y ? &y->rc : &x->rc, mode);
        out = r_alloc(sizeof(RECT));
        if (!out) return ERROR;
        out[0] = box;
        n = rect_empty(&box) ? 0 : 1;
    }
    rgn_set(d, out, n);
    return rgn_kind(d);
}

GDIAPI BOOL SetRectRgn(HRGN h, int l, int t, int r, int b)
{
    GObj *o = obj_of(h);
    if (!o) return FALSE;
    RECT *one = r_alloc(sizeof(RECT));
    if (!one) return FALSE;
    SetRect_(one, l < r ? l : r, t < b ? t : b, l < r ? r : l, t < b ? b : t);
    rgn_set(o, one, rect_empty(one) ? 0 : 1);
    return TRUE;
}

GDIAPI BOOL PtInRegion(HRGN h, int x, int y)
{
    GObj *o = obj_of(h);
    if (!o) return FALSE;
    int n;
    const RECT *r = rgn_rects(o, &n);
    for (int i = 0; i < n; i++) if (x >= r[i].left && x < r[i].right && y >= r[i].top && y < r[i].bottom) return TRUE;
    return FALSE;
}

GDIAPI BOOL RectInRegion(HRGN h, const RECT *rc)
{
    GObj *o = obj_of(h);
    if (!o || !rc) return FALSE;
    int n;
    const RECT *r = rgn_rects(o, &n);
    RECT x;
    for (int i = 0; i < n; i++) if (IntersectRect_(&x, &r[i], rc)) return TRUE;
    return FALSE;
}

GDIAPI BOOL EqualRgn(HRGN a, HRGN b)
{
    GObj *x = obj_of(a), *y = obj_of(b);
    if (!x || !y) return FALSE;
    int na, nb;
    const RECT *ra = rgn_rects(x, &na), *rb = rgn_rects(y, &nb);
    RECT *out;
    int n = rgn_op(ra, na, rb, nb, RGN_XOR_, &out);
    r_free(out);
    return n == 0;
}

GDIAPI int OffsetRgn(HRGN h, int x, int y)
{
    GObj *o = obj_of(h);
    if (!o) return ERROR;
    if (rect_empty(&o->rc)) return NULLREGION;
    OffsetRect_(&o->rc, x, y);
    for (int i = 0; i < o->nrects; i++) OffsetRect_(&o->rects[i], x, y);
    return rgn_kind(o);
}

/* RGNDATA: the region's rectangles */
GDIAPI DWORD GetRegionData(HRGN rgn, DWORD size, RGNDATA *out)
{
    GObj *o = obj_of(rgn);
    if (!o || o->kind != K_REGION) return 0;
    int n;
    const RECT *r = rgn_rects(o, &n);
    DWORD need = (DWORD)sizeof(RGNDATAHEADER) + (DWORD)n * sizeof(RECT);
    if (!out) return need;
    if (size < need) return 0;
    out->rdh.dwSize = sizeof(RGNDATAHEADER);
    out->rdh.iType = 1;                                     /* RDH_RECTANGLES */
    out->rdh.nCount = (DWORD)n;
    out->rdh.nRgnSize = (DWORD)n * sizeof(RECT);
    out->rdh.rcBound = o->rc;
    memcpy((BYTE *)out + sizeof(RGNDATAHEADER), r, (size_t)n * sizeof(RECT));
    return need;
}

/* -----------------------------------------------------------------------
 * The DC's clip region (device pixels; the program's coordinates are
 * relative to the window's origin, base_x/base_y)
 * ----------------------------------------------------------------------- */

/* The DC's clip as rectangles (*@n), or the whole surface when it has none */
static const RECT *dc_clip_rects(NOVA_DC *d, int *n, RECT *one)
{
    if (d->has_clip && d->nclip_rects > 1) { *n = d->nclip_rects; return d->clip_rects; }
    if (d->has_clip) *one = d->clip;
    else SetRect_(one, -0x4000000, -0x4000000, 0x4000000, 0x4000000);
    *n = rect_empty(one) ? 0 : 1;
    return one;
}

/* Make @r (@n rectangles, heap-allocated, taken over) the DC's clip */
static int dc_set_clip(NOVA_DC *d, RECT *r, int n)
{
    d->has_clip = 1;
    d->nclip_rects = 0;
    if (n <= 0) SetRectEmpty_(&d->clip);
    else {
        RECT box = r[0];
        for (int i = 1; i < n; i++) UnionRect_(&box, &box, &r[i]);
        d->clip = box;
        if (n > 1 && n <= NOVA_DC_CLIP_RECTS) {
            memcpy(d->clip_rects, r, sizeof(RECT) * n);
            d->nclip_rects = n;
        }
    }
    r_free(r);
    return n <= 0 ? NULLREGION : n > 1 ? COMPLEXREGION : SIMPLEREGION;
}

/* The DC's clip combined with device rectangles @b */
static int dc_combine(NOVA_DC *d, const RECT *b, int nb, int mode)
{
    if (mode == RGN_COPY_) {
        RECT *c = r_alloc(sizeof(RECT) * (nb ? nb : 1));
        if (!c) return ERROR;
        memcpy(c, b, sizeof(RECT) * nb);
        return dc_set_clip(d, c, nb);
    }
    RECT one;
    int na;
    const RECT *a = dc_clip_rects(d, &na, &one);
    RECT *out;
    int n = rgn_op(a, na, b, nb, mode, &out);
    if (n < 0) {
        RECT bb = nb ? b[0] : one, box;
        for (int i = 1; i < nb; i++) UnionRect_(&bb, &bb, &b[i]);
        box_op(&box, d->has_clip ? &d->clip : &one, &bb, mode);
        out = r_alloc(sizeof(RECT));
        if (!out) return ERROR;
        out[0] = box;
        n = rect_empty(&box) ? 0 : 1;
    }
    return dc_set_clip(d, out, n);
}

GDIAPI int ExtSelectClipRgn(HDC h, HRGN rgn, int mode)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return ERROR;
    GObj *o = obj_of(rgn);
    if (!o) {                                               /* NULL: no clipping (RGN_COPY) */
        if (mode != RGN_COPY_) return ERROR;
        d->has_clip = 0;
        d->nclip_rects = 0;
        return SIMPLEREGION;
    }
    int n;
    const RECT *r = rgn_rects(o, &n);
    RECT *dev = r_alloc(sizeof(RECT) * (n ? n : 1));
    if (!dev) return ERROR;
    for (int i = 0; i < n; i++) { dev[i] = r[i]; OffsetRect_(&dev[i], d->base_x, d->base_y); }
    int k = dc_combine(d, dev, n, mode);
    r_free(dev);
    return k;
}

GDIAPI int SelectClipRgn(HDC h, HRGN r) { return ExtSelectClipRgn(h, r, RGN_COPY_); }

GDIAPI int IntersectClipRect(HDC h, int l, int t, int r, int b)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return ERROR;
    RECT n = { l + d->org_x, t + d->org_y, r + d->org_x, b + d->org_y };
    if (rect_empty(&n)) SetRectEmpty_(&n);
    return dc_combine(d, &n, rect_empty(&n) ? 0 : 1, RGN_AND_);
}

GDIAPI int ExcludeClipRect(HDC h, int l, int t, int r, int b)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return ERROR;
    if (!d->has_clip) { RECT all = { 0, 0, d->w, d->h }; d->clip = all; d->has_clip = 1; d->nclip_rects = 0; }
    RECT x = { l + d->org_x, t + d->org_y, r + d->org_x, b + d->org_y };
    if (rect_empty(&x)) return d->nclip_rects > 1 ? COMPLEXREGION : SIMPLEREGION;
    return dc_combine(d, &x, 1, RGN_DIFF_);
}

GDIAPI int OffsetClipRgn(HDC h, int x, int y)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return ERROR;
    if (!d->has_clip) return SIMPLEREGION;
    OffsetRect_(&d->clip, x, y);
    for (int i = 0; i < d->nclip_rects; i++) OffsetRect_(&d->clip_rects[i], x, y);
    return rect_empty(&d->clip) ? NULLREGION : d->nclip_rects > 1 ? COMPLEXREGION : SIMPLEREGION;
}

GDIAPI int GetClipBox(HDC h, LPRECT r)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return ERROR;
    RECT e = { 0, 0, d->w, d->h };
    int k = dev_clip(d, &e);
    if (!k) SetRectEmpty_(&e);
    r->left = e.left - d->org_x; r->top = e.top - d->org_y; r->right = e.right - d->org_x; r->bottom = e.bottom - d->org_y;
    return !k ? NULLREGION : clip_pieces(d) ? COMPLEXREGION : SIMPLEREGION;
}

/* The program's clip region (not the window's visible part): 0 if none */
GDIAPI int GetClipRgn(HDC h, HRGN r)
{
    NOVA_DC *d = dc_of(h);
    GObj *o = obj_of(r);
    if (!d || !o) return -1;
    if (!d->has_clip) return 0;
    int n = d->nclip_rects > 1 ? d->nclip_rects : rect_empty(&d->clip) ? 0 : 1;
    RECT *c = r_alloc(sizeof(RECT) * (n ? n : 1));
    if (!c) return -1;
    for (int i = 0; i < n; i++) {
        c[i] = d->nclip_rects > 1 ? d->clip_rects[i] : d->clip;
        OffsetRect_(&c[i], -d->base_x, -d->base_y);
    }
    rgn_set(o, c, n);
    return 1;
}

GDIAPI BOOL FillRgn(HDC h, HRGN r, HBRUSH b)
{
    NOVA_DC *d = dc_of(h);
    GObj *o = obj_of(r), *br = obj_of(b);
    if (!d || !o) return FALSE;
    int n;
    const RECT *rc = rgn_rects(o, &n);
    for (int i = 0; i < n; i++) fill(d, rc[i].left, rc[i].top, rc[i].right, rc[i].bottom, br ? br->color : 0);
    return TRUE;
}

GDIAPI BOOL PaintRgn(HDC h, HRGN r)
{
    NOVA_DC *d = dc_of(h);
    GObj *o = obj_of(r);
    if (!d || !o) return FALSE;
    int n;
    const RECT *rc = rgn_rects(o, &n);
    for (int i = 0; i < n; i++) fill(d, rc[i].left, rc[i].top, rc[i].right, rc[i].bottom, d->brush_color);
    return TRUE;
}
