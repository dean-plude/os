/*
 * gdi32.dll — a software rasterizer
 *
 * A device context (NOVA_DC, from wingdi.h) points at pixels: a window's
 * client bitmap (COLORREF pixels, from user32), or for a memory DC the
 * bitmap selected into it — a compatible bitmap (COLORREF) or a DIB
 * section (BGRA, top-down or bottom-up, as the program asked).  These
 * calls plot straight into those pixels.  Text uses an embedded 8x8 font,
 * scaled to the selected font's height (16 px by default).
 */
#define NOVA_BUILD_GDI32
#include <windows.h>
#include <winternl.h>

static const unsigned char g_font[96 * 8] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00,
    0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00,
    0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00,
    0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00,
    0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00,
    0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00,
    0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00,
    0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00,
    0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00,
    0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,
    0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06,
    0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00,
    0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00,
    0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00,
    0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00,
    0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00,
    0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00,
    0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00,
    0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00,
    0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00,
    0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00,
    0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00,
    0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00,
    0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00,
    0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06,
    0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00,
    0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00,
    0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00,
    0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00,
    0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00,
    0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00,
    0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00,
    0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00,
    0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00,
    0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00,
    0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00,
    0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00,
    0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00,
    0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00,
    0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00,
    0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00,
    0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00,
    0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00,
    0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00,
    0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00,
    0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00,
    0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00,
    0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00,
    0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00,
    0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00,
    0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00,
    0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00,
    0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00,
    0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00,
    0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00,
    0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00,
    0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00,
    0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00,
    0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00,
    0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,
    0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00,
    0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00,
    0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00,
    0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00,
    0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00,
    0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00,
    0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F,
    0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00,
    0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00,
    0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E,
    0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00,
    0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00,
    0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00,
    0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00,
    0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00,
    0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F,
    0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78,
    0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00,
    0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00,
    0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00,
    0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00,
    0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00,
    0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00,
    0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00,
    0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F,
    0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00,
    0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00,
    0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00,
    0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00,
    0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,};
#define GLYPH_W 8
#define GLYPH_H 8

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

static NOVA_DC *dc_of(HDC h) { return (NOVA_DC *)h; }

/* -----------------------------------------------------------------------
 * GDI objects
 * ----------------------------------------------------------------------- */
enum { K_BRUSH = 1, K_PEN, K_NULLBRUSH, K_NULLPEN, K_FONT, K_BITMAP, K_REGION };

typedef struct GObj {
    int kind;
    COLORREF color;                 /* brush / pen (user32 reads it at offset 4) */
    int width;
    int used;
    /* fonts */
    int height, weight, italic, underline;
    WCHAR face[32];
    /* bitmaps */
    int bw, bh, bpp, fmt, flip, owns;
    DWORD *bits;
    /* regions (a rectangle) */
    RECT rc;
} GObj;

#define POOL 512
static GObj g_stock[20];
static GObj g_pool[POOL];
static int  g_stock_ready;
static SRWLOCK g_lock;

static void stock_init(void)
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
    for (int i = OEM_FIXED_FONT; i <= DEFAULT_GUI_FONT; i++) {   /* the fonts: all the built-in one */
        g_stock[i].kind = K_FONT;
        g_stock[i].height = 16;
        g_stock[i].weight = 400;
        const char *f = i == DEFAULT_GUI_FONT ? "Segoe UI" : "System";
        for (int k = 0; f[k]; k++) g_stock[i].face[k] = (WCHAR)f[k];
    }
    g_stock_ready = 1;
}

GDIAPI HGDIOBJ GetStockObject(int obj)
{
    if (!g_stock_ready) stock_init();
    if (obj < 0 || obj >= 20 || !g_stock[obj].kind) return 0;
    return (HGDIOBJ)&g_stock[obj];
}

static GObj *new_obj(int kind)
{
    AcquireSRWLockExclusive(&g_lock);
    GObj *o = 0;
    for (int i = 0; i < POOL; i++) if (!g_pool[i].used) {
        o = &g_pool[i];
        memset(o, 0, sizeof(*o));
        o->kind = kind;
        o->used = 1;
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!o) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return o;
}

