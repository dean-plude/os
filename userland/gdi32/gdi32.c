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

#define POOL 512
GObj g_stock[20];
static GObj g_pool[POOL];
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
    for (int i = OEM_FIXED_FONT; i <= DEFAULT_GUI_FONT; i++) {   /* the stock fonts */
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
    }
    g_stock_ready = 1;
}

GDIAPI HGDIOBJ GetStockObject(int obj)
{
    if (!g_stock_ready) stock_init();
    if (obj < 0 || obj >= 20 || !g_stock[obj].kind) return 0;
    return (HGDIOBJ)&g_stock[obj];
}

GObj *new_obj(int kind)
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

GObj *obj_of(HGDIOBJ h)
{
    GObj *o = h;
    if (!o) return 0;
    if (!g_stock_ready) stock_init();
    if ((o >= g_pool && o < g_pool + POOL && o->used) || (o >= g_stock && o < g_stock + 20 && o->kind)) {
        if (o->view24) dib24_sync(o);
        return o;
    }
    return 0;
}

/* A 24-bit DIB section: what the program wrote to its bits since the last
 * sync goes to the 32-bit pixels, then the 32-bit pixels (with what gdi32
 * drew) go back to the program's bits.  Rows are in the same order in both. */
void dib24_sync(void *bitmap)
{
    GObj *o = bitmap;
    if (!(o >= g_pool && o < g_pool + POOL && o->used) || o->kind != K_BITMAP || !o->view24) return;
    int w = o->bw, h = o->bh, st = o->stride24;
    for (int y = 0; y < h; y++) {
        BYTE *v = o->view24 + (size_t)y * st, *l = o->last24 + (size_t)y * st;
        DWORD *p = o->bits + (size_t)y * w;
        if (memcmp(v, l, (size_t)w * 3)) {
            for (int x = 0; x < w; x++) {
                if (v[3 * x] == l[3 * x] && v[3 * x + 1] == l[3 * x + 1] && v[3 * x + 2] == l[3 * x + 2]) continue;
                p[x] = 0xFF000000u | (DWORD)v[3 * x + 2] << 16 | (DWORD)v[3 * x + 1] << 8 | v[3 * x];
            }
        }
        for (int x = 0; x < w; x++) {
            DWORD c = p[x];
            v[3 * x] = l[3 * x] = (BYTE)c;
            v[3 * x + 1] = l[3 * x + 1] = (BYTE)(c >> 8);
            v[3 * x + 2] = l[3 * x + 2] = (BYTE)(c >> 16);
        }
    }
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
GDIAPI int SetGraphicsMode(HDC h, int m) { (void)h; (void)m; return 1; }

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

void fill(NOVA_DC *d, int x0, int y0, int x1, int y1, COLORREF c)
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
    GObj *o = make_bitmap(w, hh, 1, bh->biHeight > 0, 0);
    if (o && bh->biBitCount == 24) {
        /* gdi32 draws on 32-bit pixels; the program sees 24-bit rows of its
         * own (in the section it passed, if any), synced at each use */
        int stride = ((w * 3) + 3) & ~3;
        SIZE_T size = (SIZE_T)stride * hh;
        BYTE *v = NULL;
        if (section) v = MapViewOfFile(section, FILE_MAP_WRITE, 0, offset, size);
        else {
            v = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            o->view_owned = 1;
        }
        BYTE *last = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!v || !last) {
            if (v && o->view_owned) VirtualFree(v, 0, MEM_RELEASE);
            else if (v) UnmapViewOfFile(v);
            if (last) VirtualFree(last, 0, MEM_RELEASE);
            DeleteObject((HGDIOBJ)o);
            if (bits) *bits = 0;
            return 0;
        }
        o->view24 = v;
        o->last24 = last;
        o->stride24 = stride;
        o->bpp = 24;
        memset(o->bits, 0, (size_t)w * hh * 4);
        for (int i = 0; i < w * hh; i++) o->bits[i] = 0xFF000000u;
        dib24_sync(o);
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

static void flush_window(NOVA_DC *d, int x, int y, int w, int h);

/* An unscaled copy between two 32-bit DCs, a row at a time (presenting
 * frames: Vulkan's software swap chain blits its DIB to the window) */
static void blit_fast(NOVA_DC *dd, int x, int y, int w, int h, NOVA_DC *sd, int sx, int sy)
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

/* Can blit() take the row-copy path? */
static int can_fast(NOVA_DC *dd, int w, int h, NOVA_DC *sd, int sw, int sh, DWORD rop)
{
    return sd && dd->bits && sd->bits && sd->bits != dd->bits && w > 0 && h > 0 && w == sw && h == sh && rop == SRCCOPY && !dd->rop2;
}

GDIAPI BOOL BitBlt(HDC dst, int x, int y, int w, int hh, HDC src, int sx, int sy, DWORD rop)
{
    NOVA_DC *dd = dc_of(dst);
    if (!dd) return FALSE;
    if (rop == BLACKNESS || rop == WHITENESS || rop == PATCOPY || !src) {
        fill(dd, x, y, x + w, y + hh, rop == WHITENESS ? 0xFFFFFF : rop == PATCOPY ? dd->brush_color : 0);
        return TRUE;
    }
    NOVA_DC *sd = dc_of(src);
    if (can_fast(dd, w, hh, sd, w, hh, rop)) {
        blit_fast(dd, x, y, w, hh, sd, sx, sy);
        flush_window(dd, x, y, w, hh);
        return TRUE;
    }
    blit(dd, x, y, w, hh, sd, sx, sy, w, hh);
    return TRUE;
}

GDIAPI BOOL StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    NOVA_DC *dd = dc_of(dst);
    if (!dd) return FALSE;
    if (!src) return PatBlt(dst, x, y, w, h, rop);
    NOVA_DC *sd = dc_of(src);
    if (can_fast(dd, w, h, sd, sw, sh, rop)) blit_fast(dd, x, y, w, h, sd, sx, sy);
    else blit(dd, x, y, w, h, sd, sx, sy, sw, sh);
    flush_window(dd, x, y, w, h);
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
static int blit_dib32(NOVA_DC *d, int x, int y, int w, int h, int sx, int sy, const void *bits, const BITMAPINFO *bi)
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

GDIAPI int StretchDIBits(HDC h, int x, int y, int w, int hh, int sx, int sy, int sw, int sh, const void *bits,
                         const BITMAPINFO *bi, UINT usage, DWORD rop)
{
    (void)usage;
    NOVA_DC *d = dc_of(h);
    if (!d || !bits || !w || !hh || !sw || !sh) return 0;
    const BITMAPINFOHEADER *bih = &bi->bmiHeader;
    if (w > 0 && hh > 0 && w == sw && hh == sh && bih->biBitCount == 32 && rop == SRCCOPY && !d->rop2 &&
        (bih->biCompression == BI_RGB || bih->biCompression == 3 /* BI_BITFIELDS, the usual masks */) && d->bits) {
        blit_dib32(d, x, y, w, hh, sx, sy, bits, bi);
        flush_window(d, x, y, w, hh);
        return hh;
    }
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
            if (bh->biBitCount == 32) q[3] = o->fmt ? (BYTE)(p >> 24) : 0;   /* a DIB section keeps its alpha */
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
    case K_REGION:    return (HGDIOBJ)(ULONG_PTR)(ULONG)SelectClipRgn(h, (HRGN)obj);
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
        if (o->view24) { b.bmWidthBytes = o->stride24; b.bmBitsPixel = 24; b.bmBits = o->view24; }
        int k = n < (int)sizeof(b) ? n : (int)sizeof(b);
        memcpy(out, &b, (size_t)k);
        if (n >= (int)sizeof(DIBSECTION) && o->fmt) {
            DIBSECTION *ds = out;
            memset(&ds->dsBmih, 0, sizeof(ds->dsBmih));
            ds->dsBmih.biSize = sizeof(BITMAPINFOHEADER);
            ds->dsBmih.biWidth = o->bw; ds->dsBmih.biHeight = o->flip ? o->bh : -o->bh;
            ds->dsBmih.biPlanes = 1; ds->dsBmih.biBitCount = o->view24 ? 24 : 32;
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
        if (o->kind == K_BITMAP && o->view24) {
            if (o->view_owned) VirtualFree(o->view24, 0, MEM_RELEASE);
            else UnmapViewOfFile(o->view24);
            VirtualFree(o->last24, 0, MEM_RELEASE);
            o->view24 = o->last24 = NULL;
            o->view_owned = 0;
        }
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

/* drawing is done at once; GdiFlush brings it to the 24-bit DIB sections' bits */
GDIAPI BOOL GdiFlush(void)
{
    for (int i = 0; i < POOL; i++) if (g_pool[i].used && g_pool[i].view24) dib24_sync(&g_pool[i]);
    return TRUE;
}
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
/* The clip region is kept as its bounding box, in device pixels */
GDIAPI int ExtSelectClipRgn(HDC h, HRGN rgn, int mode)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0;
    GObj *o = obj_of(rgn);
    if (!o) {                                               /* NULL: no clipping (RGN_COPY) */
        if (mode == 5 || !rgn) d->has_clip = 0;
        return 2;
    }
    RECT r = o->rc;                                         /* device units from the window's origin */
    r.left += d->base_x; r.right += d->base_x; r.top += d->base_y; r.bottom += d->base_y;
    if (mode == 1 && d->has_clip) {                         /* RGN_AND */
        if (d->clip.left > r.left) r.left = d->clip.left;
        if (d->clip.top > r.top) r.top = d->clip.top;
        if (d->clip.right < r.right) r.right = d->clip.right;
        if (d->clip.bottom < r.bottom) r.bottom = d->clip.bottom;
    } else if (mode == 2 && d->has_clip) {                  /* RGN_OR: the bounding box */
        if (d->clip.left < r.left) r.left = d->clip.left;
        if (d->clip.top < r.top) r.top = d->clip.top;
        if (d->clip.right > r.right) r.right = d->clip.right;
        if (d->clip.bottom > r.bottom) r.bottom = d->clip.bottom;
    } else if (mode == 4 && d->has_clip) {                  /* RGN_DIFF: only whole strips come off */
        RECT c = d->clip;
        if (r.top <= c.top && r.bottom >= c.bottom) { if (r.left <= c.left && r.right > c.left) c.left = r.right; else if (r.right >= c.right && r.left < c.right) c.right = r.left; }
        else if (r.left <= c.left && r.right >= c.right) { if (r.top <= c.top && r.bottom > c.top) c.top = r.bottom; else if (r.bottom >= c.bottom && r.top < c.bottom) c.bottom = r.top; }
        r = c;
    }
    d->clip = r;
    d->has_clip = 1;
    return r.right > r.left && r.bottom > r.top ? 2 : 1;
}
GDIAPI int SelectClipRgn(HDC h, HRGN r) { return ExtSelectClipRgn(h, r, 5 /* RGN_COPY */); }
GDIAPI int IntersectClipRect(HDC h, int l, int t, int r, int b)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0;
    RECT n = { l + d->org_x, t + d->org_y, r + d->org_x, b + d->org_y };
    if (d->has_clip) {
        if (d->clip.left > n.left) n.left = d->clip.left;
        if (d->clip.top > n.top) n.top = d->clip.top;
        if (d->clip.right < n.right) n.right = d->clip.right;
        if (d->clip.bottom < n.bottom) n.bottom = d->clip.bottom;
    }
    d->clip = n;
    d->has_clip = 1;
    return n.right > n.left && n.bottom > n.top ? 2 : 1;
}
GDIAPI int ExcludeClipRect(HDC h, int l, int t, int r, int b)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0;
    if (!d->has_clip) { d->clip.left = 0; d->clip.top = 0; d->clip.right = d->w; d->clip.bottom = d->h; d->has_clip = 1; }
    GObj tmp;
    memset(&tmp, 0, sizeof(tmp));
    RECT x = { l + d->org_x, t + d->org_y, r + d->org_x, b + d->org_y };
    RECT c = d->clip;
    if (x.top <= c.top && x.bottom >= c.bottom) { if (x.left <= c.left && x.right > c.left) c.left = x.right; else if (x.right >= c.right && x.left < c.right) c.right = x.left; }
    else if (x.left <= c.left && x.right >= c.right) { if (x.top <= c.top && x.bottom > c.top) c.top = x.bottom; else if (x.bottom >= c.bottom && x.top < c.bottom) c.bottom = x.top; }
    d->clip = c;
    return 2;
}
GDIAPI int OffsetClipRgn(HDC h, int x, int y)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0;
    if (d->has_clip) { d->clip.left += x; d->clip.right += x; d->clip.top += y; d->clip.bottom += y; }
    return 2;
}
GDIAPI int GetClipBox(HDC h, LPRECT r)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return 0;
    RECT e = { 0, 0, d->w, d->h };
    int k = dev_clip(d, &e);
    r->left = e.left - d->org_x; r->top = e.top - d->org_y; r->right = e.right - d->org_x; r->bottom = e.bottom - d->org_y;
    return k ? 2 : 1;
}
GDIAPI int GetClipRgn(HDC h, HRGN r)
{
    NOVA_DC *d = dc_of(h);
    GObj *o = obj_of(r);
    if (!d || !o) return -1;
    if (!d->has_clip) return 0;
    o->rc = d->clip;
    o->rc.left -= d->base_x; o->rc.right -= d->base_x; o->rc.top -= d->base_y; o->rc.bottom -= d->base_y;
    return 1;
}
GDIAPI BOOL RectVisible(HDC h, const RECT *r)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return FALSE;
    RECT e = { r->left + d->org_x, r->top + d->org_y, r->right + d->org_x, r->bottom + d->org_y };
    return dev_clip(d, &e);
}
GDIAPI BOOL PtVisible(HDC h, int x, int y) { NOVA_DC *d = dc_of(h); return d && dev_visible(d, x + d->org_x, y + d->org_y); }
GDIAPI BOOL FillRgn(HDC h, HRGN r, HBRUSH b)
{
    NOVA_DC *d = dc_of(h);
    GObj *o = obj_of(r), *br = obj_of(b);
    if (!d || !o) return FALSE;
    fill(d, o->rc.left, o->rc.top, o->rc.right, o->rc.bottom, br ? br->color : 0);
    return TRUE;
}
GDIAPI BOOL PaintRgn(HDC h, HRGN r) { NOVA_DC *d = dc_of(h); GObj *o = obj_of(r); if (!d || !o) return FALSE; fill(d, o->rc.left, o->rc.top, o->rc.right, o->rc.bottom, d->brush_color); return TRUE; }

