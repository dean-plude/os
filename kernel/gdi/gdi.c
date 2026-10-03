/*
 * gdi.c — NovaOS Graphics Device Interface (software renderer)
 *
 * Public coordinates are logical pixels; everything is rasterized at
 * device resolution (logical × g.s).  Anti-aliasing is computed from a
 * signed distance to each shape's edge in 1/256-pixel fixed point, so no
 * floating point is needed (the kernel is built without SSE).
 *
 * Drawing targets a back buffer in RAM.  GdiPresent() copies it to the
 * framebuffer, so partially drawn frames are never visible and blending
 * reads come from fast RAM rather than uncached VRAM.
 *
 * Blending works directly on native pixels: RGB and BGR framebuffers both
 * keep one channel per byte, and a per-channel blend doesn't care which
 * byte holds which channel.
 */

#include "gdi.h"
#include "font.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../hal/display.h"
#include "syscursor.h"

#define FX            256     /* fixed-point one: 1/256 device pixel */
#define LINE_BASELINE  12     /* baseline within the 16px logical line box */
#define LARGE_BASELINE 23     /* baseline within the 30px heading line box */

/* -----------------------------------------------------------------------
 * Surface state
 *
 * The back buffer covers the whole virtual desktop: every monitor's
 * rectangle, laid out around the primary one at (0, 0) (so coordinates
 * left of or above it are negative).  g.buf points at the primary's top
 * left corner inside it; [bx0, bx1) x [by0, by1) are its device bounds.
 * ----------------------------------------------------------------------- */
static struct {
    UINT32 *vram;  int vstride;   /* the primary's framebuffer (page being drawn) */
    UINT32 *base;                 /* back buffer allocation */
    UINT32 *buf;   int bstride;   /* back buffer at device (0, 0) (== vram if none) */
    int     bx0, by0, bx1, by1;   /* back buffer bounds, device px */
    int     dw, dh;               /* the primary's device size */
    int     s;                    /* scale: device px per logical px */
    int     lw, lh;               /* the primary's logical size */
    int     cx0, cy0, cx1, cy1;   /* clip rectangle, device px, [x0,x1) */
    UINT32 *cache;                /* saved copy of the back buffer */
    bool    cache_valid;
    bool    direct;               /* no back buffer: drawing goes to vram */
    bool    bgr, ready;
} g;

/* The monitors: one per display head (hal/display.h), monitor 0 the primary */
typedef struct {
    GdiRect r;                    /* logical, on the virtual desktop */
    int     s;                    /* its own scale (device px per logical px) */
    int     k;                    /* g.s / s: back buffer px per screen px */
    int     dw, dh;               /* screen px shown (r.w * s, r.h * s) */
} Monitor;
static Monitor g_mon[GDI_MAX_MONITORS];
static int     g_nmon;
static struct { int x, y; bool set; } g_origin[GDI_MAX_MONITORS];

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int a)        { return a < 0 ? -a : a; }

/* Logical GdiColor (0x00BBGGRR) → native framebuffer pixel */
static inline UINT32 pixof(GdiColor c)
{
    UINT32 r = c & 0xFF, gg = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    if (g.bgr)
        return (r << 16) | (gg << 8) | b;   /* byte0 = Blue */
    return (b << 16) | (gg << 8) | r;       /* byte0 = Red  */
}