static GObj *obj_of(HGDIOBJ h)
{
    GObj *o = h;
    if (!o) return 0;
    if (!g_stock_ready) stock_init();
    if ((o >= g_pool && o < g_pool + POOL && o->used) || (o >= g_stock && o < g_stock + 20 && o->kind)) return o;
    return 0;
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

static inline void put(NOVA_DC *d, int x, int y, COLORREF c)
{
    x += d->org_x; y += d->org_y;
    if (!d->bits || x < 0 || y < 0 || x >= d->w || y >= d->h) return;
    *pixel_at(d, x, y) = to_native(d, c);
}

GDIAPI COLORREF SetPixel(HDC h, int x, int y, COLORREF c) { NOVA_DC *d = dc_of(h); if (d) put(d, x, y, c); return c; }
GDIAPI BOOL SetPixelV(HDC h, int x, int y, COLORREF c) { SetPixel(h, x, y, c); return TRUE; }
GDIAPI COLORREF GetPixel(HDC h, int x, int y)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0xFFFFFFFF;
    x += d->org_x; y += d->org_y;
    if (!d->bits || x < 0 || y < 0 || x >= d->w || y >= d->h) return 0xFFFFFFFF;
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
GDIAPI int SetROP2(HDC h, int m) { (void)h; (void)m; return 13; /* R2_COPYPEN */ }
GDIAPI int SetStretchBltMode(HDC h, int m) { (void)h; (void)m; return 1; }
GDIAPI int SetPolyFillMode(HDC h, int m) { (void)h; (void)m; return 1; }
GDIAPI int SetMapMode(HDC h, int m) { (void)h; (void)m; return 1; /* MM_TEXT only */ }
GDIAPI int GetMapMode(HDC h) { (void)h; return 1; }
GDIAPI int SetGraphicsMode(HDC h, int m) { (void)h; (void)m; return 1; }

GDIAPI BOOL SetViewportOrgEx(HDC h, int x, int y, LPPOINT old)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    if (old) { old->x = d->org_x; old->y = d->org_y; }
    d->org_x = x; d->org_y = y;
    return TRUE;
}
GDIAPI BOOL OffsetViewportOrgEx(HDC h, int x, int y, LPPOINT old)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    return SetViewportOrgEx(h, d->org_x + x, d->org_y + y, old);
}
GDIAPI BOOL GetViewportOrgEx(HDC h, LPPOINT p) { NOVA_DC *d = dc_of(h); if (!d) return FALSE; p->x = d->org_x; p->y = d->org_y; return TRUE; }
GDIAPI BOOL SetWindowOrgEx(HDC h, int x, int y, LPPOINT old) { return SetViewportOrgEx(h, -x, -y, old); }
GDIAPI BOOL SetBrushOrgEx(HDC h, int x, int y, LPPOINT old) { (void)h; (void)x; (void)y; if (old) old->x = old->y = 0; return TRUE; }

static void fill(NOVA_DC *d, int x0, int y0, int x1, int y1, COLORREF c)
{
    if (!d->bits) return;
    x0 += d->org_x; x1 += d->org_x; y0 += d->org_y; y1 += d->org_y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > d->w) x1 = d->w;
    if (y1 > d->h) y1 = d->h;
    DWORD v = to_native(d, c);
    for (int y = y0; y < y1; y++) {
        DWORD *row = pixel_at(d, 0, y);
        for (int x = x0; x < x1; x++) row[x] = v;
    }
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

static void line(NOVA_DC *d, int x0, int y0, int x1, int y1, COLORREF c, int width)
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
static void fill_polygon(NOVA_DC *d, const POINT *pt, int n, COLORREF c)
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

static void ellipse(NOVA_DC *d, int l, int t, int r, int b, BOOL do_fill, BOOL do_edge)
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
    (void)h;
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

GDIAPI HBITMAP CreateDIBSection(HDC h, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset)
{
    (void)h; (void)usage; (void)section; (void)offset;
    const BITMAPINFOHEADER *bh = &bi->bmiHeader;
    if (bh->biBitCount != 32 && bh->biBitCount != 24) { SetLastError(ERROR_INVALID_PARAMETER); if (bits) *bits = 0; return 0; }
    int w = bh->biWidth, hh = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    if (bh->biBitCount == 24) {
        /* 24-bit DIBs are kept as 32-bit here; the program's pointer sees 32-bit pixels */
    }
    GObj *o = make_bitmap(w, hh, 1, bh->biHeight > 0, 0);
    if (bits) *bits = o ? o->bits : 0;
    return (HBITMAP)o;
}

GDIAPI HDC CreateCompatibleDC(HDC h)
{
    (void)h;
    NOVA_DC *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) return 0;
    static DWORD one_pixel;                                 /* the default 1x1 bitmap */
    d->mem = 1;
    d->bits = &one_pixel; d->w = d->h = 1; d->stride = 1;
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

