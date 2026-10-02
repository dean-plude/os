/*
 * usp10.dll — Uniscribe.  NovaOS lays text out left to right with one
 * glyph per character (gdi32's own glyph indices and widths), so every run
 * is one "simple" script item; bidirectional reordering is ScriptLayout's
 * level arithmetic only.  The ScriptString* layer sits on top of that.
 */
#include <windows.h>
#include <string.h>

#define USP __declspec(dllexport)

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

#define OBJ_FONT_          6
#define ETO_OPAQUE_        0x0002
#define ETO_CLIPPED_       0x0004
#define ETO_GLYPH_INDEX_   0x0010
#define OPAQUE_            2
#define COLOR_HIGHLIGHTTEXT_ 14
#define E_PENDING_         ((HRESULT)0x8000000AL)
#define USP_E_SCRIPT_NOT_IN_FONT_ ((HRESULT)0x80040200L)

#define SSA_PASSWORD   0x00000001
#define SSA_TAB        0x00000002
#define SSA_GLYPHS     0x00000080
#define SSA_FALLBACK   0x00000020
#define SSA_BREAK      0x00000040
#define SSA_CLIP       0x00000004
#define SSA_FIT        0x00000010

/* -----------------------------------------------------------------------
 * The structures (bit-for-bit as in usp10.h)
 * ----------------------------------------------------------------------- */
typedef void *SCRIPT_CACHE;
typedef struct { DWORD bits; } SCRIPT_CONTROL;
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

#define SCRIPT_UNDEFINED 0
#define SCRIPT_LATIN     1
#define SCRIPT_JUSTIFY_CHARACTER 2
#define SCRIPT_JUSTIFY_BLANK     4

/* script 0: neutral/undefined, 1: Latin.  Neither is complex. */
static const SCRIPT_PROPERTIES g_props[2] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0x09 /* LANG_ENGLISH */, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static const SCRIPT_PROPERTIES *g_props_tab[2] = { &g_props[0], &g_props[1] };