/* d + (s - d) * a / 255, per channel, on packed pixels */
static inline UINT32 blend(UINT32 d, UINT32 s, UINT32 a)
{
    UINT32 na = 255 - a;
    UINT32 rb = (s & 0xFF00FF) * a + (d & 0xFF00FF) * na;
    UINT32 gg = (s & 0x00FF00) * a + (d & 0x00FF00) * na;
    rb = ((rb + 0x800080 + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;
    gg = ((gg + 0x008000 + ((gg >> 8) & 0x00FF00)) >> 8) & 0x00FF00;
    return rb | gg;
}

/* Blend one device pixel into the back buffer; a = coverage 0..255 */
static inline void plot(int x, int y, UINT32 n, int a)
{
    if (a <= 0 || x < g.cx0 || x >= g.cx1 || y < g.cy0 || y >= g.cy1)
        return;
    UINT32 *p = &g.buf[(INT64)y * g.bstride + x];
    *p = (a >= 255) ? n : blend(*p, n, (UINT32)a);
}

/* Horizontal device span [x0, x1) at uniform coverage a */
static void span(int y, int x0, int x1, UINT32 n, int a)
{
    if (a <= 0 || y < g.cy0 || y >= g.cy1) return;
    x0 = imax(x0, g.cx0);
    x1 = imin(x1, g.cx1);
    UINT32 *p = &g.buf[(INT64)y * g.bstride];
    if (a >= 255) for (int x = x0; x < x1; x++) p[x] = n;
    else          for (int x = x0; x < x1; x++) p[x] = blend(p[x], n, (UINT32)a);
}

/* Signed distance → coverage 0..255 (pixel-centre sampling, 1px ramp) */
static inline int cov_of(int sd)
{
    int c = FX / 2 - sd;
    if (c <= 0) return 0;
    if (c >= FX) return 255;
    return (c * 255) / FX;
}

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

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */
/* The scale that makes a w x h screen at least 1280x800 logical pixels */
static int natural_scale(int w, int h)
{
    int s = imin(w / 1280, h / 800);
    return s < 1 ? 1 : s > GDI_MAX_SCALE ? GDI_MAX_SCALE : s;
}

static bool overlaps(GdiRect a, GdiRect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* Touching along an edge (or overlapping) */
static bool touches(GdiRect a, GdiRect b)
{
    bool xs = a.x <= b.x + b.w && b.x <= a.x + a.w, ys = a.y <= b.y + b.h && b.y <= a.y + a.h;
    bool side = (a.x + a.w == b.x || b.x + b.w == a.x) && a.y < b.y + b.h && b.y < a.y + a.h;
    bool tb   = (a.y + a.h == b.y || b.y + b.h == a.y) && a.x < b.x + b.w && b.x < a.x + a.w;
    return xs && ys && (side || tb || overlaps(a, b));
}

/* Lay the monitors out: the primary at (0, 0), each other one where it was
 * placed (GdiSetMonitorOrigin) if that neither overlaps a monitor before
 * it nor leaves it apart from them, else to the right of the others */
static void layout_monitors(int n, const int *w, const int *h, const int *ms)
{
    g_nmon = n;
    for (int i = 0; i < n; i++) {
        Monitor *m = &g_mon[i];
        m->s = ms[i];
        m->r = RECT(0, 0, w[i] / ms[i], h[i] / ms[i]);
        m->dw = m->r.w * m->s;
        m->dh = m->r.h * m->s;
        if (i == 0) continue;
        bool ok = g_origin[i].set;
        if (ok) {
            m->r.x = g_origin[i].x;
            m->r.y = g_origin[i].y;
            bool near = false;
            for (int j = 0; j < i && ok; j++) {
                if (overlaps(m->r, g_mon[j].r)) ok = false;
                if (touches(m->r, g_mon[j].r)) near = true;
            }
            ok = ok && near;
        }
        if (!ok) {
            int right = 0;
            for (int j = 0; j < i; j++) right = imax(right, g_mon[j].r.x + g_mon[j].r.w);
            m->r.x = right;
            m->r.y = 0;
        }
    }
}

/* (Re)read the screen surfaces: at boot, and after a display mode change */
static bool gdi_setup(void)
{
    FbRawSurface s;
    fb_get_raw(&s);
    if (!s.vram || s.width <= 0 || s.height <= 0) {
        g.ready = false;
        return false;
    }
    g.vram    = s.vram;
    g.vstride = s.stride;
    g.dw      = s.width;
    g.dh      = s.height;
    g.bgr     = s.bgr;

    /* Each monitor at its own scale; the desktop is drawn at the largest,
     * and monitors at half of it get every 2x2 block averaged */
    int n = imax(1, imin(DisplayHeadCount(), GDI_MAX_MONITORS));
    int w[GDI_MAX_MONITORS], h[GDI_MAX_MONITORS], ms[GDI_MAX_MONITORS];
    w[0] = g.dw; h[0] = g.dh;
    for (int i = 1; i < n; i++) {
        DisplayMode m = DisplayHeadMode(i);
        w[i] = m.w; h[i] = m.h;
    }
    g.s = 1;
    for (int i = 0; i < n; i++) {
        ms[i] = natural_scale(w[i], h[i]);
        g.s = imax(g.s, ms[i]);
    }
    layout_monitors(n, w, h, ms);
    for (int i = 0; i < n; i++) g_mon[i].k = g.s / g_mon[i].s;
    g.lw = g_mon[0].r.w;
    g.lh = g_mon[0].r.h;

    GdiRect v = GdiVirtualRect();
    int bx0 = v.x * g.s, by0 = v.y * g.s, bx1 = (v.x + v.w) * g.s, by1 = (v.y + v.h) * g.s;
    bool resized = g.direct || bx0 != g.bx0 || by0 != g.by0 || bx1 != g.bx1 || by1 != g.by1;

    /* Back buffer; fall back to drawing on the framebuffer directly (the
     * primary monitor only) */
    if (resized) {
        if (g.base) kfree(g.base);
        kfree(g.cache);
        g.cache = NULL;
        g.base = kzalloc((size_t)(bx1 - bx0) * (by1 - by0) * sizeof(UINT32));
        g.direct = g.base == NULL;
    }
    if (g.direct) {
        g.base = NULL;
        g.buf     = g.vram;
        g.bstride = g.vstride;
        g_nmon = 1;
        g_mon[0].k = 1;
        g.s = g_mon[0].s;
        bx0 = by0 = 0; bx1 = g.dw; by1 = g.dh;
    } else {
        g.bstride = bx1 - bx0;
        g.buf = g.base - (INT64)by0 * g.bstride - bx0;
    }
    g.bx0 = bx0; g.by0 = by0; g.bx1 = bx1; g.by1 = by1;
    g.cache_valid = false;

    g.cx0 = g.bx0; g.cy0 = g.by0; g.cx1 = g.bx1; g.cy1 = g.by1;
    g.ready = true;
    kprintf("[GDI] %dx%d device, scale %dx -> %dx%d logical, %s\n",
            g.dw, g.dh, g.s, g.lw, g.lh,
            g.direct ? "direct (no back buffer)" :
            DisplayCanFlip() ? "double-buffered, page flipping" : "double-buffered");
    for (int i = 1; i < g_nmon; i++)
        kprintf("[GDI] Monitor %d: %dx%d at (%d, %d), scale %dx\n", i + 1,
                g_mon[i].r.w, g_mon[i].r.h, g_mon[i].r.x, g_mon[i].r.y, g_mon[i].s);
    return true;
}

bool GdiInitialize(void) { return gdi_setup(); }
bool GdiDisplayChanged(void) { return gdi_setup(); }

/* -----------------------------------------------------------------------
 * Monitors
 * ----------------------------------------------------------------------- */
int GdiMonitorCount(void) { return g.ready ? g_nmon : 0; }

GdiRect GdiMonitorRect(int i)
{
    if (!g.ready || i < 0 || i >= g_nmon) return RECT(0, 0, 0, 0);
    return g_mon[i].r;
}

int GdiMonitorScale(int i) { return g.ready && i >= 0 && i < g_nmon ? g_mon[i].s : 1; }

GdiRect GdiVirtualRect(void)
{
    if (!g_nmon) return RECT(0, 0, g.lw, g.lh);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < g_nmon; i++) {
        GdiRect r = g_mon[i].r;
        if (i == 0 || r.x < x0) x0 = r.x;
        if (i == 0 || r.y < y0) y0 = r.y;
        if (i == 0 || r.x + r.w > x1) x1 = r.x + r.w;
        if (i == 0 || r.y + r.h > y1) y1 = r.y + r.h;
    }
    return RECT(x0, y0, x1 - x0, y1 - y0);
}

int GdiMonitorAt(int x, int y)
{
    for (int i = 0; g.ready && i < g_nmon; i++) {
        GdiRect r = g_mon[i].r;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
    }
    return -1;
}

/* Squared distance from a point to a rectangle (0 inside) */
static INT64 dist2(GdiRect r, int x, int y)
{
    INT64 dx = x < r.x ? r.x - x : x >= r.x + r.w ? x - (r.x + r.w - 1) : 0;
    INT64 dy = y < r.y ? r.y - y : y >= r.y + r.h ? y - (r.y + r.h - 1) : 0;
    return dx * dx + dy * dy;
}

int GdiMonitorNearest(GdiRect r)
{
    if (!g.ready || g_nmon <= 1) return 0;
    int best = 0;
    INT64 area = -1, d = -1;
    for (int i = 0; i < g_nmon; i++) {
        GdiRect m = g_mon[i].r;
        INT64 w = imin(r.x + r.w, m.x + m.w) - imax(r.x, m.x), h = imin(r.y + r.h, m.y + m.h) - imax(r.y, m.y);
        INT64 a = w > 0 && h > 0 ? w * h : 0;
        if (a > area) { area = a; best = i; }
    }
    if (area > 0) return best;
    for (int i = 0; i < g_nmon; i++) {             /* none overlaps: the closest to its centre */
        INT64 e = dist2(g_mon[i].r, r.x + r.w / 2, r.y + r.h / 2);
        if (d < 0 || e < d) { d = e; best = i; }
    }
    return best;
}

void GdiClampToMonitors(int *x, int *y)
{
    if (!g.ready || GdiMonitorAt(*x, *y) >= 0) return;
    int best = 0;
    INT64 d = -1;
    for (int i = 0; i < g_nmon; i++) {
        INT64 e = dist2(g_mon[i].r, *x, *y);
        if (d < 0 || e < d) { d = e; best = i; }
    }
    GdiRect r = g_mon[best].r;
    *x = imin(imax(*x, r.x), r.x + r.w - 1);
    *y = imin(imax(*y, r.y), r.y + r.h - 1);
}

void GdiForgetMonitor(int i)
{
    if (i <= 0 || i >= GDI_MAX_MONITORS) return;
    for (int j = i; j < GDI_MAX_MONITORS - 1; j++) g_origin[j] = g_origin[j + 1];
    g_origin[GDI_MAX_MONITORS - 1].set = false;
}

void GdiSetMonitorOrigin(int i, int x, int y)
{
    if (i <= 0 || i >= GDI_MAX_MONITORS) return;
    g_origin[i].x = x;
    g_origin[i].y = y;
    g_origin[i].set = true;
}

/* -----------------------------------------------------------------------
 * Clipping and the frame cache
 * ----------------------------------------------------------------------- */
void GdiSetClip(GdiRect r)
{
    int s = g.s;
    g.cx0 = imax(r.x * s, g.bx0);            g.cy0 = imax(r.y * s, g.by0);
    g.cx1 = imin((r.x + r.w) * s, g.bx1);    g.cy1 = imin((r.y + r.h) * s, g.by1);
    if (g.cx1 < g.cx0) g.cx1 = g.cx0;
    if (g.cy1 < g.cy0) g.cy1 = g.cy0;
}

void GdiResetClip(void)
{
    g.cx0 = g.bx0; g.cy0 = g.by0; g.cx1 = g.bx1; g.cy1 = g.by1;
}

static size_t buf_bytes(void) { return (size_t)(g.bx1 - g.bx0) * (g.by1 - g.by0) * sizeof(UINT32); }

bool GdiCacheSave(void)
{
    if (!g.ready || g.direct) return false;
    if (!g.cache) {
        g.cache = kmalloc(buf_bytes());
        if (!g.cache) return false;
    }
    memcpy(g.cache, g.base, buf_bytes());
    g.cache_valid = true;
    return true;
}

bool GdiCacheRestore(void)
{
    if (!g.cache_valid) return false;
    memcpy(g.base, g.cache, buf_bytes());
    return true;
}

void GdiCacheInvalidate(void) { g.cache_valid = false; }

int GdiScreenW(void) { return g.ready ? g.lw : 0; }
int GdiScreenH(void) { return g.ready ? g.lh : 0; }
int GdiScale(void)   { return g.ready ? g.s  : 1; }

/* Monitor @m's part of the back buffer to @dst (its screen) */
static void present_monitor(const Monitor *m, UINT32 *dst, int dstride)
{
    const UINT32 *src = g.buf + (INT64)m->r.y * g.s * g.bstride + (INT64)m->r.x * g.s;
    if (m->k == 1) {
        for (int y = 0; y < m->dh; y++)
            memcpy(dst + (size_t)y * dstride, src + (INT64)y * g.bstride, (size_t)m->dw * sizeof(UINT32));
        return;
    }
    /* Half the scale: the average of each 2x2 block */
    for (int y = 0; y < m->dh; y++) {
        const UINT32 *a = src + (INT64)(2 * y) * g.bstride, *b = a + g.bstride;
        UINT32 *o = dst + (size_t)y * dstride;
        for (int x = 0; x < m->dw; x++, a += 2, b += 2) {
            UINT32 rb = (a[0] & 0xFF00FF) + (a[1] & 0xFF00FF) + (b[0] & 0xFF00FF) + (b[1] & 0xFF00FF);
            UINT32 gg = (a[0] & 0x00FF00) + (a[1] & 0x00FF00) + (b[0] & 0x00FF00) + (b[1] & 0x00FF00);
            o[x] = (((rb + 0x020002) >> 2) & 0xFF00FF) | (((gg + 0x000200) >> 2) & 0x00FF00);
        }
    }
}

void GdiPresent(void)
{
    if (!g.ready || g.direct) return;
    /* With page flipping the frame goes to the page off screen, which is
     * also where the pointer is drawn next; GdiFlip() shows it there */
    UINT32 *back = DisplayBackPage();
    if (back) g.vram = back;
    present_monitor(&g_mon[0], g.vram, g.vstride);
    DisplayHeadDamage(0, 0, 0, g_mon[0].dw, g_mon[0].dh);
    for (int i = 1; i < g_nmon; i++) {
        int stride;
        UINT32 *v = DisplayHeadSurface(i, &stride);
        if (!v) continue;
        present_monitor(&g_mon[i], v, stride);
        DisplayHeadDamage(i, 0, 0, g_mon[i].dw, g_mon[i].dh);
    }
}

void GdiFlip(void)
{
    if (!g.ready || g.direct || !DisplayCanFlip()) return;
    DisplayFlip();                        /* g.vram is now the page on screen */
}

/* -----------------------------------------------------------------------
 * Color math
 * ----------------------------------------------------------------------- */
GdiColor GdiLerp(GdiColor a, GdiColor b, int t)
{
    if (t <= 0) return a;
    if (t >= 255) return b;
    int ra = GDI_R(a), ga = GDI_G(a), ba = GDI_B(a);
    int rb = GDI_R(b), gb = GDI_G(b), bb = GDI_B(b);
    int r  = ra + ((rb - ra) * t) / 255;
    int gg = ga + ((gb - ga) * t) / 255;
    int bl = ba + ((bb - ba) * t) / 255;
    return GDI_C(r, gg, bl);
}

/* -----------------------------------------------------------------------
 * Axis-aligned fills (edges are always on device pixel boundaries)
 * ----------------------------------------------------------------------- */
static void dev_fill(int x0, int y0, int x1, int y1, UINT32 n, int a)
{
    y0 = imax(y0, g.by0);
    y1 = imin(y1, g.by1);
    for (int y = y0; y < y1; y++) span(y, x0, x1, n, a);
}

void GdiFillRect(GdiRect r, GdiColor c)
{
    if (!g.ready) return;
    int s = g.s;
    dev_fill(r.x * s, r.y * s, (r.x + r.w) * s, (r.y + r.h) * s, pixof(c), 255);
}

void GdiAlphaFill(GdiRect r, GdiColor c, int alpha)
{
    if (!g.ready || alpha <= 0) return;
    int s = g.s;
    dev_fill(r.x * s, r.y * s, (r.x + r.w) * s, (r.y + r.h) * s, pixof(c),
             imin(alpha, 255));
}

void GdiGradientV(GdiRect r, GdiColor top, GdiColor bottom)
{
    if (!g.ready || r.h <= 0) return;
    int s = g.s, y0 = r.y * s, h = r.h * s;
    int ya = imax(y0, g.by0), yb = imin(y0 + h, g.by1);
    for (int y = ya; y < yb; y++) {
        int t = ((y - y0) * 255) / (h > 1 ? h - 1 : 1);
        span(y, r.x * s, (r.x + r.w) * s, pixof(GdiLerp(top, bottom, t)), 255);
    }
}

void GdiGradientH(GdiRect r, GdiColor left, GdiColor right)
{
    if (!g.ready || r.w <= 0) return;
    int s = g.s, x0 = r.x * s, w = r.w * s;
    int xa = imax(x0, g.cx0), xb = imin(x0 + w, g.cx1);
    int ya = imax(r.y * s, g.cy0), yb = imin((r.y + r.h) * s, g.cy1);
    for (int x = xa; x < xb; x++) {
        int t = ((x - x0) * 255) / (w > 1 ? w - 1 : 1);
        UINT32 n = pixof(GdiLerp(left, right, t));
        for (int y = ya; y < yb; y++) g.buf[(INT64)y * g.bstride + x] = n;
    }
}

void GdiHLine(int y, int x0, int x1, GdiColor c)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    GdiFillRect(RECT(x0, y, x1 - x0 + 1, 1), c);
}

void GdiVLine(int x, int y0, int y1, GdiColor c)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    GdiFillRect(RECT(x, y0, 1, y1 - y0 + 1), c);
}

