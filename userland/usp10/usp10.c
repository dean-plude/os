/*
 * usp10.dll — Uniscribe on HarfBuzz.  Text is itemized into runs of one
 * script and one bidirectional level (a compact UAX #9: strong, number and
 * neutral classes), each run is shaped by HarfBuzz from novatext.dll with
 * the DC's font (its file, read through GetFontData), and the
 * ScriptString* layer picks a fallback font for scripts the DC's font
 * lacks, orders the runs visually and draws them through gdi32 as glyph
 * indices.  gdi32's ExtTextOut sends complex text here, as Windows' LPK
 * does.
 */
#include <windows.h>
#include <string.h>
#include <hb.h>
#include <hb-ot.h>

#define USP __declspec(dllexport)

int _fltused = 0x9875;          /* floating point in use (the font scale) */

GDIAPI DWORD GetGlyphIndicesW(HDC h, LPCWSTR s, int n, LPWORD out, DWORD flags);
GDIAPI BOOL GetCharWidthI(HDC h, UINT first, UINT n, LPWORD gi, LPINT out);
GDIAPI BOOL GetCharWidth32W(HDC h, UINT first, UINT last, LPINT out);
GDIAPI HGDIOBJ GetCurrentObject(HDC h, UINT type);
GDIAPI int GetObjectW(HGDIOBJ h, int n, LPVOID out);
GDIAPI BOOL ExtTextOutW(HDC h, int x, int y, UINT opts, const RECT *rc, LPCWSTR s, UINT n, const INT *dx);
GDIAPI COLORREF SetTextColor(HDC h, COLORREF c);
GDIAPI COLORREF SetBkColor(HDC h, COLORREF c);
GDIAPI int SetBkMode(HDC h, int mode);
GDIAPI int GetBkMode(HDC h);
GDIAPI DWORD GetFontData(HDC h, DWORD table, DWORD off, LPVOID buf, DWORD n);
GDIAPI UINT SetTextAlign(HDC h, UINT align);
GDIAPI UINT GetTextAlign(HDC h);

#define OBJ_FONT_          6
#define ETO_OPAQUE_        0x0002
#define ETO_CLIPPED_       0x0004
#define ETO_GLYPH_INDEX_   0x0010
#define OPAQUE_            2
#define TRANSPARENT_       1
#define TA_BASELINE_       24
#define TA_BOTTOM_         8
#define COLOR_HIGHLIGHTTEXT_ 14
#define E_PENDING_         ((HRESULT)0x8000000AL)
#define USP_E_SCRIPT_NOT_IN_FONT_ ((HRESULT)0x80040200L)

#define SSA_PASSWORD   0x00000001
#define SSA_TAB        0x00000002
#define SSA_CLIP       0x00000004
#define SSA_FIT        0x00000010
#define SSA_FALLBACK   0x00000020
#define SSA_BREAK      0x00000040
#define SSA_GLYPHS     0x00000080
#define SSA_RTL        0x00000100

#define SIC_COMPLEX    1
#define SIC_ASCIIDIGIT 2
#define SIC_NEUTRAL    4

/* -----------------------------------------------------------------------
 * The structures (bit-for-bit as in usp10.h)
 * ----------------------------------------------------------------------- */
typedef void *SCRIPT_CACHE;
typedef struct { DWORD uDefaultLanguage : 16, fContextDigits : 1, fInvertPreBoundDir : 1, fInvertPostBoundDir : 1,
                 fLinkStringBefore : 1, fLinkStringAfter : 1, fNeutralOverride : 1, fNumericOverride : 1,
                 fLegacyBidiClass : 1, fMergeNeutralItems : 1, fUseStandardBidi : 1, fReserved : 6; } SCRIPT_CONTROL;
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
typedef struct { int abcA; UINT abcB; int abcC; } ABC_;
typedef struct { BYTE fSoftBreak : 1, fWhiteSpace : 1, fCharStop : 1, fWordStop : 1, fInvalid : 1, fReserved : 3; } SCRIPT_LOGATTR;
typedef struct { DWORD langid : 16, fNumeric : 1, fComplex : 1, fNeedsWordBreaking : 1, fNeedsCaretInfo : 1,
                 bCharSet : 8, fControl : 1, fPrivateUseArea : 1, fNeedsCharacterJustify : 1,
                 fInvalidGlyph : 1, fInvalidLogAttr : 1, fCDM : 1, fAmbiguousCharSet : 1,
                 fClusterSizeVaries : 1, fRejectInvalid : 1; } SCRIPT_PROPERTIES;
typedef struct { int cBytes; WORD wgBlank, wgDefault, wgInvalid, wgKashida; int iKashidaWidth; } SCRIPT_FONTPROPERTIES;
typedef struct { DWORD NationalDigitLanguage : 16, TraditionalDigitLanguage : 16;
                 DWORD DigitSubstitute : 8, dwReserved; } SCRIPT_DIGITSUBSTITUTE;
typedef struct { int cTabStops, iScale; int *pTabStops; int iTabOrigin; } SCRIPT_TABDEF;
typedef ULONG OPENTYPE_TAG;
typedef struct { OPENTYPE_TAG tagFeature; LONG lParameter; } OPENTYPE_FEATURE_RECORD;
typedef struct { OPENTYPE_FEATURE_RECORD *potfRecords; int cotfRecords; } TEXTRANGE_PROPERTIES;
typedef struct { WORD fCanGlyphAlone : 1, reserved : 15; } SCRIPT_CHARPROP;
typedef struct { SCRIPT_VISATTR sva; WORD reserved; } SCRIPT_GLYPHPROP;

#define SCRIPT_UNDEFINED 0
#define SCRIPT_JUSTIFY_NONE      0
#define SCRIPT_JUSTIFY_CHARACTER 2
#define SCRIPT_JUSTIFY_BLANK     4

static void *u_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void u_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* -----------------------------------------------------------------------
 * Scripts: eScript indexes this table
 * ----------------------------------------------------------------------- */
typedef struct {
    hb_script_t hb;
    WORD lang;                      /* the primary language */
    BYTE complex, caret, words;     /* shaped; needs caret info; needs word breaking */
    const WCHAR *fallback;          /* the face GDI draws it with when the DC's font lacks it */
} Script;

static const Script g_scripts[] = {
    { HB_SCRIPT_COMMON,     0x00, 0, 0, 0, 0 },                         /* SCRIPT_UNDEFINED */
    { HB_SCRIPT_LATIN,      0x09, 0, 0, 0, 0 },
    { HB_SCRIPT_ARABIC,     0x01, 1, 0, 0, L"Noto Sans Arabic" },
    { HB_SCRIPT_HEBREW,     0x0D, 1, 0, 0, 0 },
    { HB_SCRIPT_DEVANAGARI, 0x39, 1, 1, 0, L"Noto Sans Devanagari" },
    { HB_SCRIPT_BENGALI,    0x45, 1, 1, 0, 0 },
    { HB_SCRIPT_GURMUKHI,   0x46, 1, 1, 0, 0 },
    { HB_SCRIPT_GUJARATI,   0x47, 1, 1, 0, 0 },
    { HB_SCRIPT_ORIYA,      0x48, 1, 1, 0, 0 },
    { HB_SCRIPT_TAMIL,      0x49, 1, 1, 0, 0 },
    { HB_SCRIPT_TELUGU,     0x4A, 1, 1, 0, 0 },
    { HB_SCRIPT_KANNADA,    0x4B, 1, 1, 0, 0 },
    { HB_SCRIPT_MALAYALAM,  0x4C, 1, 1, 0, 0 },
    { HB_SCRIPT_SINHALA,    0x5B, 1, 1, 0, 0 },
    { HB_SCRIPT_THAI,       0x1E, 1, 1, 1, 0 },
    { HB_SCRIPT_LAO,        0x54, 1, 1, 1, 0 },
    { HB_SCRIPT_TIBETAN,    0x51, 1, 1, 0, 0 },
    { HB_SCRIPT_MYANMAR,    0x55, 1, 1, 1, 0 },
    { HB_SCRIPT_KHMER,      0x53, 1, 1, 1, 0 },
    { HB_SCRIPT_SYRIAC,     0x5A, 1, 0, 0, 0 },
    { HB_SCRIPT_THAANA,     0x65, 1, 0, 0, 0 },
    { HB_SCRIPT_MONGOLIAN,  0x50, 1, 0, 0, 0 },
    { HB_SCRIPT_GREEK,      0x08, 0, 0, 0, 0 },
    { HB_SCRIPT_CYRILLIC,   0x19, 0, 0, 0, 0 },
    { HB_SCRIPT_ARMENIAN,   0x2B, 0, 0, 0, 0 },
    { HB_SCRIPT_GEORGIAN,   0x37, 0, 0, 0, 0 },
    { HB_SCRIPT_HANGUL,     0x12, 0, 0, 0, 0 },
    { HB_SCRIPT_HAN,        0x04, 0, 0, 0, 0 },
    { HB_SCRIPT_HIRAGANA,   0x11, 0, 0, 0, 0 },
    { HB_SCRIPT_KATAKANA,   0x11, 0, 0, 0, 0 },
    { HB_SCRIPT_ETHIOPIC,   0x5E, 0, 0, 0, 0 },
};
#define NSCRIPTS ((int)(sizeof(g_scripts) / sizeof(g_scripts[0])))

