/*
 * extra.c — more of gdi32: coordinate conversion, pattern brushes,
 * gradient fills, the Gdi* names msimg32 forwards to, and printing (there
 * are no printers, so a print job cannot start).
 */
#define NOVA_BUILD_GDI32
#include <windows.h>
#include "gdi_int.h"

GDIAPI BOOL SetWindowOrgEx(HDC h, int x, int y, LPPOINT old);

/* MM_TEXT: logical = device - the viewport origin */
GDIAPI BOOL LPtoDP(HDC h, LPPOINT p, int n)
{
    NOVA_DC *d = dc_of(h);
    if (!d || (!p && n)) return FALSE;
    for (int i = 0; i < n; i++) { p[i].x += d->org_x - d->base_x; p[i].y += d->org_y - d->base_y; }
    return TRUE;
}

GDIAPI BOOL DPtoLP(HDC h, LPPOINT p, int n)
{
    NOVA_DC *d = dc_of(h);
    if (!d || (!p && n)) return FALSE;
    for (int i = 0; i < n; i++) { p[i].x -= d->org_x - d->base_x; p[i].y -= d->org_y - d->base_y; }
    return TRUE;
}

GDIAPI BOOL GetWindowOrgEx(HDC h, LPPOINT p)
{
    NOVA_DC *d = dc_of(h);
    if (!d || !p) return FALSE;
    p->x = d->base_x - d->org_x;
    p->y = d->base_y - d->org_y;
    return TRUE;
}

GDIAPI BOOL OffsetWindowOrgEx(HDC h, int x, int y, LPPOINT old)
{
    POINT cur;
    if (!GetWindowOrgEx(h, &cur)) return FALSE;
    if (old) *old = cur;
    return SetWindowOrgEx(h, cur.x + x, cur.y + y, NULL);
}

GDIAPI BOOL GetWindowExtEx(HDC h, LPSIZE s) { (void)h; if (!s) return FALSE; s->cx = s->cy = 1; return TRUE; }
GDIAPI BOOL GetViewportExtEx(HDC h, LPSIZE s) { (void)h; if (!s) return FALSE; s->cx = s->cy = 1; return TRUE; }

/* A pattern brush paints in the pattern's average colour (fills here are
 * solid): a hatch or dither pattern comes out as its overall tone */
GDIAPI HBRUSH CreatePatternBrush(HBITMAP bmp)
{
    GObj *b = obj_of(bmp);
    if (!b || b->kind != K_BITMAP || !b->bits || b->bw <= 0 || b->bh <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    unsigned long long r = 0, g = 0, bl = 0, n = (unsigned long long)b->bw * (unsigned long long)b->bh;
    for (unsigned long long i = 0; i < n; i++) {
        DWORD p = b->bits[i];
        COLORREF c = b->fmt ? (p >> 16 & 0xFF) | (p & 0xFF00) | (p & 0xFF) << 16 : p & 0xFFFFFF;
        r += GetRValue(c); g += GetGValue(c); bl += GetBValue(c);
    }
    return CreateSolidBrush(RGB(r / n, g / n, bl / n));
}

GDIAPI HBRUSH CreateDIBPatternBrushPt(const void *packed, UINT usage)
{
    (void)usage;
    const BITMAPINFO *bi = packed;
    if (!bi) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    const BITMAPINFOHEADER *h = &bi->bmiHeader;
    int colors = h->biBitCount <= 8 ? (h->biClrUsed ? (int)h->biClrUsed : 1 << h->biBitCount) : 0;
    const BYTE *bits = (const BYTE *)bi + h->biSize + colors * sizeof(RGBQUAD);
    int w = h->biWidth, hh = h->biHeight < 0 ? -h->biHeight : h->biHeight;
    if (w <= 0 || hh <= 0) return CreateSolidBrush(0);
    HDC dc = CreateCompatibleDC(0);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, hh);
    HGDIOBJ old = SelectObject(dc, bmp);
    SetDIBitsToDevice(dc, 0, 0, w, hh, 0, 0, 0, hh, bits, bi, DIB_RGB_COLORS);
    SelectObject(dc, old);
    DeleteDC(dc);
    HBRUSH br = CreatePatternBrush(bmp);
    DeleteObject(bmp);
    return br;
}

