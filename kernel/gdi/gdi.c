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

#define FX            256     /* fixed-point one: 1/256 device pixel */
#define LINE_BASELINE  12     /* baseline within the 16px logical line box */
#define LARGE_BASELINE 23     /* baseline within the 30px heading line box */

/* -----------------------------------------------------------------------
 * Surface state
 * ----------------------------------------------------------------------- */
static struct {
    UINT32 *vram;  int vstride;   /* hardware framebuffer */
    UINT32 *buf;   int bstride;   /* back buffer (== vram if none) */
    int     dw, dh;               /* device size */
    int     s;                    /* scale: device px per logical px */
    int     lw, lh;               /* logical size */
    int     cx0, cy0, cx1, cy1;   /* clip rectangle, device px, [x0,x1) */
    UINT32 *cache;                /* saved copy of the back buffer */
    bool    cache_valid;
    bool    direct;               /* no back buffer: drawing goes to vram */
    bool    bgr, ready;
} g;

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
    UINT32 *p = &g.buf[(size_t)y * g.bstride + x];
    *p = (a >= 255) ? n : blend(*p, n, (UINT32)a);
}

/* Horizontal device span [x0, x1) at uniform coverage a */
static void span(int y, int x0, int x1, UINT32 n, int a)
{
    if (a <= 0 || y < g.cy0 || y >= g.cy1) return;
    x0 = imax(x0, g.cx0);
    x1 = imin(x1, g.cx1);
    UINT32 *p = &g.buf[(size_t)y * g.bstride];
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
/* (Re)read the screen surface: at boot, and after a display mode change */
static bool gdi_setup(void)
{
    FbRawSurface s;
    fb_get_raw(&s);
    if (!s.vram || s.width <= 0 || s.height <= 0) {
        g.ready = false;
        return false;
    }
    bool resized = s.width != g.dw || s.height != g.dh;
    g.vram    = s.vram;
    g.vstride = s.stride;
    g.dw      = s.width;
    g.dh      = s.height;
    g.bgr     = s.bgr;

    /* Integer scale so the logical desktop is at least 1280x800 */
    g.s = imin(g.dw / 1280, g.dh / 800);
    if (g.s < 1) g.s = 1;
    if (g.s > GDI_MAX_SCALE) g.s = GDI_MAX_SCALE;
    g.lw = g.dw / g.s;
    g.lh = g.dh / g.s;

    /* Back buffer; fall back to drawing on the framebuffer directly */
    if (resized || g.direct) {
        if (g.buf && !g.direct) kfree(g.buf);
        kfree(g.cache);
        g.cache = NULL;
        g.buf = kzalloc((size_t)g.dw * g.dh * sizeof(UINT32));
        g.direct = g.buf == NULL;
    }
    if (g.direct) {
        g.buf     = g.vram;
        g.bstride = g.vstride;
    } else {
        g.bstride = g.dw;
    }
    g.cache_valid = false;

    g.cx0 = 0; g.cy0 = 0; g.cx1 = g.dw; g.cy1 = g.dh;
    g.ready = true;
    kprintf("[GDI] %dx%d device, scale %dx -> %dx%d logical, %s\n",
            g.dw, g.dh, g.s, g.lw, g.lh,
            g.direct ? "direct (no back buffer)" :
            DisplayCanFlip() ? "double-buffered, page flipping" : "double-buffered");
    return true;
}

bool GdiInitialize(void) { return gdi_setup(); }
bool GdiDisplayChanged(void) { return gdi_setup(); }

/* -----------------------------------------------------------------------
 * Clipping and the frame cache
 * ----------------------------------------------------------------------- */
void GdiSetClip(GdiRect r)
{
    int s = g.s;
    g.cx0 = imax(r.x * s, 0);            g.cy0 = imax(r.y * s, 0);
    g.cx1 = imin((r.x + r.w) * s, g.dw); g.cy1 = imin((r.y + r.h) * s, g.dh);
    if (g.cx1 < g.cx0) g.cx1 = g.cx0;
    if (g.cy1 < g.cy0) g.cy1 = g.cy0;
}

void GdiResetClip(void)
{
    g.cx0 = 0; g.cy0 = 0; g.cx1 = g.dw; g.cy1 = g.dh;
}

bool GdiCacheSave(void)
{
    if (!g.ready || g.direct) return false;
    if (!g.cache) {
        g.cache = kmalloc((size_t)g.dw * g.dh * sizeof(UINT32));
        if (!g.cache) return false;
    }
    memcpy(g.cache, g.buf, (size_t)g.dw * g.dh * sizeof(UINT32));
    g.cache_valid = true;
    return true;
}

bool GdiCacheRestore(void)
{
    if (!g.cache_valid) return false;
    memcpy(g.buf, g.cache, (size_t)g.dw * g.dh * sizeof(UINT32));
    return true;
}

void GdiCacheInvalidate(void) { g.cache_valid = false; }

int GdiScreenW(void) { return g.ready ? g.lw : 0; }
int GdiScreenH(void) { return g.ready ? g.lh : 0; }
int GdiScale(void)   { return g.ready ? g.s  : 1; }

void GdiPresent(void)
{
    if (!g.ready || g.direct) return;
    /* With page flipping the frame goes to the page off screen, which is
     * also where the pointer is drawn next; GdiFlip() shows both at once */
    UINT32 *back = DisplayBackPage();
    if (back) g.vram = back;
    for (int y = 0; y < g.dh; y++)
        memcpy(g.vram + (size_t)y * g.vstride,
               g.buf  + (size_t)y * g.bstride,
               (size_t)g.dw * sizeof(UINT32));
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
    y0 = imax(y0, 0);
    y1 = imin(y1, g.dh);
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
    int ya = imax(y0, 0), yb = imin(y0 + h, g.dh);
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
        for (int y = ya; y < yb; y++) g.buf[(size_t)y * g.bstride + x] = n;
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
        UINT32 *out = g.buf + (size_t)y * g.bstride;
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
    UINT32 *p = &g.buf[(size_t)y * g.bstride + x];
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
    int    ya   = imax(b->y0, 0), yb = imin(b->y1, g.dh);

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
    int    ya    = imax(b->y0, 0), yb = imin(b->y1, g.dh);

    for (int y = ya; y < yb; y++) {
        if (y >= b->y0 + band && y < b->y1 - band) {       /* straight sides */
            span(y, b->x0, b->x0 + t, n, 255);
            span(y, b->x1 - t, b->x1, n, 255);
            continue;
        }
        for (int x = imax(b->x0, 0); x < imin(b->x1, g.dw); x++) {
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
    for (int y = imax(b.y0, g.cy0); y < imin(b.y1, g.cy1); y++)
        for (int x = imax(b.x0, g.cx0); x < imin(b.x1, g.cx1); x++) {
            int sd = rbox_sd(&b, x, y);
            if (sd < -w - FX) continue;           /* well inside: nothing to draw */
            int cv = cov_of(sd) - cov_of(sd + w);
            if (cv > 0) plot(x, y, n, cv * alpha / 255);
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
    int ya = imax(b.y0 - m, 0), yb = imin(b.y1 + m, g.dh);
    int xa = imax(b.x0 - m, 0), xb = imin(b.x1 + m, g.dw);
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
    for (int y = imax(y0, 0); y <= imin(y1, g.dh - 1); y++) {
        for (int x = imax(x0, 0); x <= imin(x1, g.dw - 1); x++) {
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
static int poly_to_dev(const GdiPoint *src, int n, int ox, int oy, GdiPoint *dst,
                       int *bx0, int *by0, int *bx1, int *by1)
{
    if (n > POLY_MAX) n = POLY_MAX;
    int f = g.s * (FX / 16);
    *bx0 = *by0 = 0x7FFFFFFF; *bx1 = *by1 = -0x7FFFFFFF;
    for (int i = 0; i < n; i++) {
        dst[i].x = src[i].x * f + ox * FX;
        dst[i].y = src[i].y * f + oy * FX;
        *bx0 = imin(*bx0, dst[i].x / FX); *bx1 = imax(*bx1, dst[i].x / FX);
        *by0 = imin(*by0, dst[i].y / FX); *by1 = imax(*by1, dst[i].y / FX);
    }
    return n;
}

void GdiFillPolygon(const GdiPoint *pts, int n, GdiColor c)
{
    if (!g.ready || n < 3) return;
    GdiPoint d[POLY_MAX];
    int x0, y0, x1, y1;
    n = poly_to_dev(pts, n, 0, 0, d, &x0, &y0, &x1, &y1);
    UINT32 col = pixof(c);
    for (int y = imax(y0 - 1, 0); y <= imin(y1 + 1, g.dh - 1); y++)
        for (int x = imax(x0 - 1, 0); x <= imin(x1 + 1, g.dw - 1); x++)
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
        UINT32 *p = &g.buf[(size_t)(yi + 1) * g.bstride + x];
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
        const UINT32 *row = g.buf + (size_t)(y0 + y) * g.bstride + x0;
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
        memcpy(img + (size_t)y * w, g.buf + (size_t)(y0 + y) * g.bstride + x0, (size_t)w * 4);
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
    int x0 = imax(b.x0 - 3 * br, 0), y0 = imax(b.y0 - 3 * br, 0);
    int x1 = imin(b.x1 + 3 * br, g.dw), y1 = imin(b.y1 + 3 * br, g.dh);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    UINT32 *img = blurred(x0, y0, w, h, br), *tmp = img;
    UINT32 tn = pixof(tint);
    int cx0 = imax(b.x0, g.cx0), cx1 = imin(b.x1, g.cx1);
    int cy0 = imax(b.y0, g.cy0), cy1 = imin(b.y1, g.cy1);
    for (int y = cy0; y < cy1; y++) {
        UINT32 *row = g.buf + (size_t)y * g.bstride;
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

/* Arrow outline, 1/16 logical px, tip at (0,0) */
static const GdiPoint g_arrow[] = {
    GDI_PT(0, 0),   GDI_PT(0, 17),   GDI_PT(4, 13.2), GDI_PT(6.9, 19.6),
    GDI_PT(9.6, 18.4), GDI_PT(6.8, 12.2), GDI_PT(12.2, 12.2),
};
#define ARROW_N      ((int)(sizeof(g_arrow) / sizeof(g_arrow[0])))
#define CUR_MAX_W    64
#define CUR_MAX_H    64

static UINT32 g_under[CUR_MAX_W * CUR_MAX_H];
static int    g_under_x, g_under_y, g_under_w, g_under_h;

void GdiCursorDraw(int dx, int dy)
{
    if (!g.ready) return;
    int s = g.s;
    GdiPoint d[POLY_MAX], sh[POLY_MAX];
    int x0, y0, x1, y1, t0, t1, t2, t3;
    int n = poly_to_dev(g_arrow, ARROW_N, dx, dy, d, &x0, &y0, &x1, &y1);
    poly_to_dev(g_arrow, ARROW_N, dx + s, dy + 2 * s, sh, &t0, &t1, &t2, &t3);

    int soft = 2 * s * FX;                          /* shadow softness */
    x0 -= 1; y0 -= 1; x1 += 3 * s + 1; y1 += 4 * s + 1;
    x0 = imax(x0, 0); y0 = imax(y0, 0);
    x1 = imin(x1, g.dw - 1); y1 = imin(y1, g.dh - 1);
    g_under_x = x0; g_under_y = y0;
    g_under_w = imax(imin(x1 - x0 + 1, CUR_MAX_W), 0);
    g_under_h = imax(imin(y1 - y0 + 1, CUR_MAX_H), 0);

    UINT32 black = pixof(GDI_BLACK), white = pixof(GDI_WHITE);
    int    bw    = s * FX;                          /* outline width */

    for (int j = 0; j < g_under_h; j++) {
        int y = y0 + j;
        UINT32 *row = g.vram + (size_t)y * g.vstride;
        for (int i = 0; i < g_under_w; i++) {
            int x = x0 + i;
            UINT32 px = row[x];
            g_under[j * CUR_MAX_W + i] = px;
            int fx = x * FX + FX / 2, fy = y * FX + FX / 2;

            int ss = poly_sd(sh, n, fx, fy);        /* soft shadow */
            if (ss < soft) {
                int f = ss <= 0 ? 255 : ((soft - ss) * 255) / soft;
                px = blend(px, black, (UINT32)(70 * f / 255));
            }
            int sd = poly_sd(d, n, fx, fy);
            px = blend(px, black, (UINT32)cov_of(sd));          /* outline */
            px = blend(px, white, (UINT32)cov_of(sd + bw));     /* fill    */
            row[x] = px;
        }
    }
}

void GdiCursorErase(int dx, int dy)
{
    (void)dx; (void)dy;
    if (!g.ready) return;
    for (int j = 0; j < g_under_h; j++) {
        UINT32 *row = g.vram + (size_t)(g_under_y + j) * g.vstride + g_under_x;
        for (int i = 0; i < g_under_w; i++)
            row[i] = g_under[j * CUR_MAX_W + i];
    }
    g_under_w = g_under_h = 0;
}