void GdiPutPixel(int x, int y, GdiColor c)
{
    GdiFillRect(RECT(x, y, 1, 1), c);
}

/* Blit a source image of logical pixels (GdiColor / COLORREF format, one
 * UINT32 per logical pixel, @src_stride pixels per row) into the logical
 * rect @dst, scaling each logical pixel to the display scale and clipping.
 * Used to composite a user program's window bitmap. */
void GdiBlitBGRA(GdiRect dst, const UINT32 *src, int src_stride)
{
    if (!g.ready || !src) return;
    int sc = g.s;
    int x0 = imax(dst.x * sc, g.cx0), y0 = imax(dst.y * sc, g.cy0);
    int x1 = imin((dst.x + dst.w) * sc, g.cx1), y1 = imin((dst.y + dst.h) * sc, g.cy1);
    for (int y = y0; y < y1; y++) {
        int sy = (y / sc) - dst.y;
        if (sy < 0 || sy >= dst.h) continue;
        const UINT32 *row = src + (size_t)sy * src_stride;
        UINT32 *out = g.buf + (INT64)y * g.bstride;
        for (int x = x0; x < x1; x++) {
            int sx = (x / sc) - dst.x;
            if (sx < 0 || sx >= dst.w) continue;
            out[x] = pixof(row[sx]);
        }
    }
}

/* Blend one straight-alpha 0xAARRGGBB value at device (x, y) (in the clip) */
static inline void put_argb(int x, int y, UINT32 a, UINT32 r, UINT32 gg, UINT32 b)
{
    if (!a) return;
    UINT32 *p = &g.buf[(INT64)y * g.bstride + x];
    UINT32 n = pixof(r | gg << 8 | b << 16);
    *p = a >= 255 ? n : blend(*p, n, a);
}

