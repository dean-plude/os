/*
 * layout.c — IDWriteTextFormat and IDWriteTextLayout
 *
 * A layout keeps its text and, per character, an index into a small table
 * of formatting attributes (family, collection, weight, style, stretch,
 * size, underline, strikethrough, drawing effect, locale).  Laying out:
 *
 *   1. bidi: characters are classed strong left, strong right, European
 *      number or neutral; neutrals between equal sides take that side,
 *      others the paragraph's direction; numbers after right-to-left text
 *      sit one level above it (a compact UAX #9);
 *   2. fonts: each character gets the font its attributes match in their
 *      collection, or a system font that has it (font fallback);
 *   3. runs of one attribute set, font and level are shaped with HarfBuzz
 *      from novatext.dll (OpenType features, Arabic joining, Indic
 *      reordering), or with the cmap and advances when novatext.dll is
 *      missing; glyphs are kept in logical order;
 *   4. glyphs group into clusters, clusters into lines (breaking after
 *      spaces and hyphens, between ideographs, at line separators, or
 *      anywhere when a word is wider than the line), lines are reordered
 *      visually and aligned.
 *
 * Draw hands the IDWriteTextRenderer one glyph run per visual piece of a
 * run, with underlines and strikethroughs, as DirectWrite does.
 */
#include "dwrite_int.h"

/* ---- interface ids ---- */
DEFINE_GUID(IID_IDWriteTextFormat, 0x9c906818, 0x31d7, 0x4fd3, 0xa1, 0x51, 0x7c, 0x5e, 0x22, 0x5d, 0xb5, 0x5a);
DEFINE_GUID(IID_IDWriteTextLayout, 0x53737037, 0x6d14, 0x410b, 0x9b, 0xfe, 0x0b, 0x18, 0x2b, 0xb7, 0x09, 0x61);

DWAPI HRESULT WINAPI DWriteCreateFactory(UINT32 type, REFIID riid, void **out);

/* ---- structures programs see ---- */
typedef struct { UINT32 startPosition, length; } DW_TEXT_RANGE;
typedef struct { UINT32 granularity, delimiter, delimiterCount; } DW_TRIMMING;
typedef struct { UINT32 length, trailingWhitespaceLength, newlineLength; float height, baseline; BOOL isTrimmed; } DW_LINE_METRICS;
typedef struct { float left, top, width, widthIncludingTrailingWhitespace, height, layoutWidth, layoutHeight;
                 UINT32 maxBidiReorderingDepth, lineCount; } DW_TEXT_METRICS;
typedef struct { float left, top, right, bottom; } DW_OVERHANG_METRICS;
typedef struct { float width; UINT16 length; UINT16 flags; } DW_CLUSTER_METRICS;
enum { CM_WRAP_AFTER = 1, CM_WHITESPACE = 2, CM_NEWLINE = 4, CM_SOFT_HYPHEN = 8, CM_RTL = 16 };
typedef struct { UINT32 textPosition, length; float left, top, width, height; UINT32 bidiLevel; BOOL isText, isTrimmed; } DW_HIT_TEST_METRICS;
typedef struct { float width, thickness, offset, runHeight; UINT32 readingDirection, flowDirection;
                 const WCHAR *localeName; UINT32 measuringMode; } DW_UNDERLINE;
typedef struct { float width, thickness, offset; UINT32 readingDirection, flowDirection;
                 const WCHAR *localeName; UINT32 measuringMode; } DW_STRIKETHROUGH;

enum { ALIGN_LEADING, ALIGN_TRAILING, ALIGN_CENTER, ALIGN_JUSTIFIED };
enum { PARA_NEAR, PARA_FAR, PARA_CENTER };
enum { WRAP_WRAP, WRAP_NONE, WRAP_EMERGENCY, WRAP_WHOLE_WORD, WRAP_CHARACTER };
enum { DIR_LTR, DIR_RTL };
enum { SPACING_DEFAULT, SPACING_UNIFORM, SPACING_PROPORTIONAL };

/* ---- HarfBuzz from novatext.dll, looked up at run time ---- */
typedef struct { UINT32 codepoint, mask, cluster, var1, var2; } hb_info;
typedef struct { INT32 x_advance, y_advance, x_offset, y_offset, var; } hb_pos;
static struct {
    void *(*blob_create)(const char *, unsigned, int, void *, void *);
    void (*blob_destroy)(void *);
    void *(*face_create)(void *, unsigned);
    void (*face_destroy)(void *);
    void *(*font_create)(void *);
    void (*font_destroy)(void *);
    void (*font_set_scale)(void *, int, int);
    void *(*buffer_create)(void);
    void (*buffer_destroy)(void *);
    void (*buffer_add_utf16)(void *, const UINT16 *, int, unsigned, int);
    void (*buffer_set_direction)(void *, int);
    void (*buffer_guess_segment_properties)(void *);
    void (*shape)(void *, void *, const void *, unsigned);
    hb_info *(*buffer_get_glyph_infos)(void *, unsigned *);
    hb_pos *(*buffer_get_glyph_positions)(void *, unsigned *);
} hb;
static int hb_state;            /* 0 unknown, 1 loaded, -1 missing */
static SRWLOCK hb_lock = SRWLOCK_INIT;

static BOOL hb_load(void)
{
    if (hb_state) return hb_state > 0;
    AcquireSRWLockExclusive(&hb_lock);
    if (!hb_state) {
        HMODULE m = LoadLibraryW(L"novatext.dll");
        static const char *const names[] = {
            "hb_blob_create", "hb_blob_destroy", "hb_face_create", "hb_face_destroy", "hb_font_create",
            "hb_font_destroy", "hb_font_set_scale", "hb_buffer_create", "hb_buffer_destroy", "hb_buffer_add_utf16",
            "hb_buffer_set_direction", "hb_buffer_guess_segment_properties", "hb_shape",
            "hb_buffer_get_glyph_infos", "hb_buffer_get_glyph_positions" };
        void **slots = (void **)&hb;
        int ok = m != NULL;
        for (int i = 0; ok && i < (int)(sizeof(names) / sizeof(names[0])); i++)
            ok = (slots[i] = (void *)GetProcAddress(m, names[i])) != NULL;
        hb_state = ok ? 1 : -1;
        if (!ok) dw_log("novatext.dll not available: text layout uses nominal glyphs");
    }
    ReleaseSRWLockExclusive(&hb_lock);
    return hb_state > 0;
}

/* one hb_font per face, kept for the process */
#define HB_FONTS 32
static struct { FaceData *face; void *font; } hb_fonts[HB_FONTS];

static void *hb_font_of(FaceData *fd)
{
    void *font = NULL;
    AcquireSRWLockExclusive(&hb_lock);
    int i;
    for (i = 0; i < HB_FONTS && hb_fonts[i].face; i++)
        if (hb_fonts[i].face == fd) { font = hb_fonts[i].font; break; }
    if (!font) {
        if (i == HB_FONTS) {    /* full: drop the oldest */
            hb.font_destroy(hb_fonts[0].font);
            font_data_release(hb_fonts[0].face->file);
            memmove(hb_fonts, hb_fonts + 1, sizeof(hb_fonts[0]) * (HB_FONTS - 1));
            i = HB_FONTS - 1;
        }
        void *blob = hb.blob_create((const char *)fd->file->bytes, fd->file->size, 1 /* read-only */, NULL, NULL);
        void *face = hb.face_create(blob, fd->index);
        font = hb.font_create(face);
        hb.font_set_scale(font, fd->upem, fd->upem);
        hb.face_destroy(face);
        hb.blob_destroy(blob);
        font_data_addref(fd->file);
        hb_fonts[i].face = fd;
        hb_fonts[i].font = font;
    }
    ReleaseSRWLockExclusive(&hb_lock);
    return font;
}

/* ---- small helpers ---- */
static UINT32 wlen(const WCHAR *s) { UINT32 n = 0; while (s && s[n]) n++; return n; }
static float fabs_(float v) { return v < 0 ? -v : v; }
static HRESULT copy_str(const WCHAR *s, WCHAR *buf, UINT32 size)
{
    UINT32 n = wlen(s);
    if (!buf || size <= n) { if (buf && size) buf[0] = 0; return E_NOT_SUFFICIENT_BUFFER_; }
    memcpy(buf, s, (n + 1) * sizeof(WCHAR));
    return S_OK;
}

static int is_rtl_char(UINT32 c)
{
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) ||
           (c >= 0x10800 && c <= 0x10FFF) || (c >= 0x1E800 && c <= 0x1EFFF);
}
static int is_space(UINT32 c) { return c == ' ' || c == '\t' || c == 0x3000 || c == 0xA0 || (c >= 0x2000 && c <= 0x200A); }
static int is_newline(UINT32 c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029 || c == 0x0B || c == 0x0C || c == 0x85; }
static int is_ideograph(UINT32 c)
{
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0x20000 && c <= 0x3FFFF) || (c >= 0xFF00 && c <= 0xFFEF);
}
static int is_letter_or_mark(UINT32 c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0xC0;
}

