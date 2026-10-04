/*
 * gdi32.dll — a software rasterizer
 *
 * A device context (NOVA_DC, from wingdi.h) points at pixels: a window's
 * client bitmap (COLORREF pixels, from user32), or for a memory DC the
 * bitmap selected into it — a compatible bitmap (COLORREF) or a DIB
 * section (BGRA, top-down or bottom-up, as the program asked).  These
 * calls plot straight into those pixels, clipped to the DC's visible
 * rectangle (a window's) and clip region.  Text is in text.c.
 */
#define NOVA_BUILD_GDI32
#include <windows.h>
#include <winternl.h>


void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#include "gdi_int.h"

/* A process may hold 10,000 GDI objects, as on Windows (Audacity's theme
 * alone is well over 512 bitmaps, pens and brushes); the pool is mapped on
 * first use and searched up to its high-water mark */
#define POOL 10000
GObj g_stock[STOCK_SLOTS];
static GObj *g_pool;
static int   g_high, g_hint;
int  g_stock_ready;
static SRWLOCK g_lock;

void stock_init(void)
{
    g_stock[WHITE_BRUSH]  = (GObj){ K_BRUSH, 0xFFFFFF, 0 };
    g_stock[LTGRAY_BRUSH] = (GObj){ K_BRUSH, 0xC0C0C0, 0 };
    g_stock[GRAY_BRUSH]   = (GObj){ K_BRUSH, 0x808080, 0 };
    g_stock[DKGRAY_BRUSH] = (GObj){ K_BRUSH, 0x404040, 0 };
    g_stock[BLACK_BRUSH]  = (GObj){ K_BRUSH, 0x000000, 0 };
    g_stock[NULL_BRUSH]   = (GObj){ K_NULLBRUSH, 0, 0 };
    g_stock[WHITE_PEN]    = (GObj){ K_PEN, 0xFFFFFF, 1 };
    g_stock[BLACK_PEN]    = (GObj){ K_PEN, 0x000000, 1 };
    g_stock[NULL_PEN]     = (GObj){ K_NULLPEN, 0, 0 };
    g_stock[DEFAULT_PALETTE] = (GObj){ K_PALETTE, 0, 0 };
    g_stock[DEFAULT_PALETTE].bits = (DWORD *)g_default_palette;
    g_stock[DEFAULT_PALETTE].bw = 20;
    for (int i = OEM_FIXED_FONT; i <= DEFAULT_GUI_FONT; i++) {   /* the stock fonts */
        if (i == DEFAULT_PALETTE) continue;
        g_stock[i].kind = K_FONT;
        g_stock[i].weight = 400;
        const char *f;
        switch (i) {
        case OEM_FIXED_FONT: case ANSI_FIXED_FONT: case SYSTEM_FIXED_FONT:
            f = "Courier New"; g_stock[i].height = 16; g_stock[i].pitch = 0x31; break;   /* FIXED_PITCH | FF_MODERN */
        case DEFAULT_GUI_FONT: case ANSI_VAR_FONT:
            f = "MS Shell Dlg"; g_stock[i].height = -12; break;
        default:
            f = "System"; g_stock[i].height = 16; g_stock[i].weight = 700; break;
        }
        for (int k = 0; f[k]; k++) g_stock[i].face[k] = (WCHAR)f[k];
        g_stock[STOCK_HIDPI + i - OEM_FIXED_FONT] = g_stock[i];   /* the same at 192 DPI */
        g_stock[STOCK_HIDPI + i - OEM_FIXED_FONT].height *= 2;
    }
    g_stock_ready = 1;
}

static int sys_dpi_k(void);

/* The stock fonts are made at the system DPI the calling thread sees (a
 * DPI-aware one of a process started at 192 DPI gets ones twice the size) */
GDIAPI HGDIOBJ GetStockObject(int obj)
{
    if (!g_stock_ready) stock_init();
    if (obj < 0 || obj >= 20 || !g_stock[obj].kind) return 0;
    if (obj >= OEM_FIXED_FONT && obj <= DEFAULT_GUI_FONT && g_stock[obj].kind == K_FONT && sys_dpi_k() > 1)
        return (HGDIOBJ)&g_stock[STOCK_HIDPI + obj - OEM_FIXED_FONT];
    return (HGDIOBJ)&g_stock[obj];
}