void GdiDrawImage(GdiRect dst, const UINT32 *px, int sw, int sh)
{
    if (!g.ready || !px || sw <= 0 || sh <= 0 || dst.w <= 0 || dst.h <= 0) return;
    int s = g.s, ox0 = dst.x * s, oy0 = dst.y * s, dw = dst.w * s, dh = dst.h * s;
    int x0 = imax(ox0, g.cx0), x1 = imin(ox0 + dw, g.cx1);
    int y0 = imax(oy0, g.cy0), y1 = imin(oy0 + dh, g.cy1);
    bool box = sw >= dw && sh >= dh;              /* shrinking: average; else bilinear */

    for (int y = y0; y < y1; y++) {
        int oy = y - oy0;
        for (int x = x0; x < x1; x++) {
            int ox = x - ox0;
            UINT64 sa = 0, sr = 0, sg = 0, sb = 0, wsum = 0;
            if (box) {
                int sx0 = ox * sw / dw, sx1 = ((ox + 1) * sw + dw - 1) / dw;
                int sy0 = oy * sh / dh, sy1 = ((oy + 1) * sh + dh - 1) / dh;
                if (sx1 > sw) sx1 = sw;
                if (sy1 > sh) sy1 = sh;
                for (int yy = sy0; yy < sy1; yy++)
                    for (int xx = sx0; xx < sx1; xx++) {
                        UINT32 v = px[(size_t)yy * sw + xx], a = v >> 24;
                        sa += a;
                        sr += (UINT64)(v >> 16 & 0xFF) * a;
                        sg += (UINT64)(v >> 8 & 0xFF) * a;
                        sb += (UINT64)(v & 0xFF) * a;
                        wsum++;
                    }
                if (!wsum || !sa) continue;
                put_argb(x, y, (UINT32)(sa / wsum), (UINT32)(sr / sa), (UINT32)(sg / sa), (UINT32)(sb / sa));
            } else {
                /* sample centre in source pixels, 8.8 fixed point */
                int fx = (int)(((INT64)(2 * ox + 1) * sw * 128) / dw) - 128;
                int fy = (int)(((INT64)(2 * oy + 1) * sh * 128) / dh) - 128;
                if (fx < 0) fx = 0;
                if (fy < 0) fy = 0;
                int ix = fx >> 8, iy = fy >> 8, tx = fx & 255, ty = fy & 255;
                for (int k = 0; k < 4; k++) {
                    int xx = imin(ix + (k & 1), sw - 1), yy = imin(iy + (k >> 1), sh - 1);
                    UINT32 w = (UINT32)((k & 1) ? tx : 256 - tx) * (UINT32)((k >> 1) ? ty : 256 - ty);
                    UINT32 v = px[(size_t)yy * sw + xx];
                    UINT64 wa = (UINT64)w * (v >> 24);
                    sa += wa;
                    sr += wa * (v >> 16 & 0xFF);
                    sg += wa * (v >> 8 & 0xFF);
                    sb += wa * (v & 0xFF);
                }
                if (!sa) continue;
                put_argb(x, y, (UINT32)(sa >> 16), (UINT32)(sr / sa), (UINT32)(sg / sa), (UINT32)(sb / sa));
            }
        }
    }
}

void GdiDrawImageDevice(int lx, int ly, const UINT32 *px, int w, int h)
{
    if (!g.ready || !px) return;
    int ox = lx * g.s, oy = ly * g.s;
    int x0 = imax(ox, g.cx0), x1 = imin(ox + w, g.cx1);
    int y0 = imax(oy, g.cy0), y1 = imin(oy + h, g.cy1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            UINT32 v = px[(size_t)(y - oy) * w + (x - ox)];
            put_argb(x, y, v >> 24, v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF);
        }
}

void GdiDrawImageZoom(int lx, int ly, const UINT32 *px, int w, int h, int k)
{
    if (!g.ready || !px || k < 1) return;
    int ox = lx * g.s, oy = ly * g.s;
    int x0 = imax(ox, g.cx0), x1 = imin(ox + w * k, g.cx1);
    int y0 = imax(oy, g.cy0), y1 = imin(oy + h * k, g.cy1);
    for (int y = y0; y < y1; y++) {
        const UINT32 *row = px + (size_t)((y - oy) / k) * w;
        for (int x = x0; x < x1; x++) {
            UINT32 v = row[(x - ox) / k];
            put_argb(x, y, v >> 24, v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF);
        }
    }
}

/* -----------------------------------------------------------------------
 * Rounded rectangles
 * ----------------------------------------------------------------------- */
typedef struct { int x0, y0, x1, y1, r; } RBox;   /* device px, [x0,x1) */

static RBox rbox_of(GdiRect r, int rad)
{
    int s = g.s;
    RBox b = { r.x * s, r.y * s, (r.x + r.w) * s, (r.y + r.h) * s, rad * s };
    int lim = imin(b.x1 - b.x0, b.y1 - b.y0) / 2;
    if (b.r > lim) b.r = lim;
    if (b.r < 0)   b.r = 0;
    return b;
}

/* Signed distance (1/256 px, negative inside) from the centre of device
 * pixel (px, py) to the rounded box. */
static int rbox_sd(const RBox *b, int px, int py)
{
    int qx = iabs((2 * px + 1) * FX - (b->x0 + b->x1) * FX) / 2
             - ((b->x1 - b->x0) * FX / 2 - b->r * FX);
    int qy = iabs((2 * py + 1) * FX - (b->y0 + b->y1) * FX) / 2
             - ((b->y1 - b->y0) * FX / 2 - b->r * FX);
    int outside;
    if (qx > 0 && qy > 0)
        outside = (int)isqrt64((UINT64)((INT64)qx * qx + (INT64)qy * qy));
    else
        outside = imax(imax(qx, qy), 0);
    int inside = imin(imax(qx, qy), 0);
    return outside + inside - b->r * FX;
}

/* Fill a rounded box; the colour may vary per row (vertical gradient). */
static void rbox_fill(const RBox *b, GdiColor top, GdiColor bottom, int alpha)
{
    if (b->x1 <= b->x0 || b->y1 <= b->y0 || alpha <= 0) return;
    bool   grad = (top != bottom);
    UINT32 n    = pixof(top);
    int    h    = b->y1 - b->y0;
    int    ya   = imax(b->y0, g.by0), yb = imin(b->y1, g.by1);

    for (int y = ya; y < yb; y++) {
        if (grad)
            n = pixof(GdiLerp(top, bottom, ((y - b->y0) * 255) / (h > 1 ? h - 1 : 1)));
        if (y >= b->y0 + b->r && y < b->y1 - b->r) {        /* straight rows */
            span(y, b->x0, b->x1, n, alpha);
            continue;
        }
        /* Corner rows: anti-alias the two corner zones, fill between */
        for (int x = b->x0; x < b->x0 + b->r; x++)
            plot(x, y, n, cov_of(rbox_sd(b, x, y)) * alpha / 255);
        for (int x = b->x1 - b->r; x < b->x1; x++)
            plot(x, y, n, cov_of(rbox_sd(b, x, y)) * alpha / 255);
        span(y, b->x0 + b->r, b->x1 - b->r, n, alpha);
    }
}