static UINT32 char_at(const WCHAR *s, UINT32 n, UINT32 i, UINT32 *len)
{
    UINT32 c = s[i];
    *len = 1;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
        *len = 2;
        return 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
    }
    return c;
}

/* ---- formatting attributes ---- */
typedef struct {
    WCHAR *family;
    void *coll;                 /* IDWriteFontCollection, NULL for the system's */
    UINT32 weight, style, stretch;
    float size;
    BOOL underline, strike;
    void *effect;               /* IUnknown */
    void *inline_obj;
    void *typography;
    WCHAR *locale;
} Attr;

static void attr_copy(Attr *d, const Attr *s)
{
    *d = *s;
    d->family = dw_wcsdup(s->family ? s->family : L"");
    d->locale = dw_wcsdup(s->locale ? s->locale : L"");
    if (d->coll) COM_ADDREF(d->coll);
    if (d->effect) COM_ADDREF(d->effect);
    if (d->inline_obj) COM_ADDREF(d->inline_obj);
    if (d->typography) COM_ADDREF(d->typography);
}
static void attr_free(Attr *a)
{
    dw_free(a->family);
    dw_free(a->locale);
    if (a->coll) COM_RELEASE(a->coll);
    if (a->effect) COM_RELEASE(a->effect);
    if (a->inline_obj) COM_RELEASE(a->inline_obj);
    if (a->typography) COM_RELEASE(a->typography);
}
static int attr_eq(const Attr *a, const Attr *b)
{
    return dw_wcsieq(a->family, b->family) && a->coll == b->coll && a->weight == b->weight && a->style == b->style &&
           a->stretch == b->stretch && a->size == b->size && a->underline == b->underline && a->strike == b->strike &&
           a->effect == b->effect && a->inline_obj == b->inline_obj && a->typography == b->typography &&
           dw_wcsieq(a->locale, b->locale);
}

/* ---- the format (shared by IDWriteTextFormat and IDWriteTextLayout) ---- */
typedef struct {
    Attr a;                     /* the defaults */
    UINT32 align, para_align, wrap, reading, flow;
    float tab;
    DW_TRIMMING trimming;
    void *trim_sign;
    UINT32 spacing_method;
    float spacing, baseline;
} Format;

static void format_copy(Format *d, const Format *s)
{
    *d = *s;
    attr_copy(&d->a, &s->a);
    if (d->trim_sign) COM_ADDREF(d->trim_sign);
}
static void format_free(Format *f)
{
    attr_free(&f->a);
    if (f->trim_sign) COM_RELEASE(f->trim_sign);
}

/* ---- layout results ---- */
typedef struct {
    UINT32 start, len;          /* characters */
    UINT32 attr;
    void *font;                 /* IDWriteFont */
    void *face;                 /* IDWriteFontFace */
    FaceData *fd;
    float size;
    UINT32 level;
    UINT32 g0, ng;              /* glyphs, in logical order */
    float ascent, descent, gap, ul_pos, ul_thick, st_pos, st_thick;
} Run;

typedef struct {
    UINT32 start, len;          /* characters */
    UINT32 g0, ng;              /* glyphs */
    UINT32 run;
    float width;
    UINT16 flags;
    UINT32 line;
    float x;                    /* visual left, relative to the layout */
} Cluster;

typedef struct {
    UINT32 c0, nc;              /* clusters */
    UINT32 *order;              /* visual order of its clusters */
    float width, width_ws, height, baseline, top, x;
    UINT32 trailing_ws, newline_len;
} Line;

typedef struct Layout {
    const void *const *vtbl;
    LONG ref;
    int is_layout;
    Format f;
    float max_w, max_h;
    WCHAR *text;
    UINT32 n;
    Attr *attrs;
    UINT32 nattrs;
    UINT16 *attr_of;            /* per character */
    /* computed */
    BOOL valid;
    Run *runs; UINT32 nruns;
    UINT16 *gid; float *adv; DW_GLYPH_OFFSET *off; UINT32 ng;
    UINT16 *cmap;               /* per character: glyph index within its run */
    Cluster *cl; UINT32 ncl;
    Line *lines; UINT32 nlines;
    DW_TEXT_METRICS m;
} Layout;

extern const void *const fmt_vtbl[], *const lay_vtbl[];

static void *system_collection_iface(void)
{
    void *f = NULL, *c = NULL;
    if (SUCCEEDED(DWriteCreateFactory(0, &IID_IDWriteFactory, &f)) && f) {
        typedef HRESULT (STDMETHODCALLTYPE *GetSys)(void *, void **, BOOL);
        ((GetSys)VT(f)[3])(f, &c, FALSE);
        COM_RELEASE(f);
    }
    return c;
}

static void invalidate(Layout *l);

static void layout_free_results(Layout *l)
{
    for (UINT32 i = 0; i < l->nruns; i++) {
        if (l->runs[i].face) COM_RELEASE(l->runs[i].face);
        if (l->runs[i].font) COM_RELEASE(l->runs[i].font);
    }
    for (UINT32 i = 0; i < l->nlines; i++) dw_free(l->lines[i].order);
    dw_free(l->runs); dw_free(l->gid); dw_free(l->adv); dw_free(l->off); dw_free(l->cmap);
    dw_free(l->cl); dw_free(l->lines);
    l->runs = NULL; l->gid = NULL; l->adv = NULL; l->off = NULL; l->cmap = NULL; l->cl = NULL; l->lines = NULL;
    l->nruns = l->ng = l->ncl = l->nlines = 0;
    l->valid = FALSE;
}

static void invalidate(Layout *l) { layout_free_results(l); }

/* ---- fonts ---- */
typedef HRESULT (STDMETHODCALLTYPE *FindFamily)(void *, const WCHAR *, UINT32 *, BOOL *);
typedef HRESULT (STDMETHODCALLTYPE *GetFamily)(void *, UINT32, void **);
typedef HRESULT (STDMETHODCALLTYPE *FirstMatching)(void *, UINT32, UINT32, UINT32, void **);
typedef HRESULT (STDMETHODCALLTYPE *HasChar)(void *, UINT32, BOOL *);
typedef HRESULT (STDMETHODCALLTYPE *CreateFace)(void *, void **);
typedef UINT32 (STDMETHODCALLTYPE *GetCount)(void *);

static void *match_font(void *coll, const WCHAR *family, UINT32 weight, UINT32 style, UINT32 stretch)
{
    UINT32 idx = 0;
    BOOL exists = FALSE;
    void *fam = NULL, *font = NULL;
    if (!coll || FAILED(((FindFamily)VT(coll)[5])(coll, family, &idx, &exists)) || !exists) return NULL;
    if (FAILED(((GetFamily)VT(coll)[4])(coll, idx, &fam)) || !fam) return NULL;
    ((FirstMatching)VT(fam)[7])(fam, weight, stretch, style, &font);
    COM_RELEASE(fam);
    return font;
}

static BOOL font_has(void *font, UINT32 c)
{
    BOOL has = FALSE;
    return font && SUCCEEDED(((HasChar)VT(font)[12])(font, c, &has)) && has;
}

static const WCHAR *const fallback_families[] = {
    L"Noto Sans Arabic", L"Noto Sans Hebrew", L"Noto Sans Devanagari", L"DejaVu Sans", L"Inter", 0 };

/* a font with @c in it, for text whose own font lacks it */
static void *fallback_font(void *sys, UINT32 c, const Attr *a)
{
    for (int i = 0; fallback_families[i]; i++) {
        void *f = match_font(sys, fallback_families[i], a->weight, a->style, a->stretch);
        if (font_has(f, c)) return f;
        if (f) COM_RELEASE(f);
    }
    UINT32 n = sys ? ((GetCount)VT(sys)[3])(sys) : 0;
    for (UINT32 i = 0; i < n; i++) {
        void *fam = NULL, *f = NULL;
        if (FAILED(((GetFamily)VT(sys)[4])(sys, i, &fam)) || !fam) continue;
        ((FirstMatching)VT(fam)[7])(fam, a->weight, a->stretch, a->style, &f);
        COM_RELEASE(fam);
        if (font_has(f, c)) return f;
        if (f) COM_RELEASE(f);
    }
    return NULL;
}

/* ---- bidi ---- */
enum { BC_L, BC_R, BC_EN, BC_N };

