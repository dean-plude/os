/*
 * text.c — GDI+ fonts, font families and string formats, and drawing and
 * measuring strings.  Families map onto the faces NovaOS ships the way
 * gdi32 maps LOGFONT names: monospaced names to DejaVu Sans Mono and the
 * rest to Inter.  Text is laid out in GDI+'s way: lines wrap at spaces to
 * the layout rectangle's width, the generic default format pads each line
 * by a sixth of an em on both sides, and the alignments place the lines in
 * the rectangle.
 */
#include "gdip.h"
#include <stddef.h>

#ifndef LF_FACESIZE
#define LF_FACESIZE 32
#endif
#ifndef FIXED_PITCH
#define FIXED_PITCH 1
#endif
#ifndef OBJ_FONT
#define OBJ_FONT 6
#endif

/* ---- faces --------------------------------------------------------------- */

enum { F_SANS, F_SANS_BOLD, F_MONO, F_MONO_BOLD, F_COUNT };
static const char *const g_face_files[F_COUNT] = {
    "C:\\Windows\\Fonts\\inter.ttf", "C:\\Windows\\Fonts\\interbd.ttf",
    "C:\\Windows\\Fonts\\dejavumono.ttf", "C:\\Windows\\Fonts\\dejavumonobd.ttf",
};
static plutovg_font_face_t *g_faces[F_COUNT];

static plutovg_font_face_t *face_get(int fi)
{
    plutovg_font_face_t *f = g_faces[fi];
    if (f) return f;
    f = plutovg_font_face_load_from_file(g_face_files[fi], 0);
    if (!f && (fi & 1)) return face_get(fi & ~1);           /* no bold file: the regular one */
    if (!f) return NULL;
    if (InterlockedCompareExchangePointer((void **)&g_faces[fi], f, NULL) != NULL) {
        plutovg_font_face_destroy(f);                       /* another thread loaded it first */
        f = g_faces[fi];
    }
    return f;
}

static WCHAR lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static BOOL name_is(const WCHAR *a, const char *b)
{
    for (; *a && *b; a++, b++) if (lower(*a) != lower((WCHAR)*b)) return FALSE;
    return !*a && !*b;
}
static BOOL name_has(const WCHAR *a, const char *b)
{
    for (; *a; a++) {
        int i = 0;
        while (b[i] && a[i] && lower(a[i]) == lower((WCHAR)b[i])) i++;
        if (!b[i]) return TRUE;
    }
    return FALSE;
}
static BOOL name_mono(const WCHAR *name)
{
    static const char *const mono[] = { "Courier", "Courier New", "Consolas", "Lucida Console", "Fixedsys", "Terminal",
                                        "Cascadia Mono", "Cascadia Code", "DejaVu Sans Mono", "Lucida Sans Typewriter",
                                        "Generic Monospace", 0 };
    for (int i = 0; mono[i]; i++) if (name_is(name, mono[i])) return TRUE;
    return name_has(name, "mono");
}

/* ---- font families --------------------------------------------------------- */

#define FontStyleBold      1
#define FontStyleItalic    2
#define FontStyleUnderline 4
#define FontStyleStrikeout 8

typedef struct GpFontFamily {
    WCHAR name[LF_FACESIZE];
    BOOL mono;
} GpFontFamily;

typedef struct GpFont {
    GpFontFamily fam;
    REAL size;
    INT style, unit;
} GpFont;

static GpStatus family_new(const WCHAR *name, GpFontFamily **out)
{
    GpFontFamily *f = xalloc(sizeof *f);
    if (!f) return OutOfMemory;
    lstrcpynW(f->name, name, LF_FACESIZE);
    f->mono = name_mono(name);
    *out = f;
    return Ok;
}

