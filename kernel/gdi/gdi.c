/*
 * gdi.c — NovaOS Graphics Device Interface (software renderer)
 *
 * Phase 7.  A small, dependency-free 2D rasterizer that writes directly
 * into the GOP linear framebuffer obtained from the HAL.  Everything is
 * clipped to the screen bounds; out-of-range coordinates are safe.
 *
 * The renderer keeps a private copy of the framebuffer surface so it does
 * not have to re-query the HAL on every primitive.  Pixels are stored in
 * the framebuffer's native channel order (BGR or RGB) which we resolve
 * once at init time.
 */

#include "gdi.h"
#include "../lib/string.h"

/* -----------------------------------------------------------------------
 * Surface state
 * ----------------------------------------------------------------------- */
static struct {
    UINT32 *vram;
    int     w, h, stride;
    bool    bgr;
    bool    ready;
} g;

/* Convert a logical GdiColor into the native VRAM pixel.
 *
 * GdiColor == FB_COLOR(r,g,b) == (b<<16)|(g<<8)|r, i.e. little-endian
 * bytes [R, G, B, 0].  That byte order is already what RGB hardware
 * (PixelRedGreenBlue, byte0 = Red) expects, so it is emitted as-is.  BGR
 * hardware (PixelBlueGreenRed, byte0 = Blue) needs Red and Blue swapped. */
static inline UINT32 pixof(GdiColor c)
{
    UINT32 r = c & 0xFF, gg = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    if (g.bgr)
        return (r << 16) | (gg << 8) | b;   /* byte0 = Blue */
    return (b << 16) | (gg << 8) | r;       /* byte0 = Red  (== c) */
}

static inline void put(int x, int y, UINT32 native)
{
    if ((unsigned)x >= (unsigned)g.w || (unsigned)y >= (unsigned)g.h) return;
    g.vram[(size_t)y * g.stride + x] = native;
}

/* Read a pixel back as a logical GdiColor (inverse of pixof, for blending). */
static inline GdiColor get(int x, int y)
{
    if ((unsigned)x >= (unsigned)g.w || (unsigned)y >= (unsigned)g.h) return 0;
    UINT32 n = g.vram[(size_t)y * g.stride + x];
    UINT32 c0 = n & 0xFF, c1 = (n >> 8) & 0xFF, c2 = (n >> 16) & 0xFF;
    if (g.bgr)                              /* n = [B,G,R] → FB_COLOR */
        return (c0 << 16) | (c1 << 8) | c2;
    return n;                               /* n already == FB_COLOR */
}

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */
bool GdiInitialize(void)
{
    FbRawSurface s;
    fb_get_raw(&s);
    if (!s.vram || s.width <= 0 || s.height <= 0) {
        g.ready = false;
        return false;
    }
    g.vram   = s.vram;
    g.w      = s.width;
    g.h      = s.height;
    g.stride = s.stride;
    g.bgr    = s.bgr;
    g.ready  = true;
    return true;
}

int GdiScreenW(void) { return g.ready ? g.w : 0; }
int GdiScreenH(void) { return g.ready ? g.h : 0; }

/* -----------------------------------------------------------------------
 * Color math
 * ----------------------------------------------------------------------- */
GdiColor GdiLerp(GdiColor a, GdiColor b, int t)
{
    if (t <= 0) return a;
    if (t >= 255) return b;
    int ra = GDI_R(a), ga = GDI_G(a), ba = GDI_B(a);
    int rb = GDI_R(b), gb = GDI_G(b), bb = GDI_B(b);
    int r = ra + ((rb - ra) * t) / 255;
    int gg = ga + ((gb - ga) * t) / 255;
    int bl = ba + ((bb - ba) * t) / 255;
    return GDI_C(r, gg, bl);
}

/* -----------------------------------------------------------------------
 * Rectangles & gradients
 * ----------------------------------------------------------------------- */
void GdiFillRect(GdiRect r, GdiColor c)
{
    if (!g.ready) return;
    UINT32 n = pixof(c);
    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w; if (x1 > g.w) x1 = g.w;
    int y1 = r.y + r.h; if (y1 > g.h) y1 = g.h;
    for (int y = y0; y < y1; y++) {
        UINT32 *line = g.vram + (size_t)y * g.stride;
        for (int x = x0; x < x1; x++) line[x] = n;
    }
}

