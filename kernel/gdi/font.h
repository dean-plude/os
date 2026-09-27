/*
 * font.h — pre-rasterized anti-aliased font faces for the GDI
 *
 * The kernel is built without SSE, so it cannot run a TrueType rasterizer.
 * Instead tools/mkfont.c rasterizes the font on the build host (FreeType,
 * light hinting, 8-bit coverage) into font_inter.c, one face per style
 * and per integer display scale.  At run time glyphs are only blended.
 * Styles: Inter Regular, Inter SemiBold ("bold"), Cascadia Mono, and a
 * larger Inter SemiBold for headings.
 */

#pragma once

#include "../include/types.h"

typedef struct {
    UINT32 off;      /* offset of the glyph's coverage bitmap in bits[] */
    UINT8  w, h;     /* bitmap size in device pixels */
    INT8   bx;       /* left bearing: pen x → bitmap left */
    INT8   by;       /* top bearing: baseline → bitmap top (positive = up) */
    UINT16 adv;      /* advance width in 26.6 fixed point device pixels */
} GdiGlyph;

typedef struct {
    UINT8           px;          /* nominal size in device pixels */
    UINT8           first, count;/* character range covered */
    const GdiGlyph *glyphs;
    const UINT8    *bits;        /* 8-bit coverage, row-major per glyph */
} GdiFace;

enum {
    GDI_FONT_REGULAR = 0,   /* Inter Regular 13px   */
    GDI_FONT_BOLD    = 1,   /* Inter SemiBold 13px  */
    GDI_FONT_MONO    = 2,   /* Cascadia Mono 13px   */
    GDI_FONT_DISPLAY = 3,   /* Inter SemiBold 24px (headings) */
    GDI_FONT_STYLES  = 4
};

/* Faces for display scales 1..GDI_MAX_SCALE, indexed [style][scale - 1] */
#define GDI_MAX_SCALE  2
extern const GdiFace g_gdi_faces[GDI_FONT_STYLES][GDI_MAX_SCALE];