GDIAPI BOOL BitBlt(HDC dst, int x, int y, int w, int hh, HDC src, int sx, int sy, DWORD rop)
{
    NOVA_DC *dd = dc_of(dst);
    if (!dd) return FALSE;
    if (rop == BLACKNESS || rop == WHITENESS || rop == PATCOPY || !src) {
        fill(dd, x, y, x + w, y + hh, rop == WHITENESS ? 0xFFFFFF : rop == PATCOPY ? dd->brush_color : 0);
        return TRUE;
    }
    blit(dd, x, y, w, hh, dc_of(src), sx, sy, w, hh);
    return TRUE;
}

GDIAPI BOOL StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    NOVA_DC *dd = dc_of(dst);
    if (!dd) return FALSE;
    if (!src) return PatBlt(dst, x, y, w, h, rop);
    blit(dd, x, y, w, h, dc_of(src), sx, sy, sw, sh);
    return TRUE;
}

GDIAPI BOOL TransparentBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, UINT key)
{
    NOVA_DC *dd = dc_of(dst), *sd = dc_of(src);
    if (!dd || !sd) return FALSE;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sxx = sx + i * sw / w + sd->org_x, syy = sy + j * sh / h + sd->org_y;
            if (sxx < 0 || syy < 0 || sxx >= sd->w || syy >= sd->h) continue;
            COLORREF c = from_native(sd, *pixel_at(sd, sxx, syy));
            if (c != (key & 0xFFFFFF)) put(dd, x + i, y + j, c);
        }
    return TRUE;
}

GDIAPI BOOL AlphaBlend(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION bf)
{
    NOVA_DC *dd = dc_of(dst), *sd = dc_of(src);
    if (!dd || !sd) return FALSE;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sxx = sx + i * sw / w + sd->org_x, syy = sy + j * sh / h + sd->org_y;
            int tx = x + i + dd->org_x, ty = y + j + dd->org_y;
            if (sxx < 0 || syy < 0 || sxx >= sd->w || syy >= sd->h || tx < 0 || ty < 0 || tx >= dd->w || ty >= dd->h) continue;
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
    case 32: p += 4 * x; return RGB(p[2], p[1], p[0]);
    case 24: p += 3 * x; return RGB(p[2], p[1], p[0]);
    case 16: { WORD v = ((const WORD *)p)[x]; return RGB((v >> 10 & 31) * 255 / 31, (v >> 5 & 31) * 255 / 31, (v & 31) * 255 / 31); }
    case 8: case 4: case 1: {
        int idx = h->biBitCount == 8 ? p[x] : h->biBitCount == 4 ? (p[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (p[x / 8] >> (7 - x % 8)) & 1;
        const RGBQUAD *q = &bi->bmiColors[idx];
        return RGB(q->rgbRed, q->rgbGreen, q->rgbBlue);
    }
    }
    return 0;
}

GDIAPI int StretchDIBits(HDC h, int x, int y, int w, int hh, int sx, int sy, int sw, int sh, const void *bits,
                         const BITMAPINFO *bi, UINT usage, DWORD rop)
{
    (void)usage; (void)rop;
    NOVA_DC *d = dc_of(h);
    if (!d || !bits || !w || !hh || !sw || !sh) return 0;
    int bh = bi->bmiHeader.biHeight < 0 ? -bi->bmiHeader.biHeight : bi->bmiHeader.biHeight;
    for (int j = 0; j < (hh < 0 ? -hh : hh); j++) {
        int syy = sy + j * sh / (hh < 0 ? -hh : hh);
        if (syy < 0 || syy >= bh) continue;
        for (int i = 0; i < (w < 0 ? -w : w); i++) {
            int sxx = sx + i * sw / (w < 0 ? -w : w);
            if (sxx < 0 || sxx >= bi->bmiHeader.biWidth) continue;
            put(d, w < 0 ? x - i : x + i, hh < 0 ? y - j : y + j, dib_pixel(bi, bits, sxx, syy));
        }
    }
    return hh < 0 ? -hh : hh;
}

GDIAPI int SetDIBitsToDevice(HDC h, int x, int y, DWORD w, DWORD hh, int sx, int sy, UINT start, UINT lines,
                             const void *bits, const BITMAPINFO *bi, UINT usage)
{
    (void)start; (void)lines;
    int bh = bi->bmiHeader.biHeight < 0 ? -bi->bmiHeader.biHeight : bi->bmiHeader.biHeight;
    /* sy counts from the bottom for bottom-up DIBs */
    int top = bi->bmiHeader.biHeight > 0 ? bh - sy - (int)hh : sy;
    return StretchDIBits(h, x, y, (int)w, (int)hh, sx, top, (int)w, (int)hh, bits, bi, usage, SRCCOPY) ? (int)hh : 0;
}

GDIAPI int GetDIBits(HDC h, HBITMAP bmp, UINT start, UINT lines, void *bits, BITMAPINFO *bi, UINT usage)
{
    (void)h; (void)usage;
    GObj *o = obj_of(bmp);
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
            if (bh->biBitCount == 32) q[3] = 0;
        }
    }
    return (int)n;
}