static UINT32 *bidi_levels(const WCHAR *s, UINT32 n, UINT32 para)
{
    UINT32 *lv = dw_zalloc(sizeof(UINT32) * (n ? n : 1));
    BYTE *cls = dw_zalloc(n ? n : 1);
    if (!lv || !cls) { dw_free(cls); return lv; }
    for (UINT32 i = 0; i < n;) {
        UINT32 len, c = char_at(s, n, i, &len);
        BYTE k = is_rtl_char(c) ? BC_R : (c >= '0' && c <= '9') ? BC_EN :
                 (is_letter_or_mark(c) && c != 0xA0 && !(c >= 0x2000 && c <= 0x206F) && !is_space(c)) ? BC_L : BC_N;
        for (UINT32 k2 = 0; k2 < len; k2++) cls[i + k2] = k;
        i += len;
    }
    UINT32 base = para == DIR_RTL ? 1 : 0;
    int last_strong = para == DIR_RTL ? BC_R : BC_L;
    for (UINT32 i = 0; i < n; i++) {
        if (cls[i] == BC_L) { lv[i] = base == 1 ? 2 : 0; last_strong = BC_L; }
        else if (cls[i] == BC_R) { lv[i] = 1; last_strong = BC_R; }
        else if (cls[i] == BC_EN) lv[i] = last_strong == BC_R ? 2 : (base == 1 ? 2 : 0);
        else {
            /* a neutral: the side on both sides of it, else the paragraph's */
            int next = -1;
            for (UINT32 j = i + 1; j < n; j++)
                if (cls[j] == BC_L || cls[j] == BC_R) { next = cls[j]; break; }
                else if (cls[j] == BC_EN) { next = last_strong == BC_R ? BC_R : BC_L; break; }
            int side = (next == last_strong) ? last_strong : (para == DIR_RTL ? BC_R : BC_L);
            if (next == -1) side = para == DIR_RTL ? BC_R : BC_L;
            lv[i] = side == BC_R ? 1 : (base == 1 ? 2 : 0);
        }
    }
    dw_free(cls);
    return lv;
}

/* ---- shaping ---- */
static int grow_glyphs(Layout *l, UINT32 need, UINT32 *cap)
{
    if (l->ng + need <= *cap) return 1;
    UINT32 nc = (*cap ? *cap * 2 : 64);
    while (nc < l->ng + need) nc *= 2;
    UINT16 *g = dw_zalloc(sizeof(UINT16) * nc);
    float *a = dw_zalloc(sizeof(float) * nc);
    DW_GLYPH_OFFSET *o = dw_zalloc(sizeof(DW_GLYPH_OFFSET) * nc);
    if (!g || !a || !o) { dw_free(g); dw_free(a); dw_free(o); return 0; }
    if (l->ng) {
        memcpy(g, l->gid, sizeof(UINT16) * l->ng);
        memcpy(a, l->adv, sizeof(float) * l->ng);
        memcpy(o, l->off, sizeof(DW_GLYPH_OFFSET) * l->ng);
    }
    dw_free(l->gid); dw_free(l->adv); dw_free(l->off);
    l->gid = g; l->adv = a; l->off = o;
    *cap = nc;
    return 1;
}

/* @clusters gets, per glyph, the character it starts from */
static void shape_run(Layout *l, Run *r, UINT32 *cap, UINT32 **clusters, UINT32 *ccap)
{
    float scale = r->size / (r->fd->upem ? r->fd->upem : 1000);
    r->g0 = l->ng;
    if (hb_load()) {
        void *font = hb_font_of(r->fd), *buf = hb.buffer_create();
        hb.buffer_add_utf16(buf, (const UINT16 *)l->text, (int)l->n, r->start, (int)r->len);
        hb.buffer_set_direction(buf, (r->level & 1) ? 5 : 4);
        hb.buffer_guess_segment_properties(buf);
        hb.shape(font, buf, NULL, 0);
        unsigned n = 0;
        hb_info *info = hb.buffer_get_glyph_infos(buf, &n);
        hb_pos *pos = hb.buffer_get_glyph_positions(buf, &n);
        if (grow_glyphs(l, n, cap)) {
            if (*ccap < l->ng + n) {
                UINT32 nc = (l->ng + n) * 2;
                UINT32 *c2 = dw_zalloc(sizeof(UINT32) * nc);
                if (c2 && *clusters) memcpy(c2, *clusters, sizeof(UINT32) * l->ng);
                dw_free(*clusters);
                *clusters = c2;
                *ccap = nc;
            }
            for (unsigned i = 0; i < n; i++) {
                /* right-to-left runs come out in visual order: keep them logical */
                unsigned k = (r->level & 1) ? n - 1 - i : i;
                UINT32 g = l->ng + i;
                l->gid[g] = (UINT16)info[k].codepoint;
                l->adv[g] = pos[k].x_advance * scale;
                l->off[g].advanceOffset = ((r->level & 1) ? -pos[k].x_offset : pos[k].x_offset) * scale;
                l->off[g].ascenderOffset = pos[k].y_offset * scale;
                if (*clusters) (*clusters)[g] = info[k].cluster;
            }
            l->ng += n;
        }
        hb.buffer_destroy(buf);
    } else {
        /* nominal glyphs: one per character, advances and kerning from the font */
        if (grow_glyphs(l, r->len, cap)) {
            if (*ccap < l->ng + r->len) {
                UINT32 nc = (l->ng + r->len) * 2;
                UINT32 *c2 = dw_zalloc(sizeof(UINT32) * nc);
                if (c2 && *clusters) memcpy(c2, *clusters, sizeof(UINT32) * l->ng);
                dw_free(*clusters);
                *clusters = c2;
                *ccap = nc;
            }
            for (UINT32 i = r->start; i < r->start + r->len;) {
                UINT32 len, c = char_at(l->text, l->n, i, &len);
                UINT32 g = l->ng++;
                l->gid[g] = face_glyph_index(r->fd, c == '\t' ? ' ' : c);
                DW_GLYPH_METRICS gm;
                face_glyph_metrics(r->fd, l->gid[g], &gm);
                l->adv[g] = is_newline(c) ? 0 : gm.advanceWidth * scale;
                if (g > r->g0 && !(r->level & 1)) l->adv[g - 1] += face_kern(r->fd, l->gid[g - 1], l->gid[g]) * scale;
                if (*clusters) (*clusters)[g] = i;
                i += len;
            }
        }
    }
    r->ng = l->ng - r->g0;
}

/* ---- the layout itself ---- */
static float tab_width(Layout *l, float x)
{
    float t = l->f.tab > 0 ? l->f.tab : 4 * l->f.a.size;
    int k = (int)(x / t) + 1;
    return k * t - x;
}

static void reorder_line(Layout *l, Line *ln)
{
    ln->order = dw_zalloc(sizeof(UINT32) * (ln->nc ? ln->nc : 1));
    if (!ln->order) return;
    UINT32 maxl = 0, minodd = 99;
    for (UINT32 i = 0; i < ln->nc; i++) {
        ln->order[i] = ln->c0 + i;
        UINT32 lv = l->runs[l->cl[ln->c0 + i].run].level;
        if (lv > maxl) maxl = lv;
        if ((lv & 1) && lv < minodd) minodd = lv;
    }
    for (UINT32 lvl = maxl; lvl >= minodd && lvl > 0; lvl--) {
        UINT32 i = 0;
        while (i < ln->nc) {
            if (l->runs[l->cl[ln->order[i]].run].level < lvl) { i++; continue; }
            UINT32 j = i;
            while (j < ln->nc && l->runs[l->cl[ln->order[j]].run].level >= lvl) j++;
            for (UINT32 a = i, b = j - 1; a < b; a++, b--) {
                UINT32 t = ln->order[a]; ln->order[a] = ln->order[b]; ln->order[b] = t;
            }
            i = j;
        }
    }
}

typedef struct { void *font, *face; FaceData *fd; UINT32 sims; } UFont;

static int ufont_add(UFont *u, UINT32 *nu, void *font)
{
    void *face = NULL;
    if (!font || FAILED(((CreateFace)VT(font)[13])(font, &face)) || !face) { if (font) COM_RELEASE(font); return -1; }
    FontFace *ff = font_face_from(face);
    if (!ff) { COM_RELEASE(face); COM_RELEASE(font); return -1; }
    for (UINT32 i = 0; i < *nu; i++)
        if (u[i].fd == ff->face && u[i].sims == ff->sims) { COM_RELEASE(face); COM_RELEASE(font); return (int)i; }
    if (*nu >= 64) { COM_RELEASE(face); COM_RELEASE(font); return 0; }
    u[*nu].font = font;
    u[*nu].face = face;
    u[*nu].fd = ff->face;
    u[*nu].sims = ff->sims;
    return (int)(*nu)++;
}

/* the font an attribute set asks for, or a stand-in */
static int primary_font(UFont *u, UINT32 *nu, const Attr *a, void *sys)
{
    void *f = match_font(a->coll ? a->coll : sys, a->family, a->weight, a->style, a->stretch);
    if (!f && a->coll) f = match_font(sys, a->family, a->weight, a->style, a->stretch);
    if (!f) f = match_font(sys, L"Segoe UI", a->weight, a->style, a->stretch);
    if (!f && sys && ((GetCount)VT(sys)[3])(sys)) {
        void *fam = NULL;
        if (SUCCEEDED(((GetFamily)VT(sys)[4])(sys, 0, &fam)) && fam) {
            ((FirstMatching)VT(fam)[7])(fam, a->weight, a->stretch, a->style, &f);
            COM_RELEASE(fam);
        }
    }
    return f ? ufont_add(u, nu, f) : -1;
}

static int no_font_needed(UINT32 c)
{
    return c < 0x20 || is_space(c) || is_newline(c) || (c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0xFE00 && c <= 0xFE0F) || c == 0x2060 || c == 0xFEFF;
}

