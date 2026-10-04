/* dw3test — DirectWrite's Windows 10 font model, through NovaOS's dwrite.dll,
 * the way Chromium (Steam's browser, WebView2) and Skia use it:
 * IDWriteFactory2 and IDWriteFactory3 from the factory, the system font
 * collection and font set, font face references, set filtering by full and
 * PostScript name, set builders, collections made from sets,
 * IDWriteFontFamily1/IDWriteFont3/IDWriteFontFace3, the system font
 * fallback and a built one, grayscale glyph run analysis, rendering params
 * 3, color glyph runs and the download queue.
 * Build: x86_64-w64-mingw32-gcc -O2 -o dw3test.exe dw3test.c -ldwrite
 * Run:   dw3test
 */
#define COBJMACROS
#include <windows.h>
#include <dwrite_3.h>
#include <stdio.h>
#include <wchar.h>

static const GUID IID_IDWriteFactory_  = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };
static const GUID IID_IDWriteFactory2_ = { 0x0439fc60, 0xca44, 0x4994, { 0x8d, 0xee, 0x3a, 0x9a, 0xf7, 0xb7, 0x32, 0xec } };
static const GUID IID_IDWriteFactory3_ = { 0x9a1b41c3, 0xd3bb, 0x466a, { 0x87, 0xfc, 0xfe, 0x67, 0x55, 0x6a, 0x3b, 0x65 } };
static const GUID IID_IDWriteFontFace2_ = { 0xd8b768ff, 0x64bc, 0x4e66, { 0x98, 0x2b, 0xec, 0x8e, 0x87, 0xf6, 0x93, 0xf7 } };
static const GUID IID_IDWriteRenderingParams3_ = { 0xb7924baa, 0x391b, 0x412a, { 0x8c, 0x5c, 0xe4, 0x4c, 0xc2, 0xd8, 0x67, 0xdc } };
static const GUID IID_IDWriteLocalFontFileLoader_ = { 0xb2d9f3ec, 0xc9fe, 0x4a11, { 0xa2, 0xec, 0xd8, 0x62, 0x08, 0xf7, 0xc0, 0xa2 } };

