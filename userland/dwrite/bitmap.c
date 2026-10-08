/*
 * bitmap.c — IDWriteBitmapRenderTarget(1): DirectWrite's GDI interop target
 *
 * IDWriteGdiInterop::CreateBitmapRenderTarget gives a memory DC with a
 * 32-bit top-down DIB section selected; DrawGlyphRun blends a glyph run's
 * coverage in the text colour into it, and the program blits the DC or
 * reads the pixels (Steam's vgui draws its windows' text this way).  As on
 * Windows the blended pixels' alpha byte is left 0, the way GDI writes a
 * 32-bit DIB.
 */
#include "dwrite_int.h"
#include <windows.h>

DEFINE_GUID(IID_IDWriteBitmapRenderTarget,  0x5e5a32a3, 0x8dff, 0x4773, 0x9f,0xf6, 0x06,0x96,0xea,0xb7,0x72,0x67);
DEFINE_GUID(IID_IDWriteBitmapRenderTarget1, 0x791e8298, 0x3ef3, 0x4230, 0x98,0x80, 0xc9,0xbd,0xec,0xc4,0x20,0x64);

enum { TEXT_AA_CLEARTYPE = 0, TEXT_AA_GRAYSCALE = 1 };

typedef struct {
    const void *const *vtbl;
    LONG      ref;
    HDC       dc;
    HBITMAP   bmp, old;
    DWORD    *bits;             /* top-down, w pixels a row */
    UINT32    w, h;
    float     ppd;
    DW_MATRIX m;
    UINT32    aa;
} Target;

static HRESULT STDMETHODCALLTYPE bt_qi(Target *t, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDWriteBitmapRenderTarget) ||
        IsEqualGUID(riid, &IID_IDWriteBitmapRenderTarget1)) {
        *out = t; InterlockedIncrement(&t->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE bt_addref(Target *t) { return (ULONG)InterlockedIncrement(&t->ref); }

/* A w x h DIB section selected into the DC (none for an empty size: the
 * DC keeps its default 1x1 bitmap) */
static HRESULT make_dib(Target *t, UINT32 w, UINT32 h)
{
    HBITMAP bmp = NULL;
    void *bits = NULL;
    if (w && h) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = (LONG)w;
        bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(t->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!bmp || !bits) return E_OUTOFMEMORY;
    }
    if (bmp) {
        HGDIOBJ prev = SelectObject(t->dc, bmp);
        if (!t->bmp) t->old = (HBITMAP)prev;               /* the DC's default bitmap */
    } else if (t->bmp) {
        SelectObject(t->dc, t->old);
        t->old = NULL;
    }
    if (t->bmp) DeleteObject(t->bmp);
    t->bmp = bmp;
    t->bits = bits;
    t->w = w; t->h = h;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE bt_release(Target *t)
{
    LONG r = InterlockedDecrement(&t->ref);
    if (!r) {
        if (t->old) SelectObject(t->dc, t->old);
        if (t->bmp) DeleteObject(t->bmp);
        DeleteDC(t->dc);
        dw_free(t);
    }
    return (ULONG)r;
}

static BYTE mix(BYTE d, BYTE s, unsigned a) { return (BYTE)((d * (255 - a) + s * a + 127) / 255); }

static HRESULT STDMETHODCALLTYPE bt_draw(Target *t, float x, float y, UINT32 measuring, const DW_GLYPH_RUN *run,
                                         void *params, COLORREF color, RECT *box)
{
    (void)measuring;
    if (box) memset(box, 0, sizeof(*box));
    if (!run) return E_INVALIDARG;
    if (!t->bits || !run->glyphCount) return S_OK;
    UINT32 mode = RMODE_NATURAL_SYMMETRIC;
    if (params && ((UINT32 (STDMETHODCALLTYPE *)(void *))VT(params)[7])(params) == RMODE_ALIASED) mode = RMODE_ALIASED;
    void *an = NULL;
    HRESULT hr = glyph_run_analysis_create(run, t->ppd, &t->m, mode, AA_GRAYSCALE, x, y, &an);
    if (FAILED(hr)) return hr;
    RECT b;
    ((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, RECT *))VT(an)[3])(an, TEX_ALIASED_1x1, &b);
    RECT c = { b.left < 0 ? 0 : b.left, b.top < 0 ? 0 : b.top,
               b.right > (LONG)t->w ? (LONG)t->w : b.right, b.bottom > (LONG)t->h ? (LONG)t->h : b.bottom };
    if (c.left < c.right && c.top < c.bottom) {
        UINT32 cw = (UINT32)(c.right - c.left), ch = (UINT32)(c.bottom - c.top);
        BYTE *cov = dw_alloc((SIZE_T)cw * ch);
        if (!cov) { COM_RELEASE(an); return E_OUTOFMEMORY; }
        ((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, const RECT *, BYTE *, UINT32))VT(an)[4])(an, TEX_ALIASED_1x1, &c,
                                                                                               cov, cw * ch);
        BYTE r = GetRValue(color), g = GetGValue(color), bl = GetBValue(color);
        GdiFlush();
        for (UINT32 j = 0; j < ch; j++) {
            DWORD *row = t->bits + (SIZE_T)(c.top + (LONG)j) * t->w + c.left;
            const BYTE *a = cov + (SIZE_T)j * cw;
            for (UINT32 i = 0; i < cw; i++) {
                if (!a[i]) continue;
                DWORD p = row[i];
                row[i] = (DWORD)mix((BYTE)(p >> 16), r, a[i]) << 16 | (DWORD)mix((BYTE)(p >> 8), g, a[i]) << 8 |
                         mix((BYTE)p, bl, a[i]);
            }
        }
        dw_free(cov);
        if (box) *box = c;
    }
    COM_RELEASE(an);
    return S_OK;
}