GDIAPI int SetDIBits(HDC h, HBITMAP bmp, UINT start, UINT lines, const void *bits, const BITMAPINFO *bi, UINT usage)
{
    (void)h; (void)usage;
    GObj *o = obj_of(bmp);
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
    return (int)n;
}

GDIAPI HBITMAP CreateDIBitmap(HDC h, const BITMAPINFOHEADER *bh, DWORD init, const void *bits, const BITMAPINFO *bi, UINT usage)
{
    HBITMAP b = CreateCompatibleBitmap(h, bh->biWidth, bh->biHeight < 0 ? -bh->biHeight : bh->biHeight);
    if (b && (init & 4 /* CBM_INIT */) && bits) SetDIBits(h, b, 0, (UINT)(bh->biHeight < 0 ? -bh->biHeight : bh->biHeight), bits, bi, usage);
    return b;
}

/* -----------------------------------------------------------------------
 * Fonts: the built-in 8x8 glyphs, scaled to the font's height
 * ----------------------------------------------------------------------- */
static int font_scale(NOVA_DC *d)
{
    GObj *f = d ? obj_of(d->font) : 0;
    int hgt = f ? f->height : 16;
    if (hgt < 0) hgt = -hgt;
    if (!hgt) hgt = 16;
    int s = (hgt + 4) / 8;
    return s < 1 ? 1 : s > 8 ? 8 : s;
}

GDIAPI HFONT CreateFontIndirectW(const LOGFONTW *lf)
{
    GObj *o = new_obj(K_FONT);
    if (!o) return 0;
    o->height = lf->lfHeight ? lf->lfHeight : 16;
    o->weight = lf->lfWeight ? lf->lfWeight : 400;
    o->italic = lf->lfItalic;
    o->underline = lf->lfUnderline;
    for (int i = 0; i < 31 && lf->lfFaceName[i]; i++) o->face[i] = lf->lfFaceName[i];
    return (HFONT)o;
}

GDIAPI HFONT CreateFontIndirectA(const LOGFONTA *lf)
{
    LOGFONTW w;
    memcpy(&w, lf, 28);                                     /* the numeric fields */
    for (int i = 0; i < 32; i++) w.lfFaceName[i] = (WCHAR)(BYTE)lf->lfFaceName[i];
    return CreateFontIndirectW(&w);
}

GDIAPI HFONT CreateFontW(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike,
                         DWORD charset, DWORD outprec, DWORD clip, DWORD quality, DWORD pitch, LPCWSTR face)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = h; lf.lfWidth = w; lf.lfEscapement = esc; lf.lfOrientation = orient; lf.lfWeight = weight;
    lf.lfItalic = (BYTE)italic; lf.lfUnderline = (BYTE)underline; lf.lfStrikeOut = (BYTE)strike;
    lf.lfCharSet = (BYTE)charset; lf.lfOutPrecision = (BYTE)outprec; lf.lfClipPrecision = (BYTE)clip;
    lf.lfQuality = (BYTE)quality; lf.lfPitchAndFamily = (BYTE)pitch;
    for (int i = 0; face && i < 31 && face[i]; i++) lf.lfFaceName[i] = face[i];
    return CreateFontIndirectW(&lf);
}

GDIAPI HFONT CreateFontA(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike,
                         DWORD charset, DWORD outprec, DWORD clip, DWORD quality, DWORD pitch, LPCSTR face)
{
    WCHAR f[32] = { 0 };
    for (int i = 0; face && i < 31 && face[i]; i++) f[i] = (WCHAR)(BYTE)face[i];
    return CreateFontW(h, w, esc, orient, weight, italic, underline, strike, charset, outprec, clip, quality, pitch, f);
}