GObj *new_obj(int kind)
{
    AcquireSRWLockExclusive(&g_lock);
    if (!g_pool) g_pool = VirtualAlloc(0, sizeof(GObj) * POOL, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    GObj *o = 0;
    if (g_pool) {
        for (int n = 0, i = g_hint; n < POOL; n++, i = i + 1 < POOL ? i + 1 : 0) if (!g_pool[i].used) {
            o = &g_pool[i];
            memset(o, 0, sizeof(*o));
            o->kind = kind;
            o->used = 1;
            g_hint = i + 1 < POOL ? i + 1 : 0;
            if (i >= g_high) g_high = i + 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!o) {
        static int said;
        if (!said++) OutputDebugStringA("gdi32: the process holds 10,000 GDI objects; no more can be created");
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    }
    return o;
}

/* the 1x1 monochrome bitmap every memory DC starts with: SelectObject hands
 * it back as the previous bitmap, and selecting it again restores the DC */
static DWORD g_one_pixel;
GObj g_default_bitmap = { .kind = K_BITMAP, .used = 1, .bw = 1, .bh = 1, .bpp = 32, .bits = &g_one_pixel };

GObj *obj_of(HGDIOBJ h)
{
    GObj *o = h;
    if (!o) return 0;
    if (!g_stock_ready) stock_init();
    if (o == &g_default_bitmap) return o;
    if ((g_pool && o >= g_pool && o < g_pool + POOL && o->used) || (o >= g_stock && o < g_stock + STOCK_SLOTS && o->kind)) return o;
    return 0;
}

/* A palette DIB's colour index at x of a row (1, 4 or 8 bits per pixel) */
static unsigned vget_index(const GObj *o, const BYTE *row, int x)
{
    if (o->vbpp == 8) return row[x];
    if (o->vbpp == 4) return (unsigned)(row[x / 2] >> (x & 1 ? 0 : 4)) & 15;
    return (unsigned)(row[x / 8] >> (7 - (x & 7))) & 1;
}

static unsigned vraw(const GObj *o, const BYTE *row, int x)
{
    if (o->vbpp == 16) return ((const WORD *)row)[x];
    if (o->vbpp == 24) return (unsigned)row[3 * x] | (unsigned)row[3 * x + 1] << 8 | (unsigned)row[3 * x + 2] << 16;
    return vget_index(o, row, x);
}

/* A pixel of a DIB section in the program's format: as 0xFFRRGGBB, as
 * stored (to see whether it changed), and written from 0x..RRGGBB (a
 * palette section takes the nearest colour of its table) */
static DWORD vget(const GObj *o, const BYTE *row, int x)
{
    switch (o->vbpp) {
    case 24: { const BYTE *q = row + 3 * x; return 0xFF000000u | (DWORD)q[2] << 16 | (DWORD)q[1] << 8 | q[0]; }
    case 16: {
        WORD v = ((const WORD *)row)[x];
        DWORD r = o->v565 ? (v >> 11 & 31) : (v >> 10 & 31), g = o->v565 ? (v >> 5 & 63) * 255 / 63 : (v >> 5 & 31) * 255 / 31;
        return 0xFF000000u | (r * 255 / 31) << 16 | g << 8 | (DWORD)(v & 31) * 255 / 31;
    }
    }
    int i = (int)vget_index(o, row, x);
    const RGBQUAD *q = &o->pal[i < o->npal ? i : 0];
    return 0xFF000000u | (DWORD)q->rgbRed << 16 | (DWORD)q->rgbGreen << 8 | q->rgbBlue;
}

static void vput(GObj *o, BYTE *row, int x, DWORD c)
{
    BYTE r = (BYTE)(c >> 16), g = (BYTE)(c >> 8), b = (BYTE)c;
    switch (o->vbpp) {
    case 24: row[3 * x] = b; row[3 * x + 1] = g; row[3 * x + 2] = r; return;
    case 16:
        ((WORD *)row)[x] = o->v565 ? (WORD)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
                                   : (WORD)((r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
        return;
    }
    int best = 0;
    long bd = 0x7FFFFFFF;
    for (int i = 0; i < o->npal && bd; i++) {
        long dr = r - o->pal[i].rgbRed, dg = g - o->pal[i].rgbGreen, db = b - o->pal[i].rgbBlue;
        long d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    if (o->vbpp == 8) row[x] = (BYTE)best;
    else if (o->vbpp == 4) row[x / 2] = (BYTE)(x & 1 ? (row[x / 2] & 0xF0) | best : (row[x / 2] & 0x0F) | best << 4);
    else row[x / 8] = (BYTE)(best ? row[x / 8] | 0x80 >> (x & 7) : row[x / 8] & ~(0x80 >> (x & 7)));
}

/* a bitmap object about to be read or written through its bits */
GObj *bitmap_of(HGDIOBJ h)
{
    GObj *o = obj_of(h);
    if (o && o->view24) dib24_sync(o);
    return o;
}

/* A DIB section of other than 32 bits per pixel: what the program wrote to its bits since the last
 * sync goes to the 32-bit pixels, then what gdi32 drew since goes back to
 * the program's bits.  Rows are in the same order in both.  A copy of the
 * bits as of the last sync tells the two apart; only pixels that changed
 * are written, so a sync of an unchanged bitmap costs one read of each.
 * Syncs happen when the bitmap goes into or out of a DC, around blits and
 * bit reads, and at GdiFlush (the call a program makes before it reads a
 * section's bits, as on Windows), not at every GDI call: Audacity draws
 * its waveforms with thousands of lines into one. */
void dib24_sync(void *bitmap)
{
    GObj *o = bitmap;
    if (!(g_pool && o >= g_pool && o < g_pool + POOL && o->used) || o->kind != K_BITMAP || !o->view24) return;
    int w = o->bw, h = o->bh, st = o->stride24;
    for (int y = 0; y < h; y++) {
        BYTE *v = o->view24 + (size_t)y * st, *l = o->last24 + (size_t)y * st;
        DWORD *p = o->bits + (size_t)y * w;
        if (o->vbpp != 24) {
            if (memcmp(v, l, (size_t)st))
                for (int x = 0; x < w; x++)
                    if (vraw(o, v, x) != vraw(o, l, x)) p[x] = vget(o, v, x);
            for (int x = 0; x < w; x++)                 /* only what gdi32 changed is matched to the table */
                if ((vget(o, v, x) ^ p[x]) & 0xFFFFFF) vput(o, v, x, p[x]);
            memcpy(l, v, (size_t)st);
            continue;
        }
        if (memcmp(v, l, (size_t)w * 3)) {
            for (int x = 0; x < w; x++) {
                if (v[3 * x] == l[3 * x] && v[3 * x + 1] == l[3 * x + 1] && v[3 * x + 2] == l[3 * x + 2]) continue;
                p[x] = 0xFF000000u | (DWORD)v[3 * x + 2] << 16 | (DWORD)v[3 * x + 1] << 8 | v[3 * x];
            }
        }
        for (int x = 0; x < w; x++) {
            DWORD c = p[x];
            BYTE b = (BYTE)c, g = (BYTE)(c >> 8), r = (BYTE)(c >> 16);
            if (l[3 * x] == b && l[3 * x + 1] == g && l[3 * x + 2] == r) continue;
            v[3 * x] = l[3 * x] = b;
            v[3 * x + 1] = l[3 * x + 1] = g;
            v[3 * x + 2] = l[3 * x + 2] = r;
        }
    }
}

/* A palette section's colour table changed (SetDIBColorTable, which synced
 * it first): its pixels keep their indices and take the new colours */
void dib_recolor(void *bitmap)
{
    GObj *o = bitmap;
    if (!o || !o->view24 || !o->pal) return;
    for (int y = 0; y < o->bh; y++)
        for (int x = 0; x < o->bw; x++) o->bits[(size_t)y * o->bw + x] = vget(o, o->view24 + (size_t)y * o->stride24, x);
}

GDIAPI HBRUSH CreateSolidBrush(COLORREF c) { GObj *o = new_obj(K_BRUSH); if (o) o->color = c; return (HBRUSH)o; }
GDIAPI HBRUSH CreateHatchBrush(int style, COLORREF c) { (void)style; return CreateSolidBrush(c); }
GDIAPI HBRUSH CreateBrushIndirect(const LOGBRUSH *lb)
{
    if (lb->lbStyle == 1 /* BS_NULL */) return (HBRUSH)GetStockObject(NULL_BRUSH);
    return CreateSolidBrush(lb->lbColor);
}

GDIAPI HPEN CreatePen(int style, int width, COLORREF c)
{
    GObj *o = new_obj(style == PS_NULL ? K_NULLPEN : K_PEN);
    if (o) { o->color = c; o->width = width < 1 ? 1 : width; }
    return (HPEN)o;
}
GDIAPI HPEN CreatePenIndirect(const LOGPEN *lp) { return CreatePen((int)lp->lopnStyle, lp->lopnWidth.x, lp->lopnColor); }
GDIAPI HPEN ExtCreatePen(DWORD style, DWORD width, const LOGBRUSH *lb, DWORD n, const DWORD *st)
{
    (void)n; (void)st;
    return CreatePen((int)(style & 0xF), (int)width, lb ? lb->lbColor : 0);
}

/* -----------------------------------------------------------------------
 * Pixels
 * ----------------------------------------------------------------------- */
void put(NOVA_DC *d, int x, int y, COLORREF c)
{
    x += d->org_x; y += d->org_y;
    if (!d->bits || !dev_visible(d, x, y)) return;
    DWORD *p = pixel_at(d, x, y);
    *p = rop_apply(d, *p, to_native(d, c));
}

GDIAPI COLORREF SetPixel(HDC h, int x, int y, COLORREF c) { NOVA_DC *d = dc_of(h); if (d) put(d, x, y, c); return c; }
GDIAPI BOOL SetPixelV(HDC h, int x, int y, COLORREF c) { SetPixel(h, x, y, c); return TRUE; }
GDIAPI COLORREF GetPixel(HDC h, int x, int y)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0xFFFFFFFF;
    dc_sync(d);
    x += d->org_x; y += d->org_y;
    if (!d->bits || !dev_visible(d, x, y)) return 0xFFFFFFFF;
    return from_native(d, *pixel_at(d, x, y));
}
GDIAPI COLORREF SetTextColor(HDC h, COLORREF c) { NOVA_DC *d = dc_of(h); COLORREF o = d ? d->text_color : 0; if (d) d->text_color = c; return o; }
GDIAPI COLORREF SetBkColor(HDC h, COLORREF c)   { NOVA_DC *d = dc_of(h); COLORREF o = d ? d->bk_color : 0; if (d) d->bk_color = c; return o; }
GDIAPI COLORREF GetTextColor(HDC h) { NOVA_DC *d = dc_of(h); return d ? d->text_color : 0; }
GDIAPI COLORREF GetBkColor(HDC h)   { NOVA_DC *d = dc_of(h); return d ? d->bk_color : 0; }
GDIAPI int      SetBkMode(HDC h, int m)         { NOVA_DC *d = dc_of(h); int o = d ? d->bk_mode : 0; if (d) d->bk_mode = m; return o; }
GDIAPI int      GetBkMode(HDC h)                { NOVA_DC *d = dc_of(h); return d ? d->bk_mode : 0; }
GDIAPI UINT     SetTextAlign(HDC h, UINT a)     { NOVA_DC *d = dc_of(h); UINT o = d ? d->text_align : 0; if (d) d->text_align = a; return o; }
GDIAPI UINT     GetTextAlign(HDC h)             { NOVA_DC *d = dc_of(h); return d ? d->text_align : 0; }
GDIAPI COLORREF SetDCBrushColor(HDC h, COLORREF c) { NOVA_DC *d = dc_of(h); COLORREF o = d ? d->brush_color : 0; if (d) { d->brush_color = c; d->has_brush = 1; } return o; }
GDIAPI COLORREF SetDCPenColor(HDC h, COLORREF c)   { NOVA_DC *d = dc_of(h); COLORREF o = d ? d->pen_color : 0; if (d) { d->pen_color = c; d->has_pen = 1; } return o; }
GDIAPI int SetROP2(HDC h, int m) { NOVA_DC *d = dc_of(h); if (!d) return 0; int o = d->rop2 ? d->rop2 : 13; d->rop2 = m == 13 ? 0 : m; return o; }
GDIAPI int GetROP2(HDC h) { NOVA_DC *d = dc_of(h); return d && d->rop2 ? d->rop2 : 13; }
GDIAPI int SetStretchBltMode(HDC h, int m) { (void)h; (void)m; return 1; }
GDIAPI int SetPolyFillMode(HDC h, int m) { (void)h; (void)m; return 1; }
GDIAPI int SetMapMode(HDC h, int m) { (void)h; (void)m; return 1; /* MM_TEXT only */ }
GDIAPI int GetMapMode(HDC h) { (void)h; return 1; }
/* The graphics mode and world transform are kept per DC and reported back;
 * drawing itself stays in GM_COMPATIBLE device coordinates */
GDIAPI int SetGraphicsMode(HDC h, int m)
{
    NOVA_DC *d = dc_of(h);
    if (!d || (m != GM_COMPATIBLE && m != GM_ADVANCED)) return 0;
    int old = d->gmode == GM_ADVANCED ? GM_ADVANCED : GM_COMPATIBLE;
    if (m == GM_COMPATIBLE && d->xform[0] && (d->xform[0] != 1 || d->xform[1] || d->xform[2] || d->xform[3] != 1 ||
                                              d->xform[4] || d->xform[5]))
        return 0;                                           /* Windows refuses while a transform is set */
    d->gmode = m == GM_ADVANCED ? GM_ADVANCED : 0;
    return old;
}
GDIAPI int GetGraphicsMode(HDC h)
{
    NOVA_DC *d = dc_of(h);
    return !d ? 0 : d->gmode == GM_ADVANCED ? GM_ADVANCED : GM_COMPATIBLE;
}
static void xform_get(NOVA_DC *d, XFORM *x)
{
    if (!d->xform[0] && !d->xform[1] && !d->xform[2] && !d->xform[3]) {
        x->eM11 = 1; x->eM12 = 0; x->eM21 = 0; x->eM22 = 1; x->eDx = 0; x->eDy = 0;
    } else memcpy(x, d->xform, sizeof(*x));
}
static void xform_mul(XFORM *r, const XFORM *a, const XFORM *b)   /* r = a then b */
{
    XFORM t;
    t.eM11 = a->eM11 * b->eM11 + a->eM12 * b->eM21;
    t.eM12 = a->eM11 * b->eM12 + a->eM12 * b->eM22;
    t.eM21 = a->eM21 * b->eM11 + a->eM22 * b->eM21;
    t.eM22 = a->eM21 * b->eM12 + a->eM22 * b->eM22;
    t.eDx = a->eDx * b->eM11 + a->eDy * b->eM21 + b->eDx;
    t.eDy = a->eDx * b->eM12 + a->eDy * b->eM22 + b->eDy;
    *r = t;
}
GDIAPI BOOL GetWorldTransform(HDC h, LPXFORM x)
{
    NOVA_DC *d = dc_of(h);
    if (!d || !x) return FALSE;
    xform_get(d, x);
    return TRUE;
}
GDIAPI BOOL SetWorldTransform(HDC h, const XFORM *x)
{
    NOVA_DC *d = dc_of(h);
    if (!d || !x || d->gmode != GM_ADVANCED) return FALSE;
    if (x->eM11 * x->eM22 - x->eM12 * x->eM21 == 0) return FALSE;     /* not invertible */
    memcpy(d->xform, x, sizeof(*x));
    return TRUE;
}
GDIAPI BOOL ModifyWorldTransform(HDC h, const XFORM *x, DWORD mode)
{
    NOVA_DC *d = dc_of(h);
    if (!d || d->gmode != GM_ADVANCED) return FALSE;
    XFORM cur;
    xform_get(d, &cur);
    if (mode == MWT_IDENTITY) { memset(d->xform, 0, sizeof(d->xform)); return TRUE; }
    if (!x) return FALSE;
    if (mode == MWT_LEFTMULTIPLY) xform_mul(&cur, x, &cur);
    else if (mode == MWT_RIGHTMULTIPLY) xform_mul(&cur, &cur, x);
    else return FALSE;
    if (cur.eM11 * cur.eM22 - cur.eM12 * cur.eM21 == 0) return FALSE;
    memcpy(d->xform, &cur, sizeof(cur));
    return TRUE;
}

GDIAPI BOOL SetViewportOrgEx(HDC h, int x, int y, LPPOINT old)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    if (old) { old->x = d->org_x - d->base_x; old->y = d->org_y - d->base_y; }
    d->org_x = d->base_x + x; d->org_y = d->base_y + y;
    return TRUE;
}
GDIAPI BOOL OffsetViewportOrgEx(HDC h, int x, int y, LPPOINT old)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    return SetViewportOrgEx(h, d->org_x - d->base_x + x, d->org_y - d->base_y + y, old);
}
GDIAPI BOOL GetViewportOrgEx(HDC h, LPPOINT p) { NOVA_DC *d = dc_of(h); if (!d) return FALSE; p->x = d->org_x - d->base_x; p->y = d->org_y - d->base_y; return TRUE; }
GDIAPI BOOL SetWindowOrgEx(HDC h, int x, int y, LPPOINT old) { return SetViewportOrgEx(h, -x, -y, old); }
GDIAPI BOOL SetBrushOrgEx(HDC h, int x, int y, LPPOINT old) { (void)h; (void)x; (void)y; if (old) old->x = old->y = 0; return TRUE; }

static void fill_one(NOVA_DC *d, int x0, int y0, int x1, int y1, COLORREF c)
{
    if (!d->bits) return;
    RECT r = { x0 + d->org_x, y0 + d->org_y, x1 + d->org_x, y1 + d->org_y };
    if (!dev_clip(d, &r)) return;
    DWORD v = to_native(d, c);
    for (int y = r.top; y < r.bottom; y++) {
        DWORD *row = pixel_at(d, 0, y);
        if (d->rop2) for (int x = r.left; x < r.right; x++) row[x] = rop_apply(d, row[x], v);
        else for (int x = r.left; x < r.right; x++) row[x] = v;
    }
}

void fill(NOVA_DC *d, int x0, int y0, int x1, int y1, COLORREF c)
{
    int n = clip_pieces(d);
    if (!n) { fill_one(d, x0, y0, x1, y1, c); return; }
    RECT box = d->clip;                                     /* a piece of the clip region at a time */
    for (int i = 0; i < n; i++) { d->clip = d->clip_rects[i]; fill_one(d, x0, y0, x1, y1, c); }
    d->clip = box;
}

/* -----------------------------------------------------------------------
 * Lines and shapes
 * ----------------------------------------------------------------------- */
GDIAPI BOOL MoveToEx(HDC h, int x, int y, LPPOINT old)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    if (old) { old->x = d->cx; old->y = d->cy; }
    d->cx = x; d->cy = y;
    return TRUE;
}

GDIAPI BOOL GetCurrentPositionEx(HDC h, LPPOINT p) { NOVA_DC *d = dc_of(h); if (!d) return FALSE; p->x = d->cx; p->y = d->cy; return TRUE; }

void line(NOVA_DC *d, int x0, int y0, int x1, int y1, COLORREF c, int width)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = (adx > ady ? adx : -ady) / 2, e2;
    int r = width > 1 ? width / 2 : 0;
    for (;;) {
        if (!r) put(d, x0, y0, c);
        else for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) put(d, x0 + i, y0 + j, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -adx) { err -= ady; x0 += sx; }
        if (e2 < ady)  { err += adx; y0 += sy; }
    }
}

