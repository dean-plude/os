/*
 * gdiplus.dll — GDI+'s flat API (the Gdip* calls the C++ wrapper classes in
 * gdiplus.h compile down to), drawn with plutovg (third_party/plutovg, MIT):
 * anti-aliased paths, lines, Béziers, arcs, fills and TrueType text; bitmaps
 * loaded from PNG, JPEG, BMP and GIF files or streams (stb_image, inside
 * plutovg) and saved as PNG, JPEG or BMP; bitmaps converted to and from
 * HBITMAPs; and graphics on a bitmap, a window or any device context.
 *
 * This file: startup, bitmaps and the encoders.  A bitmap is a plutovg
 * surface (premultiplied 32-bit BGRA, the layout of PixelFormat32bppPARGB);
 * LockBits converts to the format asked for.
 */
#include "gdip.h"

int _fltused = 0x9875;      /* floating point in use (the compiler references it) */

#pragma pack(push, 2)
typedef struct { WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits; } BMPFILEHEADER;
#pragma pack(pop)

typedef struct { UINT Width, Height; INT Stride; PixelFormat PixelFormat; void *Scan0; UINT_PTR Reserved; } BitmapData;
typedef struct { UINT Flags, Count; ARGB Entries[1]; } ColorPalette;
typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread, SuppressExternalCodecs; } GdiplusStartupInput;
typedef struct { void *NotificationHook, *NotificationUnhook; } GdiplusStartupOutput;
typedef struct {
    CLSID Clsid; GUID FormatID;
    const WCHAR *CodecName, *DllName, *FormatDescription, *FilenameExtension, *MimeType;
    DWORD Flags, Version, SigCount, SigSize;
    const BYTE *SigPattern, *SigMask;
} ImageCodecInfo;

GDIPAPI void *GDIPCALL GdipAlloc(SIZE_T size) { return HeapAlloc(GetProcessHeap(), 0, size); }
GDIPAPI void GDIPCALL GdipFree(void *p) { xfree(p); }

/* ---- startup ---------------------------------------------------------- */

static LONG g_started;

GDIPAPI GpStatus GDIPCALL GdiplusNotificationHook(ULONG_PTR *token) { if (token) *token = 1; return Ok; }
GDIPAPI void GDIPCALL GdiplusNotificationUnhook(ULONG_PTR token) { (void)token; }

GDIPAPI GpStatus GDIPCALL GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *in, GdiplusStartupOutput *out)
{
    if (!token || !in) return InvalidParameter;
    if (in->GdiplusVersion < 1 || in->GdiplusVersion > 3) return UnsupportedGdiplusVersion;
    if (out) {                                       /* there is no background thread: the hooks do nothing */
        out->NotificationHook = (void *)GdiplusNotificationHook;
        out->NotificationUnhook = (void *)GdiplusNotificationUnhook;
    }
    *token = (ULONG_PTR)InterlockedIncrement(&g_started);
    return Ok;
}
GDIPAPI void GDIPCALL GdiplusShutdown(ULONG_PTR token) { (void)token; }

/* ---- colours and pixels ----------------------------------------------- */