static void draw_char(NOVA_DC *d, int x, int y, WCHAR ch, int scale)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const unsigned char *g = g_font + (ch - 0x20) * 8;
    for (int gy = 0; gy < GLYPH_H; gy++) {
        unsigned char bits = g[gy];
        for (int gx = 0; gx < GLYPH_W; gx++) {
            int on = bits & (1 << gx);
            if (!on && d->bk_mode == TRANSPARENT) continue;
            COLORREF c = on ? d->text_color : d->bk_color;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    put(d, x + gx * scale + sx, y + gy * scale + sy, c);
        }
    }
}

/* Where the text starts for the DC's alignment (TA_CENTER 6, TA_RIGHT 2, TA_BOTTOM 8, TA_BASELINE 24, TA_UPDATECP 1) */
static void text_origin(NOVA_DC *d, int *x, int *y, int n, int scale)
{
    int w = n * GLYPH_W * scale, h = GLYPH_H * scale;
    if (d->text_align & 1) { *x = d->cx; *y = d->cy; }
    if ((d->text_align & 6) == 6) *x -= w / 2;
    else if (d->text_align & 2) *x -= w;
    if ((d->text_align & 24) == 24) *y -= h - scale;
    else if (d->text_align & 8) *y -= h;
    if (d->text_align & 1) d->cx += w;
}

GDIAPI BOOL TextOutW(HDC h, int x, int y, LPCWSTR s, int len)
{
    NOVA_DC *d = dc_of(h); if (!d || !s) return FALSE;
    int scale = font_scale(d);
    text_origin(d, &x, &y, len, scale);
    for (int i = 0; i < len; i++) draw_char(d, x + i * GLYPH_W * scale, y, s[i], scale);
    return TRUE;
}

GDIAPI BOOL TextOutA(HDC h, int x, int y, LPCSTR s, int len)
{
    NOVA_DC *d = dc_of(h); if (!d || !s) return FALSE;
    int scale = font_scale(d);
    text_origin(d, &x, &y, len, scale);
    for (int i = 0; i < len; i++) draw_char(d, x + i * GLYPH_W * scale, y, (BYTE)s[i], scale);
    return TRUE;
}

GDIAPI BOOL ExtTextOutW(HDC h, int x, int y, UINT opts, const RECT *rc, LPCWSTR s, UINT len, const INT *dx)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    if (rc && (opts & 2 /* ETO_OPAQUE */)) fill(d, rc->left, rc->top, rc->right, rc->bottom, d->bk_color);
    if (!s || !len) return TRUE;
    if (!dx) return TextOutW(h, x, y, s, (int)len);
    int scale = font_scale(d);
    for (UINT i = 0; i < len; i++) { draw_char(d, x, y, s[i], scale); x += dx[i]; }
    return TRUE;
}

GDIAPI BOOL ExtTextOutA(HDC h, int x, int y, UINT opts, const RECT *rc, LPCSTR s, UINT len, const INT *dx)
{
    WCHAR w[512];
    UINT n = len < 512 ? len : 512;
    for (UINT i = 0; i < n; i++) w[i] = (BYTE)s[i];
    return ExtTextOutW(h, x, y, opts, rc, s ? w : 0, n, dx);
}

GDIAPI BOOL GetTextExtentPoint32W(HDC h, LPCWSTR s, int len, LPSIZE sz)
{
    (void)s;
    int scale = font_scale(dc_of(h));
    if (sz) { sz->cx = len * GLYPH_W * scale; sz->cy = GLYPH_H * scale; }
    return TRUE;
}

GDIAPI BOOL GetTextExtentPoint32A(HDC h, LPCSTR s, int len, LPSIZE sz) { (void)s; return GetTextExtentPoint32W(h, 0, len, sz); }
GDIAPI BOOL GetTextExtentPointW(HDC h, LPCWSTR s, int len, LPSIZE sz) { return GetTextExtentPoint32W(h, s, len, sz); }
GDIAPI BOOL GetTextExtentPointA(HDC h, LPCSTR s, int len, LPSIZE sz) { return GetTextExtentPoint32A(h, s, len, sz); }

GDIAPI BOOL GetTextExtentExPointW(HDC h, LPCWSTR s, int len, int max, LPINT fit, LPINT dx, LPSIZE sz)
{
    int cw = GLYPH_W * font_scale(dc_of(h));
    if (fit) *fit = max < 0 ? len : (max / cw < len ? max / cw : len);
    for (int i = 0; dx && i < len; i++) dx[i] = (i + 1) * cw;
    return GetTextExtentPoint32W(h, s, len, sz);
}