GDIAPI BOOL LineTo(HDC h, int x, int y)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    if (d->has_pen) line(d, d->cx, d->cy, x, y, d->pen_color, d->pen_width);
    d->cx = x; d->cy = y;
    return TRUE;
}

GDIAPI BOOL Polyline(HDC h, const POINT *pt, int n)
{
    NOVA_DC *d = dc_of(h); if (!d || n < 2) return FALSE;
    if (d->has_pen) for (int i = 1; i < n; i++) line(d, pt[i - 1].x, pt[i - 1].y, pt[i].x, pt[i].y, d->pen_color, d->pen_width);
    return TRUE;
}

GDIAPI BOOL PolylineTo(HDC h, const POINT *pt, DWORD n)
{
    for (DWORD i = 0; i < n; i++) LineTo(h, pt[i].x, pt[i].y);
    return TRUE;
}

/* Scanline polygon fill (even-odd) */
void fill_polygon(NOVA_DC *d, const POINT *pt, int n, COLORREF c)
{
    int miny = pt[0].y, maxy = pt[0].y;
    for (int i = 1; i < n; i++) { if (pt[i].y < miny) miny = pt[i].y; if (pt[i].y > maxy) maxy = pt[i].y; }
    int xs[64];
    for (int y = miny; y < maxy; y++) {
        int k = 0;
        for (int i = 0; i < n && k < 64; i++) {
            POINT a = pt[i], b = pt[(i + 1) % n];
            if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y))
                xs[k++] = a.x + (int)((long long)(y - a.y) * (b.x - a.x) / (b.y - a.y));
        }
        for (int i = 1; i < k; i++) { int v = xs[i], j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
        for (int i = 0; i + 1 < k; i += 2) fill(d, xs[i], y, xs[i + 1], y + 1, c);
    }
}

GDIAPI BOOL Polygon(HDC h, const POINT *pt, int n)
{
    NOVA_DC *d = dc_of(h); if (!d || n < 2) return FALSE;
    if (d->has_brush && n >= 3) fill_polygon(d, pt, n, d->brush_color);
    if (d->has_pen) for (int i = 0; i < n; i++) line(d, pt[i].x, pt[i].y, pt[(i + 1) % n].x, pt[(i + 1) % n].y, d->pen_color, d->pen_width);
    return TRUE;
}

GDIAPI BOOL Rectangle(HDC h, int l, int t, int r, int b)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    if (d->has_brush) fill(d, l, t, r, b, d->brush_color);
    if (d->has_pen) {
        for (int x = l; x < r; x++) { put(d, x, t, d->pen_color); put(d, x, b - 1, d->pen_color); }
        for (int y = t; y < b; y++) { put(d, l, y, d->pen_color); put(d, r - 1, y, d->pen_color); }
    }
    return TRUE;
}

void ellipse(NOVA_DC *d, int l, int t, int r, int b, BOOL do_fill, BOOL do_edge)
{
    long ax = (r - l) / 2, ay = (b - t) / 2, cx = (l + r) / 2, cy = (t + b) / 2;
    if (ax <= 0 || ay <= 0) return;
    long rr = ax * ax * ay * ay;
    for (long y = -ay; y <= ay; y++) {
        /* half-width of the ellipse on this row */
        long w = 0;
        while (w <= ax && (w * w * ay * ay + y * y * ax * ax) <= rr) w++;
        w--;
        if (w < 0) continue;
        if (do_fill) fill(d, (int)(cx - w), (int)(cy + y), (int)(cx + w + 1), (int)(cy + y + 1), d->brush_color);
        if (do_edge) { put(d, (int)(cx - w), (int)(cy + y), d->pen_color); put(d, (int)(cx + w), (int)(cy + y), d->pen_color); }
    }
    if (do_edge) for (long x = -ax; x <= ax; x++) {
        long hgt = 0;
        while (hgt <= ay && (x * x * ay * ay + hgt * hgt * ax * ax) <= rr) hgt++;
        hgt--;
        if (hgt < 0) continue;
        put(d, (int)(cx + x), (int)(cy - hgt), d->pen_color);
        put(d, (int)(cx + x), (int)(cy + hgt), d->pen_color);
    }
}

