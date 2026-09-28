/*
 * gdi.h — NovaOS Graphics Device Interface (software renderer)
 *
 * Coordinates are LOGICAL pixels.  On high-resolution displays the GDI
 * picks an integer scale factor (GdiScale()) and rasterizes every shape
 * and glyph at full DEVICE resolution, so a 2x display shows the same
 * layout with twice the detail rather than bigger pixels.
 *
 * Shapes are anti-aliased (coverage from a signed distance to the edge),
 * text uses the pre-rasterized Inter font (see font.h), and drawing goes
 * to an off-screen back buffer that GdiPresent() copies to the screen.
 */

#pragma once

#include "../include/types.h"
#include "../hal/framebuffer.h"

/* -----------------------------------------------------------------------
 * Colors — 0x00BBGGRR (same layout as FB_COLOR)
 * ----------------------------------------------------------------------- */
typedef UINT32 GdiColor;

#define GDI_C(r,g,b)    FB_COLOR((r),(g),(b))
#define GDI_R(c)        ((UINT32)(c) & 0xFFu)
#define GDI_G(c)        (((UINT32)(c) >>  8) & 0xFFu)
#define GDI_B(c)        (((UINT32)(c) >> 16) & 0xFFu)

#define GDI_BLACK       GDI_C(0x00,0x00,0x00)
#define GDI_WHITE       GDI_C(0xFF,0xFF,0xFF)
#define GDI_GRAY        GDI_C(0x80,0x80,0x80)
#define GDI_LGRAY       GDI_C(0xC8,0xC8,0xC8)
#define GDI_DGRAY       GDI_C(0x28,0x28,0x30)
#define GDI_TRANSPARENT ((GdiColor)0xFF000000u)

typedef struct { int x, y, w, h; } GdiRect;
#define RECT(x_,y_,w_,h_) ((GdiRect){(x_),(y_),(w_),(h_)})

/* A point in 1/16 logical pixels, for sub-pixel polygon vertices */
typedef struct { int x, y; } GdiPoint;
#define GDI_PT(x_,y_)   ((GdiPoint){ (int)((x_) * 16), (int)((y_) * 16) })

/* -----------------------------------------------------------------------
 * Lifecycle and surface
 * ----------------------------------------------------------------------- */
bool GdiInitialize(void);

int  GdiScreenW(void);     /* logical size */
int  GdiScreenH(void);
int  GdiScale(void);       /* device pixels per logical pixel (1 or 2) */

/* Copy the back buffer to the screen (whole frame). */
void GdiPresent(void);

/* Clip all drawing to a logical rectangle (e.g. a window's client area). */
void GdiSetClip(GdiRect r);
void GdiResetClip(void);

/* Frame cache: save the back buffer (e.g. after drawing the static desktop
 * background) and restore it at the start of the next frame instead of
 * redrawing.  Save/Restore return false if no cache is available. */
bool GdiCacheSave(void);
bool GdiCacheRestore(void);
void GdiCacheInvalidate(void);

GdiColor GdiLerp(GdiColor a, GdiColor b, int t);   /* t = 0..255 */

/* -----------------------------------------------------------------------
 * Filled shapes (all anti-aliased where edges are not pixel-aligned)
 * ----------------------------------------------------------------------- */
void GdiFillRect  (GdiRect r, GdiColor c);
void GdiGradientV (GdiRect r, GdiColor top,  GdiColor bottom);
void GdiGradientH (GdiRect r, GdiColor left, GdiColor right);
void GdiAlphaFill (GdiRect r, GdiColor c, int alpha);

/* Rounded rectangle: fill and/or a 1-logical-pixel border.
 * Pass GDI_TRANSPARENT for either to skip it. */
void GdiRoundRect (GdiRect r, int rad, GdiColor fill, GdiColor border);
void GdiRoundAlpha(GdiRect r, int rad, GdiColor c, int alpha);
void GdiRoundGradV(GdiRect r, int rad, GdiColor top, GdiColor bottom);
/* A 1-logical-pixel border at opacity @alpha (hairlines on glass) */
void GdiRoundBorderAlpha(GdiRect r, int rad, GdiColor c, int alpha);

/* Frosted glass: blur what is already drawn under the rounded rectangle
 * (radius about @blur logical px), then tint it with @tint at opacity
 * @tint_alpha (0..255). */