/* -----------------------------------------------------------------------
 * GradientFill (GRADIENT_FILL_RECT_H / _V and triangles)
 * ----------------------------------------------------------------------- */
typedef struct { LONG x, y; USHORT Red, Green, Blue, Alpha; } TRIVERTEX_;
typedef struct { ULONG UpperLeft, LowerRight; } GRADIENT_RECT_;
typedef struct { ULONG Vertex1, Vertex2, Vertex3; } GRADIENT_TRIANGLE_;

static COLORREF mix(const TRIVERTEX_ *a, const TRIVERTEX_ *b, long t, long n)
{
    if (n <= 0) n = 1;
    long r = a->Red + ((long)b->Red - a->Red) * t / n;
    long g = a->Green + ((long)b->Green - a->Green) * t / n;
    long bl = a->Blue + ((long)b->Blue - a->Blue) * t / n;
    return RGB(r >> 8, g >> 8, bl >> 8);
}

GDIAPI BOOL GdiGradientFill(HDC h, TRIVERTEX_ *v, ULONG nv, PVOID mesh, ULONG nm, ULONG mode)
{
    NOVA_DC *d = dc_of(h);
    if (!d || !v || !mesh) return FALSE;
    if (mode == 0 || mode == 1) {                           /* GRADIENT_FILL_RECT_H / _V */
        GRADIENT_RECT_ *r = mesh;
        for (ULONG i = 0; i < nm; i++) {
            if (r[i].UpperLeft >= nv || r[i].LowerRight >= nv) return FALSE;
            const TRIVERTEX_ *a = &v[r[i].UpperLeft], *b = &v[r[i].LowerRight];
            LONG x0 = a->x < b->x ? a->x : b->x, x1 = a->x < b->x ? b->x : a->x;
            LONG y0 = a->y < b->y ? a->y : b->y, y1 = a->y < b->y ? b->y : a->y;
            const TRIVERTEX_ *s = mode == 0 ? (a->x <= b->x ? a : b) : (a->y <= b->y ? a : b);
            const TRIVERTEX_ *e = s == a ? b : a;
            if (mode == 0)
                for (LONG x = x0; x < x1; x++) fill(d, x, y0, x + 1, y1, mix(s, e, x - x0, x1 - x0 - 1));
            else
                for (LONG y = y0; y < y1; y++) fill(d, x0, y, x1, y + 1, mix(s, e, y - y0, y1 - y0 - 1));
        }
        return TRUE;
    }
    if (mode == 2) {                                        /* GRADIENT_FILL_TRIANGLE */
        GRADIENT_TRIANGLE_ *t = mesh;
        for (ULONG i = 0; i < nm; i++) {
            if (t[i].Vertex1 >= nv || t[i].Vertex2 >= nv || t[i].Vertex3 >= nv) return FALSE;
            const TRIVERTEX_ *p0 = &v[t[i].Vertex1], *p1 = &v[t[i].Vertex2], *p2 = &v[t[i].Vertex3];
            long area = (long)(p1->x - p0->x) * (p2->y - p0->y) - (long)(p2->x - p0->x) * (p1->y - p0->y);
            if (!area) continue;
            LONG minx = p0->x, maxx = p0->x, miny = p0->y, maxy = p0->y;
            const TRIVERTEX_ *ps[2] = { p1, p2 };
            for (int k = 0; k < 2; k++) {
                if (ps[k]->x < minx) minx = ps[k]->x;
                if (ps[k]->x > maxx) maxx = ps[k]->x;
                if (ps[k]->y < miny) miny = ps[k]->y;
                if (ps[k]->y > maxy) maxy = ps[k]->y;
            }
            for (LONG y = miny; y < maxy; y++)
                for (LONG x = minx; x < maxx; x++) {
                    long w0 = (long)(p1->x - x) * (p2->y - y) - (long)(p2->x - x) * (p1->y - y);
                    long w1 = (long)(p2->x - x) * (p0->y - y) - (long)(p0->x - x) * (p2->y - y);
                    long w2 = area - w0 - w1;
                    if (area > 0 ? (w0 < 0 || w1 < 0 || w2 < 0) : (w0 > 0 || w1 > 0 || w2 > 0)) continue;
                    long r = (w0 * p0->Red + w1 * p1->Red + w2 * p2->Red) / area;
                    long g = (w0 * p0->Green + w1 * p1->Green + w2 * p2->Green) / area;
                    long b = (w0 * p0->Blue + w1 * p1->Blue + w2 * p2->Blue) / area;
                    put(d, x, y, RGB(r >> 8, g >> 8, b >> 8));
                }
        }
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

GDIAPI BOOL TransparentBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, UINT key);

GDIAPI BOOL GdiAlphaBlend(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION bf)
{ return AlphaBlend(dst, x, y, w, h, src, sx, sy, sw, sh, bf); }
GDIAPI BOOL GdiTransparentBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, UINT key)
{ return TransparentBlt(dst, x, y, w, h, src, sx, sy, sw, sh, key); }