static void compute(Layout *l)
{
    if (l->valid) return;
    layout_free_results(l);
    l->valid = TRUE;
    UINT32 n = l->n;
    void *sys = system_collection_iface();
    UINT32 *lv = bidi_levels(l->text, n, l->f.reading);
    int *fi = dw_zalloc(sizeof(int) * (n ? n : 1));
    UFont uf[64];
    UINT32 nu = 0;
    int *prim = dw_zalloc(sizeof(int) * (l->nattrs ? l->nattrs : 1));
    if (!lv || !fi || !prim) goto out;
    for (UINT32 a = 0; a < l->nattrs; a++) prim[a] = -2;
    int def = primary_font(uf, &nu, l->nattrs ? &l->attrs[l->n ? l->attr_of[0] : 0] : &l->f.a, sys);

    /* fonts, with fallback */
    for (UINT32 i = 0; i < n;) {
        UINT32 len, c = char_at(l->text, n, i, &len), a = l->attr_of[i];
        if (prim[a] == -2) prim[a] = primary_font(uf, &nu, &l->attrs[a], sys);
        int f = prim[a];
        if (no_font_needed(c)) {
            if (i && l->attr_of[i - 1] == a && fi[i - 1] >= 0) f = fi[i - 1];
        } else if (f < 0 || !font_has(uf[f].font, c)) {
            int found = -1;
            for (UINT32 k = 0; k < nu && found < 0; k++)
                if (font_has(uf[k].font, c)) found = (int)k;
            if (found < 0) {
                void *fb = fallback_font(sys, c, &l->attrs[a]);
                if (fb) found = ufont_add(uf, &nu, fb);
            }
            if (found >= 0) f = found;
        }
        if (f < 0) f = def;
        for (UINT32 k = 0; k < len; k++) fi[i + k] = f;
        i += len;
    }
    if (def < 0 && n) def = fi[0];

    /* runs */
    UINT32 rcap = 8;
    l->runs = dw_zalloc(sizeof(Run) * rcap);
    for (UINT32 i = 0; i < n && l->runs;) {
        UINT32 j = i + 1;
        while (j < n && l->attr_of[j] == l->attr_of[i] && fi[j] == fi[i] && lv[j] == lv[i]) j++;
        if (l->nruns == rcap) {
            Run *nr = dw_zalloc(sizeof(Run) * rcap * 2);
            if (!nr) break;
            memcpy(nr, l->runs, sizeof(Run) * l->nruns);
            dw_free(l->runs);
            l->runs = nr;
            rcap *= 2;
        }
        Run *r = &l->runs[l->nruns++];
        r->start = i;
        r->len = j - i;
        r->attr = l->attr_of[i];
        r->level = lv[i];
        r->size = l->attrs[r->attr].size;
        int f = fi[i] >= 0 ? fi[i] : 0;
        if (nu) {
            r->font = uf[f].font; COM_ADDREF(r->font);
            r->face = uf[f].face; COM_ADDREF(r->face);
            r->fd = uf[f].fd;
        }
        i = j;
    }
    if (nu && l->nruns != (n ? l->nruns : 0)) {}

    /* shaping and clusters */
    UINT32 gcap = 0, ccap = 0, *gcl = NULL, clcap = 16;
    l->cmap = dw_zalloc(sizeof(UINT16) * (n ? n : 1));
    l->cl = dw_zalloc(sizeof(Cluster) * clcap);
    for (UINT32 ri = 0; ri < l->nruns && l->cl; ri++) {
        Run *r = &l->runs[ri];
        if (!r->fd) continue;
        float scale = r->size / (r->fd->upem ? r->fd->upem : 1000);
        const DW_FONT_METRICS *m = &r->fd->metrics.m;
        r->ascent = m->ascent * scale;
        r->descent = m->descent * scale;
        r->gap = m->lineGap * scale;
        r->ul_pos = -m->underlinePosition * scale;
        r->ul_thick = m->underlineThickness * scale;
        r->st_pos = -m->strikethroughPosition * scale;
        r->st_thick = m->strikethroughThickness * scale;
        shape_run(l, r, &gcap, &gcl, &ccap);
        if (!gcl) continue;
        UINT32 g = r->g0, end = r->g0 + r->ng;
        UINT32 first = l->ncl;
        while (g < end) {
            UINT32 cs = gcl[g], ge = g + 1;
            while (ge < end && gcl[ge] <= cs) ge++;
            UINT32 ce = ge < end ? gcl[ge] : r->start + r->len;
            if (ce <= cs) ce = cs + 1;
            if (cs < r->start) cs = r->start;
            if (l->ncl == clcap) {
                Cluster *nc = dw_zalloc(sizeof(Cluster) * clcap * 2);
                if (!nc) break;
                memcpy(nc, l->cl, sizeof(Cluster) * l->ncl);
                dw_free(l->cl);
                l->cl = nc;
                clcap *= 2;
            }
            Cluster *c = &l->cl[l->ncl++];
            c->start = cs;
            c->len = ce - cs;
            c->g0 = g;
            c->ng = ge - g;
            c->run = ri;
            for (UINT32 k = g; k < ge; k++) c->width += l->adv[k];
            for (UINT32 k = cs; k < ce && k < n; k++) l->cmap[k] = (UINT16)(g - r->g0);
            g = ge;
        }
        /* the characters no glyph names (dropped by the shaper) join the cluster before them */
        (void)first;
    }
    dw_free(gcl);

    /* cluster properties; "\r\n" is one newline */
    for (UINT32 i = 0; i < l->ncl; i++) {
        Cluster *c = &l->cl[i];
        UINT32 len, ch = char_at(l->text, n, c->start, &len);
        if (ch == '\r' && i + 1 < l->ncl && l->text[l->cl[i + 1].start] == '\n' &&
            l->cl[i + 1].run == c->run) {
            c->len += l->cl[i + 1].len;
            c->ng += l->cl[i + 1].ng;
            memmove(&l->cl[i + 1], &l->cl[i + 2], sizeof(Cluster) * (l->ncl - i - 2));
            l->ncl--;
        }
        BOOL ws = TRUE;
        for (UINT32 k = c->start; k < c->start + c->len; k++)
            if (!is_space(l->text[k])) ws = FALSE;
        c->flags = 0;
        if (is_newline(ch)) {
            c->flags |= CM_NEWLINE | CM_WHITESPACE | CM_WRAP_AFTER;
            for (UINT32 k = c->g0; k < c->g0 + c->ng; k++) l->adv[k] = 0;
            c->width = 0;
        } else if (ws) c->flags |= CM_WHITESPACE | CM_WRAP_AFTER;
        if (ch == '-' || ch == 0x2010 || ch == 0x2013 || ch == 0x2014 || is_ideograph(ch)) c->flags |= CM_WRAP_AFTER;
        if (ch == 0xAD) c->flags |= CM_SOFT_HYPHEN | CM_WRAP_AFTER;
        if (l->runs[c->run].level & 1) c->flags |= CM_RTL;
    }
    for (UINT32 i = 0; i + 1 < l->ncl; i++) {
        UINT32 len, next = char_at(l->text, n, l->cl[i + 1].start, &len);
        if (is_ideograph(next)) l->cl[i].flags |= CM_WRAP_AFTER;
    }

    /* the default line metrics, for empty lines */
    float d_asc = 0, d_desc = 0, d_gap = 0;
    if (def >= 0) {
        const DW_FONT_METRICS *m = &uf[def].fd->metrics.m;
        float s = l->f.a.size / (m->designUnitsPerEm ? m->designUnitsPerEm : 1000);
        d_asc = m->ascent * s; d_desc = m->descent * s; d_gap = m->lineGap * s;
    }

    /* lines */
    UINT32 lcap = 8;
    l->lines = dw_zalloc(sizeof(Line) * lcap);
    BOOL wrap = l->f.wrap != WRAP_NONE;
    UINT32 i = 0;
    while (l->lines && (i < l->ncl || !l->nlines || (l->cl[l->ncl - 1].flags & CM_NEWLINE && i == l->ncl))) {
        UINT32 s = i, j = i, last_break = (UINT32)-1;
        float x = 0;
        BOOL newline = FALSE;
        while (j < l->ncl) {
            Cluster *c = &l->cl[j];
            if (c->flags & CM_NEWLINE) { j++; newline = TRUE; break; }
            if (l->text[c->start] == '\t') {
                c->width = tab_width(l, x);
                if (c->ng) l->adv[c->g0] = c->width;
            }
            if (wrap && !(c->flags & CM_WHITESPACE) && j > s && x + c->width > l->max_w + 0.001f) {
                if (last_break != (UINT32)-1) j = last_break + 1;
                break;
            }
            x += c->width;
            if (c->flags & CM_WRAP_AFTER) last_break = j;
            j++;
        }
        if (l->nlines == lcap) {
            Line *nl = dw_zalloc(sizeof(Line) * lcap * 2);
            if (!nl) break;
            memcpy(nl, l->lines, sizeof(Line) * l->nlines);
            dw_free(l->lines);
            l->lines = nl;
            lcap *= 2;
        }
        Line *ln = &l->lines[l->nlines++];
        ln->c0 = s;
        ln->nc = j - s;
        float asc = 0, desc = 0;
        for (UINT32 k = s; k < j; k++) {
            Run *r = &l->runs[l->cl[k].run];
            if (r->ascent > asc) asc = r->ascent;
            if (r->descent + r->gap > desc) desc = r->descent + r->gap;
            ln->width_ws += l->cl[k].width;
        }
        if (s == j) { asc = d_asc; desc = d_desc + d_gap; }
        UINT32 k = j;
        while (k > s && (l->cl[k - 1].flags & CM_WHITESPACE)) {
            k--;
            ln->trailing_ws += l->cl[k].len;
            if (l->cl[k].flags & CM_NEWLINE) ln->newline_len = l->cl[k].len;
        }
        for (UINT32 q = s; q < k; q++) ln->width += l->cl[q].width;
        if (l->f.spacing_method == SPACING_UNIFORM) {
            ln->height = l->f.spacing;
            ln->baseline = l->f.baseline;
        } else if (l->f.spacing_method == SPACING_PROPORTIONAL) {
            ln->height = (asc + desc) * l->f.spacing;
            ln->baseline = asc * l->f.baseline;
        } else {
            ln->height = asc + desc;
            ln->baseline = asc;
        }
        (void)newline;
        i = j;
        if (i == l->ncl && !(l->ncl && (l->cl[l->ncl - 1].flags & CM_NEWLINE) && ln->nc)) break;
    }

    /* visual order, alignment and positions */
    float y = 0, maxw = 0, maxws = 0, minx = 1e30f;
    UINT32 maxlevel = 0;
    for (UINT32 q = 0; q < l->nruns; q++) if (l->runs[q].level > maxlevel) maxlevel = l->runs[q].level;
    BOOL rtl = l->f.reading == DIR_RTL;
    for (UINT32 q = 0; q < l->nlines; q++) {
        Line *ln = &l->lines[q];
        reorder_line(l, ln);
        float ws = ln->width_ws - ln->width, off;
        UINT32 al = l->f.align;
        if (al == ALIGN_CENTER) off = (l->max_w - ln->width) / 2;
        else if ((al == ALIGN_TRAILING) != rtl) off = l->max_w - ln->width;
        else off = 0;
        ln->x = off - (rtl ? ws : 0);
        float acc = ln->x;
        for (UINT32 c = 0; ln->order && c < ln->nc; c++) {
            Cluster *cl = &l->cl[ln->order[c]];
            cl->x = acc;
            cl->line = q;
            acc += cl->width;
        }
        ln->top = y;
        y += ln->height;
        if (ln->width > maxw) maxw = ln->width;
        if (ln->width_ws > maxws) maxws = ln->width_ws;
        float left = ln->x + (rtl ? ws : 0);
        if (left < minx) minx = left;
    }
    float poff = 0;
    if (l->f.para_align == PARA_FAR) poff = l->max_h - y;
    else if (l->f.para_align == PARA_CENTER) poff = (l->max_h - y) / 2;
    for (UINT32 q = 0; q < l->nlines; q++) l->lines[q].top += poff;
    l->m.left = l->nlines ? minx : 0;
    l->m.top = poff;
    l->m.width = maxw;
    l->m.widthIncludingTrailingWhitespace = maxws;
    l->m.height = y;
    l->m.layoutWidth = l->max_w;
    l->m.layoutHeight = l->max_h;
    l->m.maxBidiReorderingDepth = maxlevel + 1;
    l->m.lineCount = l->nlines;
out:
    for (UINT32 q = 0; q < nu; q++) { COM_RELEASE(uf[q].face); COM_RELEASE(uf[q].font); }
    if (sys) COM_RELEASE(sys);
    dw_free(prim);
    dw_free(fi);
    dw_free(lv);
}