USP HRESULT WINAPI ScriptGetProperties(const SCRIPT_PROPERTIES ***props, int *n)
{
    if (!props && !n) return E_INVALIDARG;
    if (props) *props = g_props_tab;
    if (n) *n = 2;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * The script cache: the font the first DC had selected, so later calls
 * without a DC can still measure
 * ----------------------------------------------------------------------- */
typedef struct {
    LOGFONTW lf;
    TEXTMETRICW tm;
    HDC mdc;
    HFONT font;
    HGDIOBJ old;
} Cache;

static void *u_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void u_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

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

USP HRESULT WINAPI ScriptGetFontProperties(HDC hdc, SCRIPT_CACHE *psc, SCRIPT_FONTPROPERTIES *fp)
{
    HDC dc;
    if (!fp || fp->cBytes != sizeof(*fp)) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    WORD g = 0;
    GetGlyphIndicesW(dc, L" ", 1, &g, 0);
    fp->wgBlank = g;
    fp->wgDefault = 0;
    fp->wgInvalid = 0;
    fp->wgKashida = 0xFFFF;
    fp->iKashidaWidth = 0;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * Itemizing: one left-to-right item (plus the terminating one)
 * ----------------------------------------------------------------------- */
USP HRESULT WINAPI ScriptItemize(const WCHAR *s, int n, int max, const SCRIPT_CONTROL *ctl, const SCRIPT_STATE *st,
                                 SCRIPT_ITEM *items, int *count)
{
    (void)ctl;
    if (!s || n <= 0 || !items || max < 2) return E_INVALIDARG;
    int k = 0;
    /* runs of Latin letters and of everything else, as Uniscribe splits */
    int prev = -1;
    for (int i = 0; i < n; i++) {
        WCHAR c = s[i];
        int latin = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7);
        int sc = latin ? SCRIPT_LATIN : SCRIPT_UNDEFINED;
        if (sc == SCRIPT_UNDEFINED && prev >= 0) sc = prev;     /* neutrals join the run they are in */
        if (sc != prev) {
            if (k == max - 1) return E_OUTOFMEMORY;
            memset(&items[k], 0, sizeof(items[k]));
            items[k].iCharPos = i;
            items[k].a.eScript = sc;
            if (st) items[k].a.s = *st;
            k++;
            prev = sc;
        }
    }
    memset(&items[k], 0, sizeof(items[k]));
    items[k].iCharPos = n;
    if (count) *count = k;
    return S_OK;
}

USP HRESULT WINAPI ScriptItemizeOpenType(const WCHAR *s, int n, int max, const SCRIPT_CONTROL *ctl,
                                         const SCRIPT_STATE *st, SCRIPT_ITEM *items, ULONG *tags, int *count)
{
    HRESULT r = ScriptItemize(s, n, max, ctl, st, items, count);
    if (SUCCEEDED(r) && tags && count)
        for (int i = 0; i < *count; i++) tags[i] = 0x6E74616C;     /* 'latn' */
    return r;
}

USP HRESULT WINAPI ScriptIsComplex(const WCHAR *s, int n, DWORD flags)
{
    (void)s; (void)n; (void)flags;
    return S_FALSE;
}

/* -----------------------------------------------------------------------
 * Shaping and placing: one glyph per character
 * ----------------------------------------------------------------------- */
static int zero_width(WCHAR c)
{
    return c < 0x20 || c == 0x200B || c == 0x200C || c == 0x200D || c == 0x200E || c == 0x200F ||
           c == 0xFEFF || (c >= 0x202A && c <= 0x202E) || (c >= 0xDC00 && c <= 0xDFFF);
}

USP HRESULT WINAPI ScriptShape(HDC hdc, SCRIPT_CACHE *psc, const WCHAR *s, int n, int max, SCRIPT_ANALYSIS *sa,
                               WORD *glyphs, WORD *clust, SCRIPT_VISATTR *va, int *count)
{
    HDC dc;
    if (!s || n <= 0 || !glyphs || !clust || !va || !count) return E_INVALIDARG;
    if (max < n) return E_OUTOFMEMORY;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    if (sa && sa->fNoGlyphIndex) memcpy(glyphs, s, n * sizeof(WORD));
    else GetGlyphIndicesW(dc, s, n, glyphs, 0);
    for (int i = 0; i < n; i++) {
        clust[i] = (WORD)i;
        memset(&va[i], 0, sizeof(va[i]));
        va[i].fClusterStart = 1;
        va[i].fZeroWidth = zero_width(s[i]);
        va[i].uJustification = s[i] == ' ' ? SCRIPT_JUSTIFY_BLANK : SCRIPT_JUSTIFY_CHARACTER;
    }
    *count = n;
    return S_OK;
}

USP HRESULT WINAPI ScriptPlace(HDC hdc, SCRIPT_CACHE *psc, const WORD *glyphs, int n, const SCRIPT_VISATTR *va,
                               SCRIPT_ANALYSIS *sa, int *adv, GOFFSET *goff, ABC_ *abc)
{
    HDC dc;
    if (!glyphs || n <= 0 || !adv) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    if (sa && sa->fNoGlyphIndex) {
        for (int i = 0; i < n; i++) { INT w = 0; GetCharWidth32W(dc, glyphs[i], glyphs[i], &w); adv[i] = w; }
    } else if (!GetCharWidthI(dc, 0, n, (LPWORD)glyphs, adv)) {
        for (int i = 0; i < n; i++) adv[i] = ((Cache *)*psc)->tm.tmAveCharWidth;
    }
    long total = 0;
    for (int i = 0; i < n; i++) {
        if (va && va[i].fZeroWidth) adv[i] = 0;
        if (goff) goff[i].du = goff[i].dv = 0;
        total += adv[i];
    }
    if (abc) { abc->abcA = 0; abc->abcB = (UINT)total; abc->abcC = 0; }
    return S_OK;
}

USP HRESULT WINAPI ScriptGetCMap(HDC hdc, SCRIPT_CACHE *psc, const WCHAR *s, int n, DWORD flags, WORD *glyphs)
{
    HDC dc;
    (void)flags;
    if (!s || !glyphs) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    GetGlyphIndicesW(dc, s, n, glyphs, 0);
    for (int i = 0; i < n; i++) if (!glyphs[i]) return S_FALSE;
    return S_OK;
}

USP HRESULT WINAPI ScriptGetGlyphABCWidth(HDC hdc, SCRIPT_CACHE *psc, WORD g, ABC_ *abc)
{
    HDC dc;
    if (!abc) return E_INVALIDARG;
    HRESULT r = cache_dc(hdc, psc, &dc);
    if (FAILED(r)) return r;
    INT w = 0;
    GetCharWidthI(dc, 0, 1, &g, &w);
    abc->abcA = 0; abc->abcB = (UINT)w; abc->abcC = 0;
    return S_OK;
}

USP HRESULT WINAPI ScriptTextOut(HDC hdc, SCRIPT_CACHE *psc, int x, int y, UINT opts, const RECT *rc,
                                 const SCRIPT_ANALYSIS *sa, const WCHAR *reserved, int ireserved, const WORD *glyphs,
                                 int n, const int *adv, const int *just, const GOFFSET *goff)
{
    (void)psc; (void)reserved; (void)ireserved; (void)goff;
    if (!hdc || !glyphs || !adv) return E_INVALIDARG;
    UINT o = opts & (ETO_OPAQUE_ | ETO_CLIPPED_);
    if (!(sa && sa->fNoGlyphIndex)) o |= ETO_GLYPH_INDEX_;
    return ExtTextOutW(hdc, x, y, o, rc, (LPCWSTR)glyphs, (UINT)n, just ? just : adv) ? S_OK : E_FAIL;
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
static int is_space(WCHAR c) { return c == ' ' || c == '\t' || c == 0x3000 || c == 0xA0; }

USP HRESULT WINAPI ScriptBreak(const WCHAR *s, int n, const SCRIPT_ANALYSIS *sa, SCRIPT_LOGATTR *la)
{
    (void)sa;
    if (!s || n <= 0 || !la) return E_INVALIDARG;
    for (int i = 0; i < n; i++) {
        memset(&la[i], 0, sizeof(la[i]));
        la[i].fWhiteSpace = is_space(s[i]);
        la[i].fCharStop = !(s[i] >= 0xDC00 && s[i] <= 0xDFFF);
        int after_space = i > 0 && is_space(s[i - 1]) && !is_space(s[i]);
        int after_dash = i > 0 && (s[i - 1] == '-' || s[i - 1] == 0x2010) && !is_space(s[i]);
        la[i].fSoftBreak = after_space || after_dash;
        la[i].fWordStop = i == 0 || after_space;
    }
    return S_OK;
}

USP HRESULT WINAPI ScriptCPtoX(int cp, BOOL trailing, int nchars, int nglyphs, const WORD *clust,
                               const SCRIPT_VISATTR *va, const int *adv, const SCRIPT_ANALYSIS *sa, int *x)
{
    (void)va; (void)sa;
    if (!x || !adv || !clust) return E_INVALIDARG;
    int pos = 0;
    if (cp < 0) { *x = 0; return S_OK; }
    if (cp >= nchars) cp = nchars - 1, trailing = TRUE;
    int g = clust[cp];
    for (int i = 0; i < g && i < nglyphs; i++) pos += adv[i];
    if (trailing && g < nglyphs) pos += adv[g];
    *x = pos;
    return S_OK;
}

USP HRESULT WINAPI ScriptXtoCP(int x, int nchars, int nglyphs, const WORD *clust, const SCRIPT_VISATTR *va,
                               const int *adv, const SCRIPT_ANALYSIS *sa, int *cp, int *trailing)
{
    (void)va; (void)sa;
    if (!cp || !trailing || !adv || !clust) return E_INVALIDARG;
    if (x < 0) { *cp = -1; *trailing = 1; return S_OK; }
    int pos = 0;
    for (int c = 0; c < nchars; c++) {
        int g = clust[c], w = g < nglyphs ? adv[g] : 0;
        if (x < pos + w) { *cp = c; *trailing = x >= pos + w / 2; return S_OK; }
        pos += w;
    }
    *cp = nchars; *trailing = 0;
    return S_OK;
}

USP HRESULT WINAPI ScriptGetLogicalWidths(const SCRIPT_ANALYSIS *sa, int nchars, int nglyphs, const int *adv,
                                          const WORD *clust, const SCRIPT_VISATTR *va, int *dx)
{
    (void)sa; (void)va;
    if (!adv || !clust || !dx) return E_INVALIDARG;
    for (int c = 0; c < nchars; c++) dx[c] = clust[c] < nglyphs ? adv[clust[c]] : 0;
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
 * ScriptString*: a whole line analysed, measured and drawn at once
 * ----------------------------------------------------------------------- */
typedef struct {
    HDC dc;
    SCRIPT_CACHE cache;
    int n;
    WCHAR *text;
    WORD *glyphs;
    int *adv;
    SCRIPT_LOGATTR *la;
    SIZE size;
    int out_chars;
} SSA;

USP HRESULT WINAPI ScriptStringFree(void **pssa)
{
    if (!pssa) return E_INVALIDARG;
    SSA *a = *pssa;
    if (a) {
        ScriptFreeCache(&a->cache);
        u_free(a->text); u_free(a->glyphs); u_free(a->adv); u_free(a->la);
        u_free(a);
        *pssa = 0;
    }
    return S_OK;
}

USP HRESULT WINAPI ScriptStringAnalyse(HDC hdc, const void *str, int n, int nglyphs, int charset, DWORD flags,
                                       int reqwidth, SCRIPT_CONTROL *ctl, SCRIPT_STATE *st, const int *dx,
                                       SCRIPT_TABDEF *tabs, const BYTE *inclass, void **pssa)
{
    (void)nglyphs; (void)ctl; (void)st; (void)inclass;
    if (!hdc || !str || n <= 0 || !pssa) return E_INVALIDARG;
    if (charset != -1) return E_INVALIDARG;                 /* only Unicode input */
    SSA *a = u_alloc(sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    a->dc = hdc;
    a->n = n;
    a->text = u_alloc(n * sizeof(WCHAR));
    a->glyphs = u_alloc(n * sizeof(WORD));
    a->adv = u_alloc(n * sizeof(int));
    a->la = u_alloc(n * sizeof(SCRIPT_LOGATTR));
    if (!a->text || !a->glyphs || !a->adv || !a->la) { ScriptStringFree((void **)&a); return E_OUTOFMEMORY; }
    memcpy(a->text, str, n * sizeof(WCHAR));
    if (flags & SSA_PASSWORD) for (int i = 0; i < n; i++) a->text[i] = '*';
    HDC dc;
    HRESULT r = cache_dc(hdc, &a->cache, &dc);
    if (FAILED(r)) { ScriptStringFree((void **)&a); return r; }
    GetGlyphIndicesW(dc, a->text, n, a->glyphs, 0);
    GetCharWidthI(dc, 0, n, a->glyphs, a->adv);
    ScriptBreak(a->text, n, 0, a->la);
    int tabw = 8 * ((Cache *)a->cache)->tm.tmAveCharWidth, x = 0;
    if (tabs && tabs->cTabStops == 1 && tabs->pTabStops && tabs->pTabStops[0] > 0) tabw = tabs->pTabStops[0];
    a->out_chars = n;
    for (int i = 0; i < n; i++) {
        if (dx) a->adv[i] = dx[i];
        else if (a->text[i] == '\t' && (flags & SSA_TAB) && tabw > 0) a->adv[i] = tabw - x % tabw;
        else if (zero_width(a->text[i])) a->adv[i] = 0;
        if ((flags & (SSA_CLIP | SSA_FIT)) && reqwidth > 0 && x + a->adv[i] > reqwidth && (flags & SSA_CLIP)) {
            a->out_chars = i;
            break;
        }
        x += a->adv[i];
    }
    a->size.cx = x;
    a->size.cy = ((Cache *)a->cache)->tm.tmHeight;
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
    memcpy(dx, a->adv, a->n * sizeof(int));
    return S_OK;
}

USP HRESULT WINAPI ScriptStringCPtoX(void *ssa, int cp, BOOL trailing, int *x)
{
    SSA *a = ssa;
    if (!a || !x || cp < 0 || cp > a->n) return E_INVALIDARG;
    int pos = 0;
    for (int i = 0; i < cp && i < a->n; i++) pos += a->adv[i];
    if (trailing && cp < a->n) pos += a->adv[cp];
    *x = pos;
    return S_OK;
}

USP HRESULT WINAPI ScriptStringXtoCP(void *ssa, int x, int *cp, int *trailing)
{
    SSA *a = ssa;
    if (!a || !cp || !trailing) return E_INVALIDARG;
    if (x < 0) { *cp = -1; *trailing = 1; return S_OK; }
    int pos = 0;
    for (int i = 0; i < a->n; i++) {
        if (x < pos + a->adv[i]) { *cp = i; *trailing = x >= pos + a->adv[i] / 2; return S_OK; }
        pos += a->adv[i];
    }
    *cp = a->n; *trailing = 0;
    return S_OK;
}

USP HRESULT WINAPI ScriptStringValidate(void *ssa) { return ssa ? S_OK : E_INVALIDARG; }

USP HRESULT WINAPI ScriptStringOut(void *ssa, int x, int y, UINT opts, const RECT *rc, int minsel, int maxsel,
                                   BOOL disabled)
{
    (void)disabled;
    SSA *a = ssa;
    if (!a) return E_INVALIDARG;
    UINT o = ETO_GLYPH_INDEX_ | (opts & (ETO_OPAQUE_ | ETO_CLIPPED_));
    int n = a->out_chars;
    if (!ExtTextOutW(a->dc, x, y, o, rc, (LPCWSTR)a->glyphs, (UINT)n, a->adv)) return E_FAIL;
    if (minsel < 0) minsel = 0;
    if (maxsel > n) maxsel = n;
    if (minsel < maxsel) {                                  /* the selection, redrawn highlighted */
        int sx = x;
        for (int i = 0; i < minsel; i++) sx += a->adv[i];
        int w = 0;
        for (int i = minsel; i < maxsel; i++) w += a->adv[i];
        RECT r = { sx, y, sx + w, y + a->size.cy };
        COLORREF fg = SetTextColor(a->dc, GetSysColor(COLOR_HIGHLIGHTTEXT_));
        COLORREF bg = SetBkColor(a->dc, GetSysColor(COLOR_HIGHLIGHT));
        int mode = SetBkMode(a->dc, OPAQUE_);
        ExtTextOutW(a->dc, sx, y, ETO_GLYPH_INDEX_ | ETO_OPAQUE_, &r, (LPCWSTR)(a->glyphs + minsel),
                    (UINT)(maxsel - minsel), a->adv + minsel);
        SetBkMode(a->dc, mode);
        SetBkColor(a->dc, bg);
        SetTextColor(a->dc, fg);
    }
    return S_OK;
}