/* -----------------------------------------------------------------------
 * Printing: NovaOS has no printers, so no print job starts
 * ----------------------------------------------------------------------- */
#define SP_ERROR_ (-1)
GDIAPI int StartDocW(HDC h, const void *di) { (void)h; (void)di; SetLastError(ERROR_INVALID_HANDLE); return SP_ERROR_; }
GDIAPI int StartDocA(HDC h, const void *di) { (void)h; (void)di; SetLastError(ERROR_INVALID_HANDLE); return SP_ERROR_; }
GDIAPI int EndDoc(HDC h) { (void)h; return SP_ERROR_; }
GDIAPI int AbortDoc(HDC h) { (void)h; return SP_ERROR_; }
GDIAPI int StartPage(HDC h) { (void)h; return SP_ERROR_; }
GDIAPI int EndPage(HDC h) { (void)h; return SP_ERROR_; }
GDIAPI int SetAbortProc(HDC h, PVOID proc) { (void)h; (void)proc; return SP_ERROR_; }

/* -----------------------------------------------------------------------
 * Mapping modes: MM_TEXT only, so extents stay 1:1
 * ----------------------------------------------------------------------- */
static BOOL unit_extent(LPSIZE old) { if (old) { old->cx = old->cy = 1; } return TRUE; }
GDIAPI BOOL SetViewportExtEx(HDC h, int x, int y, LPSIZE old) { (void)h; (void)x; (void)y; return unit_extent(old); }
GDIAPI BOOL SetWindowExtEx(HDC h, int x, int y, LPSIZE old) { (void)h; (void)x; (void)y; return unit_extent(old); }
GDIAPI BOOL ScaleViewportExtEx(HDC h, int xn, int xd, int yn, int yd, LPSIZE old)
{ (void)h; (void)xn; (void)xd; (void)yn; (void)yd; return unit_extent(old); }
GDIAPI BOOL ScaleWindowExtEx(HDC h, int xn, int xd, int yn, int yd, LPSIZE old)
{ (void)h; (void)xn; (void)xd; (void)yn; (void)yd; return unit_extent(old); }
GDIAPI DWORD GetLayout(HDC h) { (void)h; return 0; }                  /* left to right */
GDIAPI int GetPolyFillMode(HDC h) { (void)h; return 1; }              /* ALTERNATE */
GDIAPI int GetStretchBltMode(HDC h) { (void)h; return 1; }            /* BLACKONWHITE */