/* every name finds a family: names NovaOS has no font for get the nearest face */
GDIPAPI GpStatus GDIPCALL GdipCreateFontFamilyFromName(const WCHAR *name, void *collection, GpFontFamily **out)
{
    (void)collection;
    if (!name || !out) return InvalidParameter;
    return family_new(name, out);
}
GDIPAPI GpStatus GDIPCALL GdipGetGenericFontFamilySansSerif(GpFontFamily **out)
{ if (!out) return InvalidParameter; return family_new(L"Microsoft Sans Serif", out); }
GDIPAPI GpStatus GDIPCALL GdipGetGenericFontFamilySerif(GpFontFamily **out)
{ if (!out) return InvalidParameter; return family_new(L"Times New Roman", out); }
GDIPAPI GpStatus GDIPCALL GdipGetGenericFontFamilyMonospace(GpFontFamily **out)
{ if (!out) return InvalidParameter; return family_new(L"Courier New", out); }
GDIPAPI GpStatus GDIPCALL GdipCloneFontFamily(GpFontFamily *f, GpFontFamily **out)
{ if (!f || !out) return InvalidParameter; return family_new(f->name, out); }
GDIPAPI GpStatus GDIPCALL GdipDeleteFontFamily(GpFontFamily *f) { if (!f) return InvalidParameter; xfree(f); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetFamilyName(const GpFontFamily *f, WCHAR *name, WORD lang)
{
    (void)lang;
    if (!f || !name) return InvalidParameter;
    lstrcpynW(name, f->name, LF_FACESIZE);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipIsStyleAvailable(const GpFontFamily *f, INT style, BOOL *avail)
{ (void)style; if (!f || !avail) return InvalidParameter; *avail = TRUE; return Ok; }

static int family_face(const GpFontFamily *f, INT style)
{
    return (f->mono ? F_MONO : F_SANS) + ((style & FontStyleBold) ? 1 : 0);
}

/* design metrics, in font units of a 2048 em (what Windows' TrueType fonts use) */
static void design_metrics(const GpFontFamily *f, INT style, REAL *ascent, REAL *descent, REAL *gap)
{
    plutovg_font_face_t *face = face_get(family_face(f, style));
    float a = 1854, d = -434, g = 67;                    /* Arial's, if the file is missing */
    if (face) plutovg_font_face_get_metrics(face, 2048, &a, &d, &g, NULL);
    if (ascent) *ascent = a;
    if (descent) *descent = -d;
    if (gap) *gap = g;
}
GDIPAPI GpStatus GDIPCALL GdipGetEmHeight(const GpFontFamily *f, INT style, UINT16 *v)
{ if (!f || !v) return InvalidParameter; *v = 2048; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetCellAscent(const GpFontFamily *f, INT style, UINT16 *v)
{ REAL a; if (!f || !v) return InvalidParameter; design_metrics(f, style, &a, NULL, NULL); *v = (UINT16)(a + .5f); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetCellDescent(const GpFontFamily *f, INT style, UINT16 *v)
{ REAL d; if (!f || !v) return InvalidParameter; design_metrics(f, style, NULL, &d, NULL); *v = (UINT16)(d + .5f); return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetLineSpacing(const GpFontFamily *f, INT style, UINT16 *v)
{
    REAL a, d, g;
    if (!f || !v) return InvalidParameter;
    design_metrics(f, style, &a, &d, &g);
    *v = (UINT16)(a + d + g + .5f);
    return Ok;
}

/* ---- fonts ------------------------------------------------------------------- */

GDIPAPI GpStatus GDIPCALL GdipCreateFont(const GpFontFamily *fam, REAL size, INT style, INT unit, GpFont **out)
{
    if (!fam || !out || size <= 0 || unit == UnitDisplay || unit < UnitWorld || unit > UnitMillimeter) return InvalidParameter;
    GpFont *f = xalloc(sizeof *f);
    if (!f) return OutOfMemory;
    f->fam = *fam;
    f->size = size;
    f->style = style;
    f->unit = unit;
    *out = f;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCloneFont(GpFont *f, GpFont **out)
{ if (!f || !out) return InvalidParameter; return GdipCreateFont(&f->fam, f->size, f->style, f->unit, out); }
GDIPAPI GpStatus GDIPCALL GdipDeleteFont(GpFont *f) { if (!f) return InvalidParameter; xfree(f); return Ok; }

/* a LOGFONT's font: a negative height is the em, a positive one the cell (ascent + descent) */
static GpStatus font_from_logfont(HDC hdc, const LOGFONTW *lf, GpFont **out)
{
    GpFontFamily fam;
    lstrcpynW(fam.name, lf->lfFaceName[0] ? lf->lfFaceName : L"Microsoft Sans Serif", LF_FACESIZE);
    fam.mono = name_mono(fam.name) || (!lf->lfFaceName[0] && (lf->lfPitchAndFamily & 3) == FIXED_PITCH);
    INT style = (lf->lfWeight >= 600 ? FontStyleBold : 0) | (lf->lfItalic ? FontStyleItalic : 0) |
                (lf->lfUnderline ? FontStyleUnderline : 0) | (lf->lfStrikeOut ? FontStyleStrikeout : 0);
    REAL size;
    if (lf->lfHeight < 0) size = (REAL)-lf->lfHeight;
    else if (lf->lfHeight > 0) {
        REAL a, d;
        design_metrics(&fam, style, &a, &d, NULL);
        size = lf->lfHeight * 2048.f / (a + d);
    } else {
        size = 13.f * GetDeviceCaps(hdc, LOGPIXELSY) / 96.f;     /* the default GUI font */
        if (size <= 0) size = 13.f;
    }
    return GdipCreateFont(&fam, size, style, UnitWorld, out);
}
GDIPAPI GpStatus GDIPCALL GdipCreateFontFromLogfontW(HDC hdc, const LOGFONTW *lf, GpFont **out)
{
    if (!lf || !out) return InvalidParameter;
    return font_from_logfont(hdc, lf, out);
}
GDIPAPI GpStatus GDIPCALL GdipCreateFontFromLogfontA(HDC hdc, const LOGFONTA *lf, GpFont **out)
{
    if (!lf || !out) return InvalidParameter;
    LOGFONTW w;
    memcpy(&w, lf, offsetof(LOGFONTW, lfFaceName));
    MultiByteToWideChar(CP_ACP, 0, lf->lfFaceName, -1, w.lfFaceName, LF_FACESIZE);
    w.lfFaceName[LF_FACESIZE - 1] = 0;
    return font_from_logfont(hdc, &w, out);
}
GDIPAPI GpStatus GDIPCALL GdipCreateFontFromDC(HDC hdc, GpFont **out)
{
    if (!hdc || !out) return InvalidParameter;
    LOGFONTW lf;
    HGDIOBJ hf = GetCurrentObject(hdc, OBJ_FONT);
    if (!hf || !GetObjectW(hf, sizeof lf, &lf)) return GenericError;
    return font_from_logfont(hdc, &lf, out);
}

GDIPAPI GpStatus GDIPCALL GdipGetFamily(GpFont *f, GpFontFamily **out)
{ if (!f || !out) return InvalidParameter; return family_new(f->fam.name, out); }
GDIPAPI GpStatus GDIPCALL GdipGetFontSize(GpFont *f, REAL *size) { if (!f || !size) return InvalidParameter; *size = f->size; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetFontStyle(GpFont *f, INT *style) { if (!f || !style) return InvalidParameter; *style = f->style; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetFontUnit(GpFont *f, INT *unit) { if (!f || !unit) return InvalidParameter; *unit = f->unit; return Ok; }

/* the em in the graphics' world units */
static REAL em_world(const GpFont *f, const GpGraphics *g)
{
    if (f->unit == UnitWorld || !g) return gdip_unit_to_pixels(f->unit, f->size);
    REAL page = gdip_unit_to_pixels(g->page_unit, g->page_scale);
    return gdip_unit_to_pixels(f->unit, f->size) / (page > 0 ? page : 1);
}

GDIPAPI GpStatus GDIPCALL GdipGetFontHeight(const GpFont *f, const GpGraphics *g, REAL *h)
{
    if (!f || !h) return InvalidParameter;
    REAL a, d, gap;
    design_metrics(&f->fam, f->style, &a, &d, &gap);
    *h = em_world(f, g) * (a + d + gap) / 2048.f;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetFontHeightGivenDPI(const GpFont *f, REAL dpi, REAL *h)
{
    if (!f || !h) return InvalidParameter;
    REAL a, d, gap;
    design_metrics(&f->fam, f->style, &a, &d, &gap);
    *h = gdip_unit_to_pixels(f->unit, f->size) * dpi / 96.f * (a + d + gap) / 2048.f;
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipGetLogFontW(GpFont *f, GpGraphics *g, LOGFONTW *lf)
{
    if (!f || !g || !lf) return InvalidParameter;
    memset(lf, 0, sizeof *lf);
    lf->lfHeight = -(LONG)(em_world(f, g) * gdip_unit_to_pixels(g->page_unit, g->page_scale) + .5f);
    lf->lfWeight = (f->style & FontStyleBold) ? FW_BOLD : FW_NORMAL;
    lf->lfItalic = (f->style & FontStyleItalic) != 0;
    lf->lfUnderline = (f->style & FontStyleUnderline) != 0;
    lf->lfStrikeOut = (f->style & FontStyleStrikeout) != 0;
    lf->lfCharSet = DEFAULT_CHARSET;
    lf->lfQuality = DEFAULT_QUALITY;
    lstrcpynW(lf->lfFaceName, f->fam.name, LF_FACESIZE);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetLogFontA(GpFont *f, GpGraphics *g, LOGFONTA *lf)
{
    LOGFONTW w;
    GpStatus st = GdipGetLogFontW(f, g, &w);
    if (st != Ok) return st;
    memcpy(lf, &w, offsetof(LOGFONTA, lfFaceName));
    WideCharToMultiByte(CP_ACP, 0, w.lfFaceName, -1, lf->lfFaceName, LF_FACESIZE, NULL, NULL);
    lf->lfFaceName[LF_FACESIZE - 1] = 0;
    return Ok;
}

/* ---- string formats ------------------------------------------------------------ */

#define StringFormatFlagsNoFitBlackBox       0x0004
#define StringFormatFlagsMeasureTrailingSpaces 0x0800
#define StringFormatFlagsNoWrap              0x1000
#define StringFormatFlagsLineLimit           0x2000
#define StringFormatFlagsNoClip              0x4000
#define HotkeyPrefixNone 0
#define HotkeyPrefixShow 1
#define HotkeyPrefixHide 2

typedef struct { INT First, Length; } CharacterRange;

typedef struct GpStringFormat {
    INT flags, lang, align, line_align, trimming, hotkey, digit_subst;
    BOOL typographic;               /* no padding around the lines */
    INT nranges;
    CharacterRange *ranges;
    REAL first_tab, *tabs;
    INT ntabs;
} GpStringFormat;

GDIPAPI GpStatus GDIPCALL GdipCreateStringFormat(INT flags, LANGID lang, GpStringFormat **out)
{
    if (!out) return InvalidParameter;
    GpStringFormat *f = xalloc(sizeof *f);
    if (!f) return OutOfMemory;
    f->flags = flags;
    f->lang = lang;
    f->trimming = 1;                /* StringTrimmingCharacter */
    *out = f;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipDeleteStringFormat(GpStringFormat *f)
{
    if (!f) return InvalidParameter;
    xfree(f->ranges);
    xfree(f->tabs);
    xfree(f);
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipCloneStringFormat(const GpStringFormat *f, GpStringFormat **out)
{
    if (!f || !out) return InvalidParameter;
    GpStringFormat *n = xalloc(sizeof *n);
    if (!n) return OutOfMemory;
    *n = *f;
    n->ranges = NULL;
    n->tabs = NULL;
    if (f->nranges) {
        n->ranges = xalloc(f->nranges * sizeof *n->ranges);
        if (!n->ranges) { xfree(n); return OutOfMemory; }
        memcpy(n->ranges, f->ranges, f->nranges * sizeof *n->ranges);
    }
    if (f->ntabs) {
        n->tabs = xalloc(f->ntabs * sizeof *n->tabs);
        if (!n->tabs) { xfree(n->ranges); xfree(n); return OutOfMemory; }
        memcpy(n->tabs, f->tabs, f->ntabs * sizeof *n->tabs);
    }
    *out = n;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipStringFormatGetGenericDefault(GpStringFormat **out)
{
    return GdipCreateStringFormat(0, 0, out);
}
GDIPAPI GpStatus GDIPCALL GdipStringFormatGetGenericTypographic(GpStringFormat **out)
{
    GpStatus st = GdipCreateStringFormat(StringFormatFlagsNoFitBlackBox | StringFormatFlagsLineLimit | StringFormatFlagsNoClip,
                                         0, out);
    if (st == Ok) { (*out)->typographic = TRUE; (*out)->trimming = 0; }
    return st;
}
GDIPAPI GpStatus GDIPCALL GdipSetStringFormatFlags(GpStringFormat *f, INT flags) { if (!f) return InvalidParameter; f->flags = flags; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetStringFormatFlags(const GpStringFormat *f, INT *flags) { if (!f || !flags) return InvalidParameter; *flags = f->flags; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetStringFormatAlign(GpStringFormat *f, INT a) { if (!f) return InvalidParameter; f->align = a; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetStringFormatAlign(const GpStringFormat *f, INT *a) { if (!f || !a) return InvalidParameter; *a = f->align; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetStringFormatLineAlign(GpStringFormat *f, INT a) { if (!f) return InvalidParameter; f->line_align = a; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetStringFormatLineAlign(const GpStringFormat *f, INT *a) { if (!f || !a) return InvalidParameter; *a = f->line_align; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetStringFormatTrimming(GpStringFormat *f, INT t) { if (!f) return InvalidParameter; f->trimming = t; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetStringFormatTrimming(const GpStringFormat *f, INT *t) { if (!f || !t) return InvalidParameter; *t = f->trimming; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetStringFormatHotkeyPrefix(GpStringFormat *f, INT h) { if (!f) return InvalidParameter; f->hotkey = h; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipGetStringFormatHotkeyPrefix(const GpStringFormat *f, INT *h) { if (!f || !h) return InvalidParameter; *h = f->hotkey; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetStringFormatDigitSubstitution(GpStringFormat *f, LANGID lang, INT s)
{ if (!f) return InvalidParameter; f->lang = lang; f->digit_subst = s; return Ok; }
GDIPAPI GpStatus GDIPCALL GdipSetStringFormatTabStops(GpStringFormat *f, REAL first, INT n, const REAL *tabs)
{
    if (!f || n < 0 || (n && !tabs)) return InvalidParameter;
    REAL *t = NULL;
    if (n) {
        t = xalloc(n * sizeof *t);
        if (!t) return OutOfMemory;
        memcpy(t, tabs, n * sizeof *t);
    }
    xfree(f->tabs);
    f->tabs = t;
    f->ntabs = n;
    f->first_tab = first;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipSetStringFormatMeasurableCharacterRanges(GpStringFormat *f, INT n, const CharacterRange *r)
{
    if (!f || n < 0 || n > 32 || (n && !r)) return n > 32 ? ValueOverflow : InvalidParameter;
    CharacterRange *c = NULL;
    if (n) {
        c = xalloc(n * sizeof *c);
        if (!c) return OutOfMemory;
        memcpy(c, r, n * sizeof *c);
    }
    xfree(f->ranges);
    f->ranges = c;
    f->nranges = n;
    return Ok;
}
GDIPAPI GpStatus GDIPCALL GdipGetStringFormatMeasurableCharacterRangeCount(const GpStringFormat *f, INT *n)
{ if (!f || !n) return InvalidParameter; *n = f->nranges; return Ok; }

/* ---- layout ----------------------------------------------------------------------- */

/* The text laid out in a rectangle: each character's place (world units),
 * and the lines it is broken into. */
typedef struct {
    plutovg_font_face_t *face;
    REAL em, ascent, line_h, pad;
    int n;                          /* characters of the program's string */
    REAL *x, *w;                    /* per character: left edge in its line, advance */
    int *line;                      /* per character: its line, -1 when not shown */
    int nlines;
    struct { int start, end; REAL width, x, y; } *lines;   /* [start, end) of the string */
    GpRectF bounds;                 /* the lines' box, padding included */
    int fitted;                     /* characters laid out */
} Layout;

static void layout_free(Layout *l)
{
    xfree(l->x);
    xfree(l->w);
    xfree(l->line);
    xfree(l->lines);
}

static REAL tab_stop(const GpStringFormat *fmt, REAL em, REAL x)
{
    REAL pos = fmt ? fmt->first_tab : 0;
    if (fmt) for (int i = 0; i < fmt->ntabs; i++) {
        pos += fmt->tabs[i];
        if (pos > x && fmt->tabs[i] > 0) return pos;
    }
    REAL step = (fmt && fmt->ntabs && fmt->tabs[fmt->ntabs - 1] > 0) ? fmt->tabs[fmt->ntabs - 1] : em * 4;
    if (pos <= x) pos += ceilf((x - pos + 0.01f) / step) * step;
    return pos;
}

static GpStatus layout(Layout *l, const WCHAR *s, int n, const GpFont *font, const GpGraphics *g,
                       const GpRectF *rect, const GpStringFormat *fmt)
{
    memset(l, 0, sizeof *l);
    if (n < 0) n = (int)wcslen(s);
    l->n = n;
    l->face = face_get(family_face(&font->fam, font->style));
    l->em = em_world(font, g);
    REAL a, d, gap;
    design_metrics(&font->fam, font->style, &a, &d, &gap);
    l->ascent = l->em * a / 2048.f;
    l->line_h = l->em * (a + d + gap) / 2048.f;
    l->pad = (fmt && fmt->typographic) ? 0 : l->em / 6;
    INT flags = fmt ? fmt->flags : 0;
    int hotkey = fmt ? fmt->hotkey : HotkeyPrefixNone;

    l->x = xalloc((n + 1) * sizeof *l->x);
    l->w = xalloc((n + 1) * sizeof *l->w);
    l->line = xalloc((n + 1) * sizeof *l->line);
    l->lines = xalloc((n + 1) * sizeof *l->lines);
    if (!l->x || !l->w || !l->line || !l->lines) { layout_free(l); return OutOfMemory; }

    /* advances */
    for (int i = 0; i < n; i++) {
        WCHAR c = s[i];
        l->line[i] = 0;
        if (c == '&' && hotkey != HotkeyPrefixNone) {
            if (i + 1 < n && s[i + 1] == '&') { i++; l->line[i - 1] = -1; }
            else { l->line[i] = -1; continue; }
            c = '&';
        }
        if (c == '\r' || c == '\n' || c == '\t' || (c >= 0xDC00 && c <= 0xDFFF)) continue;
        plutovg_codepoint_t cp = c;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
            cp = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
        float adv = 0;
        if (l->face) plutovg_font_face_get_glyph_metrics(l->face, l->em, cp, &adv, NULL, NULL);
        else adv = l->em / 2;
        l->w[i] = adv;
    }

    /* lines: break at line ends, and (wrapping) after the last space that fits */
    REAL avail = (rect && rect->Width > 0 && !(flags & StringFormatFlagsNoWrap)) ? rect->Width - 2 * l->pad : 0;
    REAL max_h = (rect && rect->Height > 0) ? rect->Height : 0;
    int i = 0, nl = 0;
    REAL widest = 0;
    while (i < n || nl == 0) {
        if (max_h > 0 && nl > 0 && (nl + ((flags & StringFormatFlagsLineLimit) ? 1 : 0)) * l->line_h > max_h + 0.01f) break;
        int start = i, brk = -1;
        REAL x = 0;
        int end = i;
        BOOL hard = FALSE;
        for (; end < n; end++) {
            WCHAR c = s[end];
            if (c == '\r' || c == '\n') { hard = TRUE; break; }
            if (l->line[end] < 0) continue;
            REAL w = c == '\t' ? tab_stop(fmt, l->em, x) - x : l->w[end];
            if (avail > 0 && x + w > avail && end > start && c != ' ') {
                if (brk > start) end = brk;     /* after the space */
                break;
            }
            l->x[end] = x;
            l->w[end] = w;
            x += w;
            if (c == ' ') brk = end + 1;
        }
        /* the line's width, trailing spaces left out unless measured */
        REAL width = 0;
        for (int k = start; k < end; k++) {
            if (l->line[k] < 0) continue;
            l->line[k] = nl;
            if (s[k] != ' ' || (flags & StringFormatFlagsMeasureTrailingSpaces)) width = l->x[k] + l->w[k];
            else {
                int j = k;
                while (j < end && (s[j] == ' ' || l->line[j] < 0)) j++;
                if (j < end) width = l->x[k] + l->w[k];
            }
        }
        l->lines[nl].start = start;
        l->lines[nl].end = end;
        l->lines[nl].width = width;
        if (width > widest) widest = width;
        nl++;
        i = end;
        if (hard) {
            if (i < n && s[i] == '\r') l->line[i++] = -1;
            if (i < n && s[i] == '\n') l->line[i++] = -1;
            if (i == n) {   /* a final line end starts an empty line */
                l->lines[nl].start = l->lines[nl].end = n;
                l->lines[nl].width = 0;
                nl++;
                break;
            }
        }
        if (i >= n) break;
    }
    for (int k = i; k < n; k++) l->line[k] = -1;
    l->nlines = nl;
    l->fitted = i;

    /* placing the lines in the rectangle */
    REAL rx = rect ? rect->X : 0, ry = rect ? rect->Y : 0;
    REAL rw = rect ? rect->Width : 0, rh = rect ? rect->Height : 0;
    REAL total = nl * l->line_h;
    int align = fmt ? fmt->align : 0, valign = fmt ? fmt->line_align : 0;
    REAL top = ry;
    if (rh > 0 && valign == 1) top = ry + (rh - total) / 2;
    else if (rh > 0 && valign == 2) top = ry + rh - total;
    REAL bx0 = 1e30f, bx1 = -1e30f;
    for (int k = 0; k < nl; k++) {
        REAL lw = l->lines[k].width + 2 * l->pad, lx = rx;
        if (rw > 0 && align == 1) lx = rx + (rw - lw) / 2;
        else if (rw > 0 && align == 2) lx = rx + rw - lw;
        l->lines[k].x = lx + l->pad;
        l->lines[k].y = top + k * l->line_h;
        if (lx < bx0) bx0 = lx;
        if (lx + lw > bx1) bx1 = lx + lw;
    }
    if (nl == 0 || (nl == 1 && l->lines[0].width == 0 && n == 0)) { bx0 = rx; bx1 = rx; }
    l->bounds.X = bx0;
    l->bounds.Y = top;
    l->bounds.Width = bx1 - bx0;
    l->bounds.Height = total;
    (void)widest;
    return Ok;
}

/* ---- drawing and measuring ------------------------------------------------------------ */

GDIPAPI GpStatus GDIPCALL GdipDrawString(GpGraphics *g, const WCHAR *s, INT n, const GpFont *font,
                                         const GpRectF *rect, const GpStringFormat *fmt, const GpBrush *brush)
{
    if (!g || !s || !font || !rect || !brush) return InvalidParameter;
    Layout l;
    GpStatus st = layout(&l, s, n, font, g, rect, fmt);
    if (st != Ok) return st;
    if (!l.face || l.n == 0) { layout_free(&l); return Ok; }

    BOOL clip = !(fmt && (fmt->flags & StringFormatFlagsNoClip)) && rect->Width > 0 && rect->Height > 0;
    GpRectF area = l.bounds;
    if (clip) {
        REAL x0 = area.X > rect->X ? area.X : rect->X, y0 = area.Y > rect->Y ? area.Y : rect->Y;
        REAL x1 = area.X + area.Width < rect->X + rect->Width ? area.X + area.Width : rect->X + rect->Width;
        REAL y1 = area.Y + area.Height < rect->Y + rect->Height ? area.Y + area.Height : rect->Y + rect->Height;
        if (x1 <= x0 || y1 <= y0) { layout_free(&l); return Ok; }
        area = (GpRectF){ x0, y0, x1 - x0, y1 - y0 };
    }
    /* overhangs and italics reach a little past the advances */
    area.X -= l.em / 2; area.Width += l.em; area.Y -= l.em / 4; area.Height += l.em / 2;

    GdipOp op;
    if (!gdip_op_begin(&op, g, &area, FALSE)) { layout_free(&l); return Ok; }
    plutovg_canvas_t *c = op.c;
    if (clip) plutovg_canvas_clip_rect(c, rect->X, rect->Y, rect->Width, rect->Height);
    set_argb(c, brush->color);
    plutovg_canvas_set_font(c, l.face, l.em);
    plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
    BOOL italic = (font->style & FontStyleItalic) != 0;
    int hotkey = fmt ? fmt->hotkey : HotkeyPrefixNone;
    for (int k = 0; k < l.nlines; k++) {
        REAL base = l.lines[k].y + l.ascent;
        for (int i = l.lines[k].start; i < l.lines[k].end; i++) {
            WCHAR ch = s[i];
            if (l.line[i] < 0 || ch == ' ' || ch == '\t' || (ch >= 0xDC00 && ch <= 0xDFFF)) continue;
            plutovg_codepoint_t cp = ch;
            if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < l.n) cp = 0x10000 + ((ch - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            REAL x = l.lines[k].x + l.x[i];
            if (italic) {
                /* a 12° slant about the baseline */
                plutovg_canvas_save(c);
                plutovg_canvas_translate(c, x, base);
                plutovg_canvas_shear(c, -0.21f, 0);
                plutovg_canvas_add_glyph(c, cp, 0, 0);
                plutovg_canvas_fill(c);
                plutovg_canvas_restore(c);
            } else {
                plutovg_canvas_add_glyph(c, cp, x, base);
                plutovg_canvas_fill(c);
            }
            /* the shown hotkey prefix underlines the next character */
            if (hotkey == HotkeyPrefixShow && i > 0 && s[i - 1] == '&' && l.line[i - 1] < 0 && !(i > 1 && s[i - 2] == '&')) {
                plutovg_canvas_rect(c, x, base + l.em / 10, l.w[i], l.em / 16 > 1 ? l.em / 16 : 1);
                plutovg_canvas_fill(c);
            }
        }
        REAL thick = l.em / 14 > 1 ? l.em / 14 : 1;
        if ((font->style & FontStyleUnderline) && l.lines[k].width > 0) {
            plutovg_canvas_rect(c, l.lines[k].x, base + l.em / 10, l.lines[k].width, thick);
            plutovg_canvas_fill(c);
        }
        if ((font->style & FontStyleStrikeout) && l.lines[k].width > 0) {
            plutovg_canvas_rect(c, l.lines[k].x, base - l.em * 0.28f, l.lines[k].width, thick);
            plutovg_canvas_fill(c);
        }
    }
    gdip_op_end(&op);
    layout_free(&l);
    return Ok;
}

GDIPAPI GpStatus GDIPCALL GdipMeasureString(GpGraphics *g, const WCHAR *s, INT n, const GpFont *font,
                                            const GpRectF *rect, const GpStringFormat *fmt, GpRectF *bounds,
                                            INT *fitted, INT *lines)
{
    if (!g || !s || !font || !rect || !bounds) return InvalidParameter;
    Layout l;
    GpStatus st = layout(&l, s, n, font, g, rect, fmt);
    if (st != Ok) return st;
    *bounds = l.bounds;
    if (fitted) *fitted = l.fitted;
    if (lines) *lines = l.nlines;
    layout_free(&l);
    return Ok;
}

/* each measurable range's box: the union of its characters' cells */
GDIPAPI GpStatus GDIPCALL GdipMeasureCharacterRanges(GpGraphics *g, const WCHAR *s, INT n, const GpFont *font,
                                                     const GpRectF *rect, const GpStringFormat *fmt, INT count,
                                                     GpRegion **regions)
{
    if (!g || !s || !font || !rect || !fmt || !regions || count < fmt->nranges) return InvalidParameter;
    Layout l;
    GpStatus st = layout(&l, s, n, font, g, rect, fmt);
    if (st != Ok) return st;
    for (int r = 0; r < fmt->nranges; r++) {
        GpRegion *rg = regions[r];
        if (!rg) continue;
        int first = fmt->ranges[r].First, len = fmt->ranges[r].Length;
        if (len < 0) { first += len; len = -len; }
        REAL x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (int i = first; i < first + len && i < l.n; i++) {
            if (i < 0 || l.line[i] < 0) continue;
            int k = l.line[i];
            REAL cx = l.lines[k].x + l.x[i], cy = l.lines[k].y;
            if (cx < x0) x0 = cx;
            if (cx + l.w[i] > x1) x1 = cx + l.w[i];
            if (cy < y0) y0 = cy;
            if (cy + l.line_h > y1) y1 = cy + l.line_h;
        }
        gdip_region_reset(rg);
        if (x1 < x0) rg->kind = RgnEmpty;
        else {
            rg->kind = RgnRect;
            rg->r = (GpRectF){ x0, y0, x1 - x0, y1 - y0 };
        }
    }
    layout_free(&l);
    return Ok;
}

/* the integer forms */
GDIPAPI GpStatus GDIPCALL GdipDrawDriverString(GpGraphics *g, const UINT16 *text, INT n, const GpFont *font,
                                               const GpBrush *brush, const GpPointF *pos, INT flags, const GpMatrix *m)
{
    (void)flags; (void)m;
    if (!g || !text || !font || !brush || !pos) return InvalidParameter;
    /* glyph positions are the given ones; drawn as a string from the first */
    GpRectF r = { pos[0].X, pos[0].Y, 0, 0 };
    REAL a;
    design_metrics(&font->fam, font->style, &a, NULL, NULL);
    r.Y -= em_world(font, g) * a / 2048.f;
    GpStringFormat *fmt;
    GpStatus st = GdipStringFormatGetGenericTypographic(&fmt);
    if (st != Ok) return st;
    st = GdipDrawString(g, (const WCHAR *)text, n, font, &r, fmt, brush);
    GdipDeleteStringFormat(fmt);
    return st;
}
