/* gdi_int.h — what gdi32's sources share (not exported) */
#pragma once
#include <windows.h>

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);

void dib24_sync(void *bitmap);
void dib_recolor(void *bitmap);
static inline NOVA_DC *dc_of(HDC h) { return (NOVA_DC *)h; }

/* -----------------------------------------------------------------------
 * GDI objects
 * ----------------------------------------------------------------------- */
enum { K_BRUSH = 1, K_PEN, K_NULLBRUSH, K_NULLPEN, K_FONT, K_BITMAP, K_REGION, K_PALETTE };

typedef struct GObj {
    int kind;
    COLORREF color;                 /* brush / pen (user32 reads it at offset 4) */
    int width;
    int used;
    /* fonts (a LOGFONT's fields) */
    int height, weight, italic, underline;
    WCHAR face[32];
    int strike, pitch, charset, escapement, avg_width;
    /* bitmaps */
    int bw, bh, bpp, fmt, flip, owns;              /* owns: 1 VirtualAlloc'd bits, 2 a mapped view */
    void *view;
    DWORD *bits;
    /* DIB sections of 1, 4, 8, 16 or 24 bits per pixel: the program's
     * pixels (and a copy as of the last sync), kept in step with the 32-bit
     * `bits` gdi32 draws on; vbpp is their depth, pal their colour table
     * (1-8 bits), v565 a 16-bit section's 5-6-5 layout (else 5-5-5) */
    BYTE *view24, *last24;
    int stride24, view_owned, vbpp, npal, v565;
    RGBQUAD *pal;
    /* regions: the bounding box, and with more than one rectangle, the
     * rectangles (not overlapping, heap-allocated) */
    RECT rc;
    RECT *rects;
    int nrects;
    /* brushes: hatch style / pattern bitmap */
    int style;
    struct GObj *pattern;
} GObj;

extern GObj g_stock[20];
extern int  g_stock_ready;
void  stock_init(void);
GObj *new_obj(int kind);
GObj *obj_of(HGDIOBJ h);
GObj *bitmap_of(HGDIOBJ h);             /* a bitmap whose bits are about to be used (24-bit sections synced) */

/* Before a blit or a read of a DC's pixels, and after a drawing call that
 * ends a batch: a 24-bit DIB section is brought in step with its program's
 * bits (see dib24_sync).  Cheap when the DC draws on anything else. */
static inline void dc_sync(NOVA_DC *d)
{
    if (d && d->mem && d->bitmap && ((GObj *)d->bitmap)->view24) dib24_sync(d->bitmap);
}
void  rgn_free(GObj *o);              /* region.c: a region's rectangles */

/* -----------------------------------------------------------------------
 * Pixels.  Device coordinates are the logical ones plus the DC origin.
 * ----------------------------------------------------------------------- */
static inline DWORD *pixel_at(NOVA_DC *d, int x, int y)
{
    int row = d->flip ? d->h - 1 - y : y;
    return d->bits + (size_t)row * d->stride + x;
}

static inline DWORD to_native(NOVA_DC *d, COLORREF c)
{
    c &= 0xFFFFFF;
    return d->fmt ? 0xFF000000u | (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF) : c;
}

static inline COLORREF from_native(NOVA_DC *d, DWORD p)
{
    return d->fmt ? (p >> 16 & 0xFF) | (p & 0xFF00) | (p & 0xFF) << 16 : p & 0xFFFFFF;
}

/* May the DC draw at device pixel (x, y)? (surface, visible part, clip) */
static inline int dev_visible(NOVA_DC *d, int x, int y)
{
    if (x < 0 || y < 0 || x >= d->w || y >= d->h) return 0;
    if (d->has_vis && (x < d->vis.left || x >= d->vis.right || y < d->vis.top || y >= d->vis.bottom)) return 0;
    if (d->has_clip && (x < d->clip.left || x >= d->clip.right || y < d->clip.top || y >= d->clip.bottom)) return 0;
    if (d->has_clip && d->nclip_rects > 1) {
        for (int i = 0; i < d->nclip_rects; i++) {
            const RECT *c = &d->clip_rects[i];
            if (x >= c->left && x < c->right && y >= c->top && y < c->bottom) return 1;
        }
        return 0;
    }
    return 1;
}

