/*
 * usptest.exe — Uniscribe (usp10.dll on HarfBuzz) and GDI's complex-script
 * text: Arabic and Devanagari itemized, shaped and placed with the Noto
 * fonts, and ExtTextOut drawing such text exactly as ScriptStringOut does.
 * The expected glyphs are what HarfBuzz gives for these fonts (checked on
 * the build machine with the same HarfBuzz).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef void *SCRIPT_CACHE;
typedef struct { WORD uBidiLevel : 5, fOverrideDirection : 1, fInhibitSymSwap : 1, fCharShape : 1,
                 fDigitSubstitute : 1, fInhibitLigate : 1, fDisplayZWG : 1, fArabicNumContext : 1,
                 fGcpClusters : 1, fReserved : 1, fEngineReserved : 2; } SCRIPT_STATE;
typedef struct { WORD eScript : 10, fRTL : 1, fLayoutRTL : 1, fLinkBefore : 1, fLinkAfter : 1,
                 fLogicalOrder : 1, fNoGlyphIndex : 1;
                 SCRIPT_STATE s; } SCRIPT_ANALYSIS;
typedef struct { int iCharPos; SCRIPT_ANALYSIS a; } SCRIPT_ITEM;
typedef struct { WORD uJustification : 4, fClusterStart : 1, fDiacritic : 1, fZeroWidth : 1, fReserved : 1,
                 fShapeReserved : 8; } SCRIPT_VISATTR;
typedef struct { LONG du, dv; } GOFFSET;
typedef struct { DWORD langid : 16, fNumeric : 1, fComplex : 1, fNeedsWordBreaking : 1, fNeedsCaretInfo : 1,
                 bCharSet : 8, fControl : 1, fPrivateUseArea : 1, fNeedsCharacterJustify : 1,
                 fInvalidGlyph : 1, fInvalidLogAttr : 1, fCDM : 1, fAmbiguousCharSet : 1,
                 fClusterSizeVaries : 1, fRejectInvalid : 1; } SCRIPT_PROPERTIES;

__declspec(dllimport) HRESULT WINAPI ScriptItemize(const WCHAR *, int, int, const void *, const SCRIPT_STATE *,
                                                   SCRIPT_ITEM *, int *);
__declspec(dllimport) HRESULT WINAPI ScriptShape(HDC, SCRIPT_CACHE *, const WCHAR *, int, int, SCRIPT_ANALYSIS *,
                                                 WORD *, WORD *, SCRIPT_VISATTR *, int *);
__declspec(dllimport) HRESULT WINAPI ScriptPlace(HDC, SCRIPT_CACHE *, const WORD *, int, const SCRIPT_VISATTR *,
                                                 SCRIPT_ANALYSIS *, int *, GOFFSET *, void *);
__declspec(dllimport) HRESULT WINAPI ScriptGetProperties(const SCRIPT_PROPERTIES ***, int *);
__declspec(dllimport) HRESULT WINAPI ScriptFreeCache(SCRIPT_CACHE *);
__declspec(dllimport) HRESULT WINAPI ScriptIsComplex(const WCHAR *, int, DWORD);
__declspec(dllimport) HRESULT WINAPI ScriptCPtoX(int, BOOL, int, int, const WORD *, const SCRIPT_VISATTR *,
                                                 const int *, const SCRIPT_ANALYSIS *, int *);
__declspec(dllimport) HRESULT WINAPI ScriptStringAnalyse(HDC, const void *, int, int, int, DWORD, int, void *, void *,
                                                         const int *, void *, const BYTE *, void **);
__declspec(dllimport) HRESULT WINAPI ScriptStringOut(void *, int, int, UINT, const RECT *, int, int, BOOL);
__declspec(dllimport) HRESULT WINAPI ScriptStringFree(void **);
__declspec(dllimport) const SIZE *WINAPI ScriptString_pSize(void *);
GDIAPI BOOL GetTextExtentPoint32W(HDC h, LPCWSTR s, int len, LPSIZE sz);

#define SSA_GLYPHS   0x80
#define SSA_FALLBACK 0x20
#define ETO_IGNORELANGUAGE 0x1000

static int g_pass, g_fail;
static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

static HFONT font(const WCHAR *face, int h)
{
    return CreateFontW(h, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, face);
}

/* Shape @s with @face; returns the glyph count and fills @g, @clust */
static int shape_with(HDC dc, const WCHAR *face, const WCHAR *s, SCRIPT_ANALYSIS *sa, WORD *g, WORD *clust,
                      int *adv, GOFFSET *off)
{
    HFONT f = font(face, -24);
    HGDIOBJ old = SelectObject(dc, f);
    SCRIPT_CACHE sc = 0;
    SCRIPT_VISATTR va[32];
    int n = 0, len = (int)wcslen(s);
    HRESULT r = ScriptShape(dc, &sc, s, len, 32, sa, g, clust, va, &n);
    if (r != S_OK) { printf("  ScriptShape: 0x%08lx\n", (unsigned long)r); n = 0; }
    else if (ScriptPlace(dc, &sc, g, n, va, sa, adv, off, 0) != S_OK) n = 0;
    ScriptFreeCache(&sc);
    SelectObject(dc, old);
    DeleteObject(f);
    return n;
}