/* ---- drawing ---- */
typedef HRESULT (STDMETHODCALLTYPE *DrawGlyphRunFn)(void *, void *, float, float, UINT32, const DW_GLYPH_RUN *,
                                                    const DW_GLYPH_RUN_DESCRIPTION *, void *);
typedef HRESULT (STDMETHODCALLTYPE *DrawUnderlineFn)(void *, void *, float, float, const DW_UNDERLINE *, void *);
typedef HRESULT (STDMETHODCALLTYPE *DrawStrikeFn)(void *, void *, float, float, const DW_STRIKETHROUGH *, void *);

static void draw_segment(Layout *l, void *ctx, void *rend, float ox, float oy, Line *ln, UINT32 lo, UINT32 hi,
                         float left, float width)
{
    Cluster *c0 = &l->cl[lo], *c1 = &l->cl[hi];
    Run *r = &l->runs[c0->run];
    Attr *a = &l->attrs[r->attr];
    BOOL rtl = r->level & 1;
    UINT32 g0 = c0->g0, ng = c1->g0 + c1->ng - g0;
    float x = ox + left + (rtl ? width : 0), y = oy + ln->top + ln->baseline;
    UINT32 ts = c0->start, tl = c1->start + c1->len - ts;
    if (ng) {
        UINT16 *cm = dw_zalloc(sizeof(UINT16) * (tl ? tl : 1));
        if (cm) for (UINT32 k = 0; k < tl; k++) cm[k] = (UINT16)(l->cmap[ts + k] + l->runs[c0->run].g0 - g0);
        DW_GLYPH_RUN run = { r->face, r->size, ng, l->gid + g0, l->adv + g0, l->off + g0, FALSE, r->level };
        DW_GLYPH_RUN_DESCRIPTION d = { a->locale, l->text + ts, tl, cm, ts };
        ((DrawGlyphRunFn)VT(rend)[6])(rend, ctx, x, y, 0, &run, &d, a->effect);
        dw_free(cm);
    }
    if (a->underline) {
        DW_UNDERLINE u = { width, r->ul_thick, r->ul_pos, r->ascent + r->descent, rtl, 0, a->locale, 0 };
        ((DrawUnderlineFn)VT(rend)[7])(rend, ctx, x, y, &u, a->effect);
    }
    if (a->strike) {
        DW_STRIKETHROUGH s = { width, r->st_thick, r->st_pos, rtl, 0, a->locale, 0 };
        ((DrawStrikeFn)VT(rend)[8])(rend, ctx, x, y, &s, a->effect);
    }
}

static HRESULT layout_draw(Layout *l, void *ctx, void *rend, float ox, float oy)
{
    if (!rend) return E_INVALIDARG;
    compute(l);
    for (UINT32 q = 0; q < l->nlines; q++) {
        Line *ln = &l->lines[q];
        if (!ln->order) continue;
        UINT32 v = 0;
        while (v < ln->nc) {
            UINT32 a = ln->order[v], run = l->cl[a].run;
            BOOL rtl = l->runs[run].level & 1;
            float left = l->cl[a].x, width = l->cl[a].width;
            UINT32 w = v + 1, lo = a, hi = a;
            while (w < ln->nc) {
                UINT32 b = ln->order[w];
                if (l->cl[b].run != run || b != (rtl ? lo - 1 : hi + 1)) break;
                if (rtl) lo = b; else hi = b;
                width += l->cl[b].width;
                w++;
            }
            draw_segment(l, ctx, rend, ox, oy, ln, lo, hi, left, width);
            v = w;
        }
    }
    return S_OK;
}

/* ---- hit testing ---- */
static UINT32 cluster_at(Layout *l, UINT32 pos)
{
    for (UINT32 i = 0; i < l->ncl; i++)
        if (pos >= l->cl[i].start && pos < l->cl[i].start + l->cl[i].len) return i;
    return l->ncl ? l->ncl - 1 : 0;
}

static void hit_metrics(Layout *l, UINT32 ci, DW_HIT_TEST_METRICS *m)
{
    memset(m, 0, sizeof(*m));
    if (!l->ncl) {
        Line *ln = l->nlines ? &l->lines[0] : NULL;
        m->left = ln ? ln->x : 0;
        m->top = ln ? ln->top : 0;
        m->height = ln ? ln->height : 0;
        return;
    }
    Cluster *c = &l->cl[ci];
    Line *ln = &l->lines[c->line];
    m->textPosition = c->start;
    m->length = c->len;
    m->left = c->x;
    m->top = ln->top;
    m->width = c->width;
    m->height = ln->height;
    m->bidiLevel = l->runs[c->run].level;
    m->isText = !(c->flags & CM_NEWLINE);
}