/* 1-logical-pixel border: coverage(outer) − coverage(inner) */
static void rbox_stroke(const RBox *b, GdiColor c)
{
    int    t     = g.s;
    RBox   in    = { b->x0 + t, b->y0 + t, b->x1 - t, b->y1 - t, imax(b->r - t, 0) };
    bool   hasin = in.x1 > in.x0 && in.y1 > in.y0;
    UINT32 n     = pixof(c);
    int    band  = b->r + t;
    int    ya    = imax(b->y0, g.by0), yb = imin(b->y1, g.by1);

    for (int y = ya; y < yb; y++) {
        if (y >= b->y0 + band && y < b->y1 - band) {       /* straight sides */
            span(y, b->x0, b->x0 + t, n, 255);
            span(y, b->x1 - t, b->x1, n, 255);
            continue;
        }
        for (int x = imax(b->x0, g.bx0); x < imin(b->x1, g.bx1); x++) {
            int co = cov_of(rbox_sd(b, x, y));
            int ci = hasin ? cov_of(rbox_sd(&in, x, y)) : 0;
            plot(x, y, n, co - ci);
        }
    }
}

void GdiRoundRect(GdiRect r, int rad, GdiColor fill, GdiColor border)
{
    if (!g.ready || r.w <= 0 || r.h <= 0) return;
    RBox b = rbox_of(r, rad);
    if (fill != GDI_TRANSPARENT)   rbox_fill(&b, fill, fill, 255);
    if (border != GDI_TRANSPARENT) rbox_stroke(&b, border);
}

void GdiRoundAlpha(GdiRect r, int rad, GdiColor c, int alpha)
{
    if (!g.ready || r.w <= 0 || r.h <= 0) return;
    RBox b = rbox_of(r, rad);
    rbox_fill(&b, c, c, imin(alpha, 255));
}

void GdiRoundBorderAlpha(GdiRect r, int rad, GdiColor c, int alpha)
{
    if (!g.ready || r.w <= 0 || r.h <= 0) return;
    RBox b = rbox_of(r, rad);
    UINT32 n = pixof(c);
    int w = g.s * FX;                             /* one logical pixel wide */
    /* Pixels further than the radius and the stroke inside every edge
     * are all "well inside" (below): their rows skip from ix0 to ix1 */
    int m = b.r + g.s + 2;
    int ix0 = b.x0 + m, ix1 = b.x1 - m, iy0 = b.y0 + m, iy1 = b.y1 - m;
    for (int y = imax(b.y0, g.cy0); y < imin(b.y1, g.cy1); y++) {
        bool hole = ix0 < ix1 && y >= iy0 && y < iy1;
        for (int x = imax(b.x0, g.cx0); x < imin(b.x1, g.cx1); x++) {
            if (hole && x >= ix0 && x < ix1) { x = ix1 - 1; continue; }
            int sd = rbox_sd(&b, x, y);
            if (sd < -w - FX) continue;           /* well inside: nothing to draw */
            int cv = cov_of(sd) - cov_of(sd + w);
            if (cv > 0) plot(x, y, n, cv * alpha / 255);
        }
    }
}

void GdiRoundGradV(GdiRect r, int rad, GdiColor top, GdiColor bottom)
{
    if (!g.ready || r.w <= 0 || r.h <= 0) return;
    RBox b = rbox_of(r, rad);
    rbox_fill(&b, top, bottom, 255);
}

void GdiDropShadow(GdiRect r, int rad, int blur, int alpha)
{
    GdiDropShadowAround(r, rad, blur, alpha, RECT(0, 0, 0, 0), 0);
}

void GdiDropShadowAround(GdiRect r, int rad, int blur, int alpha, GdiRect cover, int cover_rad)
{
    if (!g.ready || r.w <= 0 || r.h <= 0 || blur <= 0 || alpha <= 0) return;
    RBox   b   = rbox_of(r, rad);
    int    bl  = blur * g.s * FX;           /* fade distance, fixed point */
    int    m   = blur * g.s;
    UINT32 blk = pixof(GDI_BLACK);
    int ya = imax(b.y0 - m, g.by0), yb = imin(b.y1 + m, g.by1);
    int xa = imax(b.x0 - m, g.bx0), xb = imin(b.x1 + m, g.bx1);
    /* The part of @cover certain to be covered (its rounded corners cut
     * off), in device pixels: nothing there needs a shadow */
    int s = g.s;
    int hx0 = (cover.x + cover_rad) * s, hx1 = (cover.x + cover.w - cover_rad) * s;
    int hy0 = (cover.y + cover_rad) * s, hy1 = (cover.y + cover.h - cover_rad) * s;
    bool hole = cover.w > 0 && cover.h > 0 && hx0 < hx1 && hy0 < hy1;

    for (int y = ya; y < yb; y++) {
        bool row_in_hole = hole && y >= hy0 && y < hy1;
        for (int x = xa; x < xb; x++) {
            if (row_in_hole && x >= hx0 && x < hx1) { x = hx1 - 1; continue; }
            int sd = rbox_sd(&b, x, y);
            if (sd >= bl) continue;
            int a = alpha;
            if (sd > 0) {                        /* quadratic falloff */
                int f = ((bl - sd) * 255) / bl;  /* 0..255 */
                a = (alpha * f * f) / (255 * 255);
            }
            plot(x, y, blk, a);
        }
    }
}

/* -----------------------------------------------------------------------
 * Circles and polygons
 * ----------------------------------------------------------------------- */
void GdiFillCircle(int cx, int cy, int rad, GdiColor c)
{
    if (!g.ready || rad <= 0) return;
    int s = g.s;
    /* Centre of logical pixel (cx, cy); diameter 2*rad+1 logical px */
    int ccx = (2 * cx + 1) * s * FX / 2, ccy = (2 * cy + 1) * s * FX / 2;
    int R   = (2 * rad + 1) * s * FX / 2;
    int ext = (R / FX) + 2;
    UINT32 n = pixof(c);
    int x0 = ccx / FX - ext, x1 = ccx / FX + ext;
    int y0 = ccy / FX - ext, y1 = ccy / FX + ext;
    for (int y = imax(y0, g.by0); y <= imin(y1, g.by1 - 1); y++) {
        for (int x = imax(x0, g.bx0); x <= imin(x1, g.bx1 - 1); x++) {
            INT64 dx = (INT64)x * FX + FX / 2 - ccx;
            INT64 dy = (INT64)y * FX + FX / 2 - ccy;
            int d = (int)isqrt64((UINT64)(dx * dx + dy * dy));
            plot(x, y, n, cov_of(d - R));
        }
    }
}

/* Signed distance (1/256 px, negative inside) from point (px,py), given
 * in 1/256 device px, to a polygon given in the same units. */
static int poly_sd(const GdiPoint *p, int n, int px, int py)
{
    INT64 best = -1;
    bool  in   = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        INT64 ax = p[j].x, ay = p[j].y, bx = p[i].x, by = p[i].y;
        /* crossing test for inside/outside */
        if (((ay > py) != (by > py)) &&
            (px < ax + (bx - ax) * (py - ay) / (by - ay)))
            in = !in;
        /* distance to segment */
        INT64 ex = bx - ax, ey = by - ay;
        INT64 wx = px - ax, wy = py - ay;
        INT64 len2 = ex * ex + ey * ey;
        INT64 cx = ax, cy = ay;
        if (len2 > 0) {
            INT64 dot = wx * ex + wy * ey;
            if (dot >= len2)   { cx = bx; cy = by; }
            else if (dot > 0)  { cx = ax + ex * dot / len2; cy = ay + ey * dot / len2; }
        }
        INT64 dx = px - cx, dy = py - cy;
        INT64 d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < best) best = d2;
    }
    int d = (int)isqrt64((UINT64)(best < 0 ? 0 : best));
    return in ? -d : d;
}

#define POLY_MAX 16

/* Convert 1/16 logical vertices to 1/256 device units, offset by (ox, oy)
 * device px; returns the device bounding box. */