static void test_itemize(void)
{
    static const WCHAR s[] = L"abc \x0633\x0644\x0627\x0645 12";
    SCRIPT_ITEM items[8];
    int n = 0;
    check(ScriptItemize(s, (int)wcslen(s), 8, 0, 0, items, &n) == S_OK, "ScriptItemize");
    const SCRIPT_PROPERTIES **props;
    int np;
    ScriptGetProperties(&props, &np);
    printf("  items:");
    for (int i = 0; i < n; i++) printf(" [%d script %d level %d]", items[i].iCharPos, items[i].a.eScript, items[i].a.s.uBidiLevel);
    printf("\n");
    check(n == 3, "Latin, Arabic, digits: three items");
    if (n == 3) {
        check(items[0].iCharPos == 0 && !items[0].a.fRTL, "the Latin item is left to right");
        check(items[1].iCharPos == 4 && items[1].a.fRTL && items[1].a.s.uBidiLevel == 1, "the Arabic item is right to left");
        check(items[1].a.eScript < np && props[items[1].a.eScript]->fComplex, "Arabic is a complex script");
        check(items[2].a.s.uBidiLevel == 2, "digits after Arabic in a left-to-right line are at level 2 (UAX #9 I1)");
    }
    check(ScriptIsComplex(s, (int)wcslen(s), 1) == S_OK, "ScriptIsComplex: Arabic");
    check(ScriptIsComplex(L"plain", 5, 1) == S_FALSE, "ScriptIsComplex: Latin");
}

static void test_arabic(HDC dc)
{
    /* seen lam alef meem: an initial seen, the lam-alef ligature, a final meem */
    static const WCHAR s[] = L"\x0633\x0644\x0627\x0645";
    SCRIPT_ITEM items[4];
    int ni = 0;
    ScriptItemize(s, 4, 4, 0, 0, items, &ni);
    WORD g[32], clust[8];
    int adv[32];
    GOFFSET off[32];
    int n = shape_with(dc, L"Noto Sans Arabic", s, &items[0].a, g, clust, adv, off);
    printf("  Arabic: %d glyphs:", n);
    for (int i = 0; i < n; i++) printf(" %u", g[i]);
    printf("  clusters %u %u %u %u\n", clust[0], clust[1], clust[2], clust[3]);
    check(n == 3, "Arabic: lam and alef ligate (3 glyphs for 4 characters)");
    check(n == 3 && g[0] == 769 && g[1] == 705 && g[2] == 1077, "Arabic: HarfBuzz's contextual forms, in visual order");
    check(clust[0] == 2 && clust[1] == 1 && clust[2] == 1 && clust[3] == 0, "Arabic: the logical clusters");
    int total = 0;
    for (int i = 0; i < n; i++) total += adv[i];
    check(n == 3 && total > 20 && total < 80, "Arabic: placed widths");
    int x0 = -1, x3 = -1;
    ScriptCPtoX(0, FALSE, 4, n, clust, 0, adv, &items[0].a, &x0);
    ScriptCPtoX(3, TRUE, 4, n, clust, 0, adv, &items[0].a, &x3);
    check(x0 == total && x3 == 0, "Arabic: the first character's caret is at the right");
}