static HRESULT layout_hit_point(Layout *l, float x, float y, BOOL *trailing, BOOL *inside, DW_HIT_TEST_METRICS *m)
{
    compute(l);
    if (!trailing || !inside || !m) return E_INVALIDARG;
    *inside = FALSE;
    *trailing = FALSE;
    if (!l->nlines) { memset(m, 0, sizeof(*m)); return S_OK; }
    UINT32 q = 0;
    while (q + 1 < l->nlines && y >= l->lines[q].top + l->lines[q].height) q++;
    Line *ln = &l->lines[q];
    BOOL in_y = y >= l->lines[0].top && y < l->lines[l->nlines - 1].top + l->lines[l->nlines - 1].height;
    if (!ln->nc || !ln->order) {
        hit_metrics(l, ln->c0 < l->ncl ? ln->c0 : (l->ncl ? l->ncl - 1 : 0), m);
        if (!ln->nc) { m->textPosition = l->n; m->length = 0; m->left = ln->x; m->width = 0; m->top = ln->top; m->height = ln->height; }
        return S_OK;
    }
    UINT32 v = 0;
    while (v + 1 < ln->nc && x >= l->cl[ln->order[v]].x + l->cl[ln->order[v]].width) v++;
    UINT32 ci = ln->order[v];
    Cluster *c = &l->cl[ci];
    if (x < l->cl[ln->order[0]].x) v = 0, ci = ln->order[0], c = &l->cl[ci];
    hit_metrics(l, ci, m);
    BOOL right_half = x >= c->x + c->width / 2;
    *trailing = (c->flags & CM_RTL) ? !right_half : right_half;
    *inside = in_y && x >= l->cl[ln->order[0]].x && x < l->cl[ln->order[ln->nc - 1]].x + l->cl[ln->order[ln->nc - 1]].width;
    if (c->flags & CM_NEWLINE) *trailing = FALSE;
    return S_OK;
}

static HRESULT layout_hit_pos(Layout *l, UINT32 pos, BOOL trailing, float *x, float *y, DW_HIT_TEST_METRICS *m)
{
    compute(l);
    if (!x || !y || !m) return E_INVALIDARG;
    if (pos >= l->n || !l->ncl) {
        /* past the end: the end of the last line */
        Line *ln = l->nlines ? &l->lines[l->nlines - 1] : NULL;
        memset(m, 0, sizeof(*m));
        float ex = ln ? ln->x + (ln->nc && !(l->cl[ln->c0 + ln->nc - 1].flags & CM_NEWLINE) ? ln->width_ws : 0) : 0;
        if (ln && ln->nc && (l->cl[ln->c0 + ln->nc - 1].flags & CM_NEWLINE)) ex = ln->x;
        m->textPosition = l->n;
        m->left = ex;
        m->top = ln ? ln->top : 0;
        m->height = ln ? ln->height : 0;
        *x = ex;
        *y = m->top;
        return S_OK;
    }
    UINT32 ci = cluster_at(l, pos);
    hit_metrics(l, ci, m);
    Cluster *c = &l->cl[ci];
    BOOL rtl = c->flags & CM_RTL;
    *x = (trailing != rtl) ? c->x + c->width : c->x;
    *y = m->top;
    return S_OK;
}

static HRESULT layout_hit_range(Layout *l, UINT32 pos, UINT32 len, float ox, float oy, DW_HIT_TEST_METRICS *out,
                                UINT32 max, UINT32 *actual)
{
    compute(l);
    if (!actual) return E_INVALIDARG;
    UINT32 cnt = 0;
    UINT32 end = pos + len < pos ? 0xFFFFFFFF : pos + len;
    for (UINT32 q = 0; q < l->nlines; q++) {
        Line *ln = &l->lines[q];
        float lo = 1e30f, hi = -1e30f;
        UINT32 first = 0xFFFFFFFF, last = 0, level = 0;
        for (UINT32 c = ln->c0; c < ln->c0 + ln->nc; c++) {
            Cluster *cl = &l->cl[c];
            if (cl->start + cl->len <= pos || cl->start >= end) continue;
            if (cl->x < lo) lo = cl->x;
            if (cl->x + cl->width > hi) hi = cl->x + cl->width;
            if (cl->start < first) first = cl->start;
            if (cl->start + cl->len > last) last = cl->start + cl->len;
            level = l->runs[cl->run].level;
        }
        if (first == 0xFFFFFFFF) continue;
        if (out && cnt < max) {
            DW_HIT_TEST_METRICS *m = &out[cnt];
            memset(m, 0, sizeof(*m));
            m->textPosition = first;
            m->length = last - first;
            m->left = lo + ox;
            m->top = ln->top + oy;
            m->width = hi - lo;
            m->height = ln->height;
            m->bidiLevel = level;
            m->isText = TRUE;
        }
        cnt++;
    }
    if (!cnt && l->nlines) {
        /* an empty range: a caret-sized rectangle at its position */
        float x, y;
        DW_HIT_TEST_METRICS m;
        layout_hit_pos(l, pos, FALSE, &x, &y, &m);
        if (out && max) { out[0] = m; out[0].width = 0; out[0].left += ox; out[0].top += oy; }
        cnt = 1;
    }
    *actual = cnt;
    return cnt > max ? E_NOT_SUFFICIENT_BUFFER_ : S_OK;
}

/* ---- per-range attributes ---- */
typedef void (*AttrSet)(Attr *a, const void *v);

/* gives the characters of @r a copy of their attributes with @set applied,
 * sharing equal attribute sets, and drops the sets no character uses */
static HRESULT set_range(Layout *l, DW_TEXT_RANGE r, AttrSet set, const void *v)
{
    if (r.startPosition >= l->n || !r.length) return S_OK;
    UINT32 end = r.length > l->n - r.startPosition ? l->n : r.startPosition + r.length;
    UINT32 nold = l->nattrs, *map = dw_zalloc(sizeof(UINT32) * nold);
    if (!map) return E_OUTOFMEMORY;
    for (UINT32 i = 0; i < nold; i++) map[i] = (UINT32)-1;
    for (UINT32 i = r.startPosition; i < end; i++) {
        UINT32 old = l->attr_of[i];
        if (map[old] == (UINT32)-1) {
            Attr na;
            attr_copy(&na, &l->attrs[old]);
            set(&na, v);
            UINT32 k;
            for (k = 0; k < l->nattrs && !attr_eq(&l->attrs[k], &na); k++) {}
            if (k == l->nattrs) {
                Attr *nt = dw_zalloc(sizeof(Attr) * (l->nattrs + 1));
                if (!nt || l->nattrs >= 0xFFFF) { dw_free(nt); attr_free(&na); dw_free(map); return E_OUTOFMEMORY; }
                memcpy(nt, l->attrs, sizeof(Attr) * l->nattrs);
                dw_free(l->attrs);
                l->attrs = nt;
                l->attrs[l->nattrs++] = na;
            } else attr_free(&na);
            map[old] = k;
        }
        l->attr_of[i] = (UINT16)map[old];
    }
    dw_free(map);
    /* compact */
    UINT32 *used = dw_zalloc(sizeof(UINT32) * l->nattrs);
    if (used) {
        for (UINT32 i = 0; i < l->n; i++) used[l->attr_of[i]] = 1;
        UINT32 k = 0;
        for (UINT32 i = 0; i < l->nattrs; i++) {
            if (used[i]) { l->attrs[k] = l->attrs[i]; used[i] = k++; }
            else attr_free(&l->attrs[i]);
        }
        l->nattrs = k;
        for (UINT32 i = 0; i < l->n; i++) l->attr_of[i] = (UINT16)used[l->attr_of[i]];
        dw_free(used);
    }
    invalidate(l);
    return S_OK;
}

/* the attributes at @pos and the run of characters sharing them */
static const Attr *get_range(Layout *l, UINT32 pos, DW_TEXT_RANGE *r)
{
    if (pos >= l->n) {
        if (r) { r->startPosition = l->n; r->length = 0xFFFFFFFF - l->n; }
        return l->nattrs ? &l->attrs[l->n ? l->attr_of[l->n - 1] : 0] : &l->f.a;
    }
    UINT32 a = l->attr_of[pos], s = pos, e = pos + 1;
    while (s > 0 && l->attr_of[s - 1] == a) s--;
    while (e < l->n && l->attr_of[e] == a) e++;
    if (r) { r->startPosition = s; r->length = e - s; }
    return &l->attrs[a];
}

static void set_coll(Attr *a, const void *v)
{
    void *c = *(void *const *)v;
    if (c) COM_ADDREF(c);
    if (a->coll) COM_RELEASE(a->coll);
    a->coll = c;
}
static void set_family(Attr *a, const void *v) { dw_free(a->family); a->family = dw_wcsdup((const WCHAR *)v); }
static void set_locale(Attr *a, const void *v) { dw_free(a->locale); a->locale = dw_wcsdup((const WCHAR *)v); }
static void set_weight(Attr *a, const void *v) { a->weight = *(const UINT32 *)v; }
static void set_style(Attr *a, const void *v) { a->style = *(const UINT32 *)v; }
static void set_stretch(Attr *a, const void *v) { a->stretch = *(const UINT32 *)v; }
static void set_size(Attr *a, const void *v) { a->size = *(const float *)v; }
static void set_underline(Attr *a, const void *v) { a->underline = *(const BOOL *)v; }
static void set_strike(Attr *a, const void *v) { a->strike = *(const BOOL *)v; }
static void set_iface(void **slot, void *p)
{
    if (p) COM_ADDREF(p);
    if (*slot) COM_RELEASE(*slot);
    *slot = p;
}
static void set_effect(Attr *a, const void *v) { set_iface(&a->effect, *(void *const *)v); }
static void set_inline(Attr *a, const void *v) { set_iface(&a->inline_obj, *(void *const *)v); }
static void set_typography(Attr *a, const void *v) { set_iface(&a->typography, *(void *const *)v); }