static HDC STDMETHODCALLTYPE bt_dc(Target *t) { return t->dc; }
static float STDMETHODCALLTYPE bt_get_ppd(Target *t) { return t->ppd; }
static HRESULT STDMETHODCALLTYPE bt_set_ppd(Target *t, float ppd)
{
    if (!(ppd > 0)) return E_INVALIDARG;
    t->ppd = ppd;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bt_get_transform(Target *t, DW_MATRIX *m) { *m = t->m; return S_OK; }
static HRESULT STDMETHODCALLTYPE bt_set_transform(Target *t, const DW_MATRIX *m)
{
    t->m = m ? *m : (DW_MATRIX){ 1, 0, 0, 1, 0, 0 };
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bt_size(Target *t, SIZE *s) { s->cx = (LONG)t->w; s->cy = (LONG)t->h; return S_OK; }
static HRESULT STDMETHODCALLTYPE bt_resize(Target *t, UINT32 w, UINT32 h)
{
    if (w == t->w && h == t->h) return S_OK;
    return make_dib(t, w, h);
}
static UINT32 STDMETHODCALLTYPE bt_get_aa(Target *t) { return t->aa; }
static HRESULT STDMETHODCALLTYPE bt_set_aa(Target *t, UINT32 aa)
{
    if (aa > TEXT_AA_GRAYSCALE) return E_INVALIDARG;
    t->aa = aa;
    return S_OK;
}

static const void *const bt_vtbl[] = {
    bt_qi, bt_addref, bt_release, bt_draw, bt_dc, bt_get_ppd, bt_set_ppd, bt_get_transform, bt_set_transform,
    bt_size, bt_resize, bt_get_aa, bt_set_aa,
};

HRESULT bitmap_target_create(HDC dc, UINT32 w, UINT32 h, void **out)
{
    *out = NULL;
    Target *t = dw_zalloc(sizeof(*t));
    if (!t) return E_OUTOFMEMORY;
    t->vtbl = bt_vtbl;
    t->ref = 1;
    t->ppd = 1.0f;
    t->m = (DW_MATRIX){ 1, 0, 0, 1, 0, 0 };
    t->dc = CreateCompatibleDC(dc);
    if (!t->dc) { dw_free(t); return E_OUTOFMEMORY; }
    HRESULT hr = make_dib(t, w, h);
    if (FAILED(hr)) { DeleteDC(t->dc); dw_free(t); return hr; }
    *out = t;
    return S_OK;
}
