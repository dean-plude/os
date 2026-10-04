/*
 * shapes.c — more of gdi32's drawing: arcs, pies, Bézier curves, several
 * polygons at once, flood fills, masked blits, polygon regions; logical
 * palettes (every surface is 32-bit, so a palette only answers queries);
 * and enhanced metafiles, which cannot be recorded or played here (the
 * recording DC draws into a 1x1 memory surface and closing it yields no
 * metafile).
 */
#define NOVA_BUILD_GDI32
#include <windows.h>
#include "gdi_int.h"
#ifndef CLR_INVALID
#define CLR_INVALID 0xFFFFFFFF
#endif

GDIAPI BOOL LineTo(HDC h, int x, int y);
GDIAPI BOOL MoveToEx(HDC h, int x, int y, LPPOINT old);
GDIAPI BOOL BitBlt(HDC dst, int x, int y, int w, int hh, HDC src, int sx, int sy, DWORD rop);
GDIAPI COLORREF GetPixel(HDC h, int x, int y);
GDIAPI HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits);
GDIAPI HRGN CreateRectRgn(int l, int t, int r, int b);
GDIAPI HDC CreateCompatibleDC(HDC h);
GDIAPI BOOL DeleteDC(HDC h);

/* -----------------------------------------------------------------------
 * Arcs: the ellipse's boundary is walked counter-clockwise in @steps
 * points (a rotation per step, its sine and cosine from their series), and
 * the part between the radial through (x1, y1) and the one through
 * (x2, y2) is kept.
 * ----------------------------------------------------------------------- */
static int arc_points(int l, int t, int r, int b, int x1, int y1, int x2, int y2, POINT *out, int cap)
{
    double ax = (r - l) / 2.0, ay = (b - t) / 2.0, cx = (l + r) / 2.0, cy = (t + b) / 2.0;
    if (ax <= 0 || ay <= 0) return 0;
    int steps = (int)(2 * (ax + ay));
    if (steps < 32) steps = 32;
    if (steps > cap) steps = cap;
    double th = 6.283185307179586 / steps, th2 = th * th;
    double c = 1 - th2 / 2 + th2 * th2 / 24 - th2 * th2 * th2 / 720;
    double s = th - th * th2 / 6 + th * th2 * th2 / 120 - th * th2 * th2 * th2 / 5040;
    /* the directions of the two radials, in the circle the ellipse is a stretch of */
    double dx1 = (x1 - cx) / ax, dy1 = -(y1 - cy) / ay, dx2 = (x2 - cx) / ax, dy2 = -(y2 - cy) / ay;
    int i1 = -1, i2 = -1;
    double best1 = -1e9, best2 = -1e9, ux = 1, uy = 0;
    for (int i = 0; i < steps; i++) {
        double d1 = ux * dx1 + uy * dy1, d2 = ux * dx2 + uy * dy2;
        if (d1 > best1) { best1 = d1; i1 = i; }
        if (d2 > best2) { best2 = d2; i2 = i; }
        double nx = ux * c - uy * s; uy = ux * s + uy * c; ux = nx;
    }
    if (i1 < 0) return 0;
    if (dx1 == 0 && dy1 == 0) i1 = 0;
    if (dx2 == 0 && dy2 == 0) i2 = i1;
    int n = i2 >= i1 ? i2 - i1 : steps - i1 + i2;
    if (n == 0) n = steps;                                   /* the same radial twice: the whole ellipse */
    ux = 1; uy = 0;
    for (int i = 0; i < i1; i++) { double nx = ux * c - uy * s; uy = ux * s + uy * c; ux = nx; }
    int k = 0;
    for (int i = 0; i <= n && k < cap; i++) {
        out[k].x = (LONG)(cx + ux * ax + (ux >= 0 ? 0.5 : -0.5));
        out[k].y = (LONG)(cy - uy * ay + (uy <= 0 ? 0.5 : -0.5));
        k++;
        double nx = ux * c - uy * s; uy = ux * s + uy * c; ux = nx;
    }
    return k;
}

#define ARC_CAP 4096
static POINT *arc_buf(void) { return HeapAlloc(GetProcessHeap(), 0, ARC_CAP * sizeof(POINT)); }