/* printer escapes: none are supported (QUERYESCSUPPORT says so) */
GDIAPI int Escape(HDC h, int code, int n, LPCSTR in, LPVOID out) { (void)h; (void)code; (void)n; (void)in; (void)out; return 0; }
GDIAPI HMETAFILE CopyMetaFileW(HMETAFILE mf, LPCWSTR file) { (void)mf; (void)file; SetLastError(ERROR_INVALID_HANDLE); return 0; }

/* Metafiles: NovaOS neither records nor plays them (Inkscape's EMF and
 * WMF import and export, GTK's printing); none can be made or opened */
static HANDLE no_metafile(void) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }
GDIAPI HDC CreateEnhMetaFileW(HDC ref, LPCWSTR file, const RECT *r, LPCWSTR desc)
{ (void)ref; (void)file; (void)r; (void)desc; return (HDC)no_metafile(); }
GDIAPI HDC CreateEnhMetaFileA(HDC ref, LPCSTR file, const RECT *r, LPCSTR desc)
{ (void)ref; (void)file; (void)r; (void)desc; return (HDC)no_metafile(); }
GDIAPI HENHMETAFILE CloseEnhMetaFile(HDC h) { (void)h; return (HENHMETAFILE)no_metafile(); }
GDIAPI HENHMETAFILE GetEnhMetaFileW(LPCWSTR file) { (void)file; return (HENHMETAFILE)no_metafile(); }
GDIAPI HENHMETAFILE GetEnhMetaFileA(LPCSTR file) { (void)file; return (HENHMETAFILE)no_metafile(); }
GDIAPI HENHMETAFILE CopyEnhMetaFileA(HENHMETAFILE mf, LPCSTR file) { (void)mf; (void)file; return (HENHMETAFILE)no_metafile(); }
GDIAPI HENHMETAFILE CopyEnhMetaFileW(HENHMETAFILE mf, LPCWSTR file) { (void)mf; (void)file; return (HENHMETAFILE)no_metafile(); }
GDIAPI HENHMETAFILE SetWinMetaFileBits(UINT n, const BYTE *bits, HDC ref, const void *mfp)
{ (void)n; (void)bits; (void)ref; (void)mfp; return (HENHMETAFILE)no_metafile(); }
GDIAPI HENHMETAFILE SetEnhMetaFileBits(UINT n, const BYTE *bits) { (void)n; (void)bits; return (HENHMETAFILE)no_metafile(); }
GDIAPI UINT GetEnhMetaFileHeader(HENHMETAFILE mf, UINT n, void *hdr) { (void)mf; (void)n; (void)hdr; SetLastError(ERROR_INVALID_HANDLE); return 0; }
GDIAPI UINT GetEnhMetaFileBits(HENHMETAFILE mf, UINT n, BYTE *bits) { (void)mf; (void)n; (void)bits; SetLastError(ERROR_INVALID_HANDLE); return 0; }
GDIAPI BOOL PlayEnhMetaFile(HDC h, HENHMETAFILE mf, const RECT *r) { (void)h; (void)mf; (void)r; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
GDIAPI BOOL DeleteEnhMetaFile(HENHMETAFILE mf) { (void)mf; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
GDIAPI HMETAFILE GetMetaFileW(LPCWSTR file) { (void)file; return (HMETAFILE)no_metafile(); }
GDIAPI HMETAFILE GetMetaFileA(LPCSTR file) { (void)file; return (HMETAFILE)no_metafile(); }
GDIAPI UINT GetMetaFileBitsEx(HMETAFILE mf, UINT n, void *bits) { (void)mf; (void)n; (void)bits; SetLastError(ERROR_INVALID_HANDLE); return 0; }
GDIAPI BOOL DeleteMetaFile(HMETAFILE mf) { (void)mf; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

/* MaskBlt: where the monochrome @mask is 1 the foreground ROP (the low 24
 * bits of @rop4), where 0 the background one (the high byte); a
 * background that leaves the destination alone (0xAA) is the usual case.
 * Drawn a row run at a time through BitBlt. */
GDIAPI BOOL MaskBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, HBITMAP mask, int mx, int my, DWORD rop4)
{
    DWORD fore = rop4 & 0xFFFFFF, back = ((rop4 >> 8) & 0xFF0000) | (rop4 & 0xFFFF);
    GObj *m = obj_of(mask);
    if (!m || m->kind != K_BITMAP || !m->bits) return BitBlt(dst, x, y, w, h, src, sx, sy, fore);
    for (int j = 0; j < h; j++) {
        int i = 0;
        while (i < w) {
            int set = 0, mxx = mx + i, myy = my + j;
            if (mxx >= 0 && myy >= 0 && mxx < m->bw && myy < m->bh) {
                int row = m->flip ? m->bh - 1 - myy : myy;
                set = (m->bits[(size_t)row * m->bw + mxx] & 0xFFFFFF) != 0;
            }
            int s = i;
            for (i++; i < w; i++) {
                int b = 0; mxx = mx + i;
                if (mxx >= 0 && myy >= 0 && mxx < m->bw && myy < m->bh) {
                    int row = m->flip ? m->bh - 1 - myy : myy;
                    b = (m->bits[(size_t)row * m->bw + mxx] & 0xFFFFFF) != 0;
                }
                if (b != set) break;
            }
            DWORD r = set ? fore : back;
            if ((r >> 16) != 0xAA) BitBlt(dst, x + s, y + j, i - s, 1, src, sx + s, sy + j, r);
        }
    }
    return TRUE;
}

GDIAPI UINT GetNearestPaletteIndex(HPALETTE pal, COLORREF c) { (void)c; return obj_of(pal) ? 0 : 0xFFFFFFFFu; }   /* CLR_INVALID */

/* A printer DC's new page settings: there are no printers */
GDIAPI HDC ResetDCW(HDC h, const void *mode) { (void)mode; return h; }
GDIAPI HDC ResetDCA(HDC h, const void *mode) { (void)mode; return h; }

/* -----------------------------------------------------------------------
 * Palettes: the screen is true colour, so a logical palette changes
 * nothing on it; the objects exist so programs can make and select them
 * ----------------------------------------------------------------------- */
GDIAPI HPALETTE CreatePalette(const LOGPALETTE *lp)
{
    if (!lp || !lp->palNumEntries) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    GObj *o = new_obj(K_PALETTE);
    if (o) o->width = lp->palNumEntries;
    return (HPALETTE)o;
}
GDIAPI HPALETTE SelectPalette(HDC h, HPALETTE pal, BOOL background)
{
    (void)background;
    NOVA_DC *d = dc_of(h);
    if (!d || !obj_of(pal)) return 0;
    return pal;                                     /* the "previous" one: as good as any to select back */
}
GDIAPI UINT RealizePalette(HDC h) { (void)h; return 0; }
GDIAPI UINT SetPaletteEntries(HPALETTE pal, UINT first, UINT n, const PALETTEENTRY *e) { (void)first; (void)e; return obj_of(pal) ? n : 0; }
GDIAPI UINT GetPaletteEntries(HPALETTE pal, UINT first, UINT n, PALETTEENTRY *e)
{
    (void)first;
    if (!obj_of(pal)) return 0;
    if (e) memset(e, 0, n * sizeof *e);
    return n;
}
GDIAPI BOOL UnrealizeObject(HGDIOBJ h) { return obj_of(h) != NULL; }
GDIAPI BOOL UpdateColors(HDC h) { return dc_of(h) != NULL; }
GDIAPI BOOL ResizePalette(HPALETTE pal, UINT n) { (void)n; return obj_of(pal) != NULL; }

GDIAPI BOOL GetCharABCWidthsFloatA(HDC h, UINT first, UINT last, void *out)
{
    BOOL WINAPI GetCharABCWidthsFloatW(HDC, UINT, UINT, void *);
    return GetCharABCWidthsFloatW(h, first, last, out);
}

/* character sets <-> code pages (TCI_SRCCHARSET, TCI_SRCCODEPAGE, TCI_SRCFONTSIG) */
GDIAPI BOOL TranslateCharsetInfo(DWORD *src, LPCHARSETINFO cs, DWORD flags)
{
    static const struct { UINT charset, cp; } map[] = {
        { 0, 1252 }, { 238, 1250 }, { 204, 1251 }, { 161, 1253 }, { 162, 1254 }, { 177, 1255 }, { 178, 1256 },
        { 186, 1257 }, { 163, 1258 }, { 222, 874 }, { 128, 932 }, { 134, 936 }, { 129, 949 }, { 136, 950 }, { 130, 1361 },
    };
    if (!cs) return FALSE;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        BOOL hit = flags == 1 ? (UINT)(UINT_PTR)src == map[i].charset :        /* TCI_SRCCHARSET */
                   flags == 2 ? (UINT)(UINT_PTR)src == map[i].cp :             /* TCI_SRCCODEPAGE */
                   flags == 3 ? src && (src[0] >> i & 1) : FALSE;              /* TCI_SRCFONTSIG: the code page bits */
        if (hit) {
            memset(cs, 0, sizeof *cs);
            cs->ciCharset = map[i].charset;
            cs->ciACP = map[i].cp;
            cs->fs.fsCsb[0] = 1u << i;
            return TRUE;
        }
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
 * Paths, world transforms and region data: what Firefox reaches for
 * (native theme drawing and printing).  Paths are not drawn (there is no
 * path rasterizer), so a path bracket succeeds and filling or stroking
 * it draws nothing; the transform is always the identity (MM_TEXT).
 * ----------------------------------------------------------------------- */
GDIAPI BOOL BeginPath(HDC h) { return h != 0; }
GDIAPI BOOL EndPath(HDC h) { return h != 0; }
GDIAPI BOOL AbortPath(HDC h) { return h != 0; }
GDIAPI BOOL CloseFigure(HDC h) { return h != 0; }
GDIAPI BOOL FillPath(HDC h) { return h != 0; }
GDIAPI BOOL StrokePath(HDC h) { return h != 0; }
GDIAPI BOOL StrokeAndFillPath(HDC h) { return h != 0; }
GDIAPI BOOL WidenPath(HDC h) { return h != 0; }
GDIAPI BOOL FlattenPath(HDC h) { return h != 0; }
GDIAPI BOOL SelectClipPath(HDC h, int mode) { (void)mode; return h != 0; }
GDIAPI BOOL PolyBezierTo(HDC h, const POINT *p, DWORD n)
{
    for (DWORD i = 2; i < n; i += 3) LineTo(h, p[i].x, p[i].y);    /* through the end points */
    return TRUE;
}
GDIAPI BOOL SetMiterLimit(HDC h, float limit, float * old) { (void)h; (void)limit; if (old) *old = 10.0f; return TRUE; }
GDIAPI BOOL GetMiterLimit(HDC h, float * limit) { (void)h; if (limit) *limit = 10.0f; return TRUE; }


/* The system (visible) region: not tracked, so none (0) */
GDIAPI int GetRandomRgn(HDC h, HRGN rgn, INT which) { (void)h; (void)rgn; (void)which; return 0; }

/* Driver escapes: none are supported (0) */
GDIAPI int ExtEscape(HDC h, int esc, int nin, LPCSTR in, int nout, LPSTR out) { (void)h; (void)esc; (void)nin; (void)in; (void)nout; (void)out; return 0; }

/* Color management: no ICC profiles installed */
GDIAPI BOOL GetICMProfileW(HDC h, LPDWORD n, LPWSTR name) { (void)h; (void)n; (void)name; return FALSE; }
GDIAPI BOOL GetICMProfileA(HDC h, LPDWORD n, LPSTR name) { (void)h; (void)n; (void)name; return FALSE; }