GDIAPI BOOL GetTextMetricsW(HDC h, TEXTMETRICW *tm)
{
    int scale = font_scale(dc_of(h));
    memset(tm, 0, sizeof(*tm));
    tm->tmHeight = GLYPH_H * scale;
    tm->tmAscent = (GLYPH_H - 1) * scale;
    tm->tmDescent = scale;
    tm->tmAveCharWidth = tm->tmMaxCharWidth = GLYPH_W * scale;
    tm->tmWeight = 400;
    tm->tmFirstChar = 0x20; tm->tmLastChar = 0x7E; tm->tmDefaultChar = '?'; tm->tmBreakChar = ' ';
    tm->tmPitchAndFamily = 0x30;                           /* fixed pitch (bit clear), FF_MODERN */
    tm->tmCharSet = 0;
    tm->tmDigitizedAspectX = tm->tmDigitizedAspectY = 96;
    return TRUE;
}

GDIAPI BOOL GetTextMetricsA(HDC h, TEXTMETRICA *tm)
{
    TEXTMETRICW w;
    GetTextMetricsW(h, &w);
    memcpy(tm, &w, 44);                                     /* the LONGs */
    tm->tmFirstChar = 0x20; tm->tmLastChar = 0x7E; tm->tmDefaultChar = '?'; tm->tmBreakChar = ' ';
    tm->tmItalic = w.tmItalic; tm->tmUnderlined = w.tmUnderlined; tm->tmStruckOut = w.tmStruckOut;
    tm->tmPitchAndFamily = w.tmPitchAndFamily; tm->tmCharSet = w.tmCharSet;
    return TRUE;
}

GDIAPI int GetTextFaceW(HDC h, int n, LPWSTR out)
{
    NOVA_DC *d = dc_of(h);
    GObj *f = d ? obj_of(d->font) : 0;
    const WCHAR sys[] = { 'S', 'y', 's', 't', 'e', 'm', 0 };
    const WCHAR *face = f && f->face[0] ? f->face : sys;
    int k = 0;
    while (face[k]) k++;
    if (!out) return k + 1;
    int m = k < n - 1 ? k : n - 1;
    for (int i = 0; i < m; i++) out[i] = face[i];
    if (n > 0) out[m] = 0;
    return m;
}

GDIAPI BOOL GetCharWidth32W(HDC h, UINT first, UINT last, LPINT out)
{
    int cw = GLYPH_W * font_scale(dc_of(h));
    for (UINT c = first; c <= last; c++) out[c - first] = cw;
    return TRUE;
}

GDIAPI BOOL GetCharABCWidthsW(HDC h, UINT first, UINT last, ABC *out)
{
    int cw = GLYPH_W * font_scale(dc_of(h));
    for (UINT c = first; c <= last; c++) { out[c - first].abcA = 0; out[c - first].abcB = (UINT)cw; out[c - first].abcC = 0; }
    return TRUE;
}

/* One font family, reported to enumeration callbacks */
typedef int (CALLBACK *FONTENUMPROCW)(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM);
GDIAPI int EnumFontFamiliesExW(HDC h, LOGFONTW *want, FONTENUMPROCW fn, LPARAM lp, DWORD flags)
{
    (void)want; (void)flags;
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = 16; lf.lfWeight = 400; lf.lfPitchAndFamily = 0x31;
    const char *face = "System";
    for (int i = 0; face[i]; i++) lf.lfFaceName[i] = (WCHAR)face[i];
    TEXTMETRICW tm;
    GetTextMetricsW(h, &tm);
    return fn(&lf, &tm, 1 /* RASTER_FONTTYPE */, lp);
}
GDIAPI int EnumFontFamiliesW(HDC h, LPCWSTR face, FONTENUMPROCW fn, LPARAM lp) { (void)face; return EnumFontFamiliesExW(h, 0, fn, lp, 0); }
GDIAPI int EnumFontsW(HDC h, LPCWSTR face, FONTENUMPROCW fn, LPARAM lp) { (void)face; return EnumFontFamiliesExW(h, 0, fn, lp, 0); }
GDIAPI int AddFontResourceExW(LPCWSTR f, DWORD fl, PVOID r) { (void)f; (void)fl; (void)r; return 0; }
GDIAPI HANDLE AddFontMemResourceEx(PVOID p, DWORD n, PVOID r, DWORD *count) { (void)p; (void)n; (void)r; if (count) *count = 0; return 0; }
GDIAPI BOOL RemoveFontMemResourceEx(HANDLE h) { (void)h; return TRUE; }

