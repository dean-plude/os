/*
 * syscursor.c — the system pointers: I-beam, busy ring, resize arrows,
 * hand, cross, "no" and the rest of Windows' IDC_* set.
 *
 * No permissively licensed cursor set has the whole Windows set (X.org's
 * MIT "whiteglass" lacks the diagonal resize arrows and the "no" sign;
 * Breeze, Bibata, DMZ and Phinger are GPL or CC-BY-SA), so the shapes are
 * outlines here, drawn like the desktop's arrow: each part is a union of
 * polygons, discs, rings and ring arcs with an anti-aliased outline and a
 * fill, so a 2x display gets twice the detail instead of bigger pixels.
 * Units are 1/16 logical pixel (GDI_PT); the shape's origin is its hot
 * spot.  Kept free of the screen so a test can render it on the host.
 */
#include "syscursor.h"
#include "gdi.h"

#define FX      256             /* 1/256 device pixel */
#define PTS_MAX 32

enum { P_POLY, P_DISC, P_RING, P_ARC };

typedef struct {
    int             kind;
    const GdiPoint *pts;        /* P_POLY */
    int             n;
    int             rot;        /* P_POLY: turned by rot/64 of a turn */
    GdiPoint        c;          /* P_DISC, P_RING, P_ARC: centre */
    int             r0, r1;     /* outer and inner radius (1/16 px) */
    int             a0, span;   /* P_ARC: from a0 for span (1/64 turns, span <= 32) */
    bool            spin;       /* P_ARC: turns with the phase */
} Prim;

typedef struct {
    const Prim *p;
    int         n;
    UINT32      fill, line;     /* 0xAARRGGBB; line 0: no outline */
    int         bw;             /* outline width, 1/16 px */
} Part;

typedef struct {
    int         id;
    int         hx, hy;         /* the hot spot in the box, logical px */
    GdiPoint    org;            /* where (0,0) is, from the hot pixel's corner */
    const Part *parts;
    int         nparts;
    bool        anim;
} Shape;

#define POLY(a, r)  { .kind = P_POLY, .pts = (a), .n = (int)(sizeof(a) / sizeof((a)[0])), .rot = (r) }
#define DISC(x, y, r) { .kind = P_DISC, .c = GDI_PT(x, y), .r0 = (int)((r) * 16) }
#define RING(x, y, R, r) { .kind = P_RING, .c = GDI_PT(x, y), .r0 = (int)((R) * 16), .r1 = (int)((r) * 16) }
#define ARC(x, y, R, r, a, s, sp) { .kind = P_ARC, .c = GDI_PT(x, y), .r0 = (int)((R) * 16), .r1 = (int)((r) * 16), .a0 = (a), .span = (s), .spin = (sp) }
#define PART(prims, f, l, w) { (prims), (int)(sizeof(prims) / sizeof((prims)[0])), (f), (l), (int)((w) * 16) }

#define WHITE   0xFFFFFFFFu
#define BLACK   0xFF000000u
#define BLUE    0xFF1A86E0u
#define RED     0xFFE0251Bu
#define TRACK   0xB0E8EEF4u

/* sin(2 pi i / 64) * 4096 */
static const INT16 g_sin[64] = {
    0, 401, 799, 1189, 1567, 1931, 2276, 2598, 2896, 3166, 3406, 3612, 3784, 3920, 4017, 4076,
    4096, 4076, 4017, 3920, 3784, 3612, 3406, 3166, 2896, 2598, 2276, 1931, 1567, 1189, 799, 401,
    0, -401, -799, -1189, -1567, -1931, -2276, -2598, -2896, -3166, -3406, -3612, -3784, -3920, -4017, -4076,
    -4096, -4076, -4017, -3920, -3784, -3612, -3406, -3166, -2896, -2598, -2276, -1931, -1567, -1189, -799, -401,
};
static inline int isin(int a) { return g_sin[a & 63]; }
static inline int icos(int a) { return g_sin[(a + 16) & 63]; }