static int poly_to_dev_s(const GdiPoint *src, int n, int sc, int ox, int oy, GdiPoint *dst,
                         int *bx0, int *by0, int *bx1, int *by1)
{
    if (n > POLY_MAX) n = POLY_MAX;
    int f = sc * (FX / 16);
    *bx0 = *by0 = 0x7FFFFFFF; *bx1 = *by1 = -0x7FFFFFFF;
    for (int i = 0; i < n; i++) {
        dst[i].x = src[i].x * f + ox * FX;
        dst[i].y = src[i].y * f + oy * FX;
        *bx0 = imin(*bx0, dst[i].x / FX); *bx1 = imax(*bx1, dst[i].x / FX);
        *by0 = imin(*by0, dst[i].y / FX); *by1 = imax(*by1, dst[i].y / FX);
    }
    return n;
}

static int poly_to_dev(const GdiPoint *src, int n, int ox, int oy, GdiPoint *dst,
                       int *bx0, int *by0, int *bx1, int *by1)
{
    return poly_to_dev_s(src, n, g.s, ox, oy, dst, bx0, by0, bx1, by1);
}

void GdiFillPolygon(const GdiPoint *pts, int n, GdiColor c)
{
    if (!g.ready || n < 3) return;
    GdiPoint d[POLY_MAX];
    int x0, y0, x1, y1;
    n = poly_to_dev(pts, n, 0, 0, d, &x0, &y0, &x1, &y1);
    UINT32 col = pixof(c);
    for (int y = imax(y0 - 1, g.by0); y <= imin(y1 + 1, g.by1 - 1); y++)
        for (int x = imax(x0 - 1, g.bx0); x <= imin(x1 + 1, g.bx1 - 1); x++)
            plot(x, y, col, cov_of(poly_sd(d, n, x * FX + FX / 2, y * FX + FX / 2)));
}

void GdiLine(GdiPoint a, GdiPoint b, int width16, GdiColor c)
{
    /* A thin quad around the segment; endpoints in 1/16 logical px */
    int dx = b.x - a.x, dy = b.y - a.y;
    int len = (int)isqrt64((UINT64)((INT64)dx * dx + (INT64)dy * dy));
    if (len == 0 || width16 <= 0) return;
    int nx = -dy * width16 / (2 * len), ny = dx * width16 / (2 * len);
    GdiPoint q[4] = {
        { a.x + nx, a.y + ny }, { b.x + nx, b.y + ny },
        { b.x - nx, b.y - ny }, { a.x - nx, a.y - ny },
    };
    GdiFillPolygon(q, 4, c);
}

void GdiFillUnderCurve(GdiRect r, GdiCurveFn fn, void *ctx, GdiColor c)
{
    if (!g.ready || !fn || r.w <= 0 || r.h <= 0) return;
    int s = g.s;
    UINT32 n = pixof(c);
    /* clamp to the clip rect too: the column fill below writes directly */
    int top = imax(r.y * s, g.cy0) * FX, bot = imin((r.y + r.h) * s, g.cy1);
    for (int x = imax(r.x * s, g.cx0); x < imin((r.x + r.w) * s, g.cx1); x++) {
        int xl = (x * FX + FX / 2) / s;             /* 1/256 logical */
        int yd = fn(xl, ctx) * s;                   /* 1/256 device  */
        if (yd < top) yd = top;
        int yi = yd / FX;
        if (yi >= bot) continue;
        /* partial pixel where the curve crosses, then solid below */
        plot(x, yi, n, ((FX - (yd % FX)) * 255) / FX);
        UINT32 *p = &g.buf[(INT64)(yi + 1) * g.bstride + x];
        for (int y = yi + 1; y < bot; y++, p += g.bstride) *p = n;
    }
}

/* -----------------------------------------------------------------------
 * Blur ("acrylic" backdrops, text shadows)
 * ----------------------------------------------------------------------- */

/* One box-blur pass of radius r along rows (stride 1) or columns, on
 * packed pixels: each byte lane is a channel.  tmp holds n values. */
static void box_pass32(UINT32 *p, int n, int step, int r, UINT32 *tmp)
{
    if (n <= 1 || r <= 0) return;
    UINT32 s0 = 0, s1 = 0, s2 = 0, d = (UINT32)(2 * r + 1);
    for (int i = 0; i < n; i++) tmp[i] = p[(size_t)i * step];
    for (int k = -r; k <= r; k++) {
        UINT32 v = tmp[imin(imax(k, 0), n - 1)];
        s0 += v & 0xFF; s1 += (v >> 8) & 0xFF; s2 += (v >> 16) & 0xFF;
    }
    for (int i = 0; i < n; i++) {
        p[(size_t)i * step] = (s0 / d) | (s1 / d) << 8 | (s2 / d) << 16;
        UINT32 a = tmp[imin(i + r + 1, n - 1)], b = tmp[imax(i - r, 0)];
        s0 += (a & 0xFF) - (b & 0xFF);
        s1 += ((a >> 8) & 0xFF) - ((b >> 8) & 0xFF);
        s2 += ((a >> 16) & 0xFF) - ((b >> 16) & 0xFF);
    }
}

static void box_pass8(UINT8 *p, int n, int step, int r, UINT8 *tmp)
{
    if (n <= 1 || r <= 0) return;
    UINT32 sum = 0, d = (UINT32)(2 * r + 1);
    for (int i = 0; i < n; i++) tmp[i] = p[(size_t)i * step];
    for (int k = -r; k <= r; k++) sum += tmp[imin(imax(k, 0), n - 1)];
    for (int i = 0; i < n; i++) {
        p[(size_t)i * step] = (UINT8)(sum / d);
        sum += tmp[imin(i + r + 1, n - 1)];
        sum -= tmp[imax(i - r, 0)];
    }
}

/* Three box passes each way approximate a Gaussian of about 2r */
static void blur32(UINT32 *p, int w, int h, int r, UINT32 *tmp)
{
    for (int it = 0; it < 3; it++) {
        for (int y = 0; y < h; y++) box_pass32(p + (size_t)y * w, w, 1, r, tmp);
        for (int x = 0; x < w; x++) box_pass32(p + x, h, w, r, tmp);
    }
}

static void blur8(UINT8 *p, int w, int h, int r, UINT8 *tmp)
{
    for (int it = 0; it < 3; it++) {
        for (int y = 0; y < h; y++) box_pass8(p + (size_t)y * w, w, 1, r, tmp);
        for (int x = 0; x < w; x++) box_pass8(p + x, h, w, r, tmp);
    }
}

/* Blurred backdrops are cached per region: the blur is redone only when
 * the pixels underneath have changed (a checksum tells), which matters
 * because the dock and tray are composited on every frame. */
#define BD_CACHE 4
static struct {
    int x0, y0, w, h, br;
    UINT64 sum;
    UINT32 *img;
    UINT32 age;
} g_bd[BD_CACHE];
static UINT32 g_bd_clock;

static UINT64 region_sum(int x0, int y0, int w, int h)
{
    UINT64 s = 1469598103934665603ull;
    for (int y = 0; y < h; y++) {
        const UINT32 *row = g.buf + (INT64)(y0 + y) * g.bstride + x0;
        for (int x = 0; x < w; x++) s = (s ^ row[x]) * 1099511628211ull;
    }
    return s;
}