/* -----------------------------------------------------------------------
 * Selecting, describing and deleting objects
 * ----------------------------------------------------------------------- */
GDIAPI HGDIOBJ SelectObject(HDC h, HGDIOBJ obj)
{
    NOVA_DC *d = dc_of(h);
    GObj *o = obj_of(obj);
    if (!d || !o) return 0;
    HGDIOBJ old = 0;
    switch (o->kind) {
    case K_BRUSH:     old = d->brush ? d->brush : GetStockObject(WHITE_BRUSH); d->brush = o; d->brush_color = o->color; d->has_brush = 1; break;
    case K_NULLBRUSH: old = d->brush ? d->brush : GetStockObject(WHITE_BRUSH); d->brush = o; d->has_brush = 0; break;
    case K_PEN:       old = d->pen ? d->pen : GetStockObject(BLACK_PEN); d->pen = o; d->pen_color = o->color; d->pen_width = o->width; d->has_pen = 1; break;
    case K_NULLPEN:   old = d->pen ? d->pen : GetStockObject(BLACK_PEN); d->pen = o; d->has_pen = 0; break;
    case K_FONT:      old = d->font ? d->font : GetStockObject(SYSTEM_FONT); d->font = o; break;
    case K_REGION:    return (HGDIOBJ)(ULONG_PTR)2;        /* SIMPLEREGION: clipping is not applied */
    case K_BITMAP:
        if (!d->mem) return 0;
        old = d->bitmap;
        d->bitmap = o;
        d->bits = o->bits; d->w = o->bw; d->h = o->bh; d->stride = o->bw; d->fmt = o->fmt; d->flip = o->flip;
        if (!old) { static GObj one = { K_BITMAP }; old = &one; }
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
        int k = n < (int)sizeof(b) ? n : (int)sizeof(b);
        memcpy(out, &b, (size_t)k);
        if (n >= (int)sizeof(DIBSECTION) && o->fmt) {
            DIBSECTION *ds = out;
            memset(&ds->dsBmih, 0, sizeof(ds->dsBmih));
            ds->dsBmih.biSize = sizeof(BITMAPINFOHEADER);
            ds->dsBmih.biWidth = o->bw; ds->dsBmih.biHeight = o->flip ? o->bh : -o->bh;
            ds->dsBmih.biPlanes = 1; ds->dsBmih.biBitCount = 32;
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
        if (o->kind == K_BITMAP && o->owns && o->bits) VirtualFree(o->bits, 0, MEM_RELEASE);
        o->used = 0;
    }
    return TRUE;
}

GDIAPI BOOL GetBitmapDimensionEx(HBITMAP h, LPSIZE sz) { GObj *o = obj_of(h); if (!o) return FALSE; sz->cx = o->bw; sz->cy = o->bh; return TRUE; }
GDIAPI LONG GetBitmapBits(HBITMAP h, LONG n, LPVOID out)
{
    GObj *o = obj_of(h);
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

GDIAPI int GetDeviceCaps(HDC h, int index)
{
    NOVA_DC *d = dc_of(h);
    ULONG sw = 1280, sh = 800;
    NtNovaGuiScreenSize(&sw, &sh);
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
    case 88: case 90: return 96;                            /* LOGPIXELSX/Y */
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

GDIAPI BOOL GdiFlush(void) { return TRUE; }
GDIAPI DWORD GdiSetBatchLimit(DWORD n) { (void)n; return 1; }
GDIAPI int SetLayout(HDC h, DWORD l) { (void)h; (void)l; return 0; }
GDIAPI UINT GetNearestColor(HDC h, COLORREF c) { (void)h; return c & 0xFFFFFF; }

/* -----------------------------------------------------------------------
 * Regions: rectangles; clipping is not applied
 * ----------------------------------------------------------------------- */
GDIAPI HRGN CreateRectRgn(int l, int t, int r, int b)
{
    GObj *o = new_obj(K_REGION);
    if (o) { o->rc.left = l; o->rc.top = t; o->rc.right = r; o->rc.bottom = b; }
    return (HRGN)o;
}
GDIAPI HRGN CreateRectRgnIndirect(const RECT *r) { return CreateRectRgn(r->left, r->top, r->right, r->bottom); }
GDIAPI HRGN CreateRoundRectRgn(int l, int t, int r, int b, int w, int h) { (void)w; (void)h; return CreateRectRgn(l, t, r, b); }
GDIAPI HRGN CreateEllipticRgn(int l, int t, int r, int b) { return CreateRectRgn(l, t, r, b); }
GDIAPI int GetRgnBox(HRGN h, LPRECT r) { GObj *o = obj_of(h); if (!o) return 0; *r = o->rc; return 2; }
GDIAPI int CombineRgn(HRGN dst, HRGN a, HRGN b, int mode)
{
    GObj *d = obj_of(dst), *x = obj_of(a), *y = obj_of(b);
    if (!d || !x) return 0;
    RECT r = x->rc;
    if (y && mode == 1) {                                   /* RGN_AND */
        if (y->rc.left > r.left) r.left = y->rc.left;
        if (y->rc.top > r.top) r.top = y->rc.top;
        if (y->rc.right < r.right) r.right = y->rc.right;
        if (y->rc.bottom < r.bottom) r.bottom = y->rc.bottom;
    } else if (y && mode == 2) {                            /* RGN_OR: the bounding box */
        if (y->rc.left < r.left) r.left = y->rc.left;
        if (y->rc.top < r.top) r.top = y->rc.top;
        if (y->rc.right > r.right) r.right = y->rc.right;
        if (y->rc.bottom > r.bottom) r.bottom = y->rc.bottom;
    }
    d->rc = r;
    return r.right > r.left && r.bottom > r.top ? 2 : 1;
}
GDIAPI BOOL SetRectRgn(HRGN h, int l, int t, int r, int b) { GObj *o = obj_of(h); if (!o) return FALSE; o->rc.left = l; o->rc.top = t; o->rc.right = r; o->rc.bottom = b; return TRUE; }
GDIAPI BOOL PtInRegion(HRGN h, int x, int y) { GObj *o = obj_of(h); return o && x >= o->rc.left && x < o->rc.right && y >= o->rc.top && y < o->rc.bottom; }
GDIAPI int OffsetRgn(HRGN h, int x, int y) { GObj *o = obj_of(h); if (!o) return 0; o->rc.left += x; o->rc.right += x; o->rc.top += y; o->rc.bottom += y; return 2; }
GDIAPI int SelectClipRgn(HDC h, HRGN r) { (void)h; (void)r; return 2; }
GDIAPI int ExtSelectClipRgn(HDC h, HRGN r, int mode) { (void)h; (void)r; (void)mode; return 2; }
GDIAPI int IntersectClipRect(HDC h, int l, int t, int r, int b) { (void)h; (void)l; (void)t; (void)r; (void)b; return 2; }
GDIAPI int ExcludeClipRect(HDC h, int l, int t, int r, int b) { (void)h; (void)l; (void)t; (void)r; (void)b; return 2; }
GDIAPI int GetClipBox(HDC h, LPRECT r)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0;
    r->left = -d->org_x; r->top = -d->org_y; r->right = d->w - d->org_x; r->bottom = d->h - d->org_y;
    return 2;
}
GDIAPI int GetClipRgn(HDC h, HRGN r) { (void)h; (void)r; return 0; }
GDIAPI BOOL FillRgn(HDC h, HRGN r, HBRUSH b)
{
    NOVA_DC *d = dc_of(h);
    GObj *o = obj_of(r), *br = obj_of(b);
    if (!d || !o) return FALSE;
    fill(d, o->rc.left, o->rc.top, o->rc.right, o->rc.bottom, br ? br->color : 0);
    return TRUE;
}
GDIAPI BOOL PaintRgn(HDC h, HRGN r) { NOVA_DC *d = dc_of(h); GObj *o = obj_of(r); if (!d || !o) return FALSE; fill(d, o->rc.left, o->rc.top, o->rc.right, o->rc.bottom, d->brush_color); return TRUE; }

/* Exposed to user32 for FillRect/DrawText */
__declspec(dllexport) void NovaGdiFill(HDC h, int l, int t, int r, int b, COLORREF c)
{
    NOVA_DC *d = dc_of(h); if (d) fill(d, l, t, r, b, c);
}
__declspec(dllexport) int  NovaGdiCellW(void) { return GLYPH_W * 2; }
__declspec(dllexport) int  NovaGdiCellH(void) { return GLYPH_H * 2; }
__declspec(dllexport) void NovaGdiChar(HDC h, int x, int y, char c) { NOVA_DC *d = dc_of(h); if (d) draw_char(d, x, y, (BYTE)c, font_scale(d)); }