/* --- the arrow (the desktop's own, as before) ----------------------------- */
static const GdiPoint s_arrow[] = {
    GDI_PT(0, 0),   GDI_PT(0, 17),   GDI_PT(4, 13.2), GDI_PT(6.9, 19.6),
    GDI_PT(9.6, 18.4), GDI_PT(6.8, 12.2), GDI_PT(12.2, 12.2),
};
static const Prim pr_arrow[] = { POLY(s_arrow, 0) };
static const Part pa_arrow[] = { PART(pr_arrow, WHITE, BLACK, 1) };

/* --- I-beam: black with a white rim --------------------------------------- */
static const GdiPoint s_ibeam[] = {
    GDI_PT(-4.5, -9.5), GDI_PT(-0.5, -9.5), GDI_PT(0, -9), GDI_PT(0.5, -9.5), GDI_PT(4.5, -9.5),
    GDI_PT(4.5, -6.5), GDI_PT(1.5, -6.5), GDI_PT(1.5, 6.5), GDI_PT(4.5, 6.5), GDI_PT(4.5, 9.5),
    GDI_PT(0.5, 9.5), GDI_PT(0, 9), GDI_PT(-0.5, 9.5), GDI_PT(-4.5, 9.5), GDI_PT(-4.5, 6.5),
    GDI_PT(-1.5, 6.5), GDI_PT(-1.5, -6.5), GDI_PT(-4.5, -6.5),
};
static const Prim pr_ibeam[] = { POLY(s_ibeam, 0) };
static const Part pa_ibeam[] = { PART(pr_ibeam, BLACK, WHITE, 1) };

/* --- cross ----------------------------------------------------------------- */
static const GdiPoint s_cross[] = {
    GDI_PT(-1.5, -10.5), GDI_PT(1.5, -10.5), GDI_PT(1.5, -1.5), GDI_PT(10.5, -1.5),
    GDI_PT(10.5, 1.5), GDI_PT(1.5, 1.5), GDI_PT(1.5, 10.5), GDI_PT(-1.5, 10.5),
    GDI_PT(-1.5, 1.5), GDI_PT(-10.5, 1.5), GDI_PT(-10.5, -1.5), GDI_PT(-1.5, -1.5),
};
static const Prim pr_cross[] = { POLY(s_cross, 0) };
static const Part pa_cross[] = { PART(pr_cross, BLACK, WHITE, 1) };

/* --- up arrow --------------------------------------------------------------- */
static const GdiPoint s_up[] = {
    GDI_PT(0.5, -0.5), GDI_PT(6.5, 5.5), GDI_PT(2, 5.5), GDI_PT(2, 18),
    GDI_PT(-1, 18), GDI_PT(-1, 5.5), GDI_PT(-5.5, 5.5),
};
static const Prim pr_up[] = { POLY(s_up, 0) };
static const Part pa_up[] = { PART(pr_up, WHITE, BLACK, 1) };

/* --- resize arrows: one double arrow, turned ------------------------------- */
static const GdiPoint s_dbl[] = {
    GDI_PT(-11, 0), GDI_PT(-6, -5), GDI_PT(-6, -1.5), GDI_PT(6, -1.5), GDI_PT(6, -5),
    GDI_PT(11, 0), GDI_PT(6, 5), GDI_PT(6, 1.5), GDI_PT(-6, 1.5), GDI_PT(-6, 5),
};
static const Prim pr_we[]   = { POLY(s_dbl, 0) };
static const Prim pr_ns[]   = { POLY(s_dbl, 16) };
static const Prim pr_nwse[] = { POLY(s_dbl, 8) };
static const Prim pr_nesw[] = { POLY(s_dbl, 56) };
static const Part pa_we[]   = { PART(pr_we, WHITE, BLACK, 1) };
static const Part pa_ns[]   = { PART(pr_ns, WHITE, BLACK, 1) };
static const Part pa_nwse[] = { PART(pr_nwse, WHITE, BLACK, 1) };
static const Part pa_nesw[] = { PART(pr_nesw, WHITE, BLACK, 1) };