static UINT32 *blurred(int x0, int y0, int w, int h, int br)
{
    UINT64 sum = region_sum(x0, y0, w, h);
    int slot = 0;
    for (int i = 0; i < BD_CACHE; i++) {
        if (g_bd[i].img && g_bd[i].x0 == x0 && g_bd[i].y0 == y0 && g_bd[i].w == w && g_bd[i].h == h && g_bd[i].br == br) {
            g_bd[i].age = ++g_bd_clock;
            if (g_bd[i].sum == sum) return g_bd[i].img;   /* unchanged: reuse */
            slot = i;
            goto refill;
        }
        if (g_bd[i].age < g_bd[slot].age) slot = i;
    }
    kfree(g_bd[slot].img);
    g_bd[slot].img = kmalloc((size_t)w * h * 4);
    if (!g_bd[slot].img) return NULL;
    g_bd[slot].x0 = x0; g_bd[slot].y0 = y0; g_bd[slot].w = w; g_bd[slot].h = h; g_bd[slot].br = br;
    g_bd[slot].age = ++g_bd_clock;
refill:;
    UINT32 *img = g_bd[slot].img, *tmp = kmalloc((size_t)imax(w, h) * 4);
    if (!tmp) { g_bd[slot].sum = 0; return NULL; }
    for (int y = 0; y < h; y++)
        memcpy(img + (size_t)y * w, g.buf + (INT64)(y0 + y) * g.bstride + x0, (size_t)w * 4);
    blur32(img, w, h, br, tmp);
    kfree(tmp);
    g_bd[slot].sum = sum;
    return img;
}

void GdiBackdrop(GdiRect r, int rad, int blur, GdiColor tint, int tint_alpha)
{
    if (!g.ready || r.w <= 0 || r.h <= 0) return;
    RBox b = rbox_of(r, rad);
    int br = imax(1, blur * g.s / 3);             /* box radius per pass, device px */
    /* the region read (a margin so edges blur against what is outside) */
    int x0 = imax(b.x0 - 3 * br, g.bx0), y0 = imax(b.y0 - 3 * br, g.by0);
    int x1 = imin(b.x1 + 3 * br, g.bx1), y1 = imin(b.y1 + 3 * br, g.by1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    UINT32 *img = blurred(x0, y0, w, h, br), *tmp = img;
    UINT32 tn = pixof(tint);
    int cx0 = imax(b.x0, g.cx0), cx1 = imin(b.x1, g.cx1);
    int cy0 = imax(b.y0, g.cy0), cy1 = imin(b.y1, g.cy1);
    for (int y = cy0; y < cy1; y++) {
        UINT32 *row = g.buf + (INT64)y * g.bstride;
        for (int x = cx0; x < cx1; x++) {
            int c = cov_of(rbox_sd(&b, x, y));
            if (!c) continue;
            UINT32 v = (img && tmp) ? img[(size_t)(y - y0) * w + (x - x0)] : row[x];
            v = blend(v, tn, (UINT32)tint_alpha);
            row[x] = c >= 255 ? v : blend(row[x], v, (UINT32)c);
        }
    }
}

/* -----------------------------------------------------------------------
 * Text
 * ----------------------------------------------------------------------- */
static const GdiFace *face(int style) { return &g_gdi_faces[style][g.s - 1]; }

static const GdiGlyph *glyph(const GdiFace *f, unsigned char c)
{
    if (c < f->first || c >= f->first + f->count) c = '?';
    return &f->glyphs[c - f->first];
}

/* Advance width of a string in 26.6 device pixels */
static int text_adv(const char *s, int style)
{
    if (!s) return 0;
    const GdiFace *f = face(style);
    int w = 0;
    for (; *s; s++) w += glyph(f, (unsigned char)*s)->adv;
    return w;
}

static int adv_to_logical(int adv) { return (adv + 64 * g.s - 1) / (64 * g.s); }

int GdiTextW(const char *s)     { return g.ready ? adv_to_logical(text_adv(s, GDI_FONT_REGULAR)) : 0; }
int GdiTextBoldW(const char *s) { return g.ready ? adv_to_logical(text_adv(s, GDI_FONT_BOLD)) : 0; }

/* Draw starting at pen position `pen` (26.6 device px) on logical line y */
static void text_draw_at(int pen, int y, const char *s, GdiColor fg, int style)
{
    if (!g.ready || !s) return;
    const GdiFace *f = face(style);
    UINT32 n    = pixof(fg);
    int    base = (y + (style == GDI_FONT_DISPLAY ? LARGE_BASELINE : LINE_BASELINE)) * g.s;
    for (; *s; s++) {
        const GdiGlyph *gl = glyph(f, (unsigned char)*s);
        int gx = ((pen + 32) >> 6) + gl->bx;
        int gy = base - gl->by;
        const UINT8 *bm = f->bits + gl->off;
        for (int r = 0; r < gl->h; r++)
            for (int col = 0; col < gl->w; col++)
                plot(gx + col, gy + r, n, bm[r * gl->w + col]);
        pen += gl->adv;
    }
}

static void text_draw(int x, int y, const char *s, GdiColor fg, int style)
{
    text_draw_at(x * g.s * 64, y, s, fg, style);
}

void GdiTextT(int x, int y, const char *s, GdiColor fg)
{
    text_draw(x, y, s, fg, GDI_FONT_REGULAR);
}

void GdiTextBold(int x, int y, const char *s, GdiColor fg)
{
    text_draw(x, y, s, fg, GDI_FONT_BOLD);
}

void GdiText(int x, int y, const char *s, GdiColor fg, GdiColor bg)
{
    GdiFillRect(RECT(x, y, GdiTextW(s), GDI_FONT_H), bg);
    text_draw(x, y, s, fg, GDI_FONT_REGULAR);
}

void GdiTextLarge(int x, int y, const char *s, GdiColor fg)
{
    text_draw(x, y, s, fg, GDI_FONT_DISPLAY);
}

int GdiTextLargeW(const char *s)
{
    return g.ready ? adv_to_logical(text_adv(s, GDI_FONT_DISPLAY)) : 0;
}

void GdiTextMono(int x, int y, const char *s, GdiColor fg)
{
    text_draw(x, y, s, fg, GDI_FONT_MONO);
}

void GdiTextMonoN(int x, int y, const char *s, int n, GdiColor fg)
{
    if (!g.ready || !s || n <= 0) return;
    char buf[256];
    while (n > 0) {                       /* draw in chunks, keeping the grid */
        int k = imin(n, (int)sizeof(buf) - 1);
        memcpy(buf, s, (size_t)k);
        buf[k] = '\0';
        text_draw(x, y, buf, fg, GDI_FONT_MONO);
        x += (k * GdiMonoCellW256()) / 256;
        s += k; n -= k;
    }
}

int GdiMonoCellW256(void)
{
    if (!g.ready) return 8 * 256;
    /* 26.6 device → 1/256 logical: × 4 / scale */
    return glyph(face(GDI_FONT_MONO), 'M')->adv * 4 / g.s;
}

void GdiTextCenter(int x, int y, int w, const char *s, GdiColor fg)
{
    if (!g.ready || !s) return;
    /* Centre with sub-pixel precision rather than rounding the width */
    int adv = text_adv(s, GDI_FONT_REGULAR);
    int off = (w * g.s * 64 - adv) / 2;
    if (off < 0) off = 0;
    text_draw_at(x * g.s * 64 + off, y, s, fg, GDI_FONT_REGULAR);
}

/* A soft drop shadow under text drawn at pen position @pen (26.6 device
 * px) on line y: the glyph coverage, blurred, in black at @alpha, one
 * logical pixel lower. */
static void text_shadow_at(int pen, int y, const char *s, int alpha)
{
    if (!g.ready || !s || !*s) return;
    const GdiFace *f = face(GDI_FONT_REGULAR);
    int br = g.s;                                 /* blur radius per pass */
    int pad = 3 * br + 2;
    int ox = (pen >> 6) - pad, oy = y * g.s - pad + g.s;   /* shadow offset: 1 logical px down */
    int w = (text_adv(s, GDI_FONT_REGULAR) >> 6) + 2 * pad + 4, h = GDI_FONT_H * g.s + 2 * pad;
    UINT8 *m = kzalloc((size_t)w * h), *tmp = kmalloc((size_t)imax(w, h));
    if (!m || !tmp) { kfree(m); kfree(tmp); return; }
    int base = LINE_BASELINE * g.s + pad, pen0 = pen;
    for (const char *c = s; *c; c++) {
        const GdiGlyph *gl = glyph(f, (unsigned char)*c);
        int gx = ((pen + 32) >> 6) + gl->bx - (pen0 >> 6) + pad, gy = base - gl->by;
        const UINT8 *bm = f->bits + gl->off;
        for (int r = 0; r < gl->h; r++)
            for (int col = 0; col < gl->w; col++) {
                int px = gx + col, py = gy + r;
                if (px >= 0 && px < w && py >= 0 && py < h) {
                    UINT8 v = bm[r * gl->w + col];
                    if (v > m[(size_t)py * w + px]) m[(size_t)py * w + px] = v;
                }
            }
        pen += gl->adv;
    }
    blur8(m, w, h, br, tmp);
    UINT32 black = pixof(GDI_BLACK);
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            int a = m[(size_t)yy * w + xx] * alpha / 255;
            if (a) {
                /* the blur spreads coverage thin: strengthen it a little */
                a = imin(255, a * 2);
                plot(ox + xx, oy + yy, black, a);
            }
        }
    kfree(m);
    kfree(tmp);
}

