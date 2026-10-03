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

GDIAPI int GetPolyFillMode(HDC h) { (void)h; return 1; }              /* ALTERNATE */
GDIAPI int GetStretchBltMode(HDC h) { (void)h; return 1; }            /* BLACKONWHITE */

/* printer escapes: none are supported (QUERYESCSUPPORT says so) */
GDIAPI int Escape(HDC h, int code, int n, LPCSTR in, LPVOID out) { (void)h; (void)code; (void)n; (void)in; (void)out; return 0; }

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
GDIAPI BOOL SetMiterLimit(HDC h, float limit, float * old) { (void)h; (void)limit; if (old) *old = 10.0f; return TRUE; }
GDIAPI BOOL GetMiterLimit(HDC h, float * limit) { (void)h; if (limit) *limit = 10.0f; return TRUE; }


/* RGNDATA: regions are their bounding rectangle (ExtCreateRegion is in
 * gdi32.c, the world transform calls too, and the outline metrics and
 * Unicode ranges in text.c) */
typedef struct { DWORD dwSize, iType, nCount, nRgnSize; RECT rcBound; } RGNDATAHEADER_;
GDIAPI DWORD GetRegionData(HRGN rgn, DWORD n, RGNDATAHEADER_ *out)
{
    RECT r;
    if (!GetRgnBox(rgn, &r)) return 0;
    int empty = r.right <= r.left || r.bottom <= r.top;
    DWORD need = (DWORD)sizeof(RGNDATAHEADER_) + (empty ? 0 : (DWORD)sizeof(RECT));
    if (!out) return need;
    if (n < need) return 0;
    out->dwSize = sizeof(RGNDATAHEADER_);
    out->iType = 1;                                 /* RDH_RECTANGLES */
    out->nCount = empty ? 0 : 1;
    out->nRgnSize = empty ? 0 : sizeof(RECT);
    out->rcBound = r;
    if (!empty) memcpy(out + 1, &r, sizeof(RECT));
    return need;
}
/* The system (visible) region: not tracked, so none (0) */
GDIAPI int GetRandomRgn(HDC h, HRGN rgn, INT which) { (void)h; (void)rgn; (void)which; return 0; }

/* Driver escapes: none are supported (0) */
GDIAPI int ExtEscape(HDC h, int esc, int nin, LPCSTR in, int nout, LPSTR out) { (void)h; (void)esc; (void)nin; (void)in; (void)nout; (void)out; return 0; }

/* Color management: no ICC profiles installed */
GDIAPI BOOL GetICMProfileW(HDC h, LPDWORD n, LPWSTR name) { (void)h; (void)n; (void)name; return FALSE; }
GDIAPI BOOL GetICMProfileA(HDC h, LPDWORD n, LPSTR name) { (void)h; (void)n; (void)name; return FALSE; }