static SCRIPT_PROPERTIES g_props[NSCRIPTS];
static const SCRIPT_PROPERTIES *g_props_tab[NSCRIPTS];

static int script_index(hb_script_t s)
{
    for (int i = 1; i < NSCRIPTS; i++) if (g_scripts[i].hb == s) return i;
    return SCRIPT_UNDEFINED;
}

USP HRESULT WINAPI ScriptGetProperties(const SCRIPT_PROPERTIES ***props, int *n)
{
    if (!props && !n) return E_INVALIDARG;
    if (!g_props_tab[0])
        for (int i = 0; i < NSCRIPTS; i++) {
            SCRIPT_PROPERTIES *p = &g_props[i];
            p->langid = g_scripts[i].lang;
            p->fComplex = g_scripts[i].complex;
            p->fNeedsCaretInfo = g_scripts[i].caret;
            p->fNeedsWordBreaking = g_scripts[i].words;
            p->fClusterSizeVaries = g_scripts[i].caret;
            p->bCharSet = g_scripts[i].hb == HB_SCRIPT_ARABIC ? 178 : g_scripts[i].hb == HB_SCRIPT_HEBREW ? 177 :
                          g_scripts[i].hb == HB_SCRIPT_THAI ? 222 : g_scripts[i].hb == HB_SCRIPT_GREEK ? 161 :
                          g_scripts[i].hb == HB_SCRIPT_CYRILLIC ? 204 : g_scripts[i].complex ? 1 : 0;
            g_props_tab[i] = p;
        }
    if (props) *props = g_props_tab;
    if (n) *n = NSCRIPTS;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * Characters
 * ----------------------------------------------------------------------- */
static hb_unicode_funcs_t *ufuncs(void) { return hb_unicode_funcs_get_default(); }

/* The code point at @s[i], and how many units it takes */
static UINT32 cp_at(const WCHAR *s, int n, int i, int *len)
{
    UINT32 c = s[i];
    *len = 1;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
        *len = 2;
    }
    return c;
}

static int zero_width(UINT32 c)
{
    return c < 0x20 || c == 0x200B || c == 0x200C || c == 0x200D || c == 0x200E || c == 0x200F ||
           c == 0xFEFF || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

static int is_space(UINT32 c) { return c == ' ' || c == '\t' || c == 0x3000 || c == 0xA0 || c == 0x2009 || c == 0x200A; }

static int is_mark(UINT32 c)
{
    hb_unicode_general_category_t g = hb_unicode_general_category(ufuncs(), c);
    return g == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK || g == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK ||
           g == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
}

/* bidirectional classes, as far as this layout needs them */
enum { B_L, B_R, B_EN, B_AN, B_N };
static int bidi_class(UINT32 c)
{
    if (c >= '0' && c <= '9') return B_EN;
    if ((c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9)) return B_AN;
    if (c == 0x200F || c == 0x061C) return B_R;              /* RLM, ALM */
    if (c == 0x200E) return B_L;                             /* LRM */
    hb_script_t s = hb_unicode_script(ufuncs(), c);
    if (s == HB_SCRIPT_COMMON || s == HB_SCRIPT_INHERITED || s == HB_SCRIPT_UNKNOWN) return B_N;
    return hb_script_get_horizontal_direction(s) == HB_DIRECTION_RTL ? B_R : B_L;
}

/* -----------------------------------------------------------------------
 * Itemizing: runs of one script at one level
 * ----------------------------------------------------------------------- */
static HRESULT itemize(const WCHAR *s, int n, int max, const SCRIPT_CONTROL *ctl, const SCRIPT_STATE *st,
                       SCRIPT_ITEM *items, ULONG *tags, int *count)
{
    (void)ctl;
    if (!s || n <= 0 || !items || max < 2) return E_INVALIDARG;
    int base = st ? st->uBidiLevel : 0;
    BYTE *cls = u_alloc(n), *lvl = u_alloc(n);
    short *scr = u_alloc(n * sizeof(short));
    if (!cls || !lvl || !scr) { u_free(cls); u_free(lvl); u_free(scr); return E_OUTOFMEMORY; }

    /* scripts per unit; marks and neutrals take the script around them */
    for (int i = 0; i < n;) {
        int len;
        UINT32 c = cp_at(s, n, i, &len);
        hb_script_t hs = hb_unicode_script(ufuncs(), c);
        int k = hs == HB_SCRIPT_COMMON || hs == HB_SCRIPT_INHERITED || hs == HB_SCRIPT_UNKNOWN ? -1 : script_index(hs);
        if (k == SCRIPT_UNDEFINED && hs != HB_SCRIPT_COMMON) k = 1;   /* other scripts: shaped as simple text */
        int b = bidi_class(c);
        if (b == B_EN || b == B_AN) k = -1;                  /* digits go with the run they are in */
        for (int j = 0; j < len; j++) { scr[i + j] = (short)k; cls[i + j] = (BYTE)b; }
        i += len;
    }
    int prev = -1;
    for (int i = 0; i < n; i++) if (scr[i] < 0) scr[i] = (short)prev; else prev = scr[i];
    int next = -1;
    for (int i = n - 1; i >= 0; i--) if (scr[i] < 0) scr[i] = (short)(next >= 0 ? next : SCRIPT_UNDEFINED); else next = scr[i];

    /* levels: numbers after R are R-like (W2); neutrals between like
     * strengths take it (N1), else the embedding direction (N2); then the
     * implicit levels (I1, I2) */
    int last_strong = base & 1 ? B_R : B_L;
    for (int i = 0; i < n; i++) {
        if (cls[i] == B_L || cls[i] == B_R) last_strong = cls[i];
        else if (cls[i] == B_EN && last_strong == B_R) cls[i] = B_AN;
    }
    for (int i = 0; i < n;) {
        if (cls[i] != B_N) { i++; continue; }
        int j = i;
        while (j < n && cls[j] == B_N) j++;
        int before = i > 0 ? (cls[i - 1] == B_L ? B_L : B_R) : (base & 1 ? B_R : B_L);
        int after = j < n ? (cls[j] == B_L || cls[j] == B_EN ? B_L : B_R) : (base & 1 ? B_R : B_L);
        if (i > 0 && cls[i - 1] == B_EN) before = B_L;
        int d = before == after ? before : (base & 1 ? B_R : B_L);
        for (int k = i; k < j; k++) cls[k] = (BYTE)d;
        i = j;
    }
    for (int i = 0; i < n; i++) {
        int c = cls[i];
        if (base & 1) lvl[i] = (BYTE)(c == B_R ? base : base + 1);
        else lvl[i] = (BYTE)(c == B_L ? base : c == B_R ? base + 1 : base + 2);
    }

    int k = 0;
    HRESULT hr = S_OK;
    for (int i = 0; i < n; i++) {
        if (i && scr[i] == scr[i - 1] && lvl[i] == lvl[i - 1]) continue;
        if (s[i] >= 0xDC00 && s[i] < 0xE000 && i) continue;  /* never split a surrogate pair */
        if (k == max - 1) { hr = E_OUTOFMEMORY; break; }
        memset(&items[k], 0, sizeof(items[k]));
        items[k].iCharPos = i;
        items[k].a.eScript = (WORD)scr[i];
        if (st) items[k].a.s = *st;
        items[k].a.s.uBidiLevel = lvl[i];
        items[k].a.fRTL = items[k].a.fLayoutRTL = lvl[i] & 1;
        if (tags) {
            hb_tag_t t[2];
            unsigned int nt = 2;
            hb_ot_tags_from_script_and_language(g_scripts[scr[i]].hb, 0, &nt, t, 0, 0);
            hb_tag_t tag = nt ? t[0] : HB_TAG('D', 'F', 'L', 'T');
            if (scr[i] == SCRIPT_UNDEFINED) tag = HB_TAG('D', 'F', 'L', 'T');
            tags[k] = (tag >> 24) | (tag >> 8 & 0xFF00) | (tag << 8 & 0xFF0000) | (tag << 24);
        }
        k++;
    }
    if (SUCCEEDED(hr)) {
        memset(&items[k], 0, sizeof(items[k]));
        items[k].iCharPos = n;
        if (count) *count = k;
    }
    u_free(cls); u_free(lvl); u_free(scr);
    return hr;
}

USP HRESULT WINAPI ScriptItemize(const WCHAR *s, int n, int max, const SCRIPT_CONTROL *ctl, const SCRIPT_STATE *st,
                                 SCRIPT_ITEM *items, int *count)
{
    return itemize(s, n, max, ctl, st, items, 0, count);
}

USP HRESULT WINAPI ScriptItemizeOpenType(const WCHAR *s, int n, int max, const SCRIPT_CONTROL *ctl,
                                         const SCRIPT_STATE *st, SCRIPT_ITEM *items, ULONG *tags, int *count)
{
    return itemize(s, n, max, ctl, st, items, tags, count);
}

USP HRESULT WINAPI ScriptIsComplex(const WCHAR *s, int n, DWORD flags)
{
    if (!s || n <= 0) return E_INVALIDARG;
    for (int i = 0; i < n;) {
        int len;
        UINT32 c = cp_at(s, n, i, &len);
        i += len;
        if ((flags & SIC_ASCIIDIGIT) && c >= '0' && c <= '9') return S_OK;
        hb_script_t hs = hb_unicode_script(ufuncs(), c);
        if ((flags & SIC_NEUTRAL) && (hs == HB_SCRIPT_COMMON || hs == HB_SCRIPT_INHERITED) && !(c >= '0' && c <= '9'))
            continue;
        if ((flags & SIC_COMPLEX) && (g_scripts[script_index(hs)].complex || c == 0x200C || c == 0x200D ||
                                      c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E)))
            return S_OK;
    }
    return S_FALSE;
}