GDIAPI BOOL ArcTo(HDC h, int l, int t, int r, int b, int x1, int y1, int x2, int y2)
{
    NOVA_DC *d = dc_of(h); if (!d) return FALSE;
    POINT *pt = arc_buf(); if (!pt) return FALSE;
    int n = d->arc_dir == 2 ? arc_points(l, t, r, b, x2, y2, x1, y1, pt, ARC_CAP)
                            : arc_points(l, t, r, b, x1, y1, x2, y2, pt, ARC_CAP);
    if (d->arc_dir == 2)                                    /* clockwise: drawn from the start ray */
        for (int i = 0, j = n - 1; i < j; i++, j--) { POINT tp = pt[i]; pt[i] = pt[j]; pt[j] = tp; }
    if (n) {
        LineTo(h, pt[0].x, pt[0].y);
        for (int i = 1; i < n; i++) LineTo(h, pt[i].x, pt[i].y);
    }
    HeapFree(GetProcessHeap(), 0, pt);
    return TRUE;
}


/* The direction Arc, ArcTo, Pie and Chord draw in: AD_COUNTERCLOCKWISE (1,
 * the default) or AD_CLOCKWISE (2); returns the old one */
GDIAPI int SetArcDirection(HDC h, int dir)
{
    NOVA_DC *d = dc_of(h);
    if (!d || (dir != 1 && dir != 2)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    int old = d->arc_dir == 2 ? 2 : 1;
    d->arc_dir = dir == 2 ? 2 : 0;
    return old;
}
GDIAPI int GetArcDirection(HDC h) { NOVA_DC *d = dc_of(h); return !d ? 0 : d->arc_dir == 2 ? 2 : 1; }

/* Drawing is synchronous here, so there is never an operation in progress
 * on another thread to cancel */
GDIAPI BOOL CancelDC(HDC h) { return dc_of(h) != 0; }


/* Cubic Bézier curves, each flattened to 16 segments */
static void bezier(NOVA_DC *d, const POINT *p, int move_first)
{
    int px = p[0].x, py = p[0].y;
    for (int i = 1; i <= 16; i++) {
        double u = i / 16.0, v = 1 - u;
        double x = v * v * v * p[0].x + 3 * v * v * u * p[1].x + 3 * v * u * u * p[2].x + u * u * u * p[3].x;
        double y = v * v * v * p[0].y + 3 * v * v * u * p[1].y + 3 * v * u * u * p[2].y + u * u * u * p[3].y;
        int nx = (int)(x + (x >= 0 ? 0.5 : -0.5)), ny = (int)(y + (y >= 0 ? 0.5 : -0.5));
        if (d->has_pen) line(d, px, py, nx, ny, d->pen_color, d->pen_width);
        px = nx; py = ny;
    }
    (void)move_first;
    d->cx = px; d->cy = py;
}

GDIAPI BOOL PolyBezier(HDC h, const POINT *p, DWORD n)
{
    NOVA_DC *d = dc_of(h); if (!d || n < 4) return FALSE;
    int cx = d->cx, cy = d->cy;
    for (DWORD i = 0; i + 3 < n; i += 3) bezier(d, p + i, 0);
    d->cx = cx; d->cy = cy;                                  /* PolyBezier leaves the current position alone */
    return TRUE;
}

GDIAPI BOOL PolyBezierTo(HDC h, const POINT *p, DWORD n)
{
    NOVA_DC *d = dc_of(h); if (!d || n < 3) return FALSE;
    for (DWORD i = 0; i + 2 < n; i += 3) {
        POINT q[4] = { { d->cx, d->cy }, p[i], p[i + 1], p[i + 2] };
        bezier(d, q, 1);
    }
    return TRUE;
}

GDIAPI BOOL PolyPolygon(HDC h, const POINT *pt, const INT *counts, int n)
{
    NOVA_DC *d = dc_of(h); if (!d || !pt || !counts) return FALSE;
    for (int i = 0; i < n; i++) {
        int c = counts[i];
        if (c >= 2) {
            if (d->has_brush && c >= 3) fill_polygon(d, pt, c, d->brush_color);
            if (d->has_pen) for (int k = 0; k < c; k++) line(d, pt[k].x, pt[k].y, pt[(k + 1) % c].x, pt[(k + 1) % c].y, d->pen_color, d->pen_width);
        }
        pt += c;
    }
    return TRUE;
}

GDIAPI BOOL PolyPolyline(HDC h, const POINT *pt, const DWORD *counts, DWORD n)
{
    NOVA_DC *d = dc_of(h); if (!d || !pt || !counts) return FALSE;
    for (DWORD i = 0; i < n; i++) {
        DWORD c = counts[i];
        if (d->has_pen) for (DWORD k = 1; k < c; k++) line(d, pt[k - 1].x, pt[k - 1].y, pt[k].x, pt[k].y, d->pen_color, d->pen_width);
        pt += c;
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Flood fill: scanline spans from (x, y), bounded by the colour @c
 * (FLOODFILLBORDER) or over the pixels of colour @c (FLOODFILLSURFACE)
 * ----------------------------------------------------------------------- */
GDIAPI BOOL ExtFloodFill(HDC h, int x, int y, COLORREF c, UINT type)
{
    NOVA_DC *d = dc_of(h); if (!d || !d->has_brush) return FALSE;
    c &= 0xFFFFFF;
    COLORREF start = GetPixel(h, x, y);
    if (start == CLR_INVALID) return FALSE;
    int surface = type == 1 /* FLOODFILLSURFACE */;
    if (surface ? start != c : start == c) return FALSE;
    COLORREF fillc = d->brush_color & 0xFFFFFF;
    if (surface && fillc == c) return TRUE;
    if (!surface && fillc == start) return TRUE;             /* already that colour: nothing to tell apart */
#define INSIDE(px, py) ({ COLORREF q = GetPixel(h, (px), (py)); q != CLR_INVALID && (surface ? q == c : q != c && q != fillc); })
    int cap = 4096, n = 0;
    POINT *st = HeapAlloc(GetProcessHeap(), 0, cap * sizeof(POINT));
    if (!st) return FALSE;
    st[n++] = (POINT){ x, y };
    while (n) {
        POINT p = st[--n];
        if (!INSIDE(p.x, p.y)) continue;
        int x0 = p.x, x1 = p.x;
        while (INSIDE(x0 - 1, p.y)) x0--;
        while (INSIDE(x1 + 1, p.y)) x1++;
        fill(d, x0, p.y, x1 + 1, p.y + 1, fillc);
        for (int dy = -1; dy <= 1; dy += 2) {
            int in = 0;
            for (int px = x0; px <= x1; px++) {
                int i = INSIDE(px, p.y + dy);
                if (i && !in) {
                    if (n == cap) {
                        POINT *ns = HeapReAlloc(GetProcessHeap(), 0, st, 2 * cap * sizeof(POINT));
                        if (!ns) { HeapFree(GetProcessHeap(), 0, st); return FALSE; }
                        st = ns; cap *= 2;
                    }
                    st[n++] = (POINT){ px, p.y + dy };
                }
                in = i;
            }
        }
    }
#undef INSIDE
    HeapFree(GetProcessHeap(), 0, st);
    return TRUE;
}

GDIAPI BOOL FloodFill(HDC h, int x, int y, COLORREF c) { return ExtFloodFill(h, x, y, c, 0 /* FLOODFILLBORDER */); }

/* -----------------------------------------------------------------------
 * MaskBlt: where the mask bitmap is set, the foreground ROP's source pixel
 * (SRCCOPY); elsewhere the destination stays (the background ROP is taken
 * as a no-op).  Without a mask it is BitBlt.
 * ----------------------------------------------------------------------- */
GDIAPI BOOL MaskBlt(HDC dst, int x, int y, int w, int hh, HDC src, int sx, int sy, HBITMAP mask, int mx, int my, DWORD rop)
{
    GObj *m = obj_of(mask);
    if (!m || m->kind != K_BITMAP || !m->bits) return BitBlt(dst, x, y, w, hh, src, sx, sy, rop & 0x00FFFFFF);   /* the foreground ROP3 */
    NOVA_DC *dd = dc_of(dst), *sd = dc_of(src);
    if (!dd) return FALSE;
    dc_sync(dd);
    dc_sync(sd);
    for (int j = 0; j < hh; j++) for (int i = 0; i < w; i++) {
        int bx = mx + i, by = my + j;
        if (bx < 0 || by < 0 || bx >= m->bw || by >= m->bh) continue;
        int row = m->flip ? m->bh - 1 - by : by;
        if (!(m->bits[(size_t)row * m->bw + bx] & 0xFFFFFF)) continue;
        COLORREF c = dd->brush_color;
        if (sd) {
            int px = sx + i + sd->org_x, py = sy + j + sd->org_y;
            if (!sd->bits || !dev_visible(sd, px, py)) continue;
            c = from_native(sd, *pixel_at(sd, px, py));
        }
        put(dd, x + i, y + j, c);
    }
    dc_sync(dd);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Regions (bounding rectangles, like the rest of gdi32's regions)
 * ----------------------------------------------------------------------- */
GDIAPI HRGN CreatePolygonRgn(const POINT *pt, int n, int mode)
{
    (void)mode;
    if (!pt || n < 1) return NULL;
    LONG l = pt[0].x, t = pt[0].y, r = pt[0].x, b = pt[0].y;
    for (int i = 1; i < n; i++) {
        if (pt[i].x < l) l = pt[i].x; if (pt[i].x > r) r = pt[i].x;
        if (pt[i].y < t) t = pt[i].y; if (pt[i].y > b) b = pt[i].y;
    }
    return CreateRectRgn(l, t, r, b);
}

GDIAPI HRGN CreatePolyPolygonRgn(const POINT *pt, const INT *counts, int n, int mode)
{
    int total = 0;
    for (int i = 0; i < n; i++) total += counts[i];
    return CreatePolygonRgn(pt, total, mode);
}



/* -----------------------------------------------------------------------
 * Mapping (MM_TEXT: the extents are 1:1), layout (left to right)
 * ----------------------------------------------------------------------- */
GDIAPI BOOL SetViewportExtEx(HDC h, int x, int y, LPSIZE old) { (void)x; (void)y; if (!dc_of(h)) return FALSE; if (old) { old->cx = old->cy = 1; } return TRUE; }
GDIAPI BOOL SetWindowExtEx(HDC h, int x, int y, LPSIZE old) { (void)x; (void)y; if (!dc_of(h)) return FALSE; if (old) { old->cx = old->cy = 1; } return TRUE; }
GDIAPI BOOL ScaleViewportExtEx(HDC h, int xn, int xd, int yn, int yd, LPSIZE old) { (void)xn; (void)xd; (void)yn; (void)yd; return SetViewportExtEx(h, 1, 1, old); }
GDIAPI BOOL ScaleWindowExtEx(HDC h, int xn, int xd, int yn, int yd, LPSIZE old) { (void)xn; (void)xd; (void)yn; (void)yd; return SetWindowExtEx(h, 1, 1, old); }
GDIAPI DWORD GetLayout(HDC h) { return dc_of(h) ? 0 : GDI_ERROR; }


/* -----------------------------------------------------------------------
 * Palettes.  Entries are PALETTEENTRYs packed in DWORDs (red in the low
 * byte); a DC's selected palette is not recorded, since nothing is drawn
 * through one.
 * ----------------------------------------------------------------------- */
#define PE(r, g, b) ((DWORD)(r) | (DWORD)(g) << 8 | (DWORD)(b) << 16)
const DWORD g_default_palette[20] = {
    PE(0, 0, 0), PE(0x80, 0, 0), PE(0, 0x80, 0), PE(0x80, 0x80, 0), PE(0, 0, 0x80), PE(0x80, 0, 0x80), PE(0, 0x80, 0x80), PE(0xC0, 0xC0, 0xC0),
    PE(0xC0, 0xDC, 0xC0), PE(0xA6, 0xCA, 0xF0), PE(0xFF, 0xFB, 0xF0), PE(0xA0, 0xA0, 0xA4),
    PE(0x80, 0x80, 0x80), PE(0xFF, 0, 0), PE(0, 0xFF, 0), PE(0xFF, 0xFF, 0), PE(0, 0, 0xFF), PE(0xFF, 0, 0xFF), PE(0, 0xFF, 0xFF), PE(0xFF, 0xFF, 0xFF),
};

typedef struct { WORD palVersion, palNumEntries; DWORD palPalEntry[1]; } NOVA_LOGPALETTE;

GDIAPI HPALETTE CreatePalette(const void *lp)
{
    const NOVA_LOGPALETTE *p = lp;
    if (!p) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    GObj *o = new_obj(K_PALETTE);
    if (!o) return NULL;
    o->bw = p->palNumEntries;
    o->bits = HeapAlloc(GetProcessHeap(), 0, (o->bw ? o->bw : 1) * sizeof(DWORD));
    if (!o->bits) { o->used = 0; return NULL; }
    memcpy(o->bits, p->palPalEntry, o->bw * sizeof(DWORD));
    return (HPALETTE)o;
}

GDIAPI HPALETTE SelectPalette(HDC h, HPALETTE pal, BOOL force)
{
    (void)force;
    GObj *o = obj_of(pal);
    if (!dc_of(h) || !o || o->kind != K_PALETTE) return NULL;
    return (HPALETTE)GetStockObject(DEFAULT_PALETTE);
}

GDIAPI UINT RealizePalette(HDC h) { return dc_of(h) ? 0 : GDI_ERROR; }
GDIAPI BOOL UnrealizeObject(HGDIOBJ o) { return obj_of(o) != NULL; }
GDIAPI BOOL UpdateColors(HDC h) { return dc_of(h) != NULL; }

GDIAPI UINT GetPaletteEntries(HPALETTE pal, UINT first, UINT n, void *out)
{
    GObj *o = obj_of(pal);
    if (!o || o->kind != K_PALETTE) return 0;
    if (!out) return o->bw;
    if (first >= (UINT)o->bw) return 0;
    if (first + n > (UINT)o->bw) n = o->bw - first;
    memcpy(out, o->bits + first, n * sizeof(DWORD));
    return n;
}

GDIAPI UINT SetPaletteEntries(HPALETTE pal, UINT first, UINT n, const void *in)
{
    GObj *o = obj_of(pal);
    if (!o || o->kind != K_PALETTE || !in || first >= (UINT)o->bw) return 0;
    if (first + n > (UINT)o->bw) n = o->bw - first;
    memcpy(o->bits + first, in, n * sizeof(DWORD));
    return n;
}

GDIAPI UINT GetSystemPaletteEntries(HDC h, UINT first, UINT n, void *out)
{
    if (!dc_of(h)) return 0;
    if (!out) return 20;
    if (first >= 20) return 0;
    if (first + n > 20) n = 20 - first;
    memcpy(out, g_default_palette + first, n * sizeof(DWORD));
    return n;
}

GDIAPI UINT GetSystemPaletteUse(HDC h) { return dc_of(h) ? 1 /* SYSPAL_STATIC */ : 0; }
GDIAPI UINT SetSystemPaletteUse(HDC h, UINT use) { (void)use; return dc_of(h) ? 1 : 0; }

GDIAPI UINT GetNearestPaletteIndex(HPALETTE pal, COLORREF c)
{
    GObj *o = obj_of(pal);
    if (!o || o->kind != K_PALETTE || !o->bw) return CLR_INVALID;
    UINT best = 0; long bd = 1L << 30;
    for (int i = 0; i < o->bw; i++) {
        long dr = (long)(o->bits[i] & 0xFF) - GetRValue(c), dg = (long)(o->bits[i] >> 8 & 0xFF) - GetGValue(c), db = (long)(o->bits[i] >> 16 & 0xFF) - GetBValue(c);
        long dist = dr * dr + dg * dg + db * db;
        if (dist < bd) { bd = dist; best = i; }
    }
    return best;
}

GDIAPI BOOL ResizePalette(HPALETTE pal, UINT n)
{
    GObj *o = obj_of(pal);
    if (!o || o->kind != K_PALETTE) return FALSE;
    DWORD *nb = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, o->bits, (n ? n : 1) * sizeof(DWORD));
    if (!nb) return FALSE;
    o->bits = nb; o->bw = n;
    return TRUE;
}

GDIAPI BOOL AnimatePalette(HPALETTE pal, UINT first, UINT n, const void *in) { return SetPaletteEntries(pal, first, n, in) == n; }

/* -----------------------------------------------------------------------
 * Metafiles: not recorded.  The recording DC draws into a memory DC, and
 * closing it gives no metafile (ERROR_NOT_SUPPORTED), as does reading or
 * playing one.
 * ----------------------------------------------------------------------- */
GDIAPI HDC CreateEnhMetaFileW(HDC ref, LPCWSTR file, const RECT *rc, LPCWSTR desc) { (void)file; (void)rc; (void)desc; return CreateCompatibleDC(ref); }
GDIAPI HDC CreateEnhMetaFileA(HDC ref, LPCSTR file, const RECT *rc, LPCSTR desc) { (void)file; (void)rc; (void)desc; return CreateCompatibleDC(ref); }
GDIAPI HDC CreateMetaFileW(LPCWSTR file) { (void)file; return CreateCompatibleDC(0); }
GDIAPI HDC CreateMetaFileA(LPCSTR file) { (void)file; return CreateCompatibleDC(0); }
GDIAPI HANDLE CloseEnhMetaFile(HDC h) { if (h) DeleteDC(h); SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
GDIAPI HANDLE CloseMetaFile(HDC h) { return CloseEnhMetaFile(h); }
GDIAPI BOOL DeleteEnhMetaFile(HANDLE h) { return h != NULL; }
GDIAPI BOOL DeleteMetaFile(HANDLE h) { return h != NULL; }
GDIAPI HANDLE CopyEnhMetaFileW(HANDLE h, LPCWSTR file) { (void)h; (void)file; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
GDIAPI HANDLE CopyEnhMetaFileA(HANDLE h, LPCSTR file) { (void)h; (void)file; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
GDIAPI HANDLE CopyMetaFileW(HANDLE h, LPCWSTR file) { (void)h; (void)file; SetLastError(ERROR_INVALID_HANDLE); return NULL; }
GDIAPI HANDLE GetEnhMetaFileW(LPCWSTR file) { (void)file; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
GDIAPI HANDLE GetEnhMetaFileA(LPCSTR file) { (void)file; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
GDIAPI HANDLE GetMetaFileW(LPCWSTR file) { (void)file; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
GDIAPI UINT GetEnhMetaFileHeader(HANDLE h, UINT n, void *out) { (void)h; (void)n; (void)out; return 0; }
GDIAPI UINT GetEnhMetaFileBits(HANDLE h, UINT n, void *out) { (void)h; (void)n; (void)out; return 0; }
GDIAPI UINT GetEnhMetaFileDescriptionW(HANDLE h, UINT n, LPWSTR out) { (void)h; (void)n; (void)out; return 0; }
GDIAPI UINT GetEnhMetaFilePaletteEntries(HANDLE h, UINT n, void *out) { (void)h; (void)n; (void)out; return 0; }
GDIAPI HANDLE SetEnhMetaFileBits(UINT n, const BYTE *bits) { (void)n; (void)bits; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
GDIAPI UINT GetMetaFileBitsEx(HANDLE h, UINT n, void *out) { (void)h; (void)n; (void)out; return 0; }
GDIAPI HANDLE SetMetaFileBitsEx(UINT n, const BYTE *bits) { (void)n; (void)bits; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
GDIAPI UINT GetWinMetaFileBits(HANDLE h, UINT n, BYTE *out, INT map, HDC ref) { (void)h; (void)n; (void)out; (void)map; (void)ref; return 0; }
GDIAPI HANDLE SetWinMetaFileBits(UINT n, const BYTE *bits, HDC ref, const void *pict) { (void)n; (void)bits; (void)ref; (void)pict; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
GDIAPI BOOL PlayEnhMetaFile(HDC h, HANDLE mf, const RECT *rc) { (void)h; (void)mf; (void)rc; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
GDIAPI BOOL PlayMetaFile(HDC h, HANDLE mf) { (void)h; (void)mf; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
GDIAPI BOOL PlayEnhMetaFileRecord(HDC h, void *table, const void *rec, UINT n) { (void)h; (void)table; (void)rec; (void)n; return FALSE; }
GDIAPI BOOL EnumEnhMetaFile(HDC h, HANDLE mf, void *proc, LPVOID data, const RECT *rc) { (void)h; (void)mf; (void)proc; (void)data; (void)rc; return FALSE; }
GDIAPI BOOL GdiComment(HDC h, UINT n, const BYTE *data) { (void)n; (void)data; return dc_of(h) != NULL; }