/* --- move (four arrows) ----------------------------------------------------- */
static const GdiPoint s_all[] = {
    GDI_PT(0, -11), GDI_PT(4.5, -6.5), GDI_PT(1.5, -6.5), GDI_PT(1.5, -1.5),
    GDI_PT(6.5, -1.5), GDI_PT(6.5, -4.5), GDI_PT(11, 0), GDI_PT(6.5, 4.5),
    GDI_PT(6.5, 1.5), GDI_PT(1.5, 1.5), GDI_PT(1.5, 6.5), GDI_PT(4.5, 6.5),
    GDI_PT(0, 11), GDI_PT(-4.5, 6.5), GDI_PT(-1.5, 6.5), GDI_PT(-1.5, 1.5),
    GDI_PT(-6.5, 1.5), GDI_PT(-6.5, 4.5), GDI_PT(-11, 0), GDI_PT(-6.5, -4.5),
    GDI_PT(-6.5, -1.5), GDI_PT(-1.5, -1.5), GDI_PT(-1.5, -6.5), GDI_PT(-4.5, -6.5),
};
static const Prim pr_all[] = { POLY(s_all, 0) };
static const Part pa_all[] = { PART(pr_all, WHITE, BLACK, 1) };

/* --- "no": a red ring with a bar ------------------------------------------- */
static const GdiPoint s_bar[] = {
    GDI_PT(-6.5, -1.75), GDI_PT(6.5, -1.75), GDI_PT(6.5, 1.75), GDI_PT(-6.5, 1.75),
};
static const Prim pr_no[] = { RING(0, 0, 9, 5.5), POLY(s_bar, 8) };
static const Part pa_no[] = { PART(pr_no, RED, BLACK, 1) };

/* --- hand: index finger up, the tip is the hot spot -------------------------- */
static const GdiPoint s_index[]  = { GDI_PT(-2, 2.25), GDI_PT(2.5, 2.25), GDI_PT(2.5, 14), GDI_PT(-2, 14) };
static const GdiPoint s_middle[] = { GDI_PT(2.5, 8), GDI_PT(6.5, 8), GDI_PT(6.5, 14), GDI_PT(2.5, 14) };
static const GdiPoint s_ring[]   = { GDI_PT(6.5, 9), GDI_PT(10.5, 9), GDI_PT(10.5, 14), GDI_PT(6.5, 14) };
static const GdiPoint s_pinky[]  = { GDI_PT(10.5, 10.5), GDI_PT(14, 10.5), GDI_PT(14, 16), GDI_PT(10.5, 16) };
static const GdiPoint s_thumb[]  = { GDI_PT(-2, 18), GDI_PT(-6.9, 12.1), GDI_PT(-4.4, 10.1), GDI_PT(-0.5, 14.5) };
static const GdiPoint s_palm[]   = {
    GDI_PT(-2, 13), GDI_PT(14, 13), GDI_PT(14, 17), GDI_PT(12, 22),
    GDI_PT(2, 22), GDI_PT(-1, 19.5),
};
static const Prim pr_hand[] = {
    DISC(0.25, 2.25, 2.25), POLY(s_index, 0), DISC(4.5, 8, 2), POLY(s_middle, 0), DISC(8.5, 9, 2),
    POLY(s_ring, 0), DISC(12.25, 10.5, 1.75), POLY(s_pinky, 0), DISC(-5.65, 11.1, 1.6),
    POLY(s_thumb, 0), POLY(s_palm, 0),
};
/* the lines between the fingers */
static const GdiPoint s_gap1[] = { GDI_PT(2, 10), GDI_PT(3, 10), GDI_PT(3, 12.5), GDI_PT(2, 12.5) };
static const GdiPoint s_gap2[] = { GDI_PT(6, 11), GDI_PT(7, 11), GDI_PT(7, 13.5), GDI_PT(6, 13.5) };
static const GdiPoint s_gap3[] = { GDI_PT(10, 12.5), GDI_PT(11, 12.5), GDI_PT(11, 15), GDI_PT(10, 15) };
static const Prim pr_gaps[] = { POLY(s_gap1, 0), POLY(s_gap2, 0), POLY(s_gap3, 0) };
static const Part pa_hand[] = { PART(pr_hand, WHITE, BLACK, 1), PART(pr_gaps, 0xFF000000u, 0, 0) };