/* -----------------------------------------------------------------------
 * The script cache: the DC's font as HarfBuzz sees it (its file, through
 * GetFontData, at gdi32's pixel size) and the last runs shaped, whose
 * positions ScriptPlace hands out
 * ----------------------------------------------------------------------- */
#define RUNS 8
typedef struct { int n; WORD *glyphs; int *adv; GOFFSET *off; } Run;
typedef struct {
    LOGFONTW lf;
    TEXTMETRICW tm;
    HDC mdc;
    HFONT font;
    HGDIOBJ old;
    hb_font_t *hb;                  /* 0: the font file could not be read */
    int upem;
    Run runs[RUNS];
    int next_run;
} Cache;

static void free_run(Run *r) { u_free(r->glyphs); u_free(r->adv); u_free(r->off); memset(r, 0, sizeof(*r)); }

/* HarfBuzz's font for the DC's: gdi32 draws a negative LOGFONT height as
 * the em and a positive one as ascent minus descent (hhea) */
static void cache_hb(Cache *c, HDC dc)
{
    DWORD size = GetFontData(dc, 0, 0, 0, 0);
    if (!size || size == 0xFFFFFFFF) return;
    void *data = HeapAlloc(GetProcessHeap(), 0, size);
    if (!data) return;
    if (GetFontData(dc, 0, 0, data, size) != size) { u_free(data); return; }
    hb_blob_t *blob = hb_blob_create(data, size, HB_MEMORY_MODE_READONLY, data, u_free);
    hb_face_t *face = hb_face_create(blob, 0);
    hb_blob_destroy(blob);
    c->upem = (int)hb_face_get_upem(face);
    c->hb = hb_font_create(face);
    hb_face_destroy(face);
    int h = c->lf.lfHeight ? c->lf.lfHeight : -12;
    if (h > 2000) h = 2000;
    if (h < -2000) h = -2000;
    double em = -h;
    if (h > 0) {
        hb_blob_t *t = hb_face_reference_table(hb_font_get_face(c->hb), HB_TAG('h', 'h', 'e', 'a'));
        unsigned int tl;
        const unsigned char *p = (const unsigned char *)hb_blob_get_data(t, &tl);
        int asc = tl >= 8 ? (short)(p[4] << 8 | p[5]) : c->upem, desc = tl >= 8 ? (short)(p[6] << 8 | p[7]) : 0;
        em = asc - desc > 0 ? (double)h * c->upem / (asc - desc) : h;
        hb_blob_destroy(t);
    }
    int sc = (int)(em * 64 + 0.5);
    hb_font_set_scale(c->hb, sc, sc);
}

/* The DC to measure with: the caller's, or the cache's own */
static HRESULT cache_dc(HDC hdc, SCRIPT_CACHE *psc, HDC *out)
{
    if (!psc) return E_INVALIDARG;
    Cache *c = *psc;
    if (!c) {
        if (!hdc) return E_PENDING_;
        c = u_alloc(sizeof(*c));
        if (!c) return E_OUTOFMEMORY;
        GetObjectW(GetCurrentObject(hdc, OBJ_FONT_), sizeof(c->lf), &c->lf);
        GetTextMetricsW(hdc, &c->tm);
        cache_hb(c, hdc);
        *psc = c;
    }
    if (hdc) { *out = hdc; return S_OK; }
    if (!c->mdc) {
        c->mdc = CreateCompatibleDC(0);
        c->font = CreateFontIndirectW(&c->lf);
        if (!c->mdc || !c->font) return E_OUTOFMEMORY;
        c->old = SelectObject(c->mdc, c->font);
    }
    *out = c->mdc;
    return S_OK;
}

USP HRESULT WINAPI ScriptFreeCache(SCRIPT_CACHE *psc)
{
    if (!psc) return E_INVALIDARG;
    Cache *c = *psc;
    if (c) {
        if (c->mdc) { SelectObject(c->mdc, c->old); DeleteDC(c->mdc); }
        if (c->font) DeleteObject(c->font);
        if (c->hb) hb_font_destroy(c->hb);
        for (int i = 0; i < RUNS; i++) free_run(&c->runs[i]);
        u_free(c);
        *psc = 0;
    }
    return S_OK;
}

USP HRESULT WINAPI ScriptCacheGetHeight(HDC hdc, SCRIPT_CACHE *psc, LONG *height)
{
    HDC dc;
    if (!height) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    *height = ((Cache *)*psc)->tm.tmHeight;
    return S_OK;
}

static int px(hb_position_t v) { return (int)((v + (v >= 0 ? 32 : -31)) >> 6); }

static int glyph_advance(Cache *c, HDC dc, WORD g)
{
    if (c->hb) return px(hb_font_get_glyph_h_advance(c->hb, g));
    INT w = 0;
    GetCharWidthI(dc, 0, 1, &g, &w);
    return w;
}

static WORD nominal_glyph(Cache *c, HDC dc, UINT32 cp)
{
    hb_codepoint_t g = 0;
    if (c->hb) return hb_font_get_nominal_glyph(c->hb, cp, &g) ? (WORD)g : 0;
    WCHAR w = (WCHAR)cp;
    WORD gi = 0;
    GetGlyphIndicesW(dc, &w, 1, &gi, 0);
    return gi;
}