static int pass, fail;
#define CHECK(what, cond) do { if (cond) { pass++; printf("ok   %s\n", what); } \
                               else { fail++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

/* a method by its vtable name (COBJMACROS' macros of the same names are function-like) */
#define V(obj, m) ((obj)->lpVtbl->m)

#define DWRITE_E_NOCOLOR_ ((HRESULT)0x8898500CL)

/* ---- an IDWriteTextAnalysisSource over a string, handed out in pieces of 4 ---- */
typedef struct { IDWriteTextAnalysisSource iface; const WCHAR *text; UINT32 len; } Source;
static HRESULT STDMETHODCALLTYPE src_qi(IDWriteTextAnalysisSource *s, REFIID iid, void **out) { (void)iid; *out = s; return S_OK; }
static ULONG STDMETHODCALLTYPE src_addref(IDWriteTextAnalysisSource *s) { (void)s; return 1; }
static ULONG STDMETHODCALLTYPE src_release(IDWriteTextAnalysisSource *s) { (void)s; return 1; }
static HRESULT STDMETHODCALLTYPE src_text(IDWriteTextAnalysisSource *s, UINT32 pos, const WCHAR **text, UINT32 *len)
{
    Source *src = (Source *)s;
    if (pos >= src->len) { *text = NULL; *len = 0; return S_OK; }
    *text = src->text + pos;
    *len = src->len - pos < 4 ? src->len - pos : 4;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE src_before(IDWriteTextAnalysisSource *s, UINT32 pos, const WCHAR **text, UINT32 *len)
{
    Source *src = (Source *)s;
    *text = pos && pos <= src->len ? src->text : NULL;
    *len = pos <= src->len ? pos : 0;
    return S_OK;
}
static DWRITE_READING_DIRECTION STDMETHODCALLTYPE src_dir(IDWriteTextAnalysisSource *s) { (void)s; return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }
static HRESULT STDMETHODCALLTYPE src_locale(IDWriteTextAnalysisSource *s, UINT32 pos, UINT32 *len, const WCHAR **locale)
{
    Source *src = (Source *)s;
    *len = pos < src->len ? src->len - pos : 0;
    *locale = L"en-us";
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE src_subst(IDWriteTextAnalysisSource *s, UINT32 pos, UINT32 *len, IDWriteNumberSubstitution **n)
{
    Source *src = (Source *)s;
    *len = pos < src->len ? src->len - pos : 0;
    *n = NULL;
    return S_OK;
}
static IDWriteTextAnalysisSourceVtbl src_vtbl = { src_qi, src_addref, src_release, src_text, src_before, src_dir, src_locale, src_subst };

/* the Win32 family name of a font */
static void family_of(IDWriteFont *font, WCHAR *out, UINT32 cap)
{
    IDWriteLocalizedStrings *s = NULL;
    BOOL exists = FALSE;
    out[0] = 0;
    if (font && SUCCEEDED(IDWriteFont_GetInformationalStrings(font, DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, &s, &exists)) && exists) {
        IDWriteLocalizedStrings_GetString(s, 0, out, cap);
        IDWriteLocalizedStrings_Release(s);
    }
}

/* the path of a face reference's file */
static void path_of(IDWriteFontFaceReference *r, WCHAR *out, UINT32 cap)
{
    IDWriteFontFile *file = NULL;
    IDWriteFontFileLoader *loader = NULL;
    IDWriteLocalFontFileLoader *local = NULL;
    const void *key = NULL;
    UINT32 size = 0;
    out[0] = 0;
    if (FAILED(V(r, GetFontFile)(r, &file))) return;
    if (SUCCEEDED(IDWriteFontFile_GetLoader(file, &loader)) &&
        SUCCEEDED(IDWriteFontFileLoader_QueryInterface(loader, &IID_IDWriteLocalFontFileLoader_, (void **)&local)) &&
        SUCCEEDED(IDWriteFontFile_GetReferenceKey(file, &key, &size)))
        IDWriteLocalFontFileLoader_GetFilePathFromKey(local, key, size, out, cap);
    if (local) IDWriteLocalFontFileLoader_Release(local);
    if (loader) IDWriteFontFileLoader_Release(loader);
    IDWriteFontFile_Release(file);
}

/* the fonts a fallback picks for a string: (length, family) pairs */
static int map_all(IDWriteFontFallback *fb, const WCHAR *text, const WCHAR *base, UINT32 *lens, WCHAR fams[][64], int max)
{
    Source src = { { &src_vtbl }, text, (UINT32)wcslen(text) };
    int n = 0;
    for (UINT32 pos = 0; pos < src.len && n < max; n++) {
        UINT32 len = 0;
        IDWriteFont *font = NULL;
        FLOAT scale = 0;
        HRESULT hr = V(fb, MapCharacters)(fb, &src.iface, pos, src.len - pos, NULL, base, DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, &len, &font, &scale);
        if (FAILED(hr) || !len) return -1;
        lens[n] = len;
        if (font) { family_of(font, fams[n], 64); IDWriteFont_Release(font); }
        else wcscpy(fams[n], L"(none)");
        pos += len;
    }
    return n;
}

int main(void)
{
    IDWriteFactory *f = NULL;
    IDWriteFactory2 *f2 = NULL;
    IDWriteFactory3 *f3 = NULL;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory_, (IUnknown **)&f);
    CHECK("DWriteCreateFactory", SUCCEEDED(hr) && f);
    if (!f) { printf("dw3test: %d passed, %d failed\n", pass, fail + 1); return 1; }
    CHECK("factory is an IDWriteFactory2", SUCCEEDED(IDWriteFactory_QueryInterface(f, &IID_IDWriteFactory2_, (void **)&f2)) && f2);
    CHECK("factory is an IDWriteFactory3", SUCCEEDED(IDWriteFactory_QueryInterface(f, &IID_IDWriteFactory3_, (void **)&f3)) && f3);
    IDWriteFactory3 *direct = NULL;
    CHECK("DWriteCreateFactory gives an IDWriteFactory3",
          SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory3_, (IUnknown **)&direct)) && direct);
    if (direct) IDWriteFactory3_Release(direct);
    if (!f2 || !f3) { printf("dw3test: %d passed, %d failed\n", pass, fail); return 1; }

    /* ---- the system collection, as Chromium's font lookup table walks it ---- */
    IDWriteFontCollection *coll = NULL;
    CHECK("IDWriteFactory3 GetSystemFontCollection (IDWriteFactory's)",
          SUCCEEDED(V(f3, GetSystemFontCollection)(f3, &coll, FALSE)) && coll);
    UINT32 nfam = coll ? IDWriteFontCollection_GetFontFamilyCount(coll) : 0;
    UINT32 walked = 0, named = 0;
    for (UINT32 i = 0; i < nfam; i++) {
        IDWriteFontFamily *fam = NULL;
        if (FAILED(IDWriteFontCollection_GetFontFamily(coll, i, &fam))) continue;
        for (UINT32 k = 0; k < IDWriteFontFamily_GetFontCount(fam); k++) {
            IDWriteFont *font = NULL;
            IDWriteFontFace *face = NULL;
            IDWriteFontFile *file = NULL;
            UINT32 nfiles = 1;
            if (FAILED(IDWriteFontFamily_GetFont(fam, k, &font))) continue;
            if (SUCCEEDED(IDWriteFont_CreateFontFace(font, &face)) && SUCCEEDED(IDWriteFontFace_GetFiles(face, &nfiles, &file)))
                walked++;
            IDWriteLocalizedStrings *s1 = NULL, *s2 = NULL;
            BOOL e1 = FALSE, e2 = FALSE;
            IDWriteFont_GetInformationalStrings(font, DWRITE_INFORMATIONAL_STRING_FULL_NAME, &s1, &e1);
            IDWriteFont_GetInformationalStrings(font, DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &s2, &e2);
            if (e1 && e2) named++;
            if (s1) IDWriteLocalizedStrings_Release(s1);
            if (s2) IDWriteLocalizedStrings_Release(s2);
            if (file) IDWriteFontFile_Release(file);
            if (face) IDWriteFontFace_Release(face);
            IDWriteFont_Release(font);
        }
        IDWriteFontFamily_Release(fam);
    }
    CHECK("every system font has a face and a file", nfam && walked > 0);
    CHECK("every system font has a full and a PostScript name", named == walked);

    IDWriteFontCollection1 *c1 = NULL;
    CHECK("IDWriteFactory3 GetSystemFontCollection (IDWriteFontCollection1)",
          SUCCEEDED(V(f3, IDWriteFactory3_GetSystemFontCollection)(f3, FALSE, &c1, FALSE)) && c1);
    CHECK("the same families", c1 && V(c1, GetFontFamilyCount)(c1) == nfam);

    /* ---- IDWriteFontFamily1, IDWriteFont3, IDWriteFontFace3 ---- */
    UINT32 inter = 0;
    BOOL exists = FALSE;
    IDWriteFontFamily1 *fam1 = NULL;
    IDWriteFont3 *font3 = NULL;
    IDWriteFontFace3 *face3 = NULL;
    if (c1) V(c1, FindFamilyName)(c1, L"Inter", &inter, &exists);
    CHECK("Inter is a system family", exists);
    CHECK("IDWriteFontCollection1 GetFontFamily", exists && SUCCEEDED(V(c1, IDWriteFontCollection1_GetFontFamily)(c1, inter, &fam1)) && fam1);
    if (fam1) {
        CHECK("IDWriteFontFamily1 GetFontLocality is local", V(fam1, GetFontLocality)(fam1, 0) == DWRITE_LOCALITY_LOCAL);
        CHECK("IDWriteFontFamily1 GetFont (IDWriteFont3)", SUCCEEDED(V(fam1, IDWriteFontFamily1_GetFont)(fam1, 0, &font3)) && font3);
    }
    if (font3) {
        IDWriteFont3 *again = NULL;
        CHECK("IDWriteFont3 HasCharacter", V(font3, IDWriteFont3_HasCharacter)(font3, 'A') &&
                                           !V(font3, IDWriteFont3_HasCharacter)(font3, 0x0645));
        CHECK("IDWriteFont2 IsColorFont is false for Inter", !V(font3, IsColorFont)(font3));
        CHECK("IDWriteFont3 GetLocality is local", V(font3, GetLocality)(font3) == DWRITE_LOCALITY_LOCAL);
        V(fam1, IDWriteFontFamily1_GetFont)(fam1, 0, &again);
        CHECK("IDWriteFont3 Equals", again && V(font3, Equals)(font3, (IDWriteFont *)again));
        if (again) V(again, Release)(again);
        CHECK("IDWriteFont3 CreateFontFace (IDWriteFontFace3)", SUCCEEDED(V(font3, IDWriteFont3_CreateFontFace)(font3, &face3)) && face3);
    }
    if (face3) {
        IDWriteLocalizedStrings *names = NULL;
        WCHAR name[64] = L"";
        if (SUCCEEDED(V(face3, GetFamilyNames)(face3, &names))) {
            IDWriteLocalizedStrings_GetString(names, 0, name, 64);
            IDWriteLocalizedStrings_Release(names);
        }
        CHECK("IDWriteFontFace3 GetFamilyNames is Inter", !wcscmp(name, L"Inter"));
        CHECK("IDWriteFontFace3 HasCharacter", V(face3, HasCharacter)(face3, 'g'));
        CHECK("IDWriteFontFace3 IsCharacterLocal", V(face3, IsCharacterLocal)(face3, 'g'));
        DWRITE_COLOR_F color;
        CHECK("IDWriteFontFace2 has no palettes in Inter", V(face3, GetColorPaletteCount)(face3) == 0 &&
              V(face3, GetPaletteEntries)(face3, 0, 0, 1, &color) == DWRITE_E_NOCOLOR_);
        IDWriteFontFace2 *face2 = NULL;
        CHECK("font faces are IDWriteFontFace2",
              SUCCEEDED(V(face3, QueryInterface)(face3, &IID_IDWriteFontFace2_, (void **)&face2)) && face2);
        if (face2) V(face2, Release)(face2);
    }

    /* ---- font sets ---- */
    IDWriteFontSet *sys = NULL, *cset = NULL;
    CHECK("GetSystemFontSet", SUCCEEDED(V(f3, GetSystemFontSet)(f3, &sys)) && sys);
    CHECK("IDWriteFontCollection1 GetFontSet", c1 && SUCCEEDED(V(c1, GetFontSet)(c1, &cset)) && cset);
    UINT32 nset = sys ? V(sys, GetFontCount)(sys) : 0;
    CHECK("the system font set has the system fonts", nset >= 4 && cset && V(cset, GetFontCount)(cset) == nset);
    IDWriteStringList *fams = NULL;
    BOOL has_inter = FALSE;
    if (sys && SUCCEEDED(V(sys, GetPropertyValues__)(sys, DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, &fams))) {
        for (UINT32 i = 0; i < V(fams, GetCount)(fams); i++) {
            WCHAR v[64];
            if (SUCCEEDED(V(fams, GetString)(fams, i, v, 64)) && !wcscmp(v, L"Inter")) has_inter = TRUE;
        }
        V(fams, Release)(fams);
    }
    CHECK("font set family names include Inter", has_inter);

    /* Chromium matches unique (full and PostScript) names in the system font set (a font installed
     * under two file names is in it twice) */
    IDWriteFontSet *bold = NULL, *ps = NULL;
    DWRITE_FONT_PROPERTY full = { DWRITE_FONT_PROPERTY_ID_FULL_NAME, L"Inter Bold", L"" };
    DWRITE_FONT_PROPERTY psname = { DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME, L"Inter-Regular", L"" };
    CHECK("GetMatchingFonts by full name", sys && SUCCEEDED(V(sys, GetMatchingFonts)(sys, &full, 1, &bold)) &&
                                         V(bold, GetFontCount)(bold) >= 1 && V(bold, GetFontCount)(bold) < nset);
    CHECK("GetMatchingFonts by PostScript name", sys && SUCCEEDED(V(sys, GetMatchingFonts)(sys, &psname, 1, &ps)) &&
                                               V(ps, GetFontCount)(ps) >= 1 && V(ps, GetFontCount)(ps) < nset);
    UINT32 count = 0;
    CHECK("GetPropertyOccurrenceCount", sys && SUCCEEDED(V(sys, GetPropertyOccurrenceCount)(sys, &full, &count)) &&
                                     bold && count == V(bold, GetFontCount)(bold));
    IDWriteFontFaceReference *bref = NULL, *rref = NULL;
    IDWriteFontFace3 *bface = NULL;
    if (bold && SUCCEEDED(V(bold, GetFontFaceReference)(bold, 0, &bref)))
        V(bref, CreateFontFace)(bref, &bface);
    CHECK("the face reference makes Inter Bold", bface && V(bface, GetWeight)(bface) == DWRITE_FONT_WEIGHT_BOLD);
    if (ps) V(ps, GetFontFaceReference)(ps, 0, &rref);

    IDWriteFontSet *seg = NULL;
    IDWriteFontFaceReference *segref = NULL;
    CHECK("GetMatchingFonts by family: Segoe UI bold is Inter Bold",
          sys && SUCCEEDED(V(sys, GetMatchingFonts_)(sys, L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL,
                                                          DWRITE_FONT_STYLE_NORMAL, &seg)) &&
          V(seg, GetFontCount)(seg) >= 2 && SUCCEEDED(V(seg, GetFontFaceReference)(seg, 0, &segref)) &&
          bref && V(segref, Equals)(segref, bref));
    if (segref) V(segref, Release)(segref);
    if (seg) V(seg, Release)(seg);

    /* ---- face references by path and by file ---- */
    WCHAR path[MAX_PATH];
    IDWriteFontFaceReference *byPath = NULL;
    UINT32 idx = 99;
    exists = FALSE;
    if (bref) path_of(bref, path, MAX_PATH); else path[0] = 0;
    CHECK("CreateFontFaceReference from a path", path[0] &&
          SUCCEEDED(V(f3, CreateFontFaceReference)(f3, path, NULL, 0, DWRITE_FONT_SIMULATIONS_NONE, &byPath)) && byPath);
    CHECK("references to the same face are Equal", byPath && V(byPath, Equals)(byPath, bref));
    CHECK("FindFontFaceReference", byPath && SUCCEEDED(V(sys, FindFontFaceReference)(sys, byPath, &idx, &exists)) &&
                                   exists && idx < nset);
    CHECK("the reference knows its file", byPath && V(byPath, GetFileSize)(byPath) > 1000 &&
                                          V(byPath, GetLocality)(byPath) == DWRITE_LOCALITY_LOCAL);
    IDWriteFontFaceReference *oblique = NULL;
    IDWriteFontFile *file = NULL;
    if (byPath && SUCCEEDED(V(byPath, GetFontFile)(byPath, &file))) {
        V(f3, CreateFontFaceReference_)(f3, file, 0, DWRITE_FONT_SIMULATIONS_OBLIQUE, &oblique);
        IDWriteFontFile_Release(file);
    }
    CHECK("a reference with simulations is another face", oblique && !V(oblique, Equals)(oblique, byPath) &&
                                                         V(oblique, GetSimulations)(oblique) == DWRITE_FONT_SIMULATIONS_OBLIQUE);

    /* ---- a set builder, and a collection from its set ---- */
    IDWriteFontSetBuilder *b = NULL;
    IDWriteFontSet *mine = NULL;
    IDWriteFontCollection1 *mc = NULL;
    CHECK("CreateFontSetBuilder", SUCCEEDED(V(f3, CreateFontSetBuilder)(f3, &b)) && b);
    if (b) {
        if (bref) V(b, AddFontFaceReference)(b, bref);
        if (rref) V(b, AddFontFaceReference)(b, rref);
        CHECK("the built set has two fonts", SUCCEEDED(V(b, CreateFontSet)(b, &mine)) && mine && V(mine, GetFontCount)(mine) == 2);
    }
    CHECK("CreateFontCollectionFromFontSet", mine && SUCCEEDED(V(f3, CreateFontCollectionFromFontSet)(f3, mine, &mc)) && mc);
    if (mc) {
        IDWriteFontFamily *fam = NULL;
        exists = FALSE;
        V(mc, FindFamilyName)(mc, L"Inter", &idx, &exists);
        CHECK("the collection has Inter, regular and bold", exists && V(mc, GetFontFamilyCount)(mc) == 1 &&
              SUCCEEDED(V(mc, GetFontFamily)(mc, idx, &fam)) && IDWriteFontFamily_GetFontCount(fam) == 2);
        if (fam) IDWriteFontFamily_Release(fam);
    }

    /* ---- font fallback ---- */
    IDWriteFontFallback *fb = NULL;
    UINT32 lens[8];
    WCHAR names[8][64];
    CHECK("GetSystemFontFallback", SUCCEEDED(V(f2, GetSystemFontFallback)(f2, &fb)) && fb);
    if (fb) {
        int n = map_all(fb, L"Hello \x645\x631\x62D\x628\x627 world", L"Inter", lens, names, 8);
        CHECK("fallback: Latin stays in Inter", n == 3 && lens[0] == 6 && !wcscmp(names[0], L"Inter"));
        CHECK("fallback: Arabic goes to Noto Sans Arabic", n == 3 && lens[1] == 6 && !wcscmp(names[1], L"Noto Sans Arabic"));
        CHECK("fallback: back to Inter", n == 3 && lens[2] == 5 && !wcscmp(names[2], L"Inter"));
        n = map_all(fb, L"abc", L"Segoe UI", lens, names, 8);
        CHECK("fallback: Segoe UI is drawn with Inter", n == 1 && lens[0] == 3 && !wcscmp(names[0], L"Inter"));
        n = map_all(fb, L"\xDBFF\xDFFDx", L"Inter", lens, names, 8);
        CHECK("fallback: characters no font has map to no font", n == 2 && lens[0] == 2 && !wcscmp(names[0], L"(none)"));
    }
    IDWriteFontFallbackBuilder *fbb = NULL;
    IDWriteFontFallback *custom = NULL;
    CHECK("CreateFontFallbackBuilder", SUCCEEDED(V(f2, CreateFontFallbackBuilder)(f2, &fbb)) && fbb);
    if (fbb) {
        DWRITE_UNICODE_RANGE digits = { '0', '9' };
        const WCHAR *mono[] = { L"DejaVu Sans Mono" };
        V(fbb, AddMapping)(fbb, &digits, 1, mono, 1, NULL, NULL, NULL, 1.0f);
        if (V(fbb, CreateFontFallback)(fbb, &custom) == S_OK && custom) {
            int n = map_all(custom, L"42x", L"", lens, names, 8);
            CHECK("built fallback: its own mapping", n == 2 && lens[0] == 2 && !wcscmp(names[0], L"DejaVu Sans Mono") &&
                  !wcscmp(names[1], L"(none)"));
        } else CHECK("built fallback", 0);
        IDWriteFontFallbackBuilder *both = NULL;
        IDWriteFontFallback *withsys = NULL;
        V(f2, CreateFontFallbackBuilder)(f2, &both);
        if (both && custom && fb) {
            V(both, AddMappings)(both, custom);
            V(both, AddMappings)(both, fb);
            V(both, CreateFontFallback)(both, &withsys);
        }
        int n = withsys ? map_all(withsys, L"42x", L"", lens, names, 8) : 0;
        CHECK("built fallback with the system's after it", n == 2 && !wcscmp(names[0], L"DejaVu Sans Mono") && wcscmp(names[1], L"(none)"));
        if (withsys) V(withsys, Release)(withsys);
        if (both) V(both, Release)(both);
    }

    /* ---- glyph rendering as Skia asks for it ---- */
    if (face3) {
        UINT16 g[2];
        UINT32 cps[2] = { 'O', 'k' };
        IDWriteFontFace_GetGlyphIndices((IDWriteFontFace *)face3, cps, 2, g);
        DWRITE_GLYPH_RUN run = { (IDWriteFontFace *)face3, 24.0f, 2, g, NULL, NULL, FALSE, 0 };
        IDWriteGlyphRunAnalysis *a = NULL;
        RECT r1 = { 0 }, r3 = { 1, 1, 1, 1 };
        hr = V(f2, IDWriteFactory2_CreateGlyphRunAnalysis)(f2, &run, NULL, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                                DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DISABLED,
                                                                DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 20, &a);
        CHECK("IDWriteFactory2 CreateGlyphRunAnalysis (grayscale)", SUCCEEDED(hr) && a);
        int partial = 0;
        if (a) {
            IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(a, DWRITE_TEXTURE_ALIASED_1x1, &r1);
            IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(a, DWRITE_TEXTURE_CLEARTYPE_3x1, &r3);
            UINT32 w = r1.right - r1.left, h = r1.bottom - r1.top;
            static BYTE tex[256 * 256];
            if (w && h && w * h <= sizeof(tex) &&
                SUCCEEDED(IDWriteGlyphRunAnalysis_CreateAlphaTexture(a, DWRITE_TEXTURE_ALIASED_1x1, &r1, tex, w * h)))
                for (UINT32 i = 0; i < w * h; i++) if (tex[i] > 0 && tex[i] < 255) partial++;
            IDWriteGlyphRunAnalysis_Release(a);
        }
        CHECK("grayscale coverage is in the 1x1 texture", r1.right > r1.left && r1.bottom > r1.top && partial > 0);
        CHECK("and not in the ClearType one", r3.right <= r3.left);
        IDWriteColorGlyphRunEnumerator *e = NULL;
        CHECK("TranslateColorGlyphRun: Inter has no color glyphs",
              V(f2, TranslateColorGlyphRun)(f2, 0, 0, &run, NULL, DWRITE_MEASURING_MODE_NATURAL, NULL, 0, &e) == DWRITE_E_NOCOLOR_);
        IDWriteGlyphRunAnalysis *a3 = NULL;
        CHECK("IDWriteFactory3 CreateGlyphRunAnalysis", SUCCEEDED(V(f3, IDWriteFactory3_CreateGlyphRunAnalysis)(f3, &run, NULL,
              DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED, DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DEFAULT,
              DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 20, &a3)) && a3);
        if (a3) IDWriteGlyphRunAnalysis_Release(a3);
    }
    IDWriteRenderingParams3 *p3 = NULL;
    CHECK("IDWriteFactory3 CreateCustomRenderingParams", SUCCEEDED(V(f3, IDWriteFactory3_CreateCustomRenderingParams)(f3,
          1.8f, 0.5f, 1.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED,
          DWRITE_GRID_FIT_MODE_DISABLED, &p3)) && p3);
    if (p3) {
        CHECK("rendering params 3 keep their mode and grid fit",
              V(p3, GetRenderingMode1)(p3) == DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED &&
              V(p3, GetRenderingMode)(p3) == DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC &&
              V(p3, GetGridFitMode)(p3) == DWRITE_GRID_FIT_MODE_DISABLED);
        V(p3, Release)(p3);
    }
    IDWriteRenderingParams *rp = NULL;
    IDWriteRenderingParams3 *rp3 = NULL;
    CHECK("default rendering params are IDWriteRenderingParams3", SUCCEEDED(IDWriteFactory_CreateRenderingParams(f, &rp)) &&
          SUCCEEDED(IDWriteRenderingParams_QueryInterface(rp, &IID_IDWriteRenderingParams3_, (void **)&rp3)) &&
          V(rp3, GetGridFitMode)(rp3) == DWRITE_GRID_FIT_MODE_DEFAULT);
    if (rp3) V(rp3, Release)(rp3);
    if (rp) IDWriteRenderingParams_Release(rp);
    IDWriteRenderingParams2 *bad = NULL;
    CHECK("CreateCustomRenderingParams rejects a bad mode", V(f2, IDWriteFactory2_CreateCustomRenderingParams)(f2,
          1.8f, 0.5f, 1.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, 99, DWRITE_GRID_FIT_MODE_DEFAULT, &bad) == E_INVALIDARG);

    IDWriteFontDownloadQueue *q = NULL;
    CHECK("the download queue is empty", SUCCEEDED(V(f3, GetFontDownloadQueue)(f3, &q)) && q && V(q, IsEmpty)(q));
    if (q) V(q, Release)(q);

    printf("dw3test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