/* --- busy: a ring with a turning arc ----------------------------------------- */
static const Prim pr_track[] = { RING(0, 0, 9.5, 4.5) };
static const Prim pr_spin[]  = { ARC(0, 0, 8.5, 5.5, 0, 22, true) };
static const Part pa_wait[]  = { PART(pr_track, TRACK, 0xFF303030u, 1), PART(pr_spin, BLUE, 0, 0) };

/* --- working in background: the arrow and a small ring ----------------------- */
static const Prim pr_strack[] = { RING(16.5, 18.5, 6, 2.5) };
static const Prim pr_sspin[]  = { ARC(16.5, 18.5, 5, 3.5, 0, 22, true) };
static const Part pa_start[]  = {
    PART(pr_arrow, WHITE, BLACK, 1), PART(pr_strack, TRACK, 0xFF303030u, 1), PART(pr_sspin, BLUE, 0, 0),
};

/* --- help: the arrow and a question mark ------------------------------------- */
static const GdiPoint s_qstem[] = { GDI_PT(15.25, 8.5), GDI_PT(18.25, 8.5), GDI_PT(18.25, 13), GDI_PT(15.25, 13) };
static const Prim pr_quest[] = {
    ARC(16.75, 5.5, 4.75, 1.5, 32, 32, false), ARC(16.75, 5.5, 4.75, 1.5, 0, 16, false),
    POLY(s_qstem, 0), DISC(16.75, 16, 1.9),
};
static const Part pa_help[] = { PART(pr_arrow, WHITE, BLACK, 1), PART(pr_quest, WHITE, BLACK, 1) };

#define SHAPE(i, x, y, ox, oy, p, a) { (i), (x), (y), GDI_PT(ox, oy), (p), (int)(sizeof(p) / sizeof((p)[0])), (a) }
static const Shape g_shapes[] = {
    SHAPE(OCR_NORMAL,      1,  1,  0,   0,   pa_arrow, false),
    SHAPE(OCR_IBEAM,       15, 15, 0.5, 0.5, pa_ibeam, false),
    SHAPE(OCR_WAIT,        15, 15, 0.5, 0.5, pa_wait,  true),
    SHAPE(OCR_CROSS,       15, 15, 0.5, 0.5, pa_cross, false),
    SHAPE(OCR_UP,          15, 1,  0,   0,   pa_up,    false),
    SHAPE(OCR_SIZENWSE,    15, 15, 0.5, 0.5, pa_nwse,  false),
    SHAPE(OCR_SIZENESW,    15, 15, 0.5, 0.5, pa_nesw,  false),
    SHAPE(OCR_SIZEWE,      15, 15, 0.5, 0.5, pa_we,    false),
    SHAPE(OCR_SIZENS,      15, 15, 0.5, 0.5, pa_ns,    false),
    SHAPE(OCR_SIZEALL,     15, 15, 0.5, 0.5, pa_all,   false),
    SHAPE(OCR_NO,          15, 15, 0.5, 0.5, pa_no,    false),
    SHAPE(OCR_HAND,        8,  1,  0,   0,   pa_hand,  false),
    SHAPE(OCR_APPSTARTING, 1,  1,  0,   0,   pa_start, true),
    SHAPE(OCR_HELP,        1,  1,  0,   0,   pa_help,  false),
};
#define NSHAPES ((int)(sizeof(g_shapes) / sizeof(g_shapes[0])))