USP HRESULT WINAPI ScriptGetFontProperties(HDC hdc, SCRIPT_CACHE *psc, SCRIPT_FONTPROPERTIES *fp)
{
    HDC dc;
    if (!fp || fp->cBytes != sizeof(*fp)) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    Cache *c = *psc;
    fp->wgBlank = nominal_glyph(c, dc, ' ');
    fp->wgDefault = 0;
    fp->wgInvalid = 0;
    WORD k = nominal_glyph(c, dc, 0x0640);                  /* the Arabic tatweel */
    fp->wgKashida = k ? k : 0xFFFF;
    fp->iKashidaWidth = k ? glyph_advance(c, dc, k) : 0;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * Shaping
 * ----------------------------------------------------------------------- */
static void remember_run(Cache *c, const WORD *g, const int *adv, const GOFFSET *off, int n)
{
    Run *r = &c->runs[c->next_run];
    c->next_run = (c->next_run + 1) % RUNS;
    free_run(r);
    r->glyphs = u_alloc(n * sizeof(WORD));
    r->adv = u_alloc(n * sizeof(int));
    r->off = u_alloc(n * sizeof(GOFFSET));
    if (!r->glyphs || !r->adv || !r->off) { free_run(r); return; }
    memcpy(r->glyphs, g, n * sizeof(WORD));
    memcpy(r->adv, adv, n * sizeof(int));
    memcpy(r->off, off, n * sizeof(GOFFSET));
    r->n = n;
}

static const Run *find_run(Cache *c, const WORD *g, int n)
{
    for (int i = 0; i < RUNS; i++) {
        int k = (c->next_run + RUNS - 1 - i) % RUNS;
        if (c->runs[k].n == n && !memcmp(c->runs[k].glyphs, g, n * sizeof(WORD))) return &c->runs[k];
    }
    return 0;
}

/* Shape @s with HarfBuzz; glyphs in visual order unless fLogicalOrder */
static HRESULT shape(HDC hdc, SCRIPT_CACHE *psc, const WCHAR *s, int n, int max, SCRIPT_ANALYSIS *sa,
                     const hb_feature_t *feat, unsigned int nfeat, hb_tag_t lang_tag,
                     WORD *glyphs, WORD *clust, SCRIPT_VISATTR *va, int *count)
{
    HDC dc;
    if (!s || n <= 0 || !glyphs || !clust || !count) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    Cache *c = *psc;
    int script = sa && sa->eScript < NSCRIPTS ? sa->eScript : SCRIPT_UNDEFINED;
    int rtl = sa && sa->fRTL;

    if ((sa && sa->fNoGlyphIndex) || !c->hb) {              /* characters as glyphs, one each */
        if (max < n) return E_OUTOFMEMORY;
        if (sa && sa->fNoGlyphIndex) memcpy(glyphs, s, n * sizeof(WORD));
        else GetGlyphIndicesW(dc, s, n, glyphs, 0);
        for (int i = 0; i < n; i++) {
            clust[i] = (WORD)i;
            if (va) {
                memset(&va[i], 0, sizeof(va[i]));
                va[i].fClusterStart = 1;
                va[i].fZeroWidth = zero_width(s[i]);
                va[i].uJustification = s[i] == ' ' ? SCRIPT_JUSTIFY_BLANK : SCRIPT_JUSTIFY_CHARACTER;
            }
        }
        *count = n;
        return S_OK;
    }

    hb_buffer_t *b = hb_buffer_create();
    hb_buffer_add_utf16(b, (const uint16_t *)s, n, 0, n);
    hb_buffer_set_direction(b, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    if (script != SCRIPT_UNDEFINED) hb_buffer_set_script(b, g_scripts[script].hb);
    if (lang_tag) hb_buffer_set_language(b, hb_ot_tag_to_language(lang_tag));
    hb_buffer_guess_segment_properties(b);
    hb_feature_t liga_off[2] = { { HB_TAG('l', 'i', 'g', 'a'), 0, 0, (unsigned)-1 },
                                 { HB_TAG('c', 'l', 'i', 'g'), 0, 0, (unsigned)-1 } };
    if (!feat && sa && sa->s.fInhibitLigate) { feat = liga_off; nfeat = 2; }
    hb_shape(c->hb, b, feat, nfeat);
    unsigned int m;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(b, &m);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(b, &m);
    if ((int)m > max) { hb_buffer_destroy(b); return E_OUTOFMEMORY; }

    int missing = 0;
    for (unsigned int i = 0; i < m; i++) {
        unsigned int cl = info[i].cluster;
        if (!info[i].codepoint && cl < (unsigned)n && !zero_width(s[cl]) && !is_space(s[cl])) missing = 1;
    }
    if (missing && g_scripts[script].complex) { hb_buffer_destroy(b); return USP_E_SCRIPT_NOT_IN_FONT_; }

    int logical = rtl && sa && sa->fLogicalOrder;
    if (logical) hb_buffer_reverse(b);                      /* back to logical order */
    info = hb_buffer_get_glyph_infos(b, &m);
    pos = hb_buffer_get_glyph_positions(b, &m);

    int *adv = u_alloc(m * sizeof(int));
    GOFFSET *off = u_alloc(m * sizeof(GOFFSET));
    if (!adv || !off) { u_free(adv); u_free(off); hb_buffer_destroy(b); return E_OUTOFMEMORY; }
    for (unsigned int i = 0; i < m; i++) {
        glyphs[i] = (WORD)info[i].codepoint;
        adv[i] = px(pos[i].x_advance);
        off[i].du = px(pos[i].x_offset);
        off[i].dv = px(pos[i].y_offset);
    }

    /* clusters: every character maps to the glyph that starts its cluster
     * in logical order (the leftmost of a left-to-right run, the rightmost
     * of a visually ordered right-to-left one) */
    int visual_rtl = rtl && !logical;
    for (int ch = 0; ch < n; ch++) clust[ch] = 0xFFFF;
    for (unsigned int i = 0; i < m; i++) {
        unsigned int cl = info[i].cluster;
        if (cl >= (unsigned)n) continue;
        if (clust[cl] == 0xFFFF || (visual_rtl ? i > clust[cl] : i < clust[cl])) clust[cl] = (WORD)i;
    }
    WORD last = visual_rtl ? (WORD)(m - 1) : 0;
    for (int ch = 0; ch < n; ch++) {                        /* characters inside a cluster */
        if (clust[ch] == 0xFFFF) clust[ch] = last;
        else last = clust[ch];
    }
    if (va) {
        hb_face_t *face = hb_font_get_face(c->hb);
        for (unsigned int i = 0; i < m; i++) {
            unsigned int cl = info[i].cluster;
            UINT32 ch = cl < (unsigned)n ? s[cl] : 0;
            memset(&va[i], 0, sizeof(va[i]));
            va[i].fClusterStart = cl < (unsigned)n && clust[cl] == i;
            va[i].fDiacritic = hb_ot_layout_get_glyph_class(face, info[i].codepoint) == HB_OT_LAYOUT_GLYPH_CLASS_MARK ||
                               (!va[i].fClusterStart && is_mark(ch));
            va[i].fZeroWidth = zero_width(ch) && va[i].fClusterStart;
            va[i].uJustification = va[i].fDiacritic ? SCRIPT_JUSTIFY_NONE :
                                   is_space(ch) ? SCRIPT_JUSTIFY_BLANK : SCRIPT_JUSTIFY_CHARACTER;
        }
    }
    remember_run(c, glyphs, adv, off, (int)m);
    u_free(adv); u_free(off);
    hb_buffer_destroy(b);
    *count = (int)m;
    return S_OK;
}

USP HRESULT WINAPI ScriptShape(HDC hdc, SCRIPT_CACHE *psc, const WCHAR *s, int n, int max, SCRIPT_ANALYSIS *sa,
                               WORD *glyphs, WORD *clust, SCRIPT_VISATTR *va, int *count)
{
    if (!va) return E_INVALIDARG;
    return shape(hdc, psc, s, n, max, sa, 0, 0, 0, glyphs, clust, va, count);
}

static hb_tag_t from_ot_tag(OPENTYPE_TAG t) { return (t >> 24) | (t >> 8 & 0xFF00) | (t << 8 & 0xFF0000) | (t << 24); }

/* the OpenType features of the first range (one feature set per run) */
static hb_feature_t *range_features(TEXTRANGE_PROPERTIES **props, int nranges, unsigned int *n)
{
    *n = 0;
    if (!props || nranges <= 0 || !props[0] || props[0]->cotfRecords <= 0) return 0;
    hb_feature_t *f = u_alloc(props[0]->cotfRecords * sizeof(hb_feature_t));
    if (!f) return 0;
    for (int i = 0; i < props[0]->cotfRecords; i++) {
        f[i].tag = from_ot_tag(props[0]->potfRecords[i].tagFeature);
        f[i].value = (uint32_t)props[0]->potfRecords[i].lParameter;
        f[i].start = 0;
        f[i].end = (unsigned int)-1;
    }
    *n = (unsigned int)props[0]->cotfRecords;
    return f;
}

USP HRESULT WINAPI ScriptShapeOpenType(HDC hdc, SCRIPT_CACHE *psc, SCRIPT_ANALYSIS *sa, OPENTYPE_TAG script_tag,
                                       OPENTYPE_TAG lang_tag, int *range_chars, TEXTRANGE_PROPERTIES **props,
                                       int nranges, const WCHAR *s, int n, int max, WORD *clust,
                                       SCRIPT_CHARPROP *cprops, WORD *glyphs, SCRIPT_GLYPHPROP *gprops, int *count)
{
    (void)script_tag; (void)range_chars;
    if (!gprops || !cprops) return E_INVALIDARG;
    unsigned int nf;
    hb_feature_t *f = range_features(props, nranges, &nf);
    SCRIPT_VISATTR *va = u_alloc((max > 0 ? max : 1) * sizeof(SCRIPT_VISATTR));
    if (!va) { u_free(f); return E_OUTOFMEMORY; }
    HRESULT r = shape(hdc, psc, s, n, max, sa, f, nf, lang_tag ? from_ot_tag(lang_tag) : 0, glyphs, clust, va, count);
    if (SUCCEEDED(r)) {
        for (int i = 0; i < *count; i++) { gprops[i].sva = va[i]; gprops[i].reserved = 0; }
        for (int i = 0; i < n; i++) { cprops[i].fCanGlyphAlone = !is_mark(s[i]); cprops[i].reserved = 0; }
    }
    u_free(va);
    u_free(f);
    return r;
}

/* -----------------------------------------------------------------------
 * Placing: HarfBuzz's positions for a run just shaped, else each glyph's
 * own advance
 * ----------------------------------------------------------------------- */
static HRESULT place(HDC hdc, SCRIPT_CACHE *psc, const WORD *glyphs, int n, const SCRIPT_VISATTR *va,
                     const SCRIPT_ANALYSIS *sa, int *adv, GOFFSET *goff, ABC_ *abc)
{
    HDC dc;
    if (!glyphs || n <= 0 || !adv) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    Cache *c = *psc;
    const Run *run = sa && sa->fNoGlyphIndex ? 0 : find_run(c, glyphs, n);
    long total = 0;
    for (int i = 0; i < n; i++) {
        if (run) {
            adv[i] = run->adv[i];
            if (goff) goff[i] = run->off[i];
        } else {
            if (sa && sa->fNoGlyphIndex) { INT w = 0; GetCharWidth32W(dc, glyphs[i], glyphs[i], &w); adv[i] = w; }
            else adv[i] = glyph_advance(c, dc, glyphs[i]);
            if (goff) goff[i].du = goff[i].dv = 0;
            if (va && va[i].fDiacritic) adv[i] = 0;
        }
        if (va && va[i].fZeroWidth) adv[i] = 0;
        total += adv[i];
    }
    if (abc) { abc->abcA = 0; abc->abcB = (UINT)total; abc->abcC = 0; }
    return S_OK;
}

USP HRESULT WINAPI ScriptPlace(HDC hdc, SCRIPT_CACHE *psc, const WORD *glyphs, int n, const SCRIPT_VISATTR *va,
                               SCRIPT_ANALYSIS *sa, int *adv, GOFFSET *goff, ABC_ *abc)
{
    return place(hdc, psc, glyphs, n, va, sa, adv, goff, abc);
}

USP HRESULT WINAPI ScriptPlaceOpenType(HDC hdc, SCRIPT_CACHE *psc, SCRIPT_ANALYSIS *sa, OPENTYPE_TAG script_tag,
                                       OPENTYPE_TAG lang_tag, int *range_chars, TEXTRANGE_PROPERTIES **props,
                                       int nranges, const WCHAR *s, WORD *clust, SCRIPT_CHARPROP *cprops, int n,
                                       const WORD *glyphs, const SCRIPT_GLYPHPROP *gprops, int nglyphs, int *adv,
                                       GOFFSET *goff, ABC_ *abc)
{
    (void)script_tag; (void)lang_tag; (void)range_chars; (void)props; (void)nranges; (void)s; (void)clust;
    (void)cprops; (void)n;
    SCRIPT_VISATTR *va = 0;
    if (gprops && nglyphs > 0 && (va = u_alloc(nglyphs * sizeof(*va))))
        for (int i = 0; i < nglyphs; i++) va[i] = gprops[i].sva;
    HRESULT r = place(hdc, psc, glyphs, nglyphs, va, sa, adv, goff, abc);
    u_free(va);
    return r;
}

USP HRESULT WINAPI ScriptGetCMap(HDC hdc, SCRIPT_CACHE *psc, const WCHAR *s, int n, DWORD flags, WORD *glyphs)
{
    HDC dc;
    (void)flags;
    if (!s || !glyphs) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    HRESULT res = S_OK;
    for (int i = 0; i < n; i++) {
        glyphs[i] = s[i] >= 0xD800 && s[i] < 0xE000 ? 0 : nominal_glyph(*psc, dc, s[i]);
        if (!glyphs[i]) res = S_FALSE;
    }
    return res;
}

USP HRESULT WINAPI ScriptGetGlyphABCWidth(HDC hdc, SCRIPT_CACHE *psc, WORD g, ABC_ *abc)
{
    HDC dc;
    if (!abc) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    abc->abcA = 0; abc->abcB = (UINT)glyph_advance(*psc, dc, g); abc->abcC = 0;
    return S_OK;
}

USP HRESULT WINAPI ScriptGetFontScriptTags(HDC hdc, SCRIPT_CACHE *psc, SCRIPT_ANALYSIS *sa, int max,
                                           OPENTYPE_TAG *tags, int *count)
{
    HDC dc;
    (void)sa;
    if (!tags || !count || max <= 0) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    Cache *c = *psc;
    hb_tag_t t[64];
    unsigned int n = 64, total = c->hb ? hb_ot_layout_table_get_script_tags(hb_font_get_face(c->hb), HB_OT_TAG_GSUB, 0, &n, t) : (n = 0);
    (void)total;
    if ((int)n > max) return E_OUTOFMEMORY;
    for (unsigned int i = 0; i < n; i++) tags[i] = from_ot_tag(t[i]);
    *count = (int)n;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * Drawing: glyph runs through gdi32, combining marks at their offsets
 * ----------------------------------------------------------------------- */
static BOOL draw_glyphs(HDC hdc, int x, int y, UINT opts, const RECT *rc, const WORD *glyphs, int n,
                        const int *adv, const GOFFSET *goff)
{
    BOOL ok = TRUE;
    int pen = x;
    for (int i = 0; i < n;) {
        if (goff && (goff[i].du || goff[i].dv)) {           /* a positioned glyph on its own */
            int zero = 0;
            ok &= ExtTextOutW(hdc, pen + goff[i].du, y - goff[i].dv, ETO_GLYPH_INDEX_ | opts, rc,
                              (LPCWSTR)&glyphs[i], 1, &zero);
            pen += adv[i];
            i++;
            continue;
        }
        int j = i;
        while (j < n && !(goff && (goff[j].du || goff[j].dv))) j++;
        ok &= ExtTextOutW(hdc, pen, y, ETO_GLYPH_INDEX_ | opts, rc, (LPCWSTR)&glyphs[i], (UINT)(j - i), adv + i);
        for (int k = i; k < j; k++) pen += adv[k];
        i = j;
    }
    return ok;
}

USP HRESULT WINAPI ScriptTextOut(HDC hdc, SCRIPT_CACHE *psc, int x, int y, UINT opts, const RECT *rc,
                                 const SCRIPT_ANALYSIS *sa, const WCHAR *reserved, int ireserved, const WORD *glyphs,
                                 int n, const int *adv, const int *just, const GOFFSET *goff)
{
    (void)psc; (void)reserved; (void)ireserved;
    if (!hdc || !glyphs || !adv || n <= 0) return E_INVALIDARG;
    if (rc && (opts & ETO_OPAQUE_)) ExtTextOutW(hdc, 0, 0, ETO_OPAQUE_, rc, 0, 0, 0);
    UINT o = opts & ETO_CLIPPED_;
    if (sa && sa->fNoGlyphIndex)
        return ExtTextOutW(hdc, x, y, o, rc, (LPCWSTR)glyphs, (UINT)n, just ? just : adv) ? S_OK : E_FAIL;
    if (sa && sa->fRTL && sa->fLogicalOrder) {              /* drawn left to right: reverse */
        WORD *g = u_alloc(n * sizeof(WORD));
        int *a = u_alloc(n * sizeof(int));
        GOFFSET *f = goff ? u_alloc(n * sizeof(GOFFSET)) : 0;
        if (!g || !a || (goff && !f)) { u_free(g); u_free(a); u_free(f); return E_OUTOFMEMORY; }
        for (int i = 0; i < n; i++) {
            g[i] = glyphs[n - 1 - i];
            a[i] = (just ? just : adv)[n - 1 - i];
            if (f) f[i] = goff[n - 1 - i];
        }
        BOOL ok = draw_glyphs(hdc, x, y, o, rc, g, n, a, f);
        u_free(g); u_free(a); u_free(f);
        return ok ? S_OK : E_FAIL;
    }
    return draw_glyphs(hdc, x, y, o, rc, glyphs, n, just ? just : adv, goff) ? S_OK : E_FAIL;
}

USP HRESULT WINAPI ScriptJustify(const SCRIPT_VISATTR *va, const int *adv, int n, int dx, int minkash, int *just)
{
    (void)minkash;
    if (!va || !adv || !just || n <= 0) return E_INVALIDARG;
    int blanks = 0;
    for (int i = 0; i < n; i++) { just[i] = adv[i]; if (va[i].uJustification == SCRIPT_JUSTIFY_BLANK) blanks++; }
    if (!blanks || !dx) return S_OK;
    for (int i = 0, k = 0; i < n; i++)
        if (va[i].uJustification == SCRIPT_JUSTIFY_BLANK) {
            just[i] += dx / blanks + (k < (dx % blanks) ? 1 : 0);
            k++;
        }
    return S_OK;
}

/* -----------------------------------------------------------------------
 * Breaks, carets and widths
 * ----------------------------------------------------------------------- */
USP HRESULT WINAPI ScriptBreak(const WCHAR *s, int n, const SCRIPT_ANALYSIS *sa, SCRIPT_LOGATTR *la)
{
    (void)sa;
    if (!s || n <= 0 || !la) return E_INVALIDARG;
    for (int i = 0; i < n; i++) {
        memset(&la[i], 0, sizeof(la[i]));
        UINT32 c = s[i];
        la[i].fWhiteSpace = is_space(c);
        /* caret stops: not inside a surrogate pair, before a mark or a virama's consonant */
        la[i].fCharStop = !(c >= 0xDC00 && c <= 0xDFFF) && !(i > 0 && is_mark(c)) && !(c >= 0x200C && c <= 0x200D);
        int after_space = i > 0 && is_space(s[i - 1]) && !is_space(c);
        int after_dash = i > 0 && (s[i - 1] == '-' || s[i - 1] == 0x2010) && !is_space(c);
        la[i].fSoftBreak = after_space || after_dash;
        la[i].fWordStop = i == 0 || after_space;
    }
    return S_OK;
}

/* the glyph range [*g0, *g1) of the cluster holding character @ch, and the
 * character range [*c0, *c1) of that cluster */
static void cluster_of(int ch, int nchars, int nglyphs, const WORD *clust, int rtl, int *c0, int *c1, int *g0, int *g1)
{
    int g = clust[ch];
    *c0 = ch; *c1 = ch + 1;
    while (*c0 > 0 && clust[*c0 - 1] == g) (*c0)--;
    while (*c1 < nchars && clust[*c1] == g) (*c1)++;
    /* the glyphs from this cluster's start to the next cluster's */
    int next = *c1 < nchars ? clust[*c1] : (rtl ? -1 : nglyphs);
    if (rtl) { *g1 = g + 1; *g0 = next + 1; }
    else { *g0 = g; *g1 = next; }
    if (*g0 < 0) *g0 = 0;
    if (*g1 > nglyphs) *g1 = nglyphs;
    if (*g1 < *g0) *g1 = *g0;
}

USP HRESULT WINAPI ScriptCPtoX(int cp, BOOL trailing, int nchars, int nglyphs, const WORD *clust,
                               const SCRIPT_VISATTR *va, const int *adv, const SCRIPT_ANALYSIS *sa, int *x)
{
    (void)va;
    if (!x || !adv || !clust || nchars <= 0) return E_INVALIDARG;
    int rtl = sa && sa->fRTL && !sa->fLogicalOrder, total = 0;
    for (int i = 0; i < nglyphs; i++) total += adv[i];
    if (cp < 0) { *x = rtl ? total : 0; return S_OK; }
    if (cp >= nchars) { *x = rtl ? 0 : total; return S_OK; }
    int c0, c1, g0, g1;
    cluster_of(cp, nchars, nglyphs, clust, rtl, &c0, &c1, &g0, &g1);
    int left = 0, w = 0;
    for (int i = 0; i < g0; i++) left += adv[i];
    for (int i = g0; i < g1; i++) w += adv[i];
    int k = cp - c0 + (trailing ? 1 : 0), cn = c1 - c0;
    *x = rtl ? left + w - w * k / cn : left + w * k / cn;
    return S_OK;
}

USP HRESULT WINAPI ScriptXtoCP(int x, int nchars, int nglyphs, const WORD *clust, const SCRIPT_VISATTR *va,
                               const int *adv, const SCRIPT_ANALYSIS *sa, int *cp, int *trailing)
{
    (void)va;
    if (!cp || !trailing || !adv || !clust || nchars <= 0) return E_INVALIDARG;
    int rtl = sa && sa->fRTL && !sa->fLogicalOrder, total = 0;
    for (int i = 0; i < nglyphs; i++) total += adv[i];
    if (x < 0) { *cp = rtl ? nchars : -1; *trailing = rtl ? 0 : 1; return S_OK; }
    if (x >= total) { *cp = rtl ? -1 : nchars; *trailing = rtl ? 1 : 0; return S_OK; }
    for (int ch = 0; ch < nchars;) {
        int c0, c1, g0, g1;
        cluster_of(ch, nchars, nglyphs, clust, rtl, &c0, &c1, &g0, &g1);
        int left = 0, w = 0;
        for (int i = 0; i < g0; i++) left += adv[i];
        for (int i = g0; i < g1; i++) w += adv[i];
        if (x >= left && x < left + w) {
            int cn = c1 - c0, off = rtl ? left + w - 1 - x : x - left;
            int k = w ? off * cn / w : 0, part = w / cn ? w / cn : 1;
            *cp = c0 + k;
            *trailing = (off - k * w / cn) * 2 >= part;
            return S_OK;
        }
        ch = c1;
    }
    *cp = rtl ? -1 : nchars; *trailing = 0;
    return S_OK;
}

USP HRESULT WINAPI ScriptGetLogicalWidths(const SCRIPT_ANALYSIS *sa, int nchars, int nglyphs, const int *adv,
                                          const WORD *clust, const SCRIPT_VISATTR *va, int *dx)
{
    (void)va;
    if (!adv || !clust || !dx) return E_INVALIDARG;
    int rtl = sa && sa->fRTL && !sa->fLogicalOrder;
    for (int ch = 0; ch < nchars;) {
        int c0, c1, g0, g1;
        cluster_of(ch, nchars, nglyphs, clust, rtl, &c0, &c1, &g0, &g1);
        int w = 0;
        for (int i = g0; i < g1; i++) w += adv[i];
        for (int i = c0; i < c1; i++) dx[i] = w / (c1 - c0) + (i - c0 < w % (c1 - c0) ? 1 : 0);
        ch = c1;
    }
    return S_OK;
}

/* Visual order from embedding levels (UAX #9, rule L2): from the highest
 * level down to the lowest odd one, reverse every run at that level or
 * above */
USP HRESULT WINAPI ScriptLayout(int n, const BYTE *levels, int *v2l, int *l2v)
{
    if (!levels || n <= 0) return E_INVALIDARG;
    int *order = u_alloc(n * sizeof(int));
    if (!order) return E_OUTOFMEMORY;
    BYTE hi = 0, lo = 0xFF;
    for (int i = 0; i < n; i++) {
        order[i] = i;
        if (levels[i] > hi) hi = levels[i];
        if ((levels[i] & 1) && levels[i] < lo) lo = levels[i];
    }
    for (int lv = hi; lo != 0xFF && lv >= lo; lv--)
        for (int i = 0; i < n;) {
            if (levels[order[i]] < lv) { i++; continue; }
            int j = i;
            while (j < n && levels[order[j]] >= lv) j++;
            for (int a = i, b = j - 1; a < b; a++, b--) { int t = order[a]; order[a] = order[b]; order[b] = t; }
            i = j;
        }
    for (int i = 0; i < n; i++) {
        if (v2l) v2l[i] = order[i];
        if (l2v) l2v[order[i]] = i;
    }
    u_free(order);
    return S_OK;
}

USP HRESULT WINAPI ScriptRecordDigitSubstitution(LCID lcid, SCRIPT_DIGITSUBSTITUTE *ds)
{
    (void)lcid;
    if (!ds) return E_INVALIDARG;
    memset(ds, 0, sizeof(*ds));
    ds->DigitSubstitute = 1;                                /* SCRIPT_DIGITSUBSTITUTE_NONE */
    return S_OK;
}
USP HRESULT WINAPI ScriptApplyDigitSubstitution(const SCRIPT_DIGITSUBSTITUTE *ds, SCRIPT_CONTROL *sc, SCRIPT_STATE *ss)
{
    (void)ds;
    if (sc) memset(sc, 0, sizeof(*sc));
    if (ss) memset(ss, 0, sizeof(*ss));
    return S_OK;
}

/* -----------------------------------------------------------------------
 * ScriptString*: a whole line itemized, shaped (each run with its own
 * font when SSA_FALLBACK finds the DC's lacking), ordered and drawn
 * ----------------------------------------------------------------------- */
typedef struct {
    int c0, nc;                     /* its characters */
    SCRIPT_ANALYSIS sa;
    HFONT font;                     /* 0: the DC's */
    SCRIPT_CACHE cache;
    int ng;
    WORD *glyphs, *clust;
    SCRIPT_VISATTR *va;
    int *adv;
    GOFFSET *off;
    int width, x;                   /* x: its left edge in the line */
} Item;

typedef struct {
    HDC dc;
    int n;
    WCHAR *text;
    int nitems;
    Item *items;
    int *visual;                    /* items in visual order */
    int *ldx;                       /* logical widths */
    SCRIPT_LOGATTR *la;
    SIZE size;
    int out_chars;
    int ascent;                     /* the DC font's: every run's baseline */
} SSA;

static void free_item(Item *it)
{
    ScriptFreeCache(&it->cache);
    if (it->font) DeleteObject(it->font);
    u_free(it->glyphs); u_free(it->clust); u_free(it->va); u_free(it->adv); u_free(it->off);
}

USP HRESULT WINAPI ScriptStringFree(void **pssa)
{
    if (!pssa) return E_INVALIDARG;
    SSA *a = *pssa;
    if (a) {
        for (int i = 0; i < a->nitems; i++) free_item(&a->items[i]);
        u_free(a->items); u_free(a->visual); u_free(a->ldx);
        u_free(a->text); u_free(a->la);
        u_free(a);
        *pssa = 0;
    }
    return S_OK;
}

/* Shape @it with the font selected in @dc */
static HRESULT shape_item(HDC dc, SSA *a, Item *it)
{
    int max = it->nc * 3 / 2 + 16;
    for (;;) {
        u_free(it->glyphs); u_free(it->va);
        it->glyphs = u_alloc(max * sizeof(WORD));
        it->va = u_alloc(max * sizeof(SCRIPT_VISATTR));
        if (!it->glyphs || !it->va) return E_OUTOFMEMORY;
        HRESULT r = ScriptShape(dc, &it->cache, a->text + it->c0, it->nc, max, &it->sa, it->glyphs, it->clust,
                                it->va, &it->ng);
        if (r != E_OUTOFMEMORY || max > it->nc * 8 + 64) return r;
        max *= 2;
    }
}

USP HRESULT WINAPI ScriptStringAnalyse(HDC hdc, const void *str, int n, int nglyphs, int charset, DWORD flags,
                                       int reqwidth, SCRIPT_CONTROL *ctl, SCRIPT_STATE *st, const int *dx,
                                       SCRIPT_TABDEF *tabs, const BYTE *inclass, void **pssa)
{
    (void)nglyphs; (void)inclass;
    if (!hdc || !str || n <= 0 || !pssa) return E_INVALIDARG;
    if (charset != -1) return E_INVALIDARG;                 /* only Unicode input */
    SSA *a = u_alloc(sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    a->dc = hdc;
    a->n = n;
    a->text = u_alloc(n * sizeof(WCHAR));
    a->la = u_alloc(n * sizeof(SCRIPT_LOGATTR));
    a->ldx = u_alloc(n * sizeof(int));
    SCRIPT_ITEM *si = u_alloc((n + 1) * sizeof(SCRIPT_ITEM));
    if (!a->text || !a->la || !a->ldx || !si) { u_free(si); ScriptStringFree((void **)&a); return E_OUTOFMEMORY; }
    memcpy(a->text, str, n * sizeof(WCHAR));
    if (flags & SSA_PASSWORD) for (int i = 0; i < n; i++) a->text[i] = '*';
    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    a->ascent = tm.tmAscent;

    SCRIPT_STATE state;
    memset(&state, 0, sizeof(state));
    if (st) state = *st;
    if (flags & SSA_RTL) state.uBidiLevel = 1;
    int count = 0;
    HRESULT r = ScriptItemize(a->text, n, n + 1, ctl, &state, si, &count);
    if (FAILED(r)) { u_free(si); ScriptStringFree((void **)&a); return r; }
    a->items = u_alloc(count * sizeof(Item));
    a->visual = u_alloc(count * sizeof(int));
    BYTE *levels = u_alloc(count);
    if (!a->items || !a->visual || !levels) { u_free(si); u_free(levels); ScriptStringFree((void **)&a); return E_OUTOFMEMORY; }
    a->nitems = count;

    LOGFONTW lf;
    GetObjectW(GetCurrentObject(hdc, OBJ_FONT_), sizeof(lf), &lf);
    for (int k = 0; k < count; k++) {
        Item *it = &a->items[k];
        it->c0 = si[k].iCharPos;
        it->nc = si[k + 1].iCharPos - it->c0;
        it->sa = si[k].a;
        levels[k] = (BYTE)it->sa.s.uBidiLevel;
        it->clust = u_alloc(it->nc * sizeof(WORD));
        if (!it->clust) { r = E_OUTOFMEMORY; break; }
        r = shape_item(hdc, a, it);
        const WCHAR *fb = g_scripts[it->sa.eScript < NSCRIPTS ? it->sa.eScript : 0].fallback;
        if (r == USP_E_SCRIPT_NOT_IN_FONT_ && (flags & SSA_FALLBACK)) {
            ScriptFreeCache(&it->cache);
            LOGFONTW f = lf;
            memset(f.lfFaceName, 0, sizeof(f.lfFaceName));
            for (int i = 0; fb && fb[i] && i < 31; i++) f.lfFaceName[i] = fb[i];
            it->font = fb ? CreateFontIndirectW(&f) : 0;
            HGDIOBJ old = it->font ? SelectObject(hdc, it->font) : 0;
            r = shape_item(hdc, a, it);
            if (old) SelectObject(hdc, old);
        }
        if (r == USP_E_SCRIPT_NOT_IN_FONT_) {               /* no font has it: the DC's, with its missing glyphs */
            ScriptFreeCache(&it->cache);
            if (it->font) { DeleteObject(it->font); it->font = 0; }
            it->sa.eScript = SCRIPT_UNDEFINED;
            r = shape_item(hdc, a, it);
        }
        if (FAILED(r)) break;
        it->adv = u_alloc(it->ng * sizeof(int));
        it->off = u_alloc(it->ng * sizeof(GOFFSET));
        if (!it->adv || !it->off) { r = E_OUTOFMEMORY; break; }
        HGDIOBJ old = it->font ? SelectObject(hdc, it->font) : 0;
        r = ScriptPlace(hdc, &it->cache, it->glyphs, it->ng, it->va, &it->sa, it->adv, it->off, 0);
        if (old) SelectObject(hdc, old);
        if (FAILED(r)) break;
    }
    u_free(si);
    if (FAILED(r)) { u_free(levels); ScriptStringFree((void **)&a); return r; }

    /* tabs, caller widths and clipping, in logical order */
    int tabw = 8 * tm.tmAveCharWidth;
    if (tabs && tabs->cTabStops >= 1 && tabs->pTabStops && tabs->pTabStops[0] > 0) tabw = tabs->pTabStops[0];
    for (int k = 0; k < count; k++) {
        Item *it = &a->items[k];
        ScriptGetLogicalWidths(&it->sa, it->nc, it->ng, it->adv, it->clust, it->va, a->ldx + it->c0);
    }
    a->out_chars = n;
    int x = 0;
    for (int i = 0; i < n; i++) {
        int w = a->ldx[i];
        if (dx) w = dx[i];
        else if (a->text[i] == '\t' && (flags & SSA_TAB) && tabw > 0) w = tabw - x % tabw;
        if (w != a->ldx[i]) {                               /* push the difference onto the char's glyph */
            for (int k = 0; k < count; k++) {
                Item *it = &a->items[k];
                if (i < it->c0 || i >= it->c0 + it->nc) continue;
                int g = it->clust[i - it->c0];
                if (g < it->ng) it->adv[g] += w - a->ldx[i];
            }
            a->ldx[i] = w;
        }
        if ((flags & SSA_CLIP) && reqwidth > 0 && x + w > reqwidth) { a->out_chars = i; break; }
        x += w;
    }
    ScriptLayout(count, levels, a->visual, 0);
    u_free(levels);
    int pen = 0;
    for (int v = 0; v < count; v++) {
        Item *it = &a->items[a->visual[v]];
        it->x = pen;
        it->width = 0;
        for (int g = 0; g < it->ng; g++) it->width += it->adv[g];
        pen += it->width;
    }
    ScriptBreak(a->text, n, 0, a->la);
    a->size.cx = pen;
    a->size.cy = tm.tmHeight;
    *pssa = a;
    return S_OK;
}

USP const SIZE *WINAPI ScriptString_pSize(void *ssa) { return ssa ? &((SSA *)ssa)->size : 0; }
USP const int *WINAPI ScriptString_pcOutChars(void *ssa) { return ssa ? &((SSA *)ssa)->out_chars : 0; }
USP const SCRIPT_LOGATTR *WINAPI ScriptString_pLogAttr(void *ssa) { return ssa ? ((SSA *)ssa)->la : 0; }

USP HRESULT WINAPI ScriptStringGetLogicalWidths(void *ssa, int *dx)
{
    SSA *a = ssa;
    if (!a || !dx) return E_INVALIDARG;
    memcpy(dx, a->ldx, a->n * sizeof(int));
    return S_OK;
}

static Item *item_of(SSA *a, int cp)
{
    for (int k = 0; k < a->nitems; k++)
        if (cp >= a->items[k].c0 && cp < a->items[k].c0 + a->items[k].nc) return &a->items[k];
    return 0;
}

USP HRESULT WINAPI ScriptStringCPtoX(void *ssa, int cp, BOOL trailing, int *x)
{
    SSA *a = ssa;
    if (!a || !x || cp < 0 || cp > a->n) return E_INVALIDARG;
    if (cp == a->n) { cp = a->n - 1; trailing = TRUE; }
    Item *it = item_of(a, cp);
    if (!it) return E_INVALIDARG;
    int ix;
    ScriptCPtoX(cp - it->c0, trailing, it->nc, it->ng, it->clust, it->va, it->adv, &it->sa, &ix);
    *x = it->x + ix;
    return S_OK;
}

USP HRESULT WINAPI ScriptStringXtoCP(void *ssa, int x, int *cp, int *trailing)
{
    SSA *a = ssa;
    if (!a || !cp || !trailing) return E_INVALIDARG;
    if (x < 0) {
        Item *it = a->nitems ? &a->items[a->visual[0]] : 0;
        *cp = it && it->sa.fRTL ? it->c0 + it->nc - 1 : -1;
        *trailing = it && it->sa.fRTL ? 1 : 1;
        if (!(it && it->sa.fRTL)) *trailing = 1;
        return S_OK;
    }
    for (int v = 0; v < a->nitems; v++) {
        Item *it = &a->items[a->visual[v]];
        if (x >= it->x && x < it->x + it->width) {
            int c, t;
            ScriptXtoCP(x - it->x, it->nc, it->ng, it->clust, it->va, it->adv, &it->sa, &c, &t);
            if (c < 0) { c = 0; t = 0; }
            if (c >= it->nc) { c = it->nc - 1; t = 1; }
            *cp = it->c0 + c; *trailing = t;
            return S_OK;
        }
    }
    *cp = a->n; *trailing = 0;
    return S_OK;
}

USP HRESULT WINAPI ScriptStringValidate(void *ssa) { return ssa ? S_OK : E_INVALIDARG; }

USP HRESULT WINAPI ScriptStringGetOrder(void *ssa, UINT *order)
{
    SSA *a = ssa;
    if (!a || !order) return E_INVALIDARG;
    int k = 0;
    for (int v = 0; v < a->nitems; v++) {
        Item *it = &a->items[a->visual[v]];
        for (int g = 0; g < it->ng; g++) order[k + g] = (UINT)(k + g);
        k += it->ng;
    }
    return S_OK;
}

/* the x range a selection covers inside @it, or 0 */
static int sel_range(SSA *a, Item *it, int minsel, int maxsel, int *l, int *r)
{
    int lo = minsel > it->c0 ? minsel : it->c0, hi = maxsel < it->c0 + it->nc ? maxsel : it->c0 + it->nc;
    if (lo >= hi) return 0;
    int x0, x1;
    ScriptStringCPtoX(a, lo, FALSE, &x0);
    ScriptStringCPtoX(a, hi - 1, TRUE, &x1);
    *l = x0 < x1 ? x0 : x1;
    *r = x0 < x1 ? x1 : x0;
    return 1;
}

static void draw_item(SSA *a, Item *it, int x, int base, UINT opts, const RECT *rc)
{
    HGDIOBJ old = it->font ? SelectObject(a->dc, it->font) : 0;
    if (it->sa.fNoGlyphIndex)
        ExtTextOutW(a->dc, x + it->x, base, opts, rc, (LPCWSTR)it->glyphs, (UINT)it->ng, it->adv);
    else
        draw_glyphs(a->dc, x + it->x, base, opts, rc, it->glyphs, it->ng, it->adv, it->off);
    if (old) SelectObject(a->dc, old);
}

USP HRESULT WINAPI ScriptStringOut(void *ssa, int x, int y, UINT opts, const RECT *rc, int minsel, int maxsel,
                                   BOOL disabled)
{
    (void)disabled;
    SSA *a = ssa;
    if (!a) return E_INVALIDARG;
    if (rc && (opts & ETO_OPAQUE_)) ExtTextOutW(a->dc, 0, 0, ETO_OPAQUE_, rc, 0, 0, 0);
    /* every run on the DC font's baseline */
    UINT align = GetTextAlign(a->dc);
    int base = (align & TA_BASELINE_) == TA_BASELINE_ ? y : (align & TA_BOTTOM_) ? y - (a->size.cy - a->ascent) : y + a->ascent;
    SetTextAlign(a->dc, TA_BASELINE_);
    UINT o = opts & ETO_CLIPPED_;
    for (int v = 0; v < a->nitems; v++) draw_item(a, &a->items[a->visual[v]], x, base, o, rc);
    if (minsel < 0) minsel = 0;
    if (maxsel > a->n) maxsel = a->n;
    if (minsel < maxsel) {                                  /* the selection, redrawn highlighted */
        COLORREF fg = SetTextColor(a->dc, GetSysColor(COLOR_HIGHLIGHTTEXT_));
        COLORREF bg = SetBkColor(a->dc, GetSysColor(COLOR_HIGHLIGHT));
        int mode = SetBkMode(a->dc, TRANSPARENT_);
        for (int v = 0; v < a->nitems; v++) {
            Item *it = &a->items[a->visual[v]];
            int l, r;
            if (!sel_range(a, it, minsel, maxsel, &l, &r)) continue;
            RECT sr = { x + l, y, x + r, y + a->size.cy };
            if (rc && (opts & ETO_CLIPPED_)) {
                if (sr.left < rc->left) sr.left = rc->left;
                if (sr.right > rc->right) sr.right = rc->right;
            }
            ExtTextOutW(a->dc, 0, 0, ETO_OPAQUE_, &sr, 0, 0, 0);
            draw_item(a, it, x, base, ETO_CLIPPED_, &sr);
        }
        SetBkMode(a->dc, mode);
        SetBkColor(a->dc, bg);
        SetTextColor(a->dc, fg);
    }
    SetTextAlign(a->dc, align);
    return S_OK;
}