/* ---- IUnknown ---- */
static HRESULT STDMETHODCALLTYPE l_qi(Layout *l, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDWriteTextFormat) ||
        (l->is_layout && IsEqualIID(riid, &IID_IDWriteTextLayout))) {
        InterlockedIncrement(&l->ref);
        *out = l;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE l_addref(Layout *l) { return InterlockedIncrement(&l->ref); }
static ULONG STDMETHODCALLTYPE l_release(Layout *l)
{
    LONG r = InterlockedDecrement(&l->ref);
    if (!r) {
        layout_free_results(l);
        for (UINT32 i = 0; i < l->nattrs; i++) attr_free(&l->attrs[i]);
        dw_free(l->attrs);
        dw_free(l->attr_of);
        dw_free(l->text);
        format_free(&l->f);
        dw_free(l);
    }
    return r;
}

/* ---- IDWriteTextFormat ---- */
static HRESULT STDMETHODCALLTYPE f_set_align(Layout *l, UINT32 v)
{
    if (v > ALIGN_JUSTIFIED) return E_INVALIDARG;
    l->f.align = v; invalidate(l); return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_set_para(Layout *l, UINT32 v)
{
    if (v > PARA_CENTER) return E_INVALIDARG;
    l->f.para_align = v; invalidate(l); return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_set_wrap(Layout *l, UINT32 v)
{
    if (v > WRAP_CHARACTER) return E_INVALIDARG;
    l->f.wrap = v; invalidate(l); return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_set_reading(Layout *l, UINT32 v)
{
    if (v > 3) return E_INVALIDARG;
    l->f.reading = v; invalidate(l); return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_set_flow(Layout *l, UINT32 v)
{
    if (v > 3) return E_INVALIDARG;
    l->f.flow = v; invalidate(l); return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_set_tab(Layout *l, float v)
{
    if (!(v > 0)) return E_INVALIDARG;
    l->f.tab = v; invalidate(l); return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_set_trimming(Layout *l, const DW_TRIMMING *t, void *sign)
{
    if (!t) return E_INVALIDARG;
    l->f.trimming = *t;
    set_iface(&l->f.trim_sign, sign);
    invalidate(l);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_set_spacing(Layout *l, UINT32 method, float spacing, float baseline)
{
    if (method > SPACING_PROPORTIONAL || spacing < 0 || baseline < 0) return E_INVALIDARG;
    l->f.spacing_method = method; l->f.spacing = spacing; l->f.baseline = baseline;
    invalidate(l);
    return S_OK;
}
static UINT32 STDMETHODCALLTYPE f_get_align(Layout *l) { return l->f.align; }
static UINT32 STDMETHODCALLTYPE f_get_para(Layout *l) { return l->f.para_align; }
static UINT32 STDMETHODCALLTYPE f_get_wrap(Layout *l) { return l->f.wrap; }
static UINT32 STDMETHODCALLTYPE f_get_reading(Layout *l) { return l->f.reading; }
static UINT32 STDMETHODCALLTYPE f_get_flow(Layout *l) { return l->f.flow; }
static float STDMETHODCALLTYPE f_get_tab(Layout *l) { return l->f.tab; }
static HRESULT STDMETHODCALLTYPE f_get_trimming(Layout *l, DW_TRIMMING *t, void **sign)
{
    if (!t || !sign) return E_INVALIDARG;
    *t = l->f.trimming;
    *sign = l->f.trim_sign;
    if (*sign) COM_ADDREF(*sign);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_get_spacing(Layout *l, UINT32 *method, float *spacing, float *baseline)
{
    if (!method || !spacing || !baseline) return E_INVALIDARG;
    *method = l->f.spacing_method; *spacing = l->f.spacing; *baseline = l->f.baseline;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_get_coll(Layout *l, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = l->f.a.coll;
    if (*out) COM_ADDREF(*out);
    else *out = system_collection_iface();
    return S_OK;
}
static UINT32 STDMETHODCALLTYPE f_get_family_len(Layout *l) { return wlen(l->f.a.family); }
static HRESULT STDMETHODCALLTYPE f_get_family(Layout *l, WCHAR *buf, UINT32 size) { return copy_str(l->f.a.family, buf, size); }
static UINT32 STDMETHODCALLTYPE f_get_weight(Layout *l) { return l->f.a.weight; }
static UINT32 STDMETHODCALLTYPE f_get_style(Layout *l) { return l->f.a.style; }
static UINT32 STDMETHODCALLTYPE f_get_stretch(Layout *l) { return l->f.a.stretch; }
static float STDMETHODCALLTYPE f_get_size(Layout *l) { return l->f.a.size; }
static UINT32 STDMETHODCALLTYPE f_get_locale_len(Layout *l) { return wlen(l->f.a.locale); }
static HRESULT STDMETHODCALLTYPE f_get_locale(Layout *l, WCHAR *buf, UINT32 size) { return copy_str(l->f.a.locale, buf, size); }

/* ---- IDWriteTextLayout ---- */
static HRESULT STDMETHODCALLTYPE t_set_max_w(Layout *l, float v)
{
    if (v < 0) return E_INVALIDARG;
    l->max_w = v; invalidate(l); return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_set_max_h(Layout *l, float v)
{
    if (v < 0) return E_INVALIDARG;
    l->max_h = v; invalidate(l); return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_set_coll(Layout *l, void *c, DW_TEXT_RANGE r) { return set_range(l, r, set_coll, &c); }
static HRESULT STDMETHODCALLTYPE t_set_family(Layout *l, const WCHAR *s, DW_TEXT_RANGE r)
{
    if (!s) return E_INVALIDARG;
    return set_range(l, r, set_family, s);
}
static HRESULT STDMETHODCALLTYPE t_set_weight(Layout *l, UINT32 v, DW_TEXT_RANGE r)
{
    if (v < 1 || v > 999) return E_INVALIDARG;
    return set_range(l, r, set_weight, &v);
}
static HRESULT STDMETHODCALLTYPE t_set_style(Layout *l, UINT32 v, DW_TEXT_RANGE r)
{
    if (v > 2) return E_INVALIDARG;
    return set_range(l, r, set_style, &v);
}
static HRESULT STDMETHODCALLTYPE t_set_stretch(Layout *l, UINT32 v, DW_TEXT_RANGE r)
{
    if (v < 1 || v > 9) return E_INVALIDARG;
    return set_range(l, r, set_stretch, &v);
}
static HRESULT STDMETHODCALLTYPE t_set_size(Layout *l, float v, DW_TEXT_RANGE r)
{
    if (!(v > 0)) return E_INVALIDARG;
    return set_range(l, r, set_size, &v);
}
static HRESULT STDMETHODCALLTYPE t_set_underline(Layout *l, BOOL v, DW_TEXT_RANGE r) { v = !!v; return set_range(l, r, set_underline, &v); }
static HRESULT STDMETHODCALLTYPE t_set_strike(Layout *l, BOOL v, DW_TEXT_RANGE r) { v = !!v; return set_range(l, r, set_strike, &v); }
static HRESULT STDMETHODCALLTYPE t_set_effect(Layout *l, void *e, DW_TEXT_RANGE r) { return set_range(l, r, set_effect, &e); }
static HRESULT STDMETHODCALLTYPE t_set_inline(Layout *l, void *o, DW_TEXT_RANGE r) { return set_range(l, r, set_inline, &o); }
static HRESULT STDMETHODCALLTYPE t_set_typography(Layout *l, void *t, DW_TEXT_RANGE r) { return set_range(l, r, set_typography, &t); }
static HRESULT STDMETHODCALLTYPE t_set_locale(Layout *l, const WCHAR *s, DW_TEXT_RANGE r)
{
    if (!s) return E_INVALIDARG;
    return set_range(l, r, set_locale, s);
}
static float STDMETHODCALLTYPE t_get_max_w(Layout *l) { return l->max_w; }
static float STDMETHODCALLTYPE t_get_max_h(Layout *l) { return l->max_h; }
static HRESULT STDMETHODCALLTYPE t_get_coll(Layout *l, UINT32 pos, void **out, DW_TEXT_RANGE *r)
{
    if (!out) return E_INVALIDARG;
    *out = get_range(l, pos, r)->coll;
    if (*out) COM_ADDREF(*out);
    else *out = system_collection_iface();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_get_family_len(Layout *l, UINT32 pos, UINT32 *len, DW_TEXT_RANGE *r)
{
    if (!len) return E_INVALIDARG;
    *len = wlen(get_range(l, pos, r)->family);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_get_family(Layout *l, UINT32 pos, WCHAR *buf, UINT32 size, DW_TEXT_RANGE *r)
{
    return copy_str(get_range(l, pos, r)->family, buf, size);
}
#define T_GET(name, type, field)                                                                 \
    static HRESULT STDMETHODCALLTYPE name(Layout *l, UINT32 pos, type *v, DW_TEXT_RANGE *r)      \
    {                                                                                            \
        if (!v) return E_INVALIDARG;                                                             \
        *v = (type)get_range(l, pos, r)->field;                                                  \
        return S_OK;                                                                             \
    }
T_GET(t_get_weight, UINT32, weight)
T_GET(t_get_style, UINT32, style)
T_GET(t_get_stretch, UINT32, stretch)
T_GET(t_get_size, float, size)
T_GET(t_get_underline, BOOL, underline)
T_GET(t_get_strike, BOOL, strike)
#define T_GET_IFACE(name, field)                                                                 \
    static HRESULT STDMETHODCALLTYPE name(Layout *l, UINT32 pos, void **v, DW_TEXT_RANGE *r)     \
    {                                                                                            \
        if (!v) return E_INVALIDARG;                                                             \
        *v = get_range(l, pos, r)->field;                                                        \
        if (*v) COM_ADDREF(*v);                                                                  \
        return S_OK;                                                                             \
    }
T_GET_IFACE(t_get_effect, effect)
T_GET_IFACE(t_get_inline, inline_obj)
T_GET_IFACE(t_get_typography, typography)
static HRESULT STDMETHODCALLTYPE t_get_locale_len(Layout *l, UINT32 pos, UINT32 *len, DW_TEXT_RANGE *r)
{
    if (!len) return E_INVALIDARG;
    *len = wlen(get_range(l, pos, r)->locale);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_get_locale(Layout *l, UINT32 pos, WCHAR *buf, UINT32 size, DW_TEXT_RANGE *r)
{
    return copy_str(get_range(l, pos, r)->locale, buf, size);
}
static HRESULT STDMETHODCALLTYPE t_draw(Layout *l, void *ctx, void *rend, float x, float y)
{
    return layout_draw(l, ctx, rend, x, y);
}
static HRESULT STDMETHODCALLTYPE t_line_metrics(Layout *l, DW_LINE_METRICS *out, UINT32 max, UINT32 *actual)
{
    compute(l);
    if (actual) *actual = l->nlines;
    for (UINT32 q = 0; out && q < l->nlines && q < max; q++) {
        Line *ln = &l->lines[q];
        UINT32 len = 0;
        for (UINT32 c = ln->c0; c < ln->c0 + ln->nc; c++) len += l->cl[c].len;
        if (!ln->nc && q == l->nlines - 1) len = 0;
        out[q].length = len;
        out[q].trailingWhitespaceLength = ln->trailing_ws;
        out[q].newlineLength = ln->newline_len;
        out[q].height = ln->height;
        out[q].baseline = ln->baseline;
        out[q].isTrimmed = FALSE;
    }
    return max < l->nlines ? E_NOT_SUFFICIENT_BUFFER_ : S_OK;
}
static HRESULT STDMETHODCALLTYPE t_metrics(Layout *l, DW_TEXT_METRICS *m)
{
    if (!m) return E_INVALIDARG;
    compute(l);
    *m = l->m;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_overhang(Layout *l, DW_OVERHANG_METRICS *o)
{
    if (!o) return E_INVALIDARG;
    compute(l);
    /* how far the text's box reaches past the layout's: the ink is taken as the line boxes */
    o->left = -l->m.left;
    o->top = -l->m.top;
    o->right = l->m.left + l->m.width - l->max_w;
    o->bottom = l->m.top + l->m.height - l->max_h;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_cluster_metrics(Layout *l, DW_CLUSTER_METRICS *out, UINT32 max, UINT32 *actual)
{
    compute(l);
    if (actual) *actual = l->ncl;
    for (UINT32 i = 0; out && i < l->ncl && i < max; i++) {
        out[i].width = l->cl[i].width;
        out[i].length = (UINT16)l->cl[i].len;
        out[i].flags = l->cl[i].flags;
    }
    return max < l->ncl ? E_NOT_SUFFICIENT_BUFFER_ : S_OK;
}
static HRESULT STDMETHODCALLTYPE t_min_width(Layout *l, float *w)
{
    if (!w) return E_INVALIDARG;
    compute(l);
    /* the widest piece between break opportunities, without its trailing spaces */
    float best = 0, cur = 0;
    for (UINT32 i = 0; i < l->ncl; i++) {
        Cluster *c = &l->cl[i];
        if (!(c->flags & CM_WHITESPACE)) cur += c->width;
        if (c->flags & CM_WRAP_AFTER || i + 1 == l->ncl) {
            if (cur > best) best = cur;
            cur = 0;
        }
    }
    *w = best;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_hit_point(Layout *l, float x, float y, BOOL *trailing, BOOL *inside,
                                             DW_HIT_TEST_METRICS *m)
{
    return layout_hit_point(l, x, y, trailing, inside, m);
}
static HRESULT STDMETHODCALLTYPE t_hit_pos(Layout *l, UINT32 pos, BOOL trailing, float *x, float *y,
                                           DW_HIT_TEST_METRICS *m)
{
    return layout_hit_pos(l, pos, trailing, x, y, m);
}
static HRESULT STDMETHODCALLTYPE t_hit_range(Layout *l, UINT32 pos, UINT32 len, float ox, float oy,
                                             DW_HIT_TEST_METRICS *out, UINT32 max, UINT32 *actual)
{
    if (!actual) return E_INVALIDARG;
    compute(l);
    return layout_hit_range(l, pos, len, ox, oy, out, max, actual);
}

#define FORMAT_METHODS                                                                              \
    l_qi, l_addref, l_release, f_set_align, f_set_para, f_set_wrap, f_set_reading, f_set_flow,     \
    f_set_tab, f_set_trimming, f_set_spacing, f_get_align, f_get_para, f_get_wrap, f_get_reading,  \
    f_get_flow, f_get_tab, f_get_trimming, f_get_spacing, f_get_coll, f_get_family_len,            \
    f_get_family, f_get_weight, f_get_style, f_get_stretch, f_get_size, f_get_locale_len,          \
    f_get_locale

const void *const fmt_vtbl[] = { FORMAT_METHODS };
const void *const lay_vtbl[] = {
    FORMAT_METHODS,
    t_set_max_w, t_set_max_h, t_set_coll, t_set_family, t_set_weight, t_set_style, t_set_stretch, t_set_size,
    t_set_underline, t_set_strike, t_set_effect, t_set_inline, t_set_typography, t_set_locale,
    t_get_max_w, t_get_max_h, t_get_coll, t_get_family_len, t_get_family, t_get_weight, t_get_style,
    t_get_stretch, t_get_size, t_get_underline, t_get_strike, t_get_effect, t_get_inline, t_get_typography,
    t_get_locale_len, t_get_locale, t_draw, t_line_metrics, t_metrics, t_overhang, t_cluster_metrics,
    t_min_width, t_hit_point, t_hit_pos, t_hit_range,
};

/* ---- creation ---- */
HRESULT text_format_create(const WCHAR *family, void *coll, UINT32 weight, UINT32 style, UINT32 stretch, float size,
                           const WCHAR *locale, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!family || !locale || !(size > 0) || weight < 1 || weight > 999 || style > 2 || stretch < 1 || stretch > 9)
        return E_INVALIDARG;
    Layout *l = dw_zalloc(sizeof(Layout));
    if (!l) return E_OUTOFMEMORY;
    l->vtbl = fmt_vtbl;
    l->ref = 1;
    Attr a = { (WCHAR *)family, coll, weight, style, stretch, size, FALSE, FALSE, NULL, NULL, NULL, (WCHAR *)locale };
    attr_copy(&l->f.a, &a);
    l->f.wrap = WRAP_WRAP;
    l->f.tab = 4 * size;
    *out = l;
    return S_OK;
}

HRESULT text_layout_create(const WCHAR *s, UINT32 n, void *fmt, float w, float h, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    Layout *src = fmt;
    if (!fmt || (!s && n) || (src->vtbl != fmt_vtbl && src->vtbl != lay_vtbl)) return E_INVALIDARG;
    Layout *l = dw_zalloc(sizeof(Layout));
    if (!l) return E_OUTOFMEMORY;
    l->vtbl = lay_vtbl;
    l->ref = 1;
    l->is_layout = 1;
    format_copy(&l->f, &src->f);
    l->max_w = w < 0 ? 0 : w;
    l->max_h = h < 0 ? 0 : h;
    l->n = n;
    l->text = dw_zalloc(sizeof(WCHAR) * (n + 1));
    l->attr_of = dw_zalloc(sizeof(UINT16) * (n ? n : 1));
    l->attrs = dw_zalloc(sizeof(Attr));
    if (!l->text || !l->attr_of || !l->attrs) {
        dw_free(l->text); dw_free(l->attr_of); dw_free(l->attrs);
        format_free(&l->f);
        dw_free(l);
        return E_OUTOFMEMORY;
    }
    if (n) memcpy(l->text, s, sizeof(WCHAR) * n);
    attr_copy(&l->attrs[0], &l->f.a);
    l->nattrs = 1;
    *out = l;
    return S_OK;
}