int SysCursorCanon(int id)
{
    switch (id) {
    case OCR_SIZE:   return OCR_SIZEALL;
    case OCR_ICON:   return OCR_NORMAL;
    case OCR_PIN:
    case OCR_PERSON: return OCR_HAND;
    }
    for (int i = 0; i < NSHAPES; i++)
        if (g_shapes[i].id == id) return id;
    return 0;
}

static const Shape *shape_of(int id)
{
    id = SysCursorCanon(id);
    for (int i = 0; i < NSHAPES; i++)
        if (g_shapes[i].id == id) return &g_shapes[i];
    return &g_shapes[0];
}

bool SysCursorAnimated(int id) { return shape_of(id)->anim; }

/* ------------------------------------------------------------------------
 * Rasterizing
 * ------------------------------------------------------------------------ */
static UINT32 isqrt64(UINT64 v)
{
    UINT64 r = 0, bit = (UINT64)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return (UINT32)r;
}

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

/* Signed distance (negative inside) from (px, py) to a polygon, all in
 * 1/256 device px */
static int poly_sd(const GdiPoint *p, int n, int px, int py)
{
    INT64 best = -1;
    bool  in   = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        INT64 ax = p[j].x, ay = p[j].y, bx = p[i].x, by = p[i].y;
        if (((ay > py) != (by > py)) && (px < ax + (bx - ax) * (py - ay) / (by - ay)))
            in = !in;
        INT64 ex = bx - ax, ey = by - ay, wx = px - ax, wy = py - ay;
        INT64 len2 = ex * ex + ey * ey, cx = ax, cy = ay;
        if (len2 > 0) {
            INT64 dot = wx * ex + wy * ey;
            if (dot >= len2)  { cx = bx; cy = by; }
            else if (dot > 0) { cx = ax + ex * dot / len2; cy = ay + ey * dot / len2; }
        }
        INT64 dx = px - cx, dy = py - cy, d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < best) best = d2;
    }
    int d = (int)isqrt64((UINT64)(best < 0 ? 0 : best));
    return in ? -d : d;
}

/* A primitive in device units */
typedef struct {
    int      kind, n;
    GdiPoint pts[PTS_MAX];
    int      cx, cy, r0, r1;
    int      u0x, u0y, u1x, u1y;    /* P_ARC: the wedge's sides, *4096 */
} DevPrim;

static int prim_sd(const DevPrim *d, int px, int py)
{
    if (d->kind == P_POLY) return poly_sd(d->pts, d->n, px, py);
    INT64 dx = px - d->cx, dy = py - d->cy;
    int r = (int)isqrt64((UINT64)(dx * dx + dy * dy));
    if (d->kind == P_DISC) return r - d->r0;
    int sd = imax(r - d->r0, d->r1 - r);
    if (d->kind == P_ARC) {
        int a = (int)(-(d->u0x * dy - d->u0y * dx) / 4096);     /* outside the first side */
        int b = (int)((d->u1x * dy - d->u1y * dx) / 4096);      /* outside the second */
        sd = imax(sd, imax(a, b));
    }
    return sd;
}

static inline int cov_of(int sd)
{
    int c = FX / 2 - sd;
    if (c <= 0) return 0;
    if (c >= FX) return 255;
    return (c * 255) / FX;
}

/* Premultiplied accumulator, 0..255 per channel */
typedef struct { int a, r, g, b; } Acc;

static inline void over(Acc *d, UINT32 c, int cov)
{
    int sa = (int)(c >> 24) * cov / 255;
    if (sa <= 0) return;
    int na = 255 - sa;
    d->r = ((int)((c >> 16) & 0xFF) * sa + d->r * na) / 255;
    d->g = ((int)((c >> 8) & 0xFF) * sa + d->g * na) / 255;
    d->b = ((int)(c & 0xFF) * sa + d->b * na) / 255;
    d->a = sa + d->a * na / 255;
}