void GdiGradientV(GdiRect r, GdiColor top, GdiColor bottom)
{
    if (!g.ready || r.h <= 0) return;
    int y0 = r.y < 0 ? 0 : r.y;
    int y1 = r.y + r.h; if (y1 > g.h) y1 = g.h;
    for (int y = y0; y < y1; y++) {
        int t = ((y - r.y) * 255) / (r.h - 1 > 0 ? r.h - 1 : 1);
        GdiColor row = GdiLerp(top, bottom, t);
        GdiFillRect(RECT(r.x, y, r.w, 1), row);
    }
}

void GdiGradientH(GdiRect r, GdiColor left, GdiColor right)
{
    if (!g.ready || r.w <= 0) return;
    int x0 = r.x < 0 ? 0 : r.x;
    int x1 = r.x + r.w; if (x1 > g.w) x1 = g.w;
    for (int x = x0; x < x1; x++) {
        int t = ((x - r.x) * 255) / (r.w - 1 > 0 ? r.w - 1 : 1);
        GdiColor col = GdiLerp(left, right, t);
        GdiVLine(x, r.y, r.y + r.h - 1, col);
    }
}

void GdiAlphaFill(GdiRect r, GdiColor c, int alpha)
{
    if (!g.ready) return;
    if (alpha >= 255) { GdiFillRect(r, c); return; }
    if (alpha <= 0) return;
    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w; if (x1 > g.w) x1 = g.w;
    int y1 = r.y + r.h; if (y1 > g.h) y1 = g.h;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            GdiColor bg = get(x, y);
            put(x, y, pixof(GdiLerp(bg, c, alpha)));
        }
    }
}

void GdiHLine(int y, int x0, int x1, GdiColor c)
{
    if (!g.ready) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    UINT32 n = pixof(c);
    for (int x = x0; x <= x1; x++) put(x, y, n);
}

void GdiVLine(int x, int y0, int y1, GdiColor c)
{
    if (!g.ready) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    UINT32 n = pixof(c);
    for (int y = y0; y <= y1; y++) put(x, y, n);
}

void GdiFillCircle(int cx, int cy, int rad, GdiColor c)
{
    if (!g.ready || rad <= 0) return;
    UINT32 n = pixof(c);
    int r2 = rad * rad;
    for (int dy = -rad; dy <= rad; dy++) {
        int dx2 = r2 - dy * dy;
        if (dx2 < 0) continue;
        /* integer sqrt */
        int dx = 0; while ((dx + 1) * (dx + 1) <= dx2) dx++;
        int y = cy + dy;
        for (int x = cx - dx; x <= cx + dx; x++) put(x, y, n);
    }
}

/* -----------------------------------------------------------------------
 * Rounded rectangles
 *
 * We compute a per-row inset for the four corner quadrants using an
 * integer circle test, then fill / blend the resulting span.
 * ----------------------------------------------------------------------- */

/* For a given row offset within a corner of radius `rad`, return how many
 * pixels are clipped off the edge (0 in the straight middle section). */
static int corner_inset(int row_from_edge, int rad)
{
    if (rad <= 0) return 0;
    if (row_from_edge >= rad) return 0;     /* straight section */
    int dy = rad - 1 - row_from_edge;       /* distance from corner centre */
    int dx2 = rad * rad - dy * dy;
    if (dx2 < 0) dx2 = 0;
    int dx = 0; while ((dx + 1) * (dx + 1) <= dx2) dx++;
    return rad - dx;
}

void GdiRoundRect(GdiRect r, int rad, GdiColor fill, GdiColor border)
{
    if (!g.ready || r.w <= 0 || r.h <= 0) return;
    if (rad * 2 > r.w) rad = r.w / 2;
    if (rad * 2 > r.h) rad = r.h / 2;
    bool do_fill   = (fill   != GDI_TRANSPARENT);
    bool do_border = (border != GDI_TRANSPARENT);
    UINT32 nf = do_fill ? pixof(fill) : 0;
    UINT32 nb = do_border ? pixof(border) : 0;

    for (int row = 0; row < r.h; row++) {
        int from_top    = row;
        int from_bottom = r.h - 1 - row;
        int edge = from_top < from_bottom ? from_top : from_bottom;
        int inset = corner_inset(edge, rad);
        int xs = r.x + inset;
        int xe = r.x + r.w - 1 - inset;
        int y  = r.y + row;
        if (do_fill)
            for (int x = xs; x <= xe; x++) put(x, y, nf);
        if (do_border) {
            put(xs, y, nb);
            put(xe, y, nb);
            /* top & bottom edges */
            if (edge == 0 || (inset != corner_inset(edge ? edge - 1 : 0, rad)))
                for (int x = xs; x <= xe; x++) put(x, y, nb);
        }
    }
}