void GdiBackdrop(GdiRect r, int rad, int blur, GdiColor tint, int tint_alpha);

/* Soft shadow around a rounded rectangle: opacity `alpha` at the edge,
 * fading to zero `blur` logical pixels outside it. */
void GdiDropShadow(GdiRect r, int rad, int blur, int alpha);

void GdiHLine(int y, int x0, int x1, GdiColor c);
void GdiVLine(int x, int y0, int y1, GdiColor c);

void GdiFillCircle(int cx, int cy, int rad, GdiColor c);

/* Filled polygon, vertices in 1/16 logical pixels (see GDI_PT). */
void GdiFillPolygon(const GdiPoint *pts, int n, GdiColor c);

/* Anti-aliased line segment; endpoints and width in 1/16 logical px.
 * Use integer arithmetic for run-time points (no floating point in the
 * kernel): e.g. (GdiPoint){ x * 16 + 8, y * 16 + 8 } for a pixel centre. */
void GdiLine(GdiPoint a, GdiPoint b, int width16, GdiColor c);

/* Fill everything inside `r` below the curve y = fn(x): x and the result
 * are in 1/256 logical pixels.  Used for smooth wallpaper shapes. */
typedef int (*GdiCurveFn)(int x_256, void *ctx);
void GdiFillUnderCurve(GdiRect r, GdiCurveFn fn, void *ctx, GdiColor c);

void GdiPutPixel(int x, int y, GdiColor c);

/* Blit a user window bitmap (GdiColor/COLORREF pixels) into a logical rect. */
void GdiBlitBGRA(GdiRect dst, const UINT32 *src, int src_stride);

/* Draw a w x h image of straight-alpha 0xAARRGGBB pixels scaled into the
 * logical rect @dst at device resolution (area-averaged when shrinking,
 * bilinear when enlarging), blending by alpha. */
void GdiDrawImage(GdiRect dst, const UINT32 *px, int w, int h);
/* The same, unscaled: one image pixel per device pixel, at logical (x, y). */
void GdiDrawImageDevice(int x, int y, const UINT32 *px, int w, int h);
/* Enlarged k times, each image pixel a sharp k x k block of device pixels. */
void GdiDrawImageZoom(int x, int y, const UINT32 *px, int w, int h, int k);

/* -----------------------------------------------------------------------
 * Text — Inter, 13px on a 16px line; (x, y) is the top of the line box
 * ----------------------------------------------------------------------- */
#define GDI_FONT_H  16

void GdiText      (int x, int y, const char *s, GdiColor fg, GdiColor bg);
void GdiTextT     (int x, int y, const char *s, GdiColor fg);
void GdiTextBold  (int x, int y, const char *s, GdiColor fg);
void GdiTextCenter(int x, int y, int w, const char *s, GdiColor fg);
/* Text over pictures: a soft, blurred black drop shadow (opacity @shadow,
 * 0..255) one pixel below, then the text. */
void GdiTextShadow      (int x, int y, const char *s, GdiColor fg, int shadow);
void GdiTextShadowCenter(int x, int y, int w, const char *s, GdiColor fg, int shadow);
int  GdiTextW     (const char *s);      /* logical width, regular */
int  GdiTextBoldW (const char *s);      /* logical width, bold */

/* Heading text (Inter SemiBold 24px) on a 30px line box at (x, y). */
#define GDI_LARGE_H 30
void GdiTextLarge (int x, int y, const char *s, GdiColor fg);
int  GdiTextLargeW(const char *s);

/* Monospace text (Cascadia Mono).  Character cells are GdiMonoCellW256()
 * 1/256-logical-pixels wide, so column c starts at x + c * cell / 256. */
void GdiTextMono  (int x, int y, const char *s, GdiColor fg);
void GdiTextMonoN (int x, int y, const char *s, int n, GdiColor fg);
int  GdiMonoCellW256(void);

/* -----------------------------------------------------------------------
 * Screen overlay (drawn straight to the screen, not the back buffer)
 *
 * Used for the mouse pointer.  Draw saves the pixels it covers and Erase
 * puts them back; after GdiPresent() the saved pixels are stale, so just
 * draw again without erasing.  Positions are DEVICE pixels (the arrow's
 * tip), so the pointer is rendered at native resolution.
 * ----------------------------------------------------------------------- */
void GdiCursorDraw (int dev_x, int dev_y);
void GdiCursorErase(int dev_x, int dev_y);