#define MAX_PRIMS 12
#define MAX_PARTS 3

void SysCursorRender(int id, int scale, int phase, bool shadow, UINT32 *argb, int *hx, int *hy)
{
    const Shape *sh = shape_of(id);
    int s = scale < 1 ? 1 : scale;
    int side = SYSCUR_BOX * s, f = s * (FX / 16);
    /* the shape's origin in the box, 1/256 device px */
    int ox = sh->hx * s * FX + sh->org.x * f, oy = sh->hy * s * FX + sh->org.y * f;
    if (hx) *hx = sh->hx * s;
    if (hy) *hy = sh->hy * s;

    static DevPrim dev[MAX_PARTS][MAX_PRIMS];
    int np[MAX_PARTS];
    int nparts = imin(sh->nparts, MAX_PARTS);
    for (int k = 0; k < nparts; k++) {
        const Part *pa = &sh->parts[k];
        np[k] = imin(pa->n, MAX_PRIMS);
        for (int i = 0; i < np[k]; i++) {
            const Prim *p = &pa->p[i];
            DevPrim *d = &dev[k][i];
            d->kind = p->kind;
            if (p->kind == P_POLY) {
                int c = icos(p->rot), sn = isin(p->rot);
                d->n = imin(p->n, PTS_MAX);
                for (int j = 0; j < d->n; j++) {
                    INT64 x = (INT64)p->pts[j].x * f, y = (INT64)p->pts[j].y * f;
                    d->pts[j].x = ox + (int)((x * c - y * sn) / 4096);
                    d->pts[j].y = oy + (int)((x * sn + y * c) / 4096);
                }
            } else {
                d->cx = ox + p->c.x * f; d->cy = oy + p->c.y * f;
                d->r0 = p->r0 * f; d->r1 = p->r1 * f;
                int a = p->a0 + (p->spin ? phase : 0);
                d->u0x = icos(a); d->u0y = isin(a);
                d->u1x = icos(a + p->span); d->u1y = isin(a + p->span);
            }
        }
    }

    int soft = 2 * s * FX;                       /* the shadow, as the desktop's arrow had it */
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            int px = x * FX + FX / 2, py = y * FX + FX / 2;
            Acc acc = { 0, 0, 0, 0 };
            if (shadow) {
                int ss = 0x7FFFFFFF;
                for (int k = 0; k < nparts; k++)
                    for (int i = 0; i < np[k]; i++) ss = imin(ss, prim_sd(&dev[k][i], px - s * FX, py - 2 * s * FX));
                if (ss < soft) over(&acc, 0xFF000000u, (70 * (ss <= 0 ? 255 : (soft - ss) * 255 / soft)) / 255);
            }
            for (int k = 0; k < nparts; k++) {
                const Part *pa = &sh->parts[k];
                int sd = 0x7FFFFFFF;
                for (int i = 0; i < np[k]; i++) sd = imin(sd, prim_sd(&dev[k][i], px, py));
                if (sd > FX) continue;
                if (pa->line) {
                    over(&acc, pa->line, cov_of(sd));
                    over(&acc, pa->fill, cov_of(sd + pa->bw * f));
                } else {
                    over(&acc, pa->fill, cov_of(sd));
                }
            }
            UINT32 out = 0;
            if (acc.a > 0) {
                int r = imin(acc.r * 255 / acc.a, 255), g = imin(acc.g * 255 / acc.a, 255), b = imin(acc.b * 255 / acc.a, 255);
                out = (UINT32)acc.a << 24 | (UINT32)r << 16 | (UINT32)g << 8 | (UINT32)b;
            }
            argb[y * side + x] = out;
        }
    }
}
