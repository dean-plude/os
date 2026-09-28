/*
 * font_nova.c — NetSurf's framebuffer font driver on NovaOS
 *
 * Replaces the frontend's 8x16 bitmap font with anti-aliased TrueType text:
 * Inter for sans-serif (and the other families) and DejaVu Sans Mono for
 * monospace, loaded from C:\Windows\Fonts and rasterized on demand by
 * stb_truetype.  Glyphs are cached per face, pixel size and quarter-pixel
 * horizontal offset; pens advance in 26.6 fixed point, so measuring and
 * drawing a string agree exactly and letter spacing stays even at small
 * sizes.  Italic text is sheared from the upright face (Inter ships no
 * italic in this build).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <libnsfb.h>
#include <libnsfb_plot.h>

#include "utils/utf8.h"
#include "utils/log.h"
#include "netsurf/utf8.h"
#include "netsurf/layout.h"
#include "netsurf/browser.h"
#include "netsurf/plot_style.h"
#include "framebuffer/gui.h"
#include "framebuffer/font.h"

#define FONT_DIR       "/Windows/Fonts/"
#define CACHE_BUCKETS  4096
#define CACHE_LIMIT    (6 * 1024 * 1024)    /* glyph bitmap bytes before a flush */
#define ITALIC_SHEAR   0.2f                  /* x shift per pixel of height */

enum { F_SANS, F_SANS_BOLD, F_MONO, F_MONO_BOLD, F_COUNT };

static const char *const g_files[F_COUNT] = {
    "Inter-Regular.ttf", "Inter-Bold.ttf", "DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf",
};

typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
    bool           ok;
    int            ascent, descent, line_gap;   /* font units */
} Face;

static Face g_face[F_COUNT];

typedef struct Glyph {
    struct Glyph  *next;
    uint32_t       ucs4;
    uint16_t       px;          /* pixel size */
    uint8_t        face, sub;   /* face index | italic flag << 7; quarter-pixel offset */
    int32_t        advance;     /* 26.6 */
    int16_t        x0, y0;      /* bitmap offset from the pen (y: from baseline) */
    uint16_t       w, h;
    unsigned char  bits[];
} Glyph;

static Glyph *g_cache[CACHE_BUCKETS];
static size_t g_cache_bytes;

/* -----------------------------------------------------------------------
 * Faces
 * ----------------------------------------------------------------------- */
static bool load_face(Face *f, const char *name)
{
    char path[128];
    snprintf(path, sizeof(path), FONT_DIR "%s", name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = f->data && fread(f->data, 1, (size_t)n, fp) == (size_t)n &&
              stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0));
    fclose(fp);
    if (!ok) {
        free(f->data);
        f->data = NULL;
        return false;
    }
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
    f->ok = true;
    return true;
}

static int pick_face(const plot_font_style_t *fs)
{
    bool bold = fs->weight >= 600;
    int f = fs->family == PLOT_FONT_FAMILY_MONOSPACE ? (bold ? F_MONO_BOLD : F_MONO)
                                                     : (bold ? F_SANS_BOLD : F_SANS);
    if (!g_face[f].ok) f = (f == F_MONO_BOLD && g_face[F_MONO].ok) ? F_MONO
                         : (f == F_SANS_BOLD && g_face[F_SANS].ok) ? F_SANS : f;
    if (!g_face[f].ok) for (f = 0; f < F_COUNT && !g_face[f].ok; f++) { }
    return f;
}

/* Font size in pixels: points at the browser's DPI */
static int pixel_size(const plot_font_style_t *fs)
{
    int px = (int)(((int64_t)fs->size * browser_get_dpi() + (72 * PLOT_STYLE_SCALE) / 2) /
                   (72 * PLOT_STYLE_SCALE));
    if (px < 4) px = 4;
    if (px > 400) px = 400;
    return px;
}

/* -----------------------------------------------------------------------
 * Glyph cache
 * ----------------------------------------------------------------------- */
static void cache_flush(void)
{
    for (int i = 0; i < CACHE_BUCKETS; i++) {
        Glyph *g = g_cache[i];
        while (g) {
            Glyph *n = g->next;
            free(g);
            g = n;
        }
        g_cache[i] = NULL;
    }
    g_cache_bytes = 0;
}

/* Face and glyph index for a character: the chosen face, else any other
 * loaded face that has it, else the chosen face's missing-glyph box */