GDIAPI BOOL Ellipse(HDC h, int l, int t, int r, int b)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    ellipse(d, l, t, r, b, d->has_brush, d->has_pen);
    return TRUE;
}

/* Arcs: the ellipse in (l, t, r, b) from the ray through (xs, ys) to the
 * ray through (xe, ye), counterclockwise as GDI draws by default
 * (clockwise after SetArcDirection(AD_CLOCKWISE): the same points as
 * counterclockwise from the end ray to the start); as a polyline of up to
 * 256 points.  @shape 0 Arc, 1 Pie (to the centre and
 * filled), 2 Chord (closed by a straight line and filled). */
static double nsqrt(double v)
{
    if (v <= 0) return 0;
    double r = v > 1 ? v : 1;
    for (int i = 0; i < 40; i++) r = 0.5 * (r + v / r);
    return r;
}

static BOOL arc_shape(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye, int shape)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    if (d->arc_dir == 2) { int tx = xs, ty = ys; xs = xe; ys = ye; xe = tx; ye = ty; }
    double ax = (r - l) / 2.0, ay = (b - t) / 2.0, cx = (l + r) / 2.0, cy = (t + b) / 2.0;
    if (ax <= 0 || ay <= 0) return TRUE;
    /* the rays as unit vectors on the circle the ellipse is a stretch of */
    double u = (xs - cx) / ax, v = (ys - cy) / ay, eu = (xe - cx) / ax, ev = (ye - cy) / ay;
    double n = nsqrt(u * u + v * v), en = nsqrt(eu * eu + ev * ev);
    if (n == 0) { u = 1; v = 0; } else { u /= n; v /= n; }
    if (en == 0) { eu = 1; ev = 0; } else { eu /= en; ev /= en; }
    static const double C = 0.99969881869620425, S = 0.024541228522912288;  /* cos, sin of 2 pi / 256 */
    POINT pt[260];
    int k = 0;
    if (shape == 1) { pt[k].x = (int)(cx + 0.5); pt[k].y = (int)(cy + 0.5); k++; }
    pt[k].x = (int)(cx + u * ax + 0.5); pt[k].y = (int)(cy + v * ay + 0.5); k++;
    for (int i = 0; i < 256; i++) {
        double nu = u * C + v * S, nv = -u * S + v * C;      /* one step counterclockwise (y down) */
        /* past the end ray: it lies between the old and the new direction */
        double before = u * ev - v * eu, after = nu * ev - nv * eu;
        int crossed = before < 0 && after >= 0 && (u * eu + v * ev) > 0;
        u = nu; v = nv;
        if (crossed || i == 255) { u = eu; v = ev; }
        pt[k].x = (int)(cx + u * ax + 0.5); pt[k].y = (int)(cy + v * ay + 0.5); k++;
        if (crossed) break;
    }
    if (!shape) return Polyline(h, pt, k);
    return Polygon(h, pt, k);
}

GDIAPI BOOL Arc(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye)
{ return arc_shape(h, l, t, r, b, xs, ys, xe, ye, 0); }
GDIAPI BOOL Pie(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye)
{ return arc_shape(h, l, t, r, b, xs, ys, xe, ye, 1); }
GDIAPI BOOL Chord(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye)
{ return arc_shape(h, l, t, r, b, xs, ys, xe, ye, 2); }

GDIAPI BOOL RoundRect(HDC h, int l, int t, int r, int b, int ew, int eh)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    int rx = ew / 2, ry = eh / 2;
    if (rx * 2 > r - l) rx = (r - l) / 2;
    if (ry * 2 > b - t) ry = (b - t) / 2;
    if (d->has_brush) {
        fill(d, l + rx, t, r - rx, b, d->brush_color);
        fill(d, l, t + ry, l + rx, b - ry, d->brush_color);
        fill(d, r - rx, t + ry, r, b - ry, d->brush_color);
        ellipse(d, l, t, l + 2 * rx, t + 2 * ry, TRUE, FALSE);
        ellipse(d, r - 2 * rx, t, r, t + 2 * ry, TRUE, FALSE);
        ellipse(d, l, b - 2 * ry, l + 2 * rx, b, TRUE, FALSE);
        ellipse(d, r - 2 * rx, b - 2 * ry, r, b, TRUE, FALSE);
    }
    if (d->has_pen) {
        line(d, l + rx, t, r - rx - 1, t, d->pen_color, 1);
        line(d, l + rx, b - 1, r - rx - 1, b - 1, d->pen_color, 1);
        line(d, l, t + ry, l, b - ry - 1, d->pen_color, 1);
        line(d, r - 1, t + ry, r - 1, b - ry - 1, d->pen_color, 1);
    }
    return TRUE;
}

GDIAPI BOOL PatBlt(HDC h, int x, int y, int w, int hh, DWORD rop)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    if (w < 0) { x += w; w = -w; }
    if (hh < 0) { y += hh; hh = -hh; }
    COLORREF c = rop == BLACKNESS ? 0 : rop == WHITENESS ? 0xFFFFFF : d->brush_color;
    fill(d, x, y, x + w, y + hh, c);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Bitmaps and memory DCs
 * ----------------------------------------------------------------------- */
static GObj *make_bitmap(int w, int h, int fmt, int flip, DWORD *bits)
{
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    GObj *o = new_obj(K_BITMAP);
    if (!o) return 0;
    o->bw = w; o->bh = h; o->bpp = 32; o->fmt = fmt; o->flip = flip;
    if (bits) o->bits = bits;
    else {
        o->bits = VirtualAlloc(0, (SIZE_T)w * h * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!o->bits) { o->used = 0; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
        o->owns = 1;
    }
    return o;
}

GDIAPI HBITMAP CreateCompatibleBitmap(HDC h, int w, int hh)
{
    return (HBITMAP)make_bitmap(w ? w : 1, hh ? hh : 1, 0, 0, 0);
}

GDIAPI HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits)
{
    (void)planes;
    GObj *o = make_bitmap(w, h, 0, 0, 0);
    if (!o || !bits) return (HBITMAP)o;
    const BYTE *b = bits;
    int stride = ((w * (int)bpp + 15) / 16) * 2;            /* rows are WORD-aligned */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            COLORREF c;
            if (bpp == 1) c = (b[y * stride + x / 8] >> (7 - x % 8)) & 1 ? 0xFFFFFF : 0;
            else if (bpp == 32) { const BYTE *p = b + y * stride + 4 * x; c = RGB(p[2], p[1], p[0]); }
            else if (bpp == 24) { const BYTE *p = b + y * stride + 3 * x; c = RGB(p[2], p[1], p[0]); }
            else c = 0;
            o->bits[(size_t)y * w + x] = c;
        }
    return (HBITMAP)o;
}

GDIAPI HBITMAP CreateBitmapIndirect(const BITMAP *bm)
{
    if (!bm) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return CreateBitmap(bm->bmWidth, bm->bmHeight, bm->bmPlanes, bm->bmBitsPixel, bm->bmBits);
}

/* A DIB header's colour table (it follows the header, whatever its size)
 * and its bit-field masks (at offset 40 in every header version) */
static const RGBQUAD *dib_colors(const BITMAPINFO *bi)
{
    return (const RGBQUAD *)((const BYTE *)bi + (bi->bmiHeader.biSize ? bi->bmiHeader.biSize : sizeof(BITMAPINFOHEADER)) +
                             (bi->bmiHeader.biSize == sizeof(BITMAPINFOHEADER) && bi->bmiHeader.biCompression == BI_BITFIELDS ? 12 : 0));
}

static int dib_565(const BITMAPINFO *bi)
{
    return bi->bmiHeader.biCompression == BI_BITFIELDS && *(const DWORD *)((const BYTE *)bi + 40) == 0xF800;
}

/* A 16- or 32-bit DIB's red, green and blue masks: its own with
 * BI_BITFIELDS (Mesa presents 5:6:5 and 4:4:4:4 frames with them),
 * otherwise 5:5:5 or 8:8:8 */
static void dib_masks(const BITMAPINFO *bi, DWORD m[3])
{
    const BITMAPINFOHEADER *h = &bi->bmiHeader;
    if ((h->biCompression == BI_BITFIELDS || h->biCompression == 6 /* BI_ALPHABITFIELDS */) &&
        (h->biBitCount == 16 || h->biBitCount == 32)) {
        const DWORD *f = (const DWORD *)((const BYTE *)bi + 40);
        m[0] = f[0]; m[1] = f[1]; m[2] = f[2];
    } else if (h->biBitCount == 16) {
        m[0] = 0x7C00; m[1] = 0x03E0; m[2] = 0x001F;
    } else {
        m[0] = 0xFF0000; m[1] = 0x00FF00; m[2] = 0x0000FF;
    }
}

/* One channel of a pixel by its mask, scaled to 0..255 */
static BYTE dib_channel(DWORD v, DWORD mask)
{
    if (!mask) return 0;
    int shift = __builtin_ctz(mask);
    return (BYTE)((unsigned long long)((v & mask) >> shift) * 255 / (mask >> shift));
}

/* Whether a 32-bit DIB's pixels are 0x00RRGGBB (copied as they are) */
static int dib_xrgb(const BITMAPINFO *bi)
{
    DWORD m[3];
    dib_masks(bi, m);
    return m[0] == 0xFF0000 && m[1] == 0x00FF00 && m[2] == 0x0000FF;
}