void GdiRoundAlpha(GdiRect r, int rad, GdiColor c, int alpha)
{
    if (!g.ready || r.w <= 0 || r.h <= 0) return;
    if (alpha >= 255) { GdiRoundRect(r, rad, c, GDI_TRANSPARENT); return; }
    if (alpha <= 0) return;
    if (rad * 2 > r.w) rad = r.w / 2;
    if (rad * 2 > r.h) rad = r.h / 2;
    for (int row = 0; row < r.h; row++) {
        int from_top    = row;
        int from_bottom = r.h - 1 - row;
        int edge = from_top < from_bottom ? from_top : from_bottom;
        int inset = corner_inset(edge, rad);
        int xs = r.x + inset;
        int xe = r.x + r.w - 1 - inset;
        int y  = r.y + row;
        for (int x = xs; x <= xe; x++) {
            GdiColor bg = get(x, y);
            put(x, y, pixof(GdiLerp(bg, c, alpha)));
        }
    }
}

void GdiRoundGradV(GdiRect r, int rad, GdiColor top, GdiColor bottom)
{
    if (!g.ready || r.w <= 0 || r.h <= 0) return;
    if (rad * 2 > r.w) rad = r.w / 2;
    if (rad * 2 > r.h) rad = r.h / 2;
    for (int row = 0; row < r.h; row++) {
        int from_top    = row;
        int from_bottom = r.h - 1 - row;
        int edge = from_top < from_bottom ? from_top : from_bottom;
        int inset = corner_inset(edge, rad);
        int xs = r.x + inset;
        int xe = r.x + r.w - 1 - inset;
        int y  = r.y + row;
        int t = (row * 255) / (r.h - 1 > 0 ? r.h - 1 : 1);
        UINT32 n = pixof(GdiLerp(top, bottom, t));
        for (int x = xs; x <= xe; x++) put(x, y, n);
    }
}

void GdiPutPixel(int x, int y, GdiColor c)
{
    if (!g.ready) return;
    put(x, y, pixof(c));
}

/* -----------------------------------------------------------------------
 * Raw blit save / restore (cursor save-under) — native pixels, clipped.
 * The buffer is always treated as r.w*r.h entries in row-major order; for
 * out-of-bounds pixels we store/restore 0 so save and restore stay aligned.
 * ----------------------------------------------------------------------- */
void GdiBlitSave(GdiRect r, UINT32 *dst)
{
    if (!g.ready || !dst) return;
    for (int row = 0; row < r.h; row++) {
        int y = r.y + row;
        for (int col = 0; col < r.w; col++) {
            int x = r.x + col;
            UINT32 v = 0;
            if ((unsigned)x < (unsigned)g.w && (unsigned)y < (unsigned)g.h)
                v = g.vram[(size_t)y * g.stride + x];
            dst[row * r.w + col] = v;
        }
    }
}

void GdiBlitRestore(GdiRect r, const UINT32 *src)
{
    if (!g.ready || !src) return;
    for (int row = 0; row < r.h; row++) {
        int y = r.y + row;
        if ((unsigned)y >= (unsigned)g.h) continue;
        for (int col = 0; col < r.w; col++) {
            int x = r.x + col;
            if ((unsigned)x < (unsigned)g.w)
                g.vram[(size_t)y * g.stride + x] = src[row * r.w + col];
        }
    }
}

/* -----------------------------------------------------------------------
 * Text
 * ----------------------------------------------------------------------- */
int GdiTextW(const char *s) { return s ? (int)(strlen(s) * GDI_FONT_W) : 0; }

void GdiText(int x, int y, const char *s, GdiColor fg, GdiColor bg)
{
    fb_draw_string(x, y, s, (FbColor)fg, (FbColor)bg);
}

void GdiTextT(int x, int y, const char *s, GdiColor fg)
{
    fb_draw_string_trans(x, y, s, (FbColor)fg);
}

void GdiTextBold(int x, int y, const char *s, GdiColor fg)
{
    fb_draw_string_trans(x,     y, s, (FbColor)fg);
    fb_draw_string_trans(x + 1, y, s, (FbColor)fg);
}

void GdiTextCenter(int x, int y, int w, const char *s, GdiColor fg)
{
    int tw = GdiTextW(s);
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    GdiTextT(tx, y, s, fg);
}
