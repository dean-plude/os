/*
 * gdi.h — NovaOS Graphics Device Interface (kernel software renderer)
 *
 * Phase 7: 2D drawing API over the GOP linear framebuffer.
 * All rendering is pure software; no GPU acceleration.
 *
 * Color format (GdiColor == FbColor):
 *   bits  7..0  = Red
 *   bits 15..8  = Green
 *   bits 23..16 = Blue
 *   bits 31..24 = unused (0)
 *   → construct with GDI_C(r,g,b) or FB_COLOR(r,g,b)
 */

#pragma once

#include "../include/types.h"
#include "../hal/framebuffer.h"

typedef UINT32 GdiColor;

/* -----------------------------------------------------------------------
 * Color helpers
 * ----------------------------------------------------------------------- */
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

/* Rectangle */
typedef struct { int x, y, w, h; } GdiRect;
#define RECT(x_,y_,w_,h_) ((GdiRect){(x_),(y_),(w_),(h_)})

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

/* Call once after fb_init(); grabs the raw framebuffer surface.
 * Returns true if a usable framebuffer is present. */
bool GdiInitialize(void);

int GdiScreenW(void);
int GdiScreenH(void);

/* Blend two colors: t=0 → a, t=255 → b. */
GdiColor GdiLerp(GdiColor a, GdiColor b, int t);

/* -----------------------------------------------------------------------
 * Primitive drawing
 * ----------------------------------------------------------------------- */

void GdiFillRect  (GdiRect r, GdiColor c);
void GdiGradientV (GdiRect r, GdiColor top,  GdiColor bottom);
void GdiGradientH (GdiRect r, GdiColor left, GdiColor right);

/* Alpha-blend a solid color over the existing content.
 * alpha: 0 = fully transparent (no change), 255 = fully opaque. */
void GdiAlphaFill (GdiRect r, GdiColor c, int alpha);

/* Filled rounded rectangle.
 * fill / border = GDI_TRANSPARENT to skip that part. */
void GdiRoundRect (GdiRect r, int rad, GdiColor fill, GdiColor border);

/* Rounded rectangle alpha-blended over existing content. */
void GdiRoundAlpha(GdiRect r, int rad, GdiColor c, int alpha);

/* Rounded rectangle filled with a vertical gradient. */
void GdiRoundGradV(GdiRect r, int rad, GdiColor top, GdiColor bottom);

void GdiHLine(int y, int x0, int x1, GdiColor c);
void GdiVLine(int x, int y0, int y1, GdiColor c);

void GdiFillCircle(int cx, int cy, int rad, GdiColor c);

/* -----------------------------------------------------------------------
 * Text  (reuses the 8×16 VGA font embedded in framebuffer.c)
 * ----------------------------------------------------------------------- */

/* Solid background */
void GdiText    (int x, int y, const char *s, GdiColor fg, GdiColor bg);
/* Transparent background */
void GdiTextT   (int x, int y, const char *s, GdiColor fg);
/* Bold (double-strike at x+1) */
void GdiTextBold(int x, int y, const char *s, GdiColor fg);
/* Centered within [x, x+w), transparent background */
void GdiTextCenter(int x, int y, int w, const char *s, GdiColor fg);
/* Pixel width of a string */
int  GdiTextW   (const char *s);

#define GDI_FONT_W  8
#define GDI_FONT_H  16