GDIAPI HBITMAP CreateDIBSection(HDC h, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset)
{
    (void)h;
    const BITMAPINFOHEADER *bh = &bi->bmiHeader;
    int bpp = bh->biBitCount;
    if (bpp != 32 && bpp != 24 && bpp != 16 && bpp != 8 && bpp != 4 && bpp != 1) {
        SetLastError(ERROR_INVALID_PARAMETER); if (bits) *bits = 0; return 0;
    }
    int w = bh->biWidth, hh = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    /* The pixels can live in a file mapping (Firefox's GPU process draws the
     * browser into one shared with the main process, which then blits it) */
    DWORD *view = 0;
    if (section && bpp == 32 && w > 0 && hh > 0) {
        view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)offset + (SIZE_T)w * hh * 4);
        if (!view) { if (bits) *bits = 0; return 0; }
    }
    GObj *o = make_bitmap(w, hh, 1, bh->biHeight > 0, view ? (DWORD *)((BYTE *)view + offset) : 0);
    if (o && view) { o->owns = 2; o->view = view; }
    if (!o && view) UnmapViewOfFile(view);
    if (o && bpp != 32) {
        /* gdi32 draws on 32-bit pixels; the program sees rows of its own
         * depth (in the section it passed, if any), synced at each use.
         * GDK's cursors are a 32-bit image and a 1-bit mask. */
        int stride = ((w * bpp + 31) / 32) * 4;
        SIZE_T size = (SIZE_T)stride * hh;
        BYTE *v = NULL;
        if (section) v = MapViewOfFile(section, FILE_MAP_WRITE, 0, offset, size);
        else {
            v = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            o->view_owned = 1;
        }
        BYTE *last = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        int npal = bpp <= 8 ? (bh->biClrUsed && bh->biClrUsed < (1u << bpp) ? (int)bh->biClrUsed : 1 << bpp) : 0;
        RGBQUAD *pal = npal ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 256 * sizeof(RGBQUAD)) : NULL;
        if (!v || !last || (npal && !pal)) {
            if (v && o->view_owned) VirtualFree(v, 0, MEM_RELEASE);
            else if (v) UnmapViewOfFile(v);
            if (last) VirtualFree(last, 0, MEM_RELEASE);
            if (pal) HeapFree(GetProcessHeap(), 0, pal);
            o->view_owned = 0;
            DeleteObject((HGDIOBJ)o);
            if (bits) *bits = 0;
            return 0;
        }
        if (pal) {
            if (usage == DIB_RGB_COLORS) memcpy(pal, dib_colors(bi), (size_t)npal * sizeof(RGBQUAD));
            else for (int i = 0; i < npal; i++)             /* DIB_PAL_COLORS: no logical palettes; greys */
                pal[i].rgbRed = pal[i].rgbGreen = pal[i].rgbBlue = (BYTE)(npal > 1 ? i * 255 / (npal - 1) : 0);
        }
        o->view24 = v;
        o->last24 = last;
        o->stride24 = stride;
        o->bpp = bpp;
        o->vbpp = bpp;
        o->pal = pal;
        o->npal = npal;
        o->v565 = bpp == 16 && dib_565(bi);
        if (bpp == 24) {
            for (int i = 0; i < w * hh; i++) o->bits[i] = 0xFF000000u;
            dib24_sync(o);
        } else {                                            /* the program's bits as they are (zero, or the section's) */
            for (int y = 0; y < hh; y++)
                for (int x = 0; x < w; x++) o->bits[(size_t)y * w + x] = vget(o, v + (size_t)y * stride, x);
            memcpy(last, v, size);
        }
        if (bits) *bits = v;
        return (HBITMAP)o;
    }
    if (bits) *bits = o ? o->bits : 0;
    return (HBITMAP)o;
}

GDIAPI HDC CreateCompatibleDC(HDC h)
{
    (void)h;
    NOVA_DC *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) return 0;
    d->mem = 1;                                             /* starts on the default 1x1 bitmap */
    d->bits = &g_one_pixel; d->w = d->h = 1; d->stride = 1;
    d->text_color = 0; d->bk_color = 0xFFFFFF; d->bk_mode = OPAQUE;
    d->has_pen = 1; d->pen_width = 1;
    d->has_brush = 1; d->brush_color = 0xFFFFFF;
    return (HDC)d;
}

GDIAPI HDC CreateDCW(LPCWSTR drv, LPCWSTR dev, LPCWSTR port, const void *mode) { (void)drv; (void)dev; (void)port; (void)mode; return CreateCompatibleDC(0); }
GDIAPI HDC CreateDCA(LPCSTR drv, LPCSTR dev, LPCSTR port, const void *mode) { (void)drv; (void)dev; (void)port; (void)mode; return CreateCompatibleDC(0); }
GDIAPI HDC CreateICW(LPCWSTR drv, LPCWSTR dev, LPCWSTR port, const void *mode) { return CreateDCW(drv, dev, port, mode); }

GDIAPI BOOL DeleteDC(HDC h)
{
    NOVA_DC *d = dc_of(h);
    if (!d || !d->mem) return FALSE;
    dc_sync(d);
    while (d->saved) { NOVA_DC *s = d->saved; d->saved = s->saved; HeapFree(GetProcessHeap(), 0, s); }
    HeapFree(GetProcessHeap(), 0, d);
    return TRUE;
}

/* nearest-neighbour copy between two DCs (mirrors on negative sizes) */
static void blit(NOVA_DC *dd, int x, int y, int w, int h, NOVA_DC *sd, int sx, int sy, int sw, int sh)
{
    if (!w || !h || !sw || !sh || !sd->bits) return;
    for (int j = 0; j < (h < 0 ? -h : h); j++) {
        int ty = h < 0 ? y - j : y + j;
        int syy = sy + (int)((long long)j * sh / h) * (h < 0 ? -1 : 1);
        if (syy + sd->org_y < 0 || syy + sd->org_y >= sd->h) continue;
        for (int i = 0; i < (w < 0 ? -w : w); i++) {
            int tx = w < 0 ? x - i : x + i;
            int sxx = sx + (int)((long long)i * sw / w) * (w < 0 ? -1 : 1);
            if (sxx + sd->org_x < 0 || sxx + sd->org_x >= sd->w) continue;
            put(dd, tx, ty, from_native(sd, *pixel_at(sd, sxx + sd->org_x, syy + sd->org_y)));
        }
    }
}

static void flush_window(NOVA_DC *d, int x, int y, int w, int h);

/* An unscaled copy between two 32-bit DCs, a row at a time (presenting
 * frames: Vulkan's software swap chain blits its DIB to the window) */
static void blit_fast_one(NOVA_DC *dd, int x, int y, int w, int h, NOVA_DC *sd, int sx, int sy)
{
    RECT r = { x + dd->org_x, y + dd->org_y, x + dd->org_x + w, y + dd->org_y + h };
    if (!dev_clip(dd, &r)) return;
    int dx = sx + sd->org_x - (x + dd->org_x), dy = sy + sd->org_y - (y + dd->org_y);
    if (r.left + dx < 0) r.left = -dx;
    if (r.top + dy < 0) r.top = -dy;
    if (r.right + dx > sd->w) r.right = sd->w - dx;
    if (r.bottom + dy > sd->h) r.bottom = sd->h - dy;
    if (r.left >= r.right) return;
    for (int ty = r.top; ty < r.bottom; ty++) {
        const DWORD *s = pixel_at(sd, r.left + dx, ty + dy);
        DWORD *t = pixel_at(dd, r.left, ty);
        int n = r.right - r.left;
        if (sd->fmt == dd->fmt) memcpy(t, s, (size_t)n * 4);
        else
            for (int i = 0; i < n; i++) t[i] = to_native(dd, from_native(sd, s[i]));
    }
}

static void blit_fast(NOVA_DC *dd, int x, int y, int w, int h, NOVA_DC *sd, int sx, int sy)
{
    int n = clip_pieces(dd);
    if (!n) { blit_fast_one(dd, x, y, w, h, sd, sx, sy); return; }
    RECT box = dd->clip;
    for (int i = 0; i < n; i++) { dd->clip = dd->clip_rects[i]; blit_fast_one(dd, x, y, w, h, sd, sx, sy); }
    dd->clip = box;
}

/* Can blit() take the row-copy path? */
static int can_fast(NOVA_DC *dd, int w, int h, NOVA_DC *sd, int sw, int sh, DWORD rop)
{
    return sd && dd->bits && sd->bits && sd->bits != dd->bits && w > 0 && h > 0 && w == sw && h == sh && rop == SRCCOPY && !dd->rop2;
}

GDIAPI BOOL BitBlt(HDC dst, int x, int y, int w, int hh, HDC src, int sx, int sy, DWORD rop)
{
    NOVA_DC *dd = dc_of(dst);
    if (!dd) return FALSE;
    dc_sync(dd);
    if (rop == BLACKNESS || rop == WHITENESS || rop == PATCOPY || !src) {
        fill(dd, x, y, x + w, y + hh, rop == WHITENESS ? 0xFFFFFF : rop == PATCOPY ? dd->brush_color : 0);
        dc_sync(dd);
        return TRUE;
    }
    NOVA_DC *sd = dc_of(src);
    dc_sync(sd);
    if (can_fast(dd, w, hh, sd, w, hh, rop)) {
        blit_fast(dd, x, y, w, hh, sd, sx, sy);
        flush_window(dd, x, y, w, hh);
        dc_sync(dd);
        return TRUE;
    }
    blit(dd, x, y, w, hh, sd, sx, sy, w, hh);
    dc_sync(dd);
    return TRUE;
}