static int find_glyph(int face, uint32_t ucs4, int *index)
{
    int gi = stbtt_FindGlyphIndex(&g_face[face].info, (int)ucs4);
    if (gi) { *index = gi; return face; }
    for (int f = 0; f < F_COUNT; f++) {
        if (f == face || !g_face[f].ok) continue;
        gi = stbtt_FindGlyphIndex(&g_face[f].info, (int)ucs4);
        if (gi) { *index = gi; return f; }
    }
    *index = 0;
    return face;
}

static Glyph *get_glyph(int face, bool italic, int px, int sub, uint32_t ucs4)
{
    unsigned h = (ucs4 * 2654435761u) ^ ((unsigned)px << 20) ^ ((unsigned)face << 28) ^
                 ((unsigned)sub << 16) ^ (italic ? 0x8000u : 0);
    h %= CACHE_BUCKETS;
    uint8_t key_face = (uint8_t)(face | (italic ? 0x80 : 0));
    for (Glyph *g = g_cache[h]; g; g = g->next)
        if (g->ucs4 == ucs4 && g->px == px && g->face == key_face && g->sub == sub) return g;

    if (g_cache_bytes > CACHE_LIMIT) cache_flush();

    int index;
    int f = find_glyph(face, ucs4, &index);
    const stbtt_fontinfo *info = &g_face[f].info;
    float scale = stbtt_ScaleForMappingEmToPixels(info, (float)px);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(info, index, &adv, &lsb);

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float shift = sub / 4.0f;
    stbtt_GetGlyphBitmapBoxSubpixel(info, index, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
    int w = x1 - x0, hgt = y1 - y0;
    if (w < 0) w = 0;
    if (hgt < 0) hgt = 0;
    int extra = italic && hgt ? (int)ceilf((hgt - 1) * ITALIC_SHEAR) + 1 : 0;
    size_t bytes = (size_t)(w + extra) * (size_t)hgt;
    Glyph *g = malloc(sizeof(Glyph) + (bytes ? bytes : 1));
    if (!g) return NULL;
    g->ucs4 = ucs4;
    g->px = (uint16_t)px;
    g->face = key_face;
    g->sub = (uint8_t)sub;
    g->advance = (int32_t)lroundf(adv * scale * 64.0f);
    g->x0 = (int16_t)x0;
    g->y0 = (int16_t)y0;
    g->w = (uint16_t)(w + extra);
    g->h = (uint16_t)hgt;
    if (w && hgt) {
        if (!extra) {
            stbtt_MakeGlyphBitmapSubpixel(info, g->bits, w, hgt, w, scale, scale, shift, 0, index);
        } else {
            /* shear: each row moves right in proportion to its height above
             * the bottom row, blending coverage between neighbouring pixels;
             * x0 then moves back so the baseline row stays where it was */
            unsigned char *up = malloc((size_t)w * hgt);
            if (up) {
                stbtt_MakeGlyphBitmapSubpixel(info, up, w, hgt, w, scale, scale, shift, 0, index);
                memset(g->bits, 0, bytes);
                for (int y = 0; y < hgt; y++) {
                    float s = (float)(hgt - 1 - y) * ITALIC_SHEAR;
                    int si = (int)s;
                    float fr = s - si;
                    unsigned char *dst = g->bits + (size_t)y * g->w;
                    for (int x = 0; x < w; x++) {
                        int c = up[(size_t)y * w + x];
                        if (!c) continue;
                        int a = si + x, b = a + 1;
                        if (a < g->w) { int v = dst[a] + (int)(c * (1.0f - fr)); dst[a] = (unsigned char)(v > 255 ? 255 : v); }
                        if (b < g->w) { int v = dst[b] + (int)(c * fr); dst[b] = (unsigned char)(v > 255 ? 255 : v); }
                    }
                }
                free(up);
                g->x0 -= (int16_t)lroundf((y1 - 1) * ITALIC_SHEAR);   /* baseline row: y = -y0 */
            } else {
                memset(g->bits, 0, bytes);
            }
        }
    } else if (bytes) {
        memset(g->bits, 0, bytes);      /* e.g. italic padding around an empty box */
    }
    g->next = g_cache[h];
    g_cache[h] = g;
    g_cache_bytes += sizeof(Glyph) + bytes;
    return g;
}

/* Advance of one character (26.6), independent of the subpixel offset */
static int32_t advance_of(int face, bool italic, int px, uint32_t ucs4)
{
    Glyph *g = get_glyph(face, italic, px, 0, ucs4);
    return g ? g->advance : 0;
}

static bool invisible(uint32_t u)
{
    return (u >= 0x200b && u <= 0x200f) || u == 0xfeff || u == '\r' || u == '\n';
}

/* -----------------------------------------------------------------------
 * Frontend interface
 * ----------------------------------------------------------------------- */
bool fb_font_init(void)
{
    int n = 0;
    for (int i = 0; i < F_COUNT; i++) n += load_face(&g_face[i], g_files[i]);
    if (!n) {
        NSLOG(netsurf, ERROR, "no fonts found in " FONT_DIR);
        return false;
    }
    return true;
}

bool fb_font_finalise(void)
{
    cache_flush();
    for (int i = 0; i < F_COUNT; i++) {
        free(g_face[i].data);
        g_face[i].data = NULL;
        g_face[i].ok = false;
    }
    return true;
}

typedef struct {
    int  face, px;
    bool italic;
} Style;

static Style style_of(const plot_font_style_t *fs)
{
    Style s;
    s.face = pick_face(fs);
    s.px = pixel_size(fs);
    s.italic = (fs->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) != 0;
    return s;
}

nserror fb_font_width(const plot_font_style_t *fstyle, const char *string, size_t length, int *width)
{
    Style s = style_of(fstyle);
    int32_t pen = 0;
    size_t i = 0;
    while (i < length) {
        uint32_t u = utf8_to_ucs4(string + i, length - i);
        i = utf8_next(string, length, i);
        if (!invisible(u)) pen += advance_of(s.face, s.italic, s.px, u);
    }
    *width = (pen + 32) >> 6;
    return NSERROR_OK;
}

nserror fb_font_position(const plot_font_style_t *fstyle, const char *string, size_t length,
                         int x, size_t *char_offset, int *actual_x)
{
    Style s = style_of(fstyle);
    int32_t pen = 0, target = (int32_t)x << 6;
    size_t i = 0;
    while (i < length) {
        uint32_t u = utf8_to_ucs4(string + i, length - i);
        int32_t a = invisible(u) ? 0 : advance_of(s.face, s.italic, s.px, u);
        if (pen + a / 2 > target) break;           /* x is nearer this character's start */
        pen += a;
        i = utf8_next(string, length, i);
    }
    *char_offset = i;
    *actual_x = (pen + 32) >> 6;
    return NSERROR_OK;
}

static nserror fb_font_split(const plot_font_style_t *fstyle, const char *string, size_t length,
                             int x, size_t *char_offset, int *actual_x)
{
    Style s = style_of(fstyle);
    int32_t pen = 0, limit = (int32_t)x << 6, space_pen = 0;
    size_t i = 0, space_idx = 0;
    while (i < length) {
        uint32_t u = utf8_to_ucs4(string + i, length - i);
        if (u == ' ' && i) {
            space_pen = pen;
            space_idx = i;
        }
        pen += invisible(u) ? 0 : advance_of(s.face, s.italic, s.px, u);
        if (pen > limit && space_idx) {
            *char_offset = space_idx;
            *actual_x = (space_pen + 32) >> 6;
            return NSERROR_OK;
        }
        i = utf8_next(string, length, i);
    }
    *char_offset = length;
    *actual_x = (pen + 32) >> 6;
    return NSERROR_OK;
}

nserror fb_nova_plot_text(nsfb_t *nsfb, const plot_font_style_t *fstyle, int x, int y,
                          const char *text, size_t length)
{
    Style s = style_of(fstyle);
    int32_t pen = (int32_t)x << 6;
    size_t i = 0;
    while (i < length) {
        uint32_t u = utf8_to_ucs4(text + i, length - i);
        i = utf8_next(text, length, i);
        if (invisible(u)) continue;
        int sub = (pen & 63) >> 4;
        Glyph *g = get_glyph(s.face, s.italic, s.px, sub, u);
        if (!g) continue;
        if (g->w && g->h && u != ' ') {
            nsfb_bbox_t loc;
            loc.x0 = (pen >> 6) + g->x0;
            loc.y0 = y + g->y0;
            loc.x1 = loc.x0 + g->w;
            loc.y1 = loc.y0 + g->h;
            nsfb_plot_glyph8(nsfb, &loc, g->bits, g->w, fstyle->foreground);
        }
        pen += g->advance;
    }
    return NSERROR_OK;
}

static struct gui_layout_table layout_table = {
    .width = fb_font_width,
    .position = fb_font_position,
    .split = fb_font_split,
};

struct gui_layout_table *framebuffer_layout_table = &layout_table;
struct gui_utf8_table *framebuffer_utf8_table = NULL;
