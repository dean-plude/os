/* dwtest — DirectWrite text layout with font fallback, through NovaOS's
 * dwrite.dll: one line of Latin, Arabic and Devanagari in a format whose
 * font has only Latin.  The layout must pick fallback fonts for the other
 * scripts, shape them (Arabic joining, Devanagari conjuncts), order the
 * Arabic right to left, and the glyph runs must draw.  The drawing is shown
 * in a window for the screenshot.
 * Build: x86_64-w64-mingw32-gcc -O2 -o dwtest.exe dwtest.c -ldwrite -lgdi32 -luser32
 * Run:   dwtest [seconds to keep the window up]
 */
#define COBJMACROS
#include <windows.h>
#include <dwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static const GUID IID_IDWriteFactory_ = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

static int pass, fail;
#define CHECK(what, cond) do { if (cond) { pass++; printf("ok   %s\n", what); } \
                               else { fail++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

#define W 900
#define H 120
static BYTE canvas[H][W];                           /* coverage, 0..255 */
static IDWriteFactory *g_f;

/* what DrawGlyphRun saw */
typedef struct {
    IDWriteFontFace *face;
    UINT32 start, count, glyphs, level;
    UINT16 ids[64];
    float x0, x1;
    int ink;
} Run;
static Run runs[16];
static int nruns;
static const WCHAR *g_text;

/* ---- an IDWriteTextRenderer that rasterizes through glyph run analysis ---- */
static HRESULT STDMETHODCALLTYPE r_qi(IDWriteTextRenderer *r, REFIID iid, void **out) { (void)iid; *out = r; return S_OK; }
static ULONG STDMETHODCALLTYPE r_addref(IDWriteTextRenderer *r) { (void)r; return 1; }
static ULONG STDMETHODCALLTYPE r_release(IDWriteTextRenderer *r) { (void)r; return 1; }
static HRESULT STDMETHODCALLTYPE r_snap(IDWriteTextRenderer *r, void *c, BOOL *off) { (void)r; (void)c; *off = FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE r_xform(IDWriteTextRenderer *r, void *c, DWRITE_MATRIX *m)
{
    (void)r; (void)c;
    m->m11 = 1; m->m12 = 0; m->m21 = 0; m->m22 = 1; m->dx = 0; m->dy = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_ppd(IDWriteTextRenderer *r, void *c, FLOAT *p) { (void)r; (void)c; *p = 1; return S_OK; }

static HRESULT STDMETHODCALLTYPE r_glyphs(IDWriteTextRenderer *r, void *c, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
                                          const DWRITE_GLYPH_RUN *run, const DWRITE_GLYPH_RUN_DESCRIPTION *desc, IUnknown *fx)
{
    (void)r; (void)c; (void)fx;
    if (nruns >= 16) return S_OK;
    Run *o = &runs[nruns++];
    o->face = run->fontFace;
    IDWriteFontFace_AddRef(o->face);
    o->start = desc ? desc->textPosition : 0;
    o->count = desc ? desc->stringLength : 0;
    o->glyphs = run->glyphCount;
    o->level = run->bidiLevel;
    for (UINT32 i = 0; i < run->glyphCount && i < 64; i++) o->ids[i] = run->glyphIndices[i];
    float adv = 0;
    for (UINT32 i = 0; i < run->glyphCount; i++) adv += run->glyphAdvances ? run->glyphAdvances[i] : 0;
    o->x0 = (run->bidiLevel & 1) ? x - adv : x;
    o->x1 = o->x0 + adv;

    IDWriteGlyphRunAnalysis *a = NULL;
    if (FAILED(IDWriteFactory_CreateGlyphRunAnalysis(g_f, run, 1.0f, NULL, DWRITE_RENDERING_MODE_ALIASED, mode, x, y, &a)) || !a)
        return S_OK;
    RECT b;
    if (SUCCEEDED(IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(a, DWRITE_TEXTURE_ALIASED_1x1, &b)) && b.right > b.left) {
        UINT32 bw = b.right - b.left, bh = b.bottom - b.top;
        BYTE *tex = malloc(bw * bh);
        if (tex && SUCCEEDED(IDWriteGlyphRunAnalysis_CreateAlphaTexture(a, DWRITE_TEXTURE_ALIASED_1x1, &b, tex, bw * bh)))
            for (UINT32 yy = 0; yy < bh; yy++)
                for (UINT32 xx = 0; xx < bw; xx++) {
                    int px = b.left + xx, py = b.top + yy;
                    BYTE v = tex[yy * bw + xx];
                    if (v && px >= 0 && px < W && py >= 0 && py < H) { canvas[py][px] = 255; o->ink++; }
                }
        free(tex);
    }
    IDWriteGlyphRunAnalysis_Release(a);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_line(IDWriteTextRenderer *r, void *c, FLOAT x, FLOAT y, const void *u, IUnknown *fx)
{ (void)r; (void)c; (void)x; (void)y; (void)u; (void)fx; return S_OK; }
static HRESULT STDMETHODCALLTYPE r_inline(IDWriteTextRenderer *r, void *c, FLOAT x, FLOAT y, IDWriteInlineObject *o, BOOL s, BOOL rtl, IUnknown *fx)
{ (void)r; (void)c; (void)x; (void)y; (void)o; (void)s; (void)rtl; (void)fx; return S_OK; }

static IDWriteTextRendererVtbl g_rvt = {
    r_qi, r_addref, r_release, r_snap, r_xform, r_ppd, r_glyphs,
    (void *)r_line, (void *)r_line, r_inline,
};
static IDWriteTextRenderer g_renderer = { &g_rvt };

/* the run that draws text position @pos */
static Run *run_at(UINT32 pos)
{
    for (int i = 0; i < nruns; i++) if (pos >= runs[i].start && pos < runs[i].start + runs[i].count) return &runs[i];
    return NULL;
}
static BOOL face_has(IDWriteFontFace *f, UINT32 c)
{
    UINT16 g = 0;
    return f && SUCCEEDED(IDWriteFontFace_GetGlyphIndices(f, &c, 1, &g)) && g;
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        static DWORD px[H][W];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) px[y][x] = canvas[y][x] ? 0x00202020 : 0x00FFFFFF;
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB } };
        StretchDIBits(dc, 10, 10, W, H, 0, 0, W, H, px, &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 0;
    /* Latin, then Arabic "marhaban" (letters that join), then Devanagari "namaste" (with a conjunct) */
    g_text = L"Hello \x0645\x0631\x062D\x0628\x0627 \x0928\x092E\x0938\x094D\x0924\x0947";
    const UINT32 ARABIC = 6, DEVA = 12, LEN = (UINT32)wcslen(g_text);

    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory_, (IUnknown **)&g_f);
    CHECK("DWriteCreateFactory", SUCCEEDED(hr) && g_f);
    if (!g_f) goto done;
    IDWriteTextFormat *fmt = NULL;
    hr = IDWriteFactory_CreateTextFormat(g_f, L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, 40.0f, L"en-us", &fmt);
    CHECK("CreateTextFormat", SUCCEEDED(hr) && fmt);
    if (!fmt) goto done;
    IDWriteTextLayout *lay = NULL;
    hr = IDWriteFactory_CreateTextLayout(g_f, g_text, LEN, fmt, (FLOAT)W, (FLOAT)H, &lay);
    CHECK("CreateTextLayout", SUCCEEDED(hr) && lay);
    if (!lay) goto done;
    DWRITE_TEXT_METRICS tm;
    CHECK("one line", SUCCEEDED(IDWriteTextLayout_GetMetrics(lay, &tm)) && tm.lineCount == 1);
    hr = IDWriteTextLayout_Draw(lay, NULL, &g_renderer, 0, 0);
    CHECK("Draw", SUCCEEDED(hr) && nruns >= 3);

    Run *lat = run_at(0), *ar = run_at(ARABIC), *dv = run_at(DEVA);
    CHECK("runs for each script", lat && ar && dv && lat != ar && ar != dv);
    if (!lat || !ar || !dv) goto done;
    CHECK("the format's font has no Arabic or Devanagari", !face_has(lat->face, 0x0645) && !face_has(lat->face, 0x0928));
    CHECK("Arabic falls back to a font with Arabic", face_has(ar->face, 0x0645) && ar->face != lat->face);
    CHECK("Devanagari falls back to a font with Devanagari", face_has(dv->face, 0x0928) && dv->face != lat->face);
    CHECK("Arabic runs right to left", ar->level & 1);
    CHECK("Latin runs left to right", !(lat->level & 1));

    /* shaped: joined Arabic forms are not the letters' nominal glyphs */
    UINT32 cps[5]; UINT16 nominal[5];
    for (int i = 0; i < 5; i++) cps[i] = g_text[ARABIC + i];
    IDWriteFontFace_GetGlyphIndices(ar->face, cps, 5, nominal);
    int differ = 0;
    for (UINT32 i = 0; i < ar->glyphs && i < 64; i++) {
        int nom = 0;
        for (int k = 0; k < 5; k++) nom |= ar->ids[i] == nominal[k];
        differ += !nom;
    }
    CHECK("Arabic is shaped into joining forms", differ > 0);
    CHECK("Devanagari is shaped (a conjunct: fewer glyphs than letters)", dv->glyphs < dv->count);

    for (int i = 0; i < nruns; i++) {
        char what[64];
        snprintf(what, sizeof(what), "run %d (text %u+%u) draws", i, runs[i].start, runs[i].count);
        int blank = 1;                              /* (a run of spaces has nothing to draw) */
        for (UINT32 k = 0; k < runs[i].count; k++) blank &= g_text[runs[i].start + k] == L' ';
        CHECK(what, runs[i].ink > 20 || blank);
    }
    CHECK("Arabic is drawn to the right of the Latin", ar->x0 >= lat->x1 - 1);
    CHECK("Devanagari is drawn to the right of the Arabic", dv->x0 >= ar->x1 - 1);

    if (secs > 0) {
        WNDCLASSW wc = { 0, wndproc, 0, 0, GetModuleHandleW(NULL), 0, LoadCursor(NULL, IDC_ARROW), 0, 0, L"dwtest" };
        RegisterClassW(&wc);
        HWND w = CreateWindowW(L"dwtest", L"DirectWrite fallback", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, W + 40, H + 70,
                               0, 0, wc.hInstance, 0);
        DWORD until = GetTickCount() + secs * 1000;
        printf("DirectWrite fallback on screen\n");
        fflush(stdout);
        MSG m;
        while (GetTickCount() < until) {
            while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            Sleep(20);
        }
        DestroyWindow(w);
    }
done:
    printf("dwtest: %d passed, %d failed\n", pass, fail);
    fflush(stdout);
    return fail != 0;
}