void GdiTextShadowCenter(int x, int y, int w, const char *s, GdiColor fg, int shadow)
{
    if (!g.ready || !s) return;
    int adv = text_adv(s, GDI_FONT_REGULAR);
    int off = (w * g.s * 64 - adv) / 2;
    if (off < 0) off = 0;
    text_shadow_at(x * g.s * 64 + off, y, s, shadow);
    text_draw_at(x * g.s * 64 + off, y, s, fg, GDI_FONT_REGULAR);
}

void GdiTextShadow(int x, int y, const char *s, GdiColor fg, int shadow)
{
    if (!g.ready || !s) return;
    text_shadow_at(x * g.s * 64, y, s, shadow);
    text_draw_at(x * g.s * 64, y, s, fg, GDI_FONT_REGULAR);
}

/* -----------------------------------------------------------------------
 * Mouse pointer overlay
 * ----------------------------------------------------------------------- */

#define CUR_MAX_W    (GDI_CURSOR_MAX * GDI_MAX_SCALE)
#define CUR_MAX_H    (GDI_CURSOR_MAX * GDI_MAX_SCALE)

static UINT32 g_under[CUR_MAX_W * CUR_MAX_H];
static int    g_under_x, g_under_y, g_under_w, g_under_h;
static UINT32 *g_under_vram;              /* the screen the save-under came from */
static int    g_under_stride;
static int    g_under_head;               /* ... and its display head */

/* The pointer goes on the screen of the monitor it is on: @dx, @dy (back
 * buffer device px) become that screen's px, *s its scale */
typedef struct { UINT32 *vram; int stride, w, h, s; } CurTarget;

static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static bool cursor_target(int *dx, int *dy, CurTarget *t)
{
    int i = GdiMonitorAt(floordiv(*dx, g.s), floordiv(*dy, g.s));
    if (i < 0) i = 0;
    const Monitor *m = &g_mon[i];
    if (i == 0) { t->vram = g.vram; t->stride = g.vstride; }
    else if (!(t->vram = DisplayHeadSurface(i, &t->stride))) return false;
    t->w = m->dw; t->h = m->dh; t->s = m->s;
    *dx = floordiv(*dx - m->r.x * g.s, m->k);
    *dy = floordiv(*dy - m->r.y * g.s, m->k);
    g_under_vram = t->vram;
    g_under_stride = t->stride;
    g_under_head = i;
    return true;
}

/* Blends @w x @h pixels (0xAARRGGBB), each @s screen px a side, with the
 * hot spot (hx, hy) of the image at screen px (dx, dy) of @t, saving what
 * is under */
static void cursor_blit(const CurTarget *t, int dx, int dy, const UINT32 *px, int w, int h, int hx, int hy, int s)
{
    int x0 = dx - hx * s, y0 = dy - hy * s;
    int x1 = imin(x0 + w * s, t->w), y1 = imin(y0 + h * s, t->h);
    int cx0 = imax(x0, 0), cy0 = imax(y0, 0);
    g_under_x = cx0; g_under_y = cy0;
    g_under_w = imax(imin(x1 - cx0, CUR_MAX_W), 0);
    g_under_h = imax(imin(y1 - cy0, CUR_MAX_H), 0);
    for (int j = 0; j < g_under_h; j++) {
        int y = cy0 + j;
        UINT32 *row = t->vram + (size_t)y * t->stride;
        const UINT32 *src = px + (size_t)((y - y0) / s) * w;
        for (int i = 0; i < g_under_w; i++) {
            int x = cx0 + i;
            UINT32 d = row[x];
            g_under[j * CUR_MAX_W + i] = d;
            UINT32 p = src[(x - x0) / s], a = p >> 24;
            if (!a) continue;
            GdiColor col = (GdiColor)(((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16));
            row[x] = a == 255 ? pixof(col) : blend(d, pixof(col), a);
        }
    }
    DisplayHeadDamage(g_under_head, g_under_x, g_under_y, g_under_w, g_under_h);
}

/* The system pointers are rendered once per shape, scale and phase */
static UINT32 g_sys_px[SYSCUR_BOX * GDI_MAX_SCALE * SYSCUR_BOX * GDI_MAX_SCALE];
static int    g_sys_id, g_sys_scale, g_sys_phase, g_sys_hx, g_sys_hy;
static UINT64 g_sys_scratch[SYSCUR_SCRATCH / 8];      /* the desktop's (it draws the pointer) */

void GdiCursorDrawSys(int dx, int dy, int id, int phase)
{
    if (!g.ready) return;
    g_under_w = g_under_h = 0;
    CurTarget t;
    if (!cursor_target(&dx, &dy, &t)) return;
    int s = t.s < 1 ? 1 : t.s > GDI_MAX_SCALE ? GDI_MAX_SCALE : t.s;
    if (!SysCursorAnimated(id)) phase = 0;
    if (id != g_sys_id || s != g_sys_scale || phase != g_sys_phase) {
        SysCursorRender(id, s, phase, true, g_sys_px, &g_sys_hx, &g_sys_hy, g_sys_scratch);
        g_sys_id = id; g_sys_scale = s; g_sys_phase = phase;
    }
    cursor_blit(&t, dx, dy, g_sys_px, SYSCUR_BOX * s, SYSCUR_BOX * s, g_sys_hx, g_sys_hy, 1);
}

/* The arrow */
void GdiCursorDraw(int dx, int dy) { GdiCursorDrawSys(dx, dy, OCR_NORMAL, 0); }

void GdiCursorDrawShape(int dx, int dy, const GdiCursorShape *c, int frame)
{
    if (!g.ready) return;
    g_under_w = g_under_h = 0;
    if (!c || c->hidden || c->nframes <= 0) return;
    CurTarget t;
    if (!cursor_target(&dx, &dy, &t)) return;
    if (frame < 0 || frame >= c->nframes) frame = 0;
    const UINT32 *px = c->argb + (size_t)frame * c->w * c->h;
    cursor_blit(&t, dx, dy, px, c->w, c->h, c->hot_x, c->hot_y, c->dev ? 1 : t.s);
}

void GdiCursorErase(int dx, int dy)
{
    (void)dx; (void)dy;
    if (!g.ready || !g_under_vram) return;
    for (int j = 0; j < g_under_h; j++) {
        UINT32 *row = g_under_vram + (size_t)(g_under_y + j) * g_under_stride + g_under_x;
        for (int i = 0; i < g_under_w; i++)
            row[i] = g_under[j * CUR_MAX_W + i];
    }
    DisplayHeadDamage(g_under_head, g_under_x, g_under_y, g_under_w, g_under_h);
    g_under_w = g_under_h = 0;
}

void GdiCursorForget(void)
{
    g_under_vram = NULL;
    g_under_w = g_under_h = 0;
}