/* A complex clip region's rectangles, for drawing that clips to one
 * rectangle at a time: 0 when the clip is one rectangle (or none) */
static inline int clip_pieces(NOVA_DC *d) { return d->has_clip && d->nclip_rects > 1 ? d->nclip_rects : 0; }

/* Clip a device rectangle to where the DC may draw; false if nothing is left */
static inline int dev_clip(NOVA_DC *d, RECT *r)
{
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > d->w) r->right = d->w;
    if (r->bottom > d->h) r->bottom = d->h;
    if (d->has_vis) {
        if (r->left < d->vis.left) r->left = d->vis.left;
        if (r->top < d->vis.top) r->top = d->vis.top;
        if (r->right > d->vis.right) r->right = d->vis.right;
        if (r->bottom > d->vis.bottom) r->bottom = d->vis.bottom;
    }
    if (d->has_clip) {
        if (r->left < d->clip.left) r->left = d->clip.left;
        if (r->top < d->clip.top) r->top = d->clip.top;
        if (r->right > d->clip.right) r->right = d->clip.right;
        if (r->bottom > d->clip.bottom) r->bottom = d->clip.bottom;
    }
    return r->left < r->right && r->top < r->bottom;
}

/* A pixel written with the DC's ROP2 mix (native formats) */
static inline DWORD rop_apply(NOVA_DC *d, DWORD dst, DWORD src)
{
    switch (d->rop2) {
    case 0: case 13: return src;                            /* R2_COPYPEN */
    case 1:  return (dst & 0xFF000000u) | 0;                 /* R2_BLACK */
    case 16: return dst | 0xFFFFFF;                          /* R2_WHITE */
    case 6:  return (dst & 0xFF000000u) | (~dst & 0xFFFFFF); /* R2_NOT */
    case 7:  return (dst & 0xFF000000u) | ((dst ^ src) & 0xFFFFFF);   /* R2_XORPEN */
    case 11: return dst;                                     /* R2_NOP */
    case 9:  return (dst & 0xFF000000u) | ((dst & src) & 0xFFFFFF);   /* R2_MASKPEN */
    case 15: return (dst & 0xFF000000u) | ((dst | src) & 0xFFFFFF);   /* R2_MERGEPEN */
    case 4:  return (dst & 0xFF000000u) | (~src & 0xFFFFFF); /* R2_NOTCOPYPEN */
    case 10: return (dst & 0xFF000000u) | (~(dst ^ src) & 0xFFFFFF);  /* R2_NOTXORPEN */
    }
    return src;
}

/* Logical coordinates: clipped, through the ROP */
void put(NOVA_DC *d, int x, int y, COLORREF c);
void fill(NOVA_DC *d, int x0, int y0, int x1, int y1, COLORREF c);
void line(NOVA_DC *d, int x0, int y0, int x1, int y1, COLORREF c, int width);
void fill_polygon(NOVA_DC *d, const POINT *pt, int n, COLORREF c);      /* even-odd scanline fill */
void ellipse(NOVA_DC *d, int l, int t, int r, int b, BOOL do_fill, BOOL do_edge);

/* The 20 colours of the default palette (DEFAULT_PALETTE), as PALETTEENTRYs
 * (peRed, peGreen, peBlue, peFlags packed little-endian) */
extern const DWORD g_default_palette[20];

/* Blend @c over device pixel (x, y) with coverage @a (0-255); clipped */
static inline void blend(NOVA_DC *d, int x, int y, COLORREF c, int a)
{
    if (a <= 0 || !dev_visible(d, x, y)) return;
    DWORD *p = pixel_at(d, x, y);
    if (a >= 255) { *p = to_native(d, c); return; }
    COLORREF t = from_native(d, *p);
    int r = (GetRValue(c) * a + GetRValue(t) * (255 - a) + 127) / 255;
    int g = (GetGValue(c) * a + GetGValue(t) * (255 - a) + 127) / 255;
    int b = (GetBValue(c) * a + GetBValue(t) * (255 - a) + 127) / 255;
    *p = to_native(d, RGB(r, g, b));
}