static void test_devanagari(HDC dc)
{
    /* ka virama ssa + i: the kssa conjunct, the i sign moved before it */
    static const WCHAR s[] = L"\x0915\x094D\x0937\x093F";
    SCRIPT_ITEM items[4];
    int ni = 0;
    ScriptItemize(s, 4, 4, 0, 0, items, &ni);
    WORD g[32], clust[8];
    int adv[32];
    GOFFSET off[32];
    int n = shape_with(dc, L"Noto Sans Devanagari", s, &items[0].a, g, clust, adv, off);
    printf("  Devanagari: %d glyphs:", n);
    for (int i = 0; i < n; i++) printf(" %u", g[i]);
    printf("  clusters %u %u %u %u\n", clust[0], clust[1], clust[2], clust[3]);
    check(ni == 1 && !items[0].a.fRTL, "Devanagari: one left-to-right item");
    check(n == 2 && g[0] == 610 && g[1] == 179, "Devanagari: i sign reordered before the kssa conjunct");
    check(clust[0] == 0 && clust[1] == 0 && clust[2] == 0 && clust[3] == 0, "Devanagari: one cluster");

    static const WCHAR namaste[] = L"\x0928\x092E\x0938\x094D\x0924\x0947";
    n = shape_with(dc, L"Noto Sans Devanagari", namaste, &items[0].a, g, clust, adv, off);
    printf("  namaste: %d glyphs:", n);
    for (int i = 0; i < n; i++) printf(" %u", g[i]);
    printf("\n");
    check(n == 5 && g[0] == 44 && g[1] == 50 && g[2] == 215 && g[3] == 40 && g[4] == 75,
          "Devanagari: half sa and the e sign");
    check(n == 5 && adv[4] == 0, "Devanagari: the e sign takes no advance");
}

/* a white 32-bit DIB of 480 x 48 in a memory DC */
static HDC canvas(DWORD **bits, HBITMAP *bm)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 480;
    bi.bmiHeader.biHeight = -48;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(0);
    *bm = CreateDIBSection(dc, &bi, 0, (void **)bits, 0, 0);
    SelectObject(dc, *bm);
    for (int i = 0; i < 480 * 48; i++) (*bits)[i] = 0xFFFFFF;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, 0);
    return dc;
}

static int dark(const DWORD *b, int x0, int x1)
{
    int n = 0;
    for (int y = 0; y < 48; y++)
        for (int x = x0; x < x1; x++) if ((b[y * 480 + x] & 0xFF) < 128) n++;
    return n;
}