GDIAPI BOOL StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    NOVA_DC *dd = dc_of(dst);
    if (!dd) return FALSE;
    if (!src) return PatBlt(dst, x, y, w, h, rop);
    NOVA_DC *sd = dc_of(src);
    dc_sync(dd);
    dc_sync(sd);
    if (can_fast(dd, w, h, sd, sw, sh, rop)) blit_fast(dd, x, y, w, h, sd, sx, sy);
    else blit(dd, x, y, w, h, sd, sx, sy, sw, sh);
    flush_window(dd, x, y, w, h);
    dc_sync(dd);
    return TRUE;
}

GDIAPI BOOL TransparentBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, UINT key)
{
    NOVA_DC *dd = dc_of(dst), *sd = dc_of(src);
    if (!dd || !sd) return FALSE;
    dc_sync(dd);
    dc_sync(sd);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sxx = sx + i * sw / w + sd->org_x, syy = sy + j * sh / h + sd->org_y;
            if (sxx < 0 || syy < 0 || sxx >= sd->w || syy >= sd->h) continue;
            COLORREF c = from_native(sd, *pixel_at(sd, sxx, syy));
            if (c != (key & 0xFFFFFF)) put(dd, x + i, y + j, c);
        }
    dc_sync(dd);
    return TRUE;
}

GDIAPI BOOL AlphaBlend(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION bf)
{
    NOVA_DC *dd = dc_of(dst), *sd = dc_of(src);
    if (!dd || !sd) return FALSE;
    dc_sync(dd);
    dc_sync(sd);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sxx = sx + i * sw / w + sd->org_x, syy = sy + j * sh / h + sd->org_y;
            int tx = x + i + dd->org_x, ty = y + j + dd->org_y;
            if (sxx < 0 || syy < 0 || sxx >= sd->w || syy >= sd->h || !dev_visible(dd, tx, ty)) continue;
            DWORD sp = *pixel_at(sd, sxx, syy);
            COLORREF s = from_native(sd, sp), t = from_native(dd, *pixel_at(dd, tx, ty));
            int a = bf.SourceConstantAlpha;
            int sa = (bf.AlphaFormat & 1) && sd->fmt ? (int)(sp >> 24) * a / 255 : a;
            int pre = (bf.AlphaFormat & 1) != 0;             /* AC_SRC_ALPHA: premultiplied source */
            int r = pre ? GetRValue(s) * a / 255 + GetRValue(t) * (255 - sa) / 255 : (GetRValue(s) * sa + GetRValue(t) * (255 - sa)) / 255;
            int g = pre ? GetGValue(s) * a / 255 + GetGValue(t) * (255 - sa) / 255 : (GetGValue(s) * sa + GetGValue(t) * (255 - sa)) / 255;
            int b = pre ? GetBValue(s) * a / 255 + GetBValue(t) * (255 - sa) / 255 : (GetBValue(s) * sa + GetBValue(t) * (255 - sa)) / 255;
            *pixel_at(dd, tx, ty) = to_native(dd, RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b));
        }
    dc_sync(dd);
    return TRUE;
}