/* premultiplied BGRA <-> ARGB */
static DWORD premul(DWORD p)
{
    DWORD a = p >> 24;
    if (a == 255) return p;
    if (!a) return 0;
    DWORD r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
    return a << 24 | r << 16 | g << 8 | b;
}
static DWORD unpremul(DWORD p)
{
    DWORD a = p >> 24;
    if (a == 255) return p;
    if (!a) return 0;
    DWORD r = ((p >> 16) & 255) * 255 / a, g = ((p >> 8) & 255) * 255 / a, b = (p & 255) * 255 / a;
    return a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static int bpp_of(PixelFormat f) { return (f >> 8) & 0xFF; }

/* ---- images ----------------------------------------------------------- */

static const GUID FMT_MEMBMP = { 0xb96b3caa, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID FMT_BMP    = { 0xb96b3cab, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID FMT_JPEG   = { 0xb96b3cae, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID FMT_PNG    = { 0xb96b3caf, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID FMT_GIF    = { 0xb96b3cb0, 0x0728, 0x11d3, { 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

static GpImage *image_new(plutovg_surface_t *s, PixelFormat fmt)
{
    if (!s) return NULL;
    GpImage *i = xalloc(sizeof *i);
    if (!i) { plutovg_surface_destroy(s); return NULL; }
    i->type = ImageTypeBitmap;
    i->s = s;
    i->fmt = fmt;
    i->raw = FMT_MEMBMP;
    return i;
}

static const GUID *sniff(const BYTE *d, size_t n)
{
    if (n >= 8 && !memcmp(d, "\x89PNG", 4)) return &FMT_PNG;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) return &FMT_JPEG;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return &FMT_BMP;
    if (n >= 4 && !memcmp(d, "GIF8", 4)) return &FMT_GIF;
    return NULL;
}

static GpStatus image_from_memory(const BYTE *d, size_t n, GpImage **out)
{
    const GUID *f = sniff(d, n);
    plutovg_surface_t *s = plutovg_surface_load_from_image_data(d, (int)n);
    if (!s) return UnknownImageFormat;
    GpImage *i = image_new(s, PixelFormat32bppARGB);
    if (!i) return OutOfMemory;
    if (f) i->raw = *f;
    if (f == &FMT_JPEG) i->fmt = PixelFormat24bppRGB;
    *out = i;
    return Ok;
}

static BYTE *read_file(const WCHAR *name, size_t *len)
{
    HANDLE h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(h, NULL), got = 0;
    BYTE *b = n != INVALID_FILE_SIZE ? xalloc(n + 1) : NULL;
    if (b && !ReadFile(h, b, n, &got, NULL)) { xfree(b); b = NULL; }
    CloseHandle(h);
    *len = got;
    return b;
}

static BYTE *read_stream(IStream *st, size_t *len)
{
    size_t cap = 65536, n = 0;
    BYTE *b = xalloc(cap);
    LARGE_INTEGER zero = { 0 };
    STATSTG stat;
    if (SUCCEEDED(st->lpVtbl->Stat(st, &stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0 &&
        stat.cbSize.QuadPart < 0x40000000) {
        ULARGE_INTEGER pos;
        LARGE_INTEGER cur = { 0 };
        st->lpVtbl->Seek(st, cur, STREAM_SEEK_CUR, &pos);
        size_t want = (size_t)(stat.cbSize.QuadPart - pos.QuadPart) + 1;
        if (want > cap) { xfree(b); cap = want; b = xalloc(cap); }
    }
    (void)zero;
    while (b) {
        ULONG got = 0;
        HRESULT hr = st->lpVtbl->Read(st, b + n, (ULONG)(cap - n), &got);
        n += got;
        if (FAILED(hr) || !got) break;
        if (n == cap) {
            BYTE *nb = xalloc(cap * 2);
            if (!nb) { xfree(b); b = NULL; break; }
            memcpy(nb, b, n);
            xfree(b);
            b = nb;
            cap *= 2;
        }
    }
    *len = n;
    return b;
}

GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromFile(const WCHAR *name, GpBitmap **out)
{
    if (!name || !out) return InvalidParameter;
    size_t n;
    BYTE *d = read_file(name, &n);
    if (!d) return FileNotFound;
    GpStatus st = image_from_memory(d, n, out);
    xfree(d);
    return st == UnknownImageFormat ? OutOfMemory : st;   /* what GDI+ says for an unreadable file */
}
GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromFileICM(const WCHAR *name, GpBitmap **out) { return GdipCreateBitmapFromFile(name, out); }
GDIPAPI GpStatus GDIPCALL GdipLoadImageFromFile(const WCHAR *name, GpImage **out) { return GdipCreateBitmapFromFile(name, out); }
GDIPAPI GpStatus GDIPCALL GdipLoadImageFromFileICM(const WCHAR *name, GpImage **out) { return GdipCreateBitmapFromFile(name, out); }

GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromStream(IStream *st, GpBitmap **out)
{
    if (!st || !out) return InvalidParameter;
    size_t n;
    BYTE *d = read_stream(st, &n);
    if (!d) return OutOfMemory;
    GpStatus s = image_from_memory(d, n, out);
    xfree(d);
    return s;
}
GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromStreamICM(IStream *st, GpBitmap **out) { return GdipCreateBitmapFromStream(st, out); }
GDIPAPI GpStatus GDIPCALL GdipLoadImageFromStream(IStream *st, GpImage **out) { return GdipCreateBitmapFromStream(st, out); }
GDIPAPI GpStatus GDIPCALL GdipLoadImageFromStreamICM(IStream *st, GpImage **out) { return GdipCreateBitmapFromStream(st, out); }

/* copies rows of the given format into a surface (premultiplying) */
static void import_rows(plutovg_surface_t *s, const BYTE *src, INT stride, PixelFormat f, int x0, int y0, int w, int h)
{
    BYTE *dst = plutovg_surface_get_data(s);
    int ds = plutovg_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        const BYTE *r = src + (ptrdiff_t)y * stride;
        DWORD *d = (DWORD *)(dst + (ptrdiff_t)(y0 + y) * ds) + x0;
        for (int x = 0; x < w; x++) {
            DWORD p;
            switch (f) {
            case PixelFormat24bppRGB: p = 0xFF000000u | r[3 * x + 2] << 16 | r[3 * x + 1] << 8 | r[3 * x]; break;
            case PixelFormat32bppRGB: p = ((const DWORD *)r)[x] | 0xFF000000u; break;
            case PixelFormat32bppPARGB: p = ((const DWORD *)r)[x]; break;
            case PixelFormat16bppRGB565: {
                WORD v = ((const WORD *)r)[x];
                p = 0xFF000000u | ((v >> 11) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31;
                break;
            }
            case PixelFormat16bppRGB555: {
                WORD v = ((const WORD *)r)[x];
                p = 0xFF000000u | (((v >> 10) & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 | (v & 31) * 255 / 31;
                break;
            }
            default: p = premul(((const DWORD *)r)[x]); break;   /* 32bppARGB */
            }
            d[x] = p;
        }
    }
}

static void export_rows(plutovg_surface_t *s, BYTE *dst, INT stride, PixelFormat f, int x0, int y0, int w, int h)
{
    const BYTE *src = plutovg_surface_get_data(s);
    int ss = plutovg_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        BYTE *r = dst + (ptrdiff_t)y * stride;
        const DWORD *sp = (const DWORD *)(src + (ptrdiff_t)(y0 + y) * ss) + x0;
        for (int x = 0; x < w; x++) {
            DWORD p = sp[x];
            switch (f) {
            case PixelFormat24bppRGB: p = unpremul(p); r[3 * x] = (BYTE)p; r[3 * x + 1] = (BYTE)(p >> 8); r[3 * x + 2] = (BYTE)(p >> 16); break;
            case PixelFormat32bppRGB: ((DWORD *)r)[x] = unpremul(p) | 0xFF000000u; break;
            case PixelFormat32bppPARGB: ((DWORD *)r)[x] = p; break;
            case PixelFormat16bppRGB565: p = unpremul(p);
                ((WORD *)r)[x] = (WORD)(((p >> 19) & 31) << 11 | ((p >> 10) & 63) << 5 | ((p >> 3) & 31)); break;
            case PixelFormat16bppRGB555: p = unpremul(p);
                ((WORD *)r)[x] = (WORD)(((p >> 19) & 31) << 10 | ((p >> 11) & 31) << 5 | ((p >> 3) & 31)); break;
            default: ((DWORD *)r)[x] = unpremul(p); break;
            }
        }
    }
}

static BOOL format_ok(PixelFormat f)
{
    return f == PixelFormat24bppRGB || f == PixelFormat32bppRGB || f == PixelFormat32bppARGB ||
           f == PixelFormat32bppPARGB || f == PixelFormat16bppRGB565 || f == PixelFormat16bppRGB555;
}

GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromScan0(INT w, INT h, INT stride, PixelFormat f, BYTE *scan0, GpBitmap **out)
{
    if (!out || w <= 0 || h <= 0) return InvalidParameter;
    if (!format_ok(f)) {
        if (!scan0 && (f == PixelFormat8bppIndexed || f == PixelFormat4bppIndexed || f == PixelFormat1bppIndexed)) f = PixelFormat32bppARGB;
        else return NotImplemented;
    }
    plutovg_surface_t *s = plutovg_surface_create(w, h);
    if (!s) return OutOfMemory;
    if (scan0) {
        if (!stride) return InvalidParameter;
        import_rows(s, scan0, stride, f, 0, 0, w, h);
    } else if (!(f & 0x00040000) && f != PixelFormat32bppPARGB) {
        plutovg_color_t black = { 0, 0, 0, 1 };                 /* no alpha channel: opaque black, as GDI+ */
        plutovg_surface_clear(s, &black);
    }
    GpImage *i = image_new(s, f);
    if (!i) return OutOfMemory;
    *out = i;
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromGraphics(INT w, INT h, GpGraphics *g, GpBitmap **out)
{
    (void)g;
    return GdipCreateBitmapFromScan0(w, h, 0, PixelFormat32bppPARGB, NULL, out);
}

GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromHBITMAP(HBITMAP hbm, HPALETTE pal, GpBitmap **out)
{
    (void)pal;
    BITMAP bm;
    if (!hbm || !out || !GetObjectW(hbm, sizeof bm, &bm)) return InvalidParameter;
    int w = bm.bmWidth, h = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    DWORD *px = xalloc((size_t)w * h * 4);
    if (!px) return OutOfMemory;
    HDC dc = CreateCompatibleDC(NULL);
    int got = GetDIBits(dc, hbm, 0, h, px, &bi, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (!got) { xfree(px); return GenericError; }
    plutovg_surface_t *s = plutovg_surface_create(w, h);
    if (s) import_rows(s, (BYTE *)px, w * 4, PixelFormat32bppRGB, 0, 0, w, h);
    xfree(px);
    GpImage *i = image_new(s, PixelFormat32bppRGB);
    if (!i) return OutOfMemory;
    *out = i;
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromHICON(HICON icon, GpBitmap **out)
{
    ICONINFO ii;
    if (!icon || !out || !GetIconInfo(icon, &ii)) return InvalidParameter;
    BITMAP bm;
    GpStatus st = GenericError;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm)) {
        int w = bm.bmWidth, h = bm.bmHeight;
        BITMAPINFO bi = { 0 };
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        DWORD *px = xalloc((size_t)w * h * 4), *mask = xalloc((size_t)w * h * 4);
        HDC dc = CreateCompatibleDC(NULL);
        if (px && mask && GetDIBits(dc, ii.hbmColor, 0, h, px, &bi, DIB_RGB_COLORS)) {
            BOOL alpha = FALSE;
            for (int k = 0; k < w * h; k++) if (px[k] >> 24) { alpha = TRUE; break; }
            if (!alpha && ii.hbmMask && GetDIBits(dc, ii.hbmMask, 0, h, mask, &bi, DIB_RGB_COLORS))
                for (int k = 0; k < w * h; k++) px[k] = (mask[k] & 0xFFFFFF) ? 0 : px[k] | 0xFF000000u;
            else if (!alpha)
                for (int k = 0; k < w * h; k++) px[k] |= 0xFF000000u;
            plutovg_surface_t *s = plutovg_surface_create(w, h);
            if (s) import_rows(s, (BYTE *)px, w * 4, PixelFormat32bppARGB, 0, 0, w, h);
            GpImage *i = image_new(s, PixelFormat32bppARGB);
            if (i) { *out = i; st = Ok; } else st = OutOfMemory;
        }
        DeleteDC(dc);
        xfree(px);
        xfree(mask);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return st;
}

HBITMAP gdip_dib_section(int w, int h, DWORD **bits)
{
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

GDIPAPI GpStatus GDIPCALL GdipCreateHBITMAPFromBitmap(GpBitmap *b, HBITMAP *out, ARGB background)
{
    if (!b || !out) return InvalidParameter;
    int w = plutovg_surface_get_width(b->s), h = plutovg_surface_get_height(b->s);
    DWORD *bits;
    HBITMAP hbm = gdip_dib_section(w, h, &bits);
    if (!hbm) return OutOfMemory;
    const BYTE *src = plutovg_surface_get_data(b->s);
    int ss = plutovg_surface_get_stride(b->s);
    DWORD bg = premul(background), bga = bg >> 24;
    for (int y = 0; y < h; y++) {
        const DWORD *sp = (const DWORD *)(src + (ptrdiff_t)y * ss);
        for (int x = 0; x < w; x++) {
            DWORD p = sp[x], ia = 255 - (p >> 24);
            if (!bga) { bits[y * w + x] = p; continue; }    /* a transparent background keeps the alpha */
            DWORD r = ((p >> 16) & 255) + ((bg >> 16) & 255) * ia / 255;
            DWORD g = ((p >> 8) & 255) + ((bg >> 8) & 255) * ia / 255;
            DWORD bl = (p & 255) + (bg & 255) * ia / 255;
            DWORD a = (p >> 24) + bga * ia / 255;
            bits[y * w + x] = a << 24 | r << 16 | g << 8 | bl;
        }
    }
    *out = hbm;
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipCreateHICONFromBitmap(GpBitmap *b, HICON *out)
{
    if (!b || !out) return InvalidParameter;
    HBITMAP color;
    GpStatus st = GdipCreateHBITMAPFromBitmap(b, &color, 0);
    if (st != Ok) return st;
    int w = plutovg_surface_get_width(b->s), h = plutovg_surface_get_height(b->s);
    HBITMAP mask = CreateBitmap(w, h, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    *out = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return *out ? Ok : GenericError;
}

GDIPAPI GpStatus GDIPCALL GdipCloneImage(GpImage *i, GpImage **out)
{
    if (!i || !out) return InvalidParameter;
    int w = plutovg_surface_get_width(i->s), h = plutovg_surface_get_height(i->s);
    plutovg_surface_t *s = plutovg_surface_create(w, h);
    if (!s) return OutOfMemory;
    for (int y = 0; y < h; y++)
        memcpy(plutovg_surface_get_data(s) + (ptrdiff_t)y * plutovg_surface_get_stride(s),
               plutovg_surface_get_data(i->s) + (ptrdiff_t)y * plutovg_surface_get_stride(i->s), (size_t)w * 4);
    GpImage *c = image_new(s, i->fmt);
    if (!c) return OutOfMemory;
    c->raw = i->raw;
    *out = c;
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipCloneBitmapAreaI(INT x, INT y, INT w, INT h, PixelFormat f, GpBitmap *src, GpBitmap **out)
{
    if (!src || !out || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > plutovg_surface_get_width(src->s) || y + h > plutovg_surface_get_height(src->s)) return InvalidParameter;
    plutovg_surface_t *s = plutovg_surface_create(w, h);
    if (!s) return OutOfMemory;
    for (int r = 0; r < h; r++)
        memcpy(plutovg_surface_get_data(s) + (ptrdiff_t)r * plutovg_surface_get_stride(s),
               plutovg_surface_get_data(src->s) + (ptrdiff_t)(y + r) * plutovg_surface_get_stride(src->s) + x * 4, (size_t)w * 4);
    GpImage *c = image_new(s, format_ok(f) ? f : src->fmt);
    if (!c) return OutOfMemory;
    *out = c;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCloneBitmapArea(REAL x, REAL y, REAL w, REAL h, PixelFormat f, GpBitmap *src, GpBitmap **out)
{ return GdipCloneBitmapAreaI((INT)x, (INT)y, (INT)w, (INT)h, f, src, out); }

GDIPAPI GpStatus GDIPCALL GdipDisposeImage(GpImage *i)
{
    if (!i) return InvalidParameter;
    if (i->lock_own) xfree(i->lock_buf);
    plutovg_surface_destroy(i->s);
    xfree(i);
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipGetImageWidth(GpImage *i, UINT *w) { if (!i || !w) return InvalidParameter; *w = plutovg_surface_get_width(i->s); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetImageHeight(GpImage *i, UINT *h) { if (!i || !h) return InvalidParameter; *h = plutovg_surface_get_height(i->s); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetImageDimension(GpImage *i, REAL *w, REAL *h)
{
    if (!i || !w || !h) return InvalidParameter;
    *w = (REAL)plutovg_surface_get_width(i->s);
    *h = (REAL)plutovg_surface_get_height(i->s);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetImageBounds(GpImage *i, GpRectF *r, INT *unit)
{
    if (!i || !r || !unit) return InvalidParameter;
    r->X = r->Y = 0;
    GdipGetImageDimension(i, &r->Width, &r->Height);
    *unit = UnitPixel;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetImagePixelFormat(GpImage *i, PixelFormat *f) { if (!i || !f) return InvalidParameter; *f = i->fmt; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetImageType(GpImage *i, INT *t) { if (!i || !t) return InvalidParameter; *t = ImageTypeBitmap; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetImageRawFormat(GpImage *i, GUID *f) { if (!i || !f) return InvalidParameter; *f = i->raw; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetImageFlags(GpImage *i, UINT *flags)
{
    if (!i || !flags) return InvalidParameter;
    *flags = 0x10000 | 0x2000 | ((i->fmt & 0x00040000) ? 2 : 0);     /* ReadOnly-ish: ColorSpaceRGB | HasRealDPI | HasAlpha */
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetImageHorizontalResolution(GpImage *i, REAL *r) { if (!i || !r) return InvalidParameter; *r = 96.f; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetImageVerticalResolution(GpImage *i, REAL *r) { if (!i || !r) return InvalidParameter; *r = 96.f; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipBitmapSetResolution(GpBitmap *b, REAL x, REAL y) { (void)x; (void)y; return b ? Ok : InvalidParameter; }

/* every bitmap is direct colour: an empty palette */
GDIPAPI GpStatus GDIPCALL GdipGetImagePaletteSize(GpImage *i, INT *size) { if (!i || !size) return InvalidParameter; *size = sizeof(ColorPalette); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetImagePalette(GpImage *i, ColorPalette *p, INT size)
{
    if (!i || !p || size < (INT)sizeof(ColorPalette)) return InvalidParameter;
    p->Flags = 0;
    p->Count = 0;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipSetImagePalette(GpImage *i, const ColorPalette *p) { return i && p ? Ok : InvalidParameter; }

GDIPAPI GpStatus GDIPCALL GdipGetImageThumbnail(GpImage *i, UINT w, UINT h, GpImage **out, void *cb, void *data);

GDIPAPI GpStatus GDIPCALL GdipBitmapGetPixel(GpBitmap *b, INT x, INT y, ARGB *c)
{
    if (!b || !c || x < 0 || y < 0 || x >= plutovg_surface_get_width(b->s) || y >= plutovg_surface_get_height(b->s)) return InvalidParameter;
    *c = unpremul(((DWORD *)(plutovg_surface_get_data(b->s) + (ptrdiff_t)y * plutovg_surface_get_stride(b->s)))[x]);
    if (!(b->fmt & 0x00040000)) *c |= 0xFF000000u;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipBitmapSetPixel(GpBitmap *b, INT x, INT y, ARGB c)
{
    if (!b || x < 0 || y < 0 || x >= plutovg_surface_get_width(b->s) || y >= plutovg_surface_get_height(b->s)) return InvalidParameter;
    ((DWORD *)(plutovg_surface_get_data(b->s) + (ptrdiff_t)y * plutovg_surface_get_stride(b->s)))[x] = premul(c);
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipBitmapLockBits(GpBitmap *b, const GpRect *rect, UINT flags, PixelFormat f, BitmapData *d)
{
    if (!b || !d) return InvalidParameter;
    if (b->locked) return WrongState;
    int W = plutovg_surface_get_width(b->s), H = plutovg_surface_get_height(b->s);
    GpRect r = rect ? *rect : (GpRect){ 0, 0, W, H };
    if (r.X < 0 || r.Y < 0 || r.Width <= 0 || r.Height <= 0 || r.X + r.Width > W || r.Y + r.Height > H) return InvalidParameter;
    if (!format_ok(f)) return NotImplemented;
    INT stride = ((r.Width * bpp_of(f) + 31) / 32) * 4;
    BYTE *buf;
    if (flags & ImageLockModeUserInputBuf) {
        if (!d->Scan0) return InvalidParameter;
        buf = d->Scan0;
        stride = d->Stride;
        b->lock_own = FALSE;
    } else {
        buf = xalloc((size_t)stride * r.Height);
        if (!buf) return OutOfMemory;
        b->lock_own = TRUE;
    }
    if (flags & ImageLockModeRead) export_rows(b->s, buf, stride, f, r.X, r.Y, r.Width, r.Height);
    b->locked = TRUE;
    b->lock_flags = flags;
    b->lock_rect = r;
    b->lock_fmt = f;
    b->lock_buf = buf;
    b->lock_stride = stride;
    d->Width = r.Width;
    d->Height = r.Height;
    d->Stride = stride;
    d->PixelFormat = f;
    d->Scan0 = buf;
    d->Reserved = 0;
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipBitmapUnlockBits(GpBitmap *b, BitmapData *d)
{
    if (!b || !d) return InvalidParameter;
    if (!b->locked) return WrongState;
    if (b->lock_flags & ImageLockModeWrite)
        import_rows(b->s, b->lock_buf, b->lock_stride, b->lock_fmt, b->lock_rect.X, b->lock_rect.Y, b->lock_rect.Width, b->lock_rect.Height);
    if (b->lock_own) xfree(b->lock_buf);
    b->lock_buf = NULL;
    b->lock_own = FALSE;
    b->locked = FALSE;
    return Ok;
}

/* ---- encoders --------------------------------------------------------- */

#define CODEC_GUID(n) { 0x557cf400 + (n), 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } }
static const CLSID ENC_BMP = CODEC_GUID(0), ENC_JPEG = CODEC_GUID(1), ENC_PNG = CODEC_GUID(6);

static const struct {
    const CLSID *clsid; const GUID *fmt;
    const WCHAR *name, *desc, *ext, *mime;
} g_encoders[] = {
    { &ENC_BMP,  &FMT_BMP,  L"Built-in BMP Codec",  L"BMP",  L"*.BMP;*.DIB;*.RLE",          L"image/bmp" },
    { &ENC_JPEG, &FMT_JPEG, L"Built-in JPEG Codec", L"JPEG", L"*.JPG;*.JPEG;*.JPE;*.JFIF", L"image/jpeg" },
    { &ENC_PNG,  &FMT_PNG,  L"Built-in PNG Codec",  L"PNG",  L"*.PNG",                     L"image/png" },
};
#define NENC (sizeof g_encoders / sizeof g_encoders[0])

static UINT wbytes(const WCHAR *s) { return (UINT)(wcslen(s) + 1) * sizeof(WCHAR); }

static UINT encoders_size(void)
{
    UINT n = NENC * sizeof(ImageCodecInfo);
    for (UINT i = 0; i < NENC; i++)
        n += wbytes(g_encoders[i].name) + wbytes(g_encoders[i].desc) + wbytes(g_encoders[i].ext) + wbytes(g_encoders[i].mime);
    return n;
}

GDIPAPI GpStatus GDIPCALL GdipGetImageEncodersSize(UINT *num, UINT *size)
{
    if (!num || !size) return InvalidParameter;
    *num = NENC;
    *size = encoders_size();
    return Ok;
}

static const WCHAR *put_str(BYTE **p, const WCHAR *s)
{
    UINT n = wbytes(s);
    memcpy(*p, s, n);
    const WCHAR *r = (const WCHAR *)*p;
    *p += n;
    return r;
}

GDIPAPI GpStatus GDIPCALL GdipGetImageEncoders(UINT num, UINT size, ImageCodecInfo *out)
{
    if (!out || num != NENC || size != encoders_size()) return GenericError;
    BYTE *p = (BYTE *)(out + NENC);
    for (UINT i = 0; i < NENC; i++) {
        ImageCodecInfo *c = &out[i];
        memset(c, 0, sizeof *c);
        c->Clsid = *g_encoders[i].clsid;
        c->FormatID = *g_encoders[i].fmt;
        c->CodecName = put_str(&p, g_encoders[i].name);
        c->FormatDescription = put_str(&p, g_encoders[i].desc);
        c->FilenameExtension = put_str(&p, g_encoders[i].ext);
        c->MimeType = put_str(&p, g_encoders[i].mime);
        c->Flags = 0x00010000 | 0x00000001 | 0x00000004;    /* Builtin | Encoder | SupportBitmap */
        c->Version = 1;
    }
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipGetImageDecodersSize(UINT *num, UINT *size) { return GdipGetImageEncodersSize(num, size); }
GDIPAPI GpStatus GDIPCALL GdipGetImageDecoders(UINT num, UINT size, ImageCodecInfo *out)
{
    GpStatus st = GdipGetImageEncoders(num, size, out);
    if (st == Ok) for (UINT i = 0; i < NENC; i++) out[i].Flags = 0x00010000 | 0x00000002 | 0x00000004;
    return st;
}

typedef struct { IStream *st; HANDLE file; BOOL failed; } Sink;

static void sink_write(void *closure, void *data, int size)
{
    Sink *s = closure;
    if (s->failed || size <= 0) return;
    ULONG done = 0;
    if (s->st) s->failed = FAILED(s->st->lpVtbl->Write(s->st, data, (ULONG)size, &done)) || done != (ULONG)size;
    else s->failed = !WriteFile(s->file, data, (DWORD)size, (DWORD *)&done, NULL) || done != (ULONG)size;
}

static BOOL write_bmp(GpImage *i, Sink *s)
{
    int w = plutovg_surface_get_width(i->s), h = plutovg_surface_get_height(i->s);
    BOOL alpha = (i->fmt & 0x00040000) != 0;
    int bpp = alpha ? 32 : 24, stride = ((w * bpp + 31) / 32) * 4;
    BMPFILEHEADER fh = { 0 };
    BITMAPINFOHEADER ih = { 0 };
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + stride * h;
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = h;                                   /* bottom-up, as Windows writes them */
    ih.biPlanes = 1;
    ih.biBitCount = (WORD)bpp;
    ih.biSizeImage = stride * h;
    ih.biXPelsPerMeter = ih.biYPelsPerMeter = 3780;
    sink_write(s, &fh, sizeof fh);
    sink_write(s, &ih, sizeof ih);
    BYTE *row = xalloc(stride);
    if (!row) return FALSE;
    for (int y = h - 1; y >= 0; y--) {
        export_rows(i->s, row, stride, alpha ? PixelFormat32bppARGB : PixelFormat24bppRGB, 0, y, w, 1);
        sink_write(s, row, stride);
    }
    xfree(row);
    return !s->failed;
}

/* plutovg writes RGBA from its premultiplied BGRA itself */
static GpStatus encode(GpImage *i, const CLSID *enc, Sink *s)
{
    BOOL ok;
    if (IsEqualGUID(enc, &ENC_PNG)) ok = plutovg_surface_write_to_png_stream(i->s, sink_write, s);
    else if (IsEqualGUID(enc, &ENC_JPEG)) ok = plutovg_surface_write_to_jpg_stream(i->s, sink_write, s, 90);
    else if (IsEqualGUID(enc, &ENC_BMP)) ok = write_bmp(i, s);
    else return FileNotFound;                          /* an encoder GDI+ hasn't got */
    return ok && !s->failed ? Ok : Win32Error;
}

GDIPAPI GpStatus GDIPCALL GdipSaveImageToStream(GpImage *i, IStream *st, const CLSID *enc, const void *params)
{
    (void)params;
    if (!i || !st || !enc) return InvalidParameter;
    Sink s = { st, NULL, FALSE };
    return encode(i, enc, &s);
}

GDIPAPI GpStatus GDIPCALL GdipSaveImageToFile(GpImage *i, const WCHAR *name, const CLSID *enc, const void *params)
{
    (void)params;
    if (!i || !name || !enc) return InvalidParameter;
    HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return Win32Error;
    Sink s = { NULL, h, FALSE };
    GpStatus st = encode(i, enc, &s);
    CloseHandle(h);
    if (st != Ok) DeleteFileW(name);
    return st;
}

/* ---- image attributes (kept; drawing ignores them) --------------------- */

/* ColorAdjustType: Default, Bitmap, Brush, Pen, Text, Count */
#define ADJ_COUNT 5
typedef struct {
    BOOL matrix_on, gamma_on, keys_on, remap_on, threshold_on, noop, channel_on;
    REAL matrix[25], gray[25];
    int matrix_flags, channel;
    REAL gamma, threshold;
    ARGB key_lo, key_hi;
    int nremap;
} AdjustSet;
typedef struct GpImageAttributes {
    AdjustSet adj[ADJ_COUNT];
    int wrap; ARGB wrap_color; BOOL wrap_clamp;
    BOOL cached_bg;
} GpImageAttributes;
static AdjustSet *adj_of(GpImageAttributes *a, INT type) { return a && type >= 0 && type < ADJ_COUNT ? &a->adj[type] : NULL; }

GDIPAPI GpStatus GDIPCALL GdipCreateImageAttributes(GpImageAttributes **out)
{
    if (!out) return InvalidParameter;
    *out = xalloc(sizeof **out);
    return *out ? Ok : OutOfMemory;
}
GDIPAPI GpStatus GDIPCALL GdipCloneImageAttributes(const GpImageAttributes *a, GpImageAttributes **out)
{
    if (!a || !out) return InvalidParameter;
    *out = xalloc(sizeof **out);
    if (!*out) return OutOfMemory;
    **out = *a;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDisposeImageAttributes(GpImageAttributes *a) { if (!a) return InvalidParameter; xfree(a); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesToIdentity(GpImageAttributes *a, INT type)
{ AdjustSet *s = adj_of(a, type); if (!s) return InvalidParameter; memset(s, 0, sizeof *s); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipResetImageAttributes(GpImageAttributes *a, INT type)
{ AdjustSet *s = adj_of(a, type); if (!s) return InvalidParameter; memset(s, 0, sizeof *s); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesColorMatrix(GpImageAttributes *a, INT type, BOOL on, const REAL *m, const REAL *gray, INT flags)
{
    AdjustSet *s = adj_of(a, type);
    if (!s || (on && !m)) return InvalidParameter;
    s->matrix_on = on;
    if (on) { memcpy(s->matrix, m, sizeof s->matrix); if (gray) memcpy(s->gray, gray, sizeof s->gray); s->matrix_flags = flags; }
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesThreshold(GpImageAttributes *a, INT type, BOOL on, REAL t)
{ AdjustSet *s = adj_of(a, type); if (!s) return InvalidParameter; s->threshold_on = on; s->threshold = t; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesGamma(GpImageAttributes *a, INT type, BOOL on, REAL gamma)
{ AdjustSet *s = adj_of(a, type); if (!s || (on && gamma <= 0)) return InvalidParameter; s->gamma_on = on; s->gamma = gamma; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesNoOp(GpImageAttributes *a, INT type, BOOL on)
{ AdjustSet *s = adj_of(a, type); if (!s) return InvalidParameter; s->noop = on; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesColorKeys(GpImageAttributes *a, INT type, BOOL on, ARGB lo, ARGB hi)
{ AdjustSet *s = adj_of(a, type); if (!s) return InvalidParameter; s->keys_on = on; s->key_lo = lo; s->key_hi = hi; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesOutputChannel(GpImageAttributes *a, INT type, BOOL on, INT channel)
{ AdjustSet *s = adj_of(a, type); if (!s) return InvalidParameter; s->channel_on = on; s->channel = channel; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesOutputChannelColorProfile(GpImageAttributes *a, INT type, BOOL on, const WCHAR *profile)
{ (void)on; (void)profile; return adj_of(a, type) ? Ok : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesRemapTable(GpImageAttributes *a, INT type, BOOL on, UINT n, const void *map)
{ AdjustSet *s = adj_of(a, type); if (!s || (on && n && !map)) return InvalidParameter; s->remap_on = on; s->nremap = (int)n; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesWrapMode(GpImageAttributes *a, INT wrap, ARGB c, BOOL clamp)
{ if (!a) return InvalidParameter; a->wrap = wrap; a->wrap_color = c; a->wrap_clamp = clamp; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetImageAttributesCachedBackground(GpImageAttributes *a, BOOL on)
{ if (!a) return InvalidParameter; a->cached_bg = on; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetImageAttributesAdjustedPalette(GpImageAttributes *a, ColorPalette *pal, INT type)
{ (void)type; return a && pal ? Ok : InvalidParameter; }

/* ---- cached bitmaps: a copy of the bitmap, drawn at a point ------------- */

typedef struct GpCachedBitmap { GpImage *img; } GpCachedBitmap;
GDIPAPI GpStatus GDIPCALL GdipCreateCachedBitmap(GpBitmap *b, GpGraphics *g, GpCachedBitmap **out)
{
    if (!b || !g || !out) return InvalidParameter;
    GpCachedBitmap *c = xalloc(sizeof *c);
    if (!c) return OutOfMemory;
    GpStatus st = GdipCloneImage(b, &c->img);
    if (st != Ok) { xfree(c); return st; }
    *out = c;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDeleteCachedBitmap(GpCachedBitmap *c)
{
    if (!c) return InvalidParameter;
    GdipDisposeImage(c->img);
    xfree(c);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDrawCachedBitmap(GpGraphics *g, GpCachedBitmap *c, INT x, INT y)
{
    if (!g || !c) return InvalidParameter;
    return GdipDrawImage(g, c->img, (REAL)x, (REAL)y);
}

/* ---- rotating and flipping ------------------------------------------- */

/* RotateFlipType: bits 0-1 the quarter turns, bit 2 (values 4..7) a flip of x */
GDIPAPI GpStatus GDIPCALL GdipImageRotateFlip(GpImage *i, INT type)
{
    if (!i || type < 0 || type > 7) return InvalidParameter;
    if (i->locked) return WrongState;
    int turns = type & 3, flip = type & 4;
    int w = plutovg_surface_get_width(i->s), h = plutovg_surface_get_height(i->s);
    int nw = (turns & 1) ? h : w, nh = (turns & 1) ? w : h;
    plutovg_surface_t *d = plutovg_surface_create(nw, nh);
    if (!d) return OutOfMemory;
    const BYTE *src = plutovg_surface_get_data(i->s);
    BYTE *dst = plutovg_surface_get_data(d);
    int ss = plutovg_surface_get_stride(i->s), ds = plutovg_surface_get_stride(d);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int tx, ty;
            switch (turns) {
            case 0: tx = x; ty = y; break;
            case 1: tx = h - 1 - y; ty = x; break;
            case 2: tx = w - 1 - x; ty = h - 1 - y; break;
            default: tx = y; ty = w - 1 - x; break;
            }
            if (flip) tx = nw - 1 - tx;
            *(DWORD *)(dst + (ptrdiff_t)ty * ds + tx * 4) = *(const DWORD *)(src + (ptrdiff_t)y * ss + x * 4);
        }
    }
    plutovg_surface_destroy(i->s);
    i->s = d;
    return Ok;
}

/* ---- odds and ends -------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipGetEncoderParameterListSize(GpImage *i, const CLSID *enc, UINT *size)
{ (void)enc; if (!i || !size) return InvalidParameter; *size = 0; return NotImplemented; }
GDIPAPI GpStatus GDIPCALL GdipGetEncoderParameterList(GpImage *i, const CLSID *enc, UINT size, void *out)
{ (void)enc; (void)size; (void)out; return i ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipSaveAdd(GpImage *i, const void *params) { (void)params; return i ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipSaveAddImage(GpImage *i, GpImage *add, const void *params)
{ (void)params; return i && add ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromDirectDrawSurface(void *surface, GpBitmap **out)
{ (void)surface; return out ? NotImplemented : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromResource(HINSTANCE inst, const WCHAR *name, GpBitmap **out)
{
    if (!out) return InvalidParameter;
    HRSRC r = FindResourceW(inst, name, (LPCWSTR)RT_BITMAP);
    if (!r) return InvalidParameter;
    HGLOBAL g = LoadResource(inst, r);
    const BITMAPINFO *bi = g ? LockResource(g) : NULL;
    if (!bi) return InvalidParameter;
    /* a BITMAP resource: the info header, its palette, then the bits */
    int ncol = bi->bmiHeader.biClrUsed;
    if (!ncol && bi->bmiHeader.biBitCount <= 8) ncol = 1 << bi->bmiHeader.biBitCount;
    const BYTE *bits = (const BYTE *)bi + bi->bmiHeader.biSize + ncol * sizeof(RGBQUAD);
    return GdipCreateBitmapFromGdiDib(bi, (void *)bits, out);
}
GDIPAPI GpStatus GDIPCALL GdipGetAllPropertyItems(GpImage *i, UINT size, UINT n, void *items)
{ (void)size; (void)n; (void)items; return i ? PropertyNotFound : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipImageForceValidation(GpImage *i) { return i ? Ok : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipTestControl(INT control, void *param) { (void)control; (void)param; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetNearestColor(GpGraphics *g, ARGB *c) { return g && c ? Ok : InvalidParameter; }   /* 32-bit screens: the colour itself */
GDIPAPI GpStatus GDIPCALL GdipCreateStreamOnFile(const WCHAR *name, UINT access, IStream **out)
{ (void)name; (void)access; return out ? NotImplemented : InvalidParameter; }

/* ---- frames and properties -------------------------------------------- */

/* stb_image reads the first frame of an animated GIF: one frame, one dimension */
static const GUID FRAMEDIM_PAGE = { 0x7462dc86, 0x6180, 0x4c7e, { 0x8e, 0x3f, 0xee, 0x73, 0x33, 0xa7, 0xa4, 0x83 } };
static const GUID FRAMEDIM_TIME = { 0x6aedbd6d, 0x3fb5, 0x418a, { 0x83, 0xa6, 0x7f, 0x45, 0x22, 0x9d, 0xc8, 0x72 } };
GDIPAPI GpStatus GDIPCALL GdipImageGetFrameDimensionsCount(GpImage *i, UINT *n) { if (!i || !n) return InvalidParameter; *n = 1; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipImageGetFrameDimensionsList(GpImage *i, GUID *ids, UINT n)
{
    if (!i || !ids || n < 1) return InvalidParameter;
    ids[0] = IsEqualGUID(&i->raw, &FMT_GIF) ? FRAMEDIM_TIME : FRAMEDIM_PAGE;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipImageGetFrameCount(GpImage *i, const GUID *dim, UINT *n) { (void)dim; if (!i || !n) return InvalidParameter; *n = 1; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipImageSelectActiveFrame(GpImage *i, const GUID *dim, UINT frame)
{ (void)dim; if (!i) return InvalidParameter; return frame == 0 ? Ok : InvalidParameter; }

/* no EXIF or other metadata is read */
GDIPAPI GpStatus GDIPCALL GdipGetPropertyCount(GpImage *i, UINT *n) { if (!i || !n) return InvalidParameter; *n = 0; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPropertyIdList(GpImage *i, UINT n, ULONG *ids) { (void)ids; if (!i) return InvalidParameter; return n ? InvalidParameter : Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetPropertyItemSize(GpImage *i, ULONG id, UINT *size) { (void)id; if (!i || !size) return InvalidParameter; *size = 0; return PropertyNotFound; }
GDIPAPI GpStatus GDIPCALL GdipGetPropertyItem(GpImage *i, ULONG id, UINT size, void *item) { (void)id; (void)size; (void)item; return i ? PropertyNotFound : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipGetPropertySize(GpImage *i, UINT *total, UINT *n)
{ if (!i || !total || !n) return InvalidParameter; *total = *n = 0; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetPropertyItem(GpImage *i, const void *item) { return i && item ? Ok : InvalidParameter; }
GDIPAPI GpStatus GDIPCALL GdipRemovePropertyItem(GpImage *i, ULONG id) { (void)id; return i ? PropertyNotFound : InvalidParameter; }

/* a bitmap from a packed DIB (BITMAPINFO and its bits), as for the clipboard's CF_DIB */
GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromGdiDib(const BITMAPINFO *bi, void *bits, GpBitmap **out)
{
    if (!bi || !bits || !out) return InvalidParameter;
    int w = bi->bmiHeader.biWidth, h = bi->bmiHeader.biHeight, ah = h < 0 ? -h : h;
    if (w <= 0 || !ah) return InvalidParameter;
    /* let GDI convert whatever the DIB holds to 32-bit rows */
    HDC dc = CreateCompatibleDC(NULL);
    DWORD *px;
    HBITMAP hbm = gdip_dib_section(w, ah, &px);
    GpStatus st = OutOfMemory;
    if (hbm) {
        HBITMAP old = SelectObject(dc, hbm);
        StretchDIBits(dc, 0, 0, w, ah, 0, 0, w, ah, bits, bi, DIB_RGB_COLORS, SRCCOPY);
        SelectObject(dc, old);
        plutovg_surface_t *s = plutovg_surface_create(w, ah);
        if (s) import_rows(s, (BYTE *)px, w * 4, bi->bmiHeader.biBitCount == 32 ? PixelFormat32bppARGB : PixelFormat32bppRGB, 0, 0, w, ah);
        GpImage *i = image_new(s, bi->bmiHeader.biBitCount == 32 ? PixelFormat32bppARGB : PixelFormat32bppRGB);
        if (i) { *out = i; st = Ok; }
        DeleteObject(hbm);
    }
    DeleteDC(dc);
    return st;
}