static void test_gdi(const WCHAR *what, const WCHAR *s)
{
    DWORD *a, *b, *c;
    HBITMAP ba, bb, bc;
    HDC da = canvas(&a, &ba), db = canvas(&b, &bb), dcc = canvas(&c, &bc);
    HFONT f = font(L"Segoe UI", -20);
    SelectObject(da, f); SelectObject(db, f); SelectObject(dcc, f);
    int len = (int)wcslen(s);
    char name[64];
    WideCharToMultiByte(CP_UTF8, 0, what, -1, name, sizeof(name), 0, 0);

    BOOL ok = ExtTextOutW(da, 8, 8, 0, 0, s, (UINT)len, 0);
    void *ssa = 0;
    HRESULT r = ScriptStringAnalyse(db, s, len, len * 2 + 16, -1, SSA_GLYPHS | SSA_FALLBACK, 0, 0, 0, 0, 0, 0, &ssa);
    if (r == S_OK) ScriptStringOut(ssa, 8, 8, 0, 0, 0, 0, FALSE);
    ExtTextOutW(dcc, 8, 8, ETO_IGNORELANGUAGE, 0, s, (UINT)len, 0);
    int same = 1, plain_same = 1;
    for (int i = 0; i < 480 * 48; i++) {
        if (a[i] != b[i]) same = 0;
        if (a[i] != c[i]) plain_same = 0;
    }
    char m[160];
    sprintf(m, "%s: ExtTextOut draws", name);
    check(ok && dark(a, 0, 480) > 40, m);
    sprintf(m, "%s: ScriptStringAnalyse", name);
    check(r == S_OK, m);
    sprintf(m, "%s: ExtTextOut draws exactly what ScriptStringOut draws", name);
    check(same, m);
    sprintf(m, "%s: and not the unshaped characters", name);
    check(!plain_same, m);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(da, s, len, &sz);
    const SIZE *ps = ssa ? ScriptString_pSize(ssa) : 0;
    printf("  %s: extent %ld, Uniscribe %ld, ink %d px\n", name, sz.cx, ps ? ps->cx : -1L, dark(a, 0, 480));
    sprintf(m, "%s: GetTextExtentPoint32 measures the shaped text", name);
    check(ps && sz.cx == ps->cx, m);
    ScriptStringFree(&ssa);
    DeleteDC(da); DeleteDC(db); DeleteDC(dcc);
    DeleteObject(ba); DeleteObject(bb); DeleteObject(bc);
    DeleteObject(f);
}

/* usptest bmp PATH: sample lines drawn with ExtTextOut, saved as a bitmap */
static int save_sample(const char *path)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 640;
    bi.bmiHeader.biHeight = -200;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    DWORD *bits;
    HDC dc = CreateCompatibleDC(0);
    HBITMAP bm = CreateDIBSection(dc, &bi, 0, (void **)&bits, 0, 0);
    SelectObject(dc, bm);
    for (int i = 0; i < 640 * 200; i++) bits[i] = 0xFFFFFF;
    SetBkMode(dc, TRANSPARENT);
    HFONT f = font(L"Segoe UI", -28);
    SelectObject(dc, f);
    static const WCHAR *const lines[] = {
        L"Arabic: \x0645\x0631\x062D\x0628\x0627 \x0628\x0627\x0644\x0639\x0627\x0644\x0645 (hello world)",
        L"Devanagari: \x0928\x092E\x0938\x094D\x0924\x0947 \x0926\x0941\x0928\x093F\x092F\x093E",
        L"Mixed: NovaOS \x0633\x0644\x0627\x0645 123 \x0915\x094D\x0937\x093F",
        L"Plain Latin text is unchanged.",
    };
    for (int i = 0; i < 4; i++) ExtTextOutW(dc, 16, 12 + i * 46, 0, 0, lines[i], (UINT)wcslen(lines[i]), 0);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return 1;
    BITMAPFILEHEADER fh;
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + 640 * 200 * 4;
    DWORD w;
    WriteFile(h, &fh, sizeof(fh), &w, 0);
    WriteFile(h, &bi.bmiHeader, sizeof(bi.bmiHeader), &w, 0);
    WriteFile(h, bits, 640 * 200 * 4, &w, 0);
    CloseHandle(h);
    printf("usptest: wrote %s\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "bmp")) return save_sample(argv[2]);
    HDC dc = CreateCompatibleDC(0);
    test_itemize();
    test_arabic(dc);
    test_devanagari(dc);
    test_gdi(L"Arabic", L"Hello \x0633\x0644\x0627\x0645 world");
    test_gdi(L"Devanagari", L"\x0928\x092E\x0938\x094D\x0924\x0947 \x0915\x094D\x0937\x093F");
    DeleteDC(dc);
    printf("usptest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