/* A DIB's pixel (x, y from the top) as COLORREF */
static COLORREF dib_pixel(const BITMAPINFO *bi, const BYTE *bits, int x, int y)
{
    const BITMAPINFOHEADER *h = &bi->bmiHeader;
    int hh = h->biHeight < 0 ? -h->biHeight : h->biHeight;
    int row = h->biHeight > 0 ? hh - 1 - y : y;
    int stride = ((h->biWidth * h->biBitCount + 31) / 32) * 4;
    const BYTE *p = bits + (size_t)row * stride;
    switch (h->biBitCount) {
    case 24: p += 3 * x; return RGB(p[2], p[1], p[0]);
    case 32: case 16: {
        DWORD m[3], v = h->biBitCount == 32 ? ((const DWORD *)p)[x] : ((const WORD *)p)[x];
        dib_masks(bi, m);
        return RGB(dib_channel(v, m[0]), dib_channel(v, m[1]), dib_channel(v, m[2]));
    }
    case 8: case 4: case 1: {
        int idx = h->biBitCount == 8 ? p[x] : h->biBitCount == 4 ? (p[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (p[x / 8] >> (7 - x % 8)) & 1;
        const RGBQUAD *q = &dib_colors(bi)[idx];
        return RGB(q->rgbRed, q->rgbGreen, q->rgbBlue);
    }
    }
    return 0;
}

/* A window's DC drawn into outside painting (a GL frame through a
 * long-lived DC): have user32 show the change now */
static void flush_window(NOVA_DC *d, int x, int y, int w, int h)
{
    static void (WINAPI *flush)(HDC, const RECT *);
    static int looked;
    if (d->mem || !d->hwnd) return;
    if (!looked) {
        HMODULE u = GetModuleHandleA("user32.dll");
        if (u) flush = (void *)GetProcAddress(u, "NovaFlushDC");
        looked = 1;
    }
    RECT r = { x, y, x + w, y + h };
    if (flush) flush((HDC)d, &r);
}

/* An unscaled 32-bit DIB copied straight into the DC (presenting frames) */
static int blit_dib32_one(NOVA_DC *d, int x, int y, int w, int h, int sx, int sy, const void *bits, const BITMAPINFO *bi)
{
    const BITMAPINFOHEADER *bh = &bi->bmiHeader;
    int bw = bh->biWidth, bht = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    RECT r = { x + d->org_x, y + d->org_y, x + d->org_x + w, y + d->org_y + h };
    if (!dev_clip(d, &r)) return 1;
    for (int ty = r.top; ty < r.bottom; ty++) {
        int syy = sy + (ty - (y + d->org_y));
        if (syy < 0 || syy >= bht) continue;
        int row = bh->biHeight > 0 ? bht - 1 - syy : syy;
        const DWORD *src = (const DWORD *)bits + (size_t)row * bw;
        DWORD *dst = pixel_at(d, 0, ty);
        int tx0 = r.left, tx1 = r.right, sx0 = sx + (tx0 - (x + d->org_x));
        if (sx0 < 0) { tx0 -= sx0; sx0 = 0; }
        if (sx0 + (tx1 - tx0) > bw) tx1 = tx0 + (bw - sx0);
        if (d->fmt) memcpy(dst + tx0, src + sx0, (size_t)(tx1 - tx0) * 4);
        else
            for (int tx = tx0, k = sx0; tx < tx1; tx++, k++) {
                DWORD p = src[k];                                   /* 0x00RRGGBB -> COLORREF */
                dst[tx] = (p >> 16 & 0xFF) | (p & 0xFF00) | (p & 0xFF) << 16;
            }
    }
    return 1;
}

static int blit_dib32(NOVA_DC *d, int x, int y, int w, int h, int sx, int sy, const void *bits, const BITMAPINFO *bi)
{
    int n = clip_pieces(d);
    if (!n) return blit_dib32_one(d, x, y, w, h, sx, sy, bits, bi);
    RECT box = d->clip;
    for (int i = 0; i < n; i++) { d->clip = d->clip_rects[i]; blit_dib32_one(d, x, y, w, h, sx, sy, bits, bi); }
    d->clip = box;
    return 1;
}

/* Copy a DIB rectangle (sy counted from the image's top) into the DC.
 * Negative extents work as on Windows: x, -w covers x-w+1..x, and the
 * image is mirrored only when a destination and source extent differ in sign */
static int stretch_dib(NOVA_DC *d, int x, int y, int w, int hh, int sx, int sy, int sw, int sh, const void *bits,
                       const BITMAPINFO *bi, DWORD rop)
{
    int mx = (w < 0) != (sw < 0), my = (hh < 0) != (sh < 0);
    if (w < 0) { x += w + 1; w = -w; }
    if (hh < 0) { y += hh + 1; hh = -hh; }
    if (sw < 0) { sx += sw + 1; sw = -sw; }
    if (sh < 0) { sy += sh + 1; sh = -sh; }
    const BITMAPINFOHEADER *bih = &bi->bmiHeader;
    if (!mx && !my && w == sw && hh == sh && bih->biBitCount == 32 && rop == SRCCOPY && !d->rop2 &&
        (bih->biCompression == BI_RGB || dib_xrgb(bi)) && d->bits) {
        blit_dib32(d, x, y, w, hh, sx, sy, bits, bi);
        flush_window(d, x, y, w, hh);
        dc_sync(d);
        return hh;
    }
    int bh = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
    for (int j = 0; j < hh; j++) {
        int syy = sy + (my ? hh - 1 - j : j) * sh / hh;
        if (syy < 0 || syy >= bh) continue;
        for (int i = 0; i < w; i++) {
            int sxx = sx + (mx ? w - 1 - i : i) * sw / w;
            if (sxx < 0 || sxx >= bih->biWidth) continue;
            put(d, x + i, y + j, dib_pixel(bi, bits, sxx, syy));
        }
    }
    flush_window(d, x, y, w, hh);
    dc_sync(d);
    return hh;
}

GDIAPI int StretchDIBits(HDC h, int x, int y, int w, int hh, int sx, int sy, int sw, int sh, const void *bits,
                         const BITMAPINFO *bi, UINT usage, DWORD rop)
{
    (void)usage;
    NOVA_DC *d = dc_of(h);
    if (!d || !bits || !w || !hh || !sw || !sh) return 0;
    /* The source y counts from the bottom of the image, top-down DIBs
     * included (cairo's win32 backend relies on this; wxWidgets blits a
     * window's part of a larger shared buffer this way, and a top-down
     * reading drew the buffer's empty bottom rows instead) */
    int bh = bi->bmiHeader.biHeight < 0 ? -bi->bmiHeader.biHeight : bi->bmiHeader.biHeight;
    return stretch_dib(d, x, y, w, hh, sx, bh - sy - sh, sw, sh, bits, bi, rop);
}


GDIAPI int SetDIBitsToDevice(HDC h, int x, int y, DWORD w, DWORD hh, int sx, int sy, UINT start, UINT lines,
                             const void *bits, const BITMAPINFO *bi, UINT usage)
{
    (void)start; (void)lines; (void)usage;
    NOVA_DC *d = dc_of(h);
    if (!d || !bits || !w || !hh) return 0;
    int bh = bi->bmiHeader.biHeight < 0 ? -bi->bmiHeader.biHeight : bi->bmiHeader.biHeight;
    int top = bi->bmiHeader.biHeight > 0 ? bh - sy - (int)hh : sy;
    return stretch_dib(d, x, y, (int)w, (int)hh, sx, top, (int)w, (int)hh, bits, bi, SRCCOPY) ? (int)hh : 0;
}

GDIAPI int GetDIBits(HDC h, HBITMAP bmp, UINT start, UINT lines, void *bits, BITMAPINFO *bi, UINT usage)
{
    (void)h; (void)usage;
    GObj *o = bitmap_of(bmp);
    if (!o || o->kind != K_BITMAP) return 0;
    BITMAPINFOHEADER *bh = &bi->bmiHeader;
    if (!bits) {                                            /* just describe the bitmap */
        bh->biWidth = o->bw; bh->biHeight = o->bh; bh->biPlanes = 1; bh->biBitCount = 32;
        bh->biCompression = 0; bh->biSizeImage = (DWORD)o->bw * o->bh * 4;
        return o->bh;
    }
    if (bh->biBitCount != 32 && bh->biBitCount != 24) return 0;
    int topdown = bh->biHeight < 0, stride = ((o->bw * bh->biBitCount + 31) / 32) * 4;
    UINT n = 0;
    for (UINT r = start; r < start + lines && (int)r < o->bh; r++, n++) {
        int srcrow = topdown ? (int)r : o->bh - 1 - (int)r;         /* the r-th scan line of the output */
        BYTE *out = (BYTE *)bits + (size_t)(r - start) * stride;
        for (int x = 0; x < o->bw; x++) {
            int row = o->flip ? o->bh - 1 - srcrow : srcrow;
            DWORD p = o->bits[(size_t)row * o->bw + x];
            COLORREF c = o->fmt ? (p >> 16 & 0xFF) | (p & 0xFF00) | (p & 0xFF) << 16 : p;
            BYTE *q = out + x * (bh->biBitCount / 8);
            q[0] = GetBValue(c); q[1] = GetGValue(c); q[2] = GetRValue(c);
            if (bh->biBitCount == 32) q[3] = o->fmt ? (BYTE)(p >> 24) : 0;   /* a DIB section keeps its alpha */
        }
    }
    return (int)n;
}

GDIAPI int SetDIBits(HDC h, HBITMAP bmp, UINT start, UINT lines, const void *bits, const BITMAPINFO *bi, UINT usage)
{
    (void)h; (void)usage;
    GObj *o = bitmap_of(bmp);
    if (!o || o->kind != K_BITMAP) return 0;
    int bh = bi->bmiHeader.biHeight < 0 ? -bi->bmiHeader.biHeight : bi->bmiHeader.biHeight;
    UINT n = 0;
    for (int y = 0; y < o->bh && y < bh; y++) {
        int srow = bi->bmiHeader.biHeight > 0 ? bh - 1 - y : y;
        if (srow < (int)start || srow >= (int)(start + lines)) continue;
        for (int x = 0; x < o->bw && x < bi->bmiHeader.biWidth; x++) {
            COLORREF c = dib_pixel(bi, bits, x, y);
            int row = o->flip ? o->bh - 1 - y : y;
            o->bits[(size_t)row * o->bw + x] = o->fmt ? 0xFF000000u | (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF) : c;
        }
        n++;
    }
    if (o->view24) dib24_sync(o);
    return (int)n;
}

GDIAPI HBITMAP CreateDIBitmap(HDC h, const BITMAPINFOHEADER *bh, DWORD init, const void *bits, const BITMAPINFO *bi, UINT usage)
{
    HBITMAP b = CreateCompatibleBitmap(h, bh->biWidth, bh->biHeight < 0 ? -bh->biHeight : bh->biHeight);
    if (b && (init & 4 /* CBM_INIT */) && bits) SetDIBits(h, b, 0, (UINT)(bh->biHeight < 0 ? -bh->biHeight : bh->biHeight), bits, bi, usage);
    return b;
}

/* -----------------------------------------------------------------------
 * Selecting, describing and deleting objects
 * ----------------------------------------------------------------------- */
GDIAPI HGDIOBJ SelectObject(HDC h, HGDIOBJ obj)
{
    NOVA_DC *d = dc_of(h);
    GObj *o = obj_of(obj);
    if (!d || !o) { return 0; }
    HGDIOBJ old = 0;
    switch (o->kind) {
    case K_BRUSH:     old = d->brush ? d->brush : GetStockObject(WHITE_BRUSH); d->brush = o; d->brush_color = o->color; d->has_brush = 1; break;
    case K_NULLBRUSH: old = d->brush ? d->brush : GetStockObject(WHITE_BRUSH); d->brush = o; d->has_brush = 0; break;
    case K_PEN:       old = d->pen ? d->pen : GetStockObject(BLACK_PEN); d->pen = o; d->pen_color = o->color; d->pen_width = o->width; d->has_pen = 1; break;
    case K_NULLPEN:   old = d->pen ? d->pen : GetStockObject(BLACK_PEN); d->pen = o; d->has_pen = 0; break;
    case K_FONT:      old = d->font ? d->font : GetStockObject(SYSTEM_FONT); d->font = o; break;
    case K_REGION:    return (HGDIOBJ)(ULONG_PTR)(ULONG)SelectClipRgn(h, (HRGN)obj);
    case K_BITMAP:
        if (!d->mem) return 0;
        old = d->bitmap;
        dc_sync(d);                                         /* what was drawn on the old one */
        if (o->view24) dib24_sync(o);                       /* what the program wrote to the new one */
        d->bitmap = o;
        d->bits = o->bits; d->w = o->bw; d->h = o->bh; d->stride = o->bw; d->fmt = o->fmt; d->flip = o->flip;
        if (!old) old = &g_default_bitmap;
        break;
    }
    return old;
}

GDIAPI HGDIOBJ GetCurrentObject(HDC h, UINT type)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0;
    switch (type) {
    case 1: return d->pen ? d->pen : GetStockObject(BLACK_PEN);       /* OBJ_PEN */
    case 2: return d->brush ? d->brush : GetStockObject(WHITE_BRUSH); /* OBJ_BRUSH */
    case 6: return d->font ? d->font : GetStockObject(SYSTEM_FONT);   /* OBJ_FONT */
    case 7: return d->bitmap;                                         /* OBJ_BITMAP */
    }
    return 0;
}

GDIAPI DWORD GetObjectType(HGDIOBJ h)
{
    GObj *o = obj_of(h);
    if (!o) {
        NOVA_DC *d = dc_of(h);
        return d ? (d->mem ? 10 /* OBJ_MEMDC */ : 3 /* OBJ_DC */) : 0;
    }
    switch (o->kind) {
    case K_BRUSH: case K_NULLBRUSH: return 2;
    case K_PEN: case K_NULLPEN: return 1;
    case K_FONT: return 6;
    case K_BITMAP: return 7;
    case K_REGION: return 8;
    case K_PALETTE: return 5;                           /* OBJ_PAL */
    }
    return 0;
}

GDIAPI int GetObjectW(HGDIOBJ h, int n, LPVOID out)
{
    GObj *o = obj_of(h);
    if (!o) return 0;
    switch (o->kind) {
    case K_BITMAP: {
        if (!out) return sizeof(BITMAP);
        BITMAP b;
        memset(&b, 0, sizeof(b));
        b.bmWidth = o->bw; b.bmHeight = o->bh; b.bmWidthBytes = o->bw * 4; b.bmPlanes = 1; b.bmBitsPixel = 32;
        b.bmBits = o->fmt ? o->bits : 0;                   /* DIB sections expose their bits */
        if (o->view24) { b.bmWidthBytes = o->stride24; b.bmBitsPixel = (WORD)o->vbpp; b.bmBits = o->view24; }
        int k = n < (int)sizeof(b) ? n : (int)sizeof(b);
        memcpy(out, &b, (size_t)k);
        if (n >= (int)sizeof(DIBSECTION) && o->fmt) {
            DIBSECTION *ds = out;
            memset(&ds->dsBmih, 0, sizeof(ds->dsBmih));
            ds->dsBmih.biSize = sizeof(BITMAPINFOHEADER);
            ds->dsBmih.biWidth = o->bw; ds->dsBmih.biHeight = o->flip ? o->bh : -o->bh;
            ds->dsBmih.biPlanes = 1; ds->dsBmih.biBitCount = (WORD)(o->view24 ? o->vbpp : 32);
            ds->dsBmih.biClrUsed = (DWORD)o->npal;
            ds->dsBmih.biSizeImage = (DWORD)(o->view24 ? o->stride24 : o->bw * 4) * (DWORD)o->bh;
            return sizeof(DIBSECTION);
        }
        return k;
    }
    case K_FONT: {
        if (!out) return sizeof(LOGFONTW);
        LOGFONTW lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = o->height; lf.lfWeight = o->weight; lf.lfItalic = (BYTE)o->italic; lf.lfUnderline = (BYTE)o->underline;
        for (int i = 0; i < 32; i++) lf.lfFaceName[i] = o->face[i];
        int k = n < (int)sizeof(lf) ? n : (int)sizeof(lf);
        memcpy(out, &lf, (size_t)k);
        return k;
    }
    case K_BRUSH: case K_NULLBRUSH: {
        if (!out) return sizeof(LOGBRUSH);
        LOGBRUSH lb = { o->kind == K_BRUSH ? 0u : 1u, o->color, 0 };
        int k = n < (int)sizeof(lb) ? n : (int)sizeof(lb);
        memcpy(out, &lb, (size_t)k);
        return k;
    }
    case K_PEN: case K_NULLPEN: {
        if (!out) return sizeof(LOGPEN);
        LOGPEN lp;
        lp.lopnStyle = o->kind == K_PEN ? 0 : 5;
        lp.lopnWidth.x = o->width; lp.lopnWidth.y = 0;
        lp.lopnColor = o->color;
        int k = n < (int)sizeof(lp) ? n : (int)sizeof(lp);
        memcpy(out, &lp, (size_t)k);
        return k;
    }
    }
    return 0;
}

GDIAPI int GetObjectA(HGDIOBJ h, int n, LPVOID out)
{
    GObj *o = obj_of(h);
    if (!o || o->kind != K_FONT) return GetObjectW(h, n, out);
    if (!out) return sizeof(LOGFONTA);
    LOGFONTW w;
    GetObjectW(h, sizeof(w), &w);
    LOGFONTA a;
    memcpy(&a, &w, 28);
    for (int i = 0; i < 32; i++) a.lfFaceName[i] = (char)w.lfFaceName[i];
    int k = n < (int)sizeof(a) ? n : (int)sizeof(a);
    memcpy(out, &a, (size_t)k);
    return k;
}

GDIAPI BOOL DeleteObject(HGDIOBJ obj)
{
    GObj *o = obj_of(obj);
    if (!o) return FALSE;
    if (o >= g_pool && o < g_pool + POOL) {                 /* stock objects are not freed */
        if (o->kind == K_BITMAP && o->owns == 1 && o->bits) VirtualFree(o->bits, 0, MEM_RELEASE);
        if (o->kind == K_BITMAP && o->owns == 2) UnmapViewOfFile(o->view);
        if (o->kind == K_PALETTE && o->bits) HeapFree(GetProcessHeap(), 0, o->bits);
        if (o->kind == K_BITMAP && o->view24) {
            if (o->view_owned) VirtualFree(o->view24, 0, MEM_RELEASE);
            else UnmapViewOfFile(o->view24);
            VirtualFree(o->last24, 0, MEM_RELEASE);
            if (o->pal) HeapFree(GetProcessHeap(), 0, o->pal);
            o->view24 = o->last24 = NULL;
            o->pal = NULL;
            o->view_owned = o->vbpp = o->npal = o->v565 = 0;
        }
        if (o->kind == K_REGION) rgn_free(o);
        o->used = 0;
    }
    return TRUE;
}

GDIAPI BOOL GetBitmapDimensionEx(HBITMAP h, LPSIZE sz) { GObj *o = obj_of(h); if (!o) return FALSE; sz->cx = o->bw; sz->cy = o->bh; return TRUE; }
GDIAPI LONG GetBitmapBits(HBITMAP h, LONG n, LPVOID out)
{
    GObj *o = bitmap_of(h);
    if (!o || o->kind != K_BITMAP) return 0;
    LONG k = n < o->bw * o->bh * 4 ? n : o->bw * o->bh * 4;
    memcpy(out, o->bits, (size_t)k);
    return k;
}

/* -----------------------------------------------------------------------
 * DC state and information
 * ----------------------------------------------------------------------- */
GDIAPI int SaveDC(HDC h)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0;
    NOVA_DC *s = HeapAlloc(GetProcessHeap(), 0, sizeof(*s));
    if (!s) return 0;
    *s = *d;
    s->saved = d->saved;
    d->saved = s;
    int n = 0;
    for (NOVA_DC *p = d->saved; p; p = p->saved) n++;
    return n;
}

GDIAPI BOOL RestoreDC(HDC h, int which)
{
    NOVA_DC *d = dc_of(h);
    if (!d || !d->saved) return FALSE;
    int depth = 0;
    for (NOVA_DC *p = d->saved; p; p = p->saved) depth++;
    int pops = which < 0 ? -which : depth - which + 1;
    if (pops < 1 || pops > depth) return FALSE;
    NOVA_DC *s = 0;
    for (int i = 0; i < pops; i++) {
        s = d->saved;
        d->saved = s->saved;
        if (i + 1 < pops) HeapFree(GetProcessHeap(), 0, s);
    }
    NOVA_DC *keep = d->saved;
    void *bits = d->bits; int w = d->w, hh = d->h, stride = d->stride;   /* the surface stays */
    *d = *s;
    d->saved = keep;
    if (!d->mem) { d->bits = bits; d->w = w; d->h = hh; d->stride = stride; }
    HeapFree(GetProcessHeap(), 0, s);
    return TRUE;
}

/* The system DPI / 96 the process sees (user32's DPI awareness: 1 for
 * DPI-unaware programs, and without user32) */
static int sys_dpi_k(void)
{
    static UINT (WINAPI *get)(void);
    static int tried;
    if (!tried) {
        HMODULE u = GetModuleHandleA("user32.dll");
        if (u) get = (UINT (WINAPI *)(void))GetProcAddress(u, "GetDpiForSystem");
        tried = u != NULL;
    }
    UINT dpi = get ? get() : 96;
    return dpi >= 192 ? 2 : 1;
}

GDIAPI int GetDeviceCaps(HDC h, int index)
{
    NOVA_DC *d = dc_of(h);
    ULONG sw = 1280, sh = 800;
    NtNovaGuiScreenSize(&sw, &sh);
    int k = index == 8 || index == 10 || index == 88 || index == 90 || index == 117 || index == 118 ? sys_dpi_k() : 1;
    sw *= (ULONG)k; sh *= (ULONG)k;                         /* a DPI-aware program's pixels */
    switch (index) {
    case 2:   return 0x0600;                                /* DRIVERVERSION */
    case 4:   return (int)(sw * 254 / 960);                 /* HORZSIZE (mm at 96 dpi) */
    case 6:   return (int)(sh * 254 / 960);                 /* VERTSIZE */
    case 8:   return d && d->mem && d->bitmap ? d->w : (int)sw;   /* HORZRES */
    case 10:  return d && d->mem && d->bitmap ? d->h : (int)sh;   /* VERTRES */
    case 12:  return 32;                                    /* BITSPIXEL */
    case 14:  return 1;                                     /* PLANES */
    case 24:  return -1;                                    /* NUMCOLORS */
    case 38:  return 0x7E99;                                /* RASTERCAPS: BITBLT, STRETCHBLT, DIBTODEV, ... */
    case 88: case 90: return 96 * k;                        /* LOGPIXELSX/Y */
    case 104: return 0;                                     /* SIZEPALETTE */
    case 108: return 32;                                    /* COLORRES */
    case 116: return 60;                                    /* VREFRESH */
    case 117: return (int)sh;                               /* DESKTOPVERTRES */
    case 118: return (int)sw;                               /* DESKTOPHORZRES */
    case 120: return 0;                                     /* BLTALIGNMENT */
    case 121: return 1;                                     /* SHADEBLENDCAPS: SB_CONST_ALPHA */
    }
    return 0;
}

/* drawing is done at once; GdiFlush brings it to the bits of DIB sections
 * of other than 32 bits per pixel */
GDIAPI BOOL GdiFlush(void)
{
    for (int i = 0; i < g_high; i++) if (g_pool[i].used && g_pool[i].view24) dib24_sync(&g_pool[i]);
    return TRUE;
}
GDIAPI DWORD GdiSetBatchLimit(DWORD n) { (void)n; return 1; }
GDIAPI int SetLayout(HDC h, DWORD l) { (void)h; (void)l; return 0; }
GDIAPI UINT GetNearestColor(HDC h, COLORREF c) { (void)h; return c & 0xFFFFFF; }

/* -----------------------------------------------------------------------
 * Regions and the clip region: region.c
 * ----------------------------------------------------------------------- */
GDIAPI BOOL RectVisible(HDC h, const RECT *r)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return FALSE;
    RECT e = { r->left + d->org_x, r->top + d->org_y, r->right + d->org_x, r->bottom + d->org_y };
    return dev_clip(d, &e);
}
GDIAPI BOOL PtVisible(HDC h, int x, int y) { NOVA_DC *d = dc_of(h); return d && dev_visible(d, x + d->org_x, y + d->org_y); }

