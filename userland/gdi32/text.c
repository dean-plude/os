/*
 * text.c — fonts and text for gdi32
 *
 * Fonts are TrueType files in C:\Windows\Fonts, rasterized with
 * stb_truetype into anti-aliased glyphs that are cached per face and size
 * and blended into the DC.  Every face a program asks for maps onto one of
 * two families: Inter (proportional: "Segoe UI", "MS Shell Dlg", "Arial",
 * "Tahoma"...) and DejaVu Sans Mono (fixed pitch: "Consolas", "Courier
 * New", "Lucida Console"...), each in regular and bold.
 *
 * Advances are whole pixels, as in GDI, so measuring a string and drawing
 * it agree exactly (edit controls place their caret by measuring).
 */
#define NOVA_BUILD_GDI32
#include "gdi_int.h"
#include <winternl.h>

size_t strlen(const char *s);

/* -----------------------------------------------------------------------
 * stb_truetype, with what it needs from a C library
 * ----------------------------------------------------------------------- */
static double t_floor(double x) { double f = (double)(long long)x; return f > x ? f - 1 : f; }
static double t_ceil(double x)  { double f = (double)(long long)x; return f < x ? f + 1 : f; }
static double t_sqrt(double x)  { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static double t_fabs(double x)  { return x < 0 ? -x : x; }
static double t_fmod(double x, double y) { return y ? x - (double)(long long)(x / y) * y : 0; }
static double t_pow(double x, double y)
{
    /* only reached by stb_truetype's signed-distance code, which gdi32 does not use */
    double r = 1;
    for (int i = 0; i < (int)y; i++) r *= x;
    return r;
}
static double t_cos(double x)
{
    while (x > 3.14159265358979) x -= 6.28318530717959;
    while (x < -3.14159265358979) x += 6.28318530717959;
    double x2 = x * x;
    return 1 - x2 / 2 + x2 * x2 / 24 - x2 * x2 * x2 / 720 + x2 * x2 * x2 * x2 / 40320;
}
static double t_acos(double x)
{
    /* Newton on cos(y) = x, from a linear first guess (SDF only) */
    double y = 1.5707963267949 - x;
    for (int i = 0; i < 8; i++) {
        double s = t_sqrt(1 - t_cos(y) * t_cos(y));
        if (s < 1e-9) break;
        y += (t_cos(y) - x) / s;
    }
    return y;
}
static void *t_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n); }
static void  t_free(void *p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }

#define STBTT_ifloor(x)   ((int)t_floor(x))
#define STBTT_iceil(x)    ((int)t_ceil(x))
#define STBTT_sqrt(x)     t_sqrt(x)
#define STBTT_pow(x, y)   t_pow(x, y)
#define STBTT_fmod(x, y)  t_fmod(x, y)
#define STBTT_cos(x)      t_cos(x)
#define STBTT_acos(x)     t_acos(x)
#define STBTT_fabs(x)     t_fabs(x)
#define STBTT_malloc(x, u) ((void)(u), t_alloc(x))
#define STBTT_free(x, u)   ((void)(u), t_free(x))
#define STBTT_assert(x)   ((void)0)
#define STBTT_strlen(x)   strlen(x)
#define STBTT_memcpy      memcpy
#define STBTT_memset      memset
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "../../third_party/stb/stb_truetype.h"
#pragma clang diagnostic pop

/* -----------------------------------------------------------------------
 * Faces
 * ----------------------------------------------------------------------- */
/* the complex-script faces have no bold file: bold is drawn twice, a pixel apart */
enum { F_SANS, F_SANS_BOLD, F_MONO, F_MONO_BOLD, F_ARABIC, F_DEVANAGARI, F_COUNT };
static const WCHAR *const g_face_file[F_COUNT] = {
    L"C:\\Windows\\Fonts\\inter.ttf", L"C:\\Windows\\Fonts\\interbd.ttf",
    L"C:\\Windows\\Fonts\\dejavumono.ttf", L"C:\\Windows\\Fonts\\dejavumonobd.ttf",
    L"C:\\Windows\\Fonts\\notosansarabic.ttf", L"C:\\Windows\\Fonts\\notosansdevanagari.ttf",
};
static int is_bold_face(int fi) { return fi == F_SANS_BOLD || fi == F_MONO_BOLD; }

typedef struct {
    int state;                      /* 0 not tried, 1 loaded, -1 missing */
    unsigned char *data;
    DWORD size;
    stbtt_fontinfo info;
    int ascent, descent, gap;       /* font units */
} Face;
static Face g_faces[F_COUNT];
static SRWLOCK g_text_lock;

static Face *face(int i)
{
    Face *f = &g_faces[i];
    if (f->state) return f->state > 0 ? f : (i != F_SANS ? face(i == F_MONO_BOLD ? F_MONO : F_SANS) : 0);
    f->state = -1;
    HANDLE h = CreateFileW(g_face_file[i], GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD n = GetFileSize(h, 0), got = 0;
        f->data = n && n != INVALID_FILE_SIZE ? t_alloc(n) : 0;
        if (f->data && ReadFile(h, f->data, n, &got, 0) && got == n &&
            stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
            f->size = n;
            stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->gap);
            f->state = 1;
        }
        CloseHandle(h);
    }
    if (f->state > 0) return f;
    return i != F_SANS ? face(i == F_MONO_BOLD ? F_MONO : F_SANS) : 0;
}

static int wieq(const WCHAR *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        WCHAR x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        char y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != (WCHAR)y) return 0;
    }
    return !*a && !*b;
}

static int wcontains(const WCHAR *a, const char *sub)
{
    for (; *a; a++) {
        int k = 0;
        while (sub[k] && a[k] && ((a[k] | 32) == (sub[k] | 32))) k++;
        if (!sub[k]) return 1;
    }
    return 0;
}

/* Which face draws a font object */
static int face_index(const GObj *o)
{
    static const char *const mono[] = { "Courier", "Courier New", "Consolas", "Lucida Console", "Fixedsys", "Terminal",
                                        "Cascadia Mono", "Cascadia Code", "DejaVu Sans Mono", "Lucida Sans Typewriter", 0 };
    /* the scripts' own faces, under their Noto names and Windows' */
    static const char *const arabic[] = { "Noto Sans Arabic", "Noto Naskh Arabic", "Traditional Arabic",
                                          "Simplified Arabic", "Arabic Typesetting", "Sakkal Majalla", "Andalus", 0 };
    static const char *const deva[] = { "Noto Sans Devanagari", "Mangal", "Nirmala UI", "Aparajita", "Kokila",
                                        "Utsaah", 0 };
    for (int i = 0; arabic[i]; i++) if (wieq(o->face, arabic[i])) return F_ARABIC;
    for (int i = 0; deva[i]; i++) if (wieq(o->face, deva[i])) return F_DEVANAGARI;
    int is_mono = 0;
    for (int i = 0; mono[i]; i++) if (wieq(o->face, mono[i])) is_mono = 1;
    if (!is_mono && wcontains(o->face, "mono")) is_mono = 1;
    if (!o->face[0] && ((o->pitch & 3) == 1 /* FIXED_PITCH */ || (o->pitch & 0xF0) == 0x30 /* FF_MODERN */)) is_mono = 1;
    int bold = o->weight >= 600;
    return is_mono ? (bold ? F_MONO_BOLD : F_MONO) : (bold ? F_SANS_BOLD : F_SANS);
}

/* -----------------------------------------------------------------------
 * Sizes and glyphs
 * ----------------------------------------------------------------------- */
typedef struct Glyph {
    struct Glyph *next;
    UINT32 key;                     /* code point, or glyph index | 0x80000000 */
    short x0, y0, w, h;             /* bitmap box relative to the pen on the baseline */
    short adv;                      /* whole-pixel advance */
    unsigned char *bmp;             /* w*h coverage */
} Glyph;

#define GLYPH_BUCKETS 128
typedef struct Size {
    struct Size *next;
    int fi, height;                 /* face, LOGFONT height (non-zero) */
    Face *f;
    float scale;
    int ascent, descent, ext_gap, em;
    int ave, max;                   /* TEXTMETRIC widths */
    Glyph *g[GLYPH_BUCKETS];
} Size;
static Size *g_sizes;

static Size *size_for_locked(int fi, int height);
static Size *size_for(int fi, int height)
{
    AcquireSRWLockExclusive(&g_text_lock);
    Size *s = size_for_locked(fi, height);
    ReleaseSRWLockExclusive(&g_text_lock);
    return s;
}

static Size *size_for_locked(int fi, int height)
{
    if (!height) height = -12;
    if (height > 2000) height = 2000;
    if (height < -2000) height = -2000;
    for (Size *s = g_sizes; s; s = s->next) if (s->fi == fi && s->height == height) return s;
    Face *f = face(fi);
    if (!f) return 0;
    Size *s = t_alloc(sizeof(*s));
    if (!s) return 0;
    memset(s, 0, sizeof(*s));
    s->fi = fi; s->height = height; s->f = f;
    /* negative: the em (character) height; positive: the cell height */
    s->scale = height < 0 ? stbtt_ScaleForMappingEmToPixels(&f->info, (float)-height)
                          : stbtt_ScaleForPixelHeight(&f->info, (float)height);
    s->ascent = (int)t_ceil(f->ascent * s->scale);
    s->descent = (int)t_ceil(-f->descent * s->scale);
    s->ext_gap = (int)(f->gap * s->scale + 0.5f);
    s->em = height < 0 ? -height : (int)((s->ascent + s->descent) * 0.8f + 0.5f);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f->info, 'x', &adv, &lsb);
    s->ave = (int)(adv * s->scale + 0.5f);
    stbtt_GetCodepointHMetrics(&f->info, 'W', &adv, &lsb);
    s->max = (int)(adv * s->scale + 0.5f);
    s->next = g_sizes;
    g_sizes = s;
    return s;
}

static Glyph *glyph_locked(Size *s, UINT32 key);
static Glyph *glyph(Size *s, UINT32 key)
{
    AcquireSRWLockExclusive(&g_text_lock);
    Glyph *g = glyph_locked(s, key);
    ReleaseSRWLockExclusive(&g_text_lock);
    return g;
}

static Glyph *glyph_locked(Size *s, UINT32 key)
{
    Glyph **b = &s->g[key % GLYPH_BUCKETS];
    for (Glyph *g = *b; g; g = g->next) if (g->key == key) return g;
    Glyph *g = t_alloc(sizeof(*g));
    if (!g) return 0;
    memset(g, 0, sizeof(*g));
    g->key = key;
    int gi = key & 0x80000000u ? (int)(key & 0x7FFFFFFF) : stbtt_FindGlyphIndex(&s->f->info, (int)key);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&s->f->info, gi, &adv, &lsb);
    g->adv = (short)(adv * s->scale + 0.5f);
    if (key == '\t') g->adv = (short)(s->ave * 8);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&s->f->info, gi, s->scale, s->scale, &x0, &y0, &x1, &y1);
    if (x1 > x0 && y1 > y0 && key != '\t' && key != ' ') {
        g->x0 = (short)x0; g->y0 = (short)y0; g->w = (short)(x1 - x0); g->h = (short)(y1 - y0);
        g->bmp = t_alloc((size_t)g->w * g->h);
        if (g->bmp) stbtt_MakeGlyphBitmap(&s->f->info, g->bmp, g->w, g->h, g->w, s->scale, s->scale, gi);
        else g->w = g->h = 0;
    }
    g->next = *b;
    *b = g;
    return g;
}

/* The size a DC's selected font draws at */
static Size *dc_size(NOVA_DC *d, GObj **font_out)
{
    if (!g_stock_ready) stock_init();
    GObj *o = d ? obj_of(d->font) : 0;
    if (!o || o->kind != K_FONT) o = &g_stock[SYSTEM_FONT];
    if (font_out) *font_out = o;
    return size_for(face_index(o), o->height);
}

/* Width of @n characters (UTF-16, surrogates combined) */
static int text_width(Size *s, const WCHAR *t, int n, int glyph_idx, int *dx_out, int extra)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        UINT32 cp = t[i];
        if (!glyph_idx && cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && t[i + 1] >= 0xDC00 && t[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (t[i + 1] - 0xDC00);
            if (dx_out) dx_out[i] = w;
            i++;
        }
        Glyph *g = glyph(s, glyph_idx ? cp | 0x80000000u : cp);
        w += (g ? g->adv : 0) + extra;
        if (dx_out) dx_out[i] = w;
    }
    return w;
}

/* -----------------------------------------------------------------------
 * Creating fonts
 * ----------------------------------------------------------------------- */
GDIAPI HFONT CreateFontIndirectW(const LOGFONTW *lf)
{
    GObj *o = new_obj(K_FONT);
    if (!o) return 0;
    o->height = lf->lfHeight;
    o->avg_width = lf->lfWidth;
    o->weight = lf->lfWeight ? lf->lfWeight : 400;
    o->italic = lf->lfItalic;
    o->underline = lf->lfUnderline;
    o->strike = lf->lfStrikeOut;
    o->pitch = lf->lfPitchAndFamily;
    o->charset = lf->lfCharSet;
    o->escapement = lf->lfEscapement;
    for (int i = 0; i < 31 && lf->lfFaceName[i]; i++) o->face[i] = lf->lfFaceName[i];
    return (HFONT)o;
}

GDIAPI HFONT CreateFontIndirectA(const LOGFONTA *lf)
{
    LOGFONTW w;
    memcpy(&w, lf, 28);                                     /* the numeric fields */
    memset(w.lfFaceName, 0, sizeof(w.lfFaceName));
    MultiByteToWideChar(CP_ACP, 0, lf->lfFaceName, -1, w.lfFaceName, 32);
    w.lfFaceName[31] = 0;
    return CreateFontIndirectW(&w);
}

GDIAPI HFONT CreateFontIndirectExW(const void *elf) { return CreateFontIndirectW(elf); }   /* ENUMLOGFONTEXDVW starts with LOGFONTW */

GDIAPI HFONT CreateFontW(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike,
                         DWORD charset, DWORD outprec, DWORD clip, DWORD quality, DWORD pitch, LPCWSTR face_name)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = h; lf.lfWidth = w; lf.lfEscapement = esc; lf.lfOrientation = orient; lf.lfWeight = weight;
    lf.lfItalic = (BYTE)italic; lf.lfUnderline = (BYTE)underline; lf.lfStrikeOut = (BYTE)strike;
    lf.lfCharSet = (BYTE)charset; lf.lfOutPrecision = (BYTE)outprec; lf.lfClipPrecision = (BYTE)clip;
    lf.lfQuality = (BYTE)quality; lf.lfPitchAndFamily = (BYTE)pitch;
    for (int i = 0; face_name && i < 31 && face_name[i]; i++) lf.lfFaceName[i] = face_name[i];
    return CreateFontIndirectW(&lf);
}

GDIAPI HFONT CreateFontA(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike,
                         DWORD charset, DWORD outprec, DWORD clip, DWORD quality, DWORD pitch, LPCSTR face_name)
{
    WCHAR f[32] = { 0 };
    if (face_name) MultiByteToWideChar(CP_ACP, 0, face_name, -1, f, 32);
    f[31] = 0;
    return CreateFontW(h, w, esc, orient, weight, italic, underline, strike, charset, outprec, clip, quality, pitch, f);
}

/* -----------------------------------------------------------------------
 * Drawing text
 * ----------------------------------------------------------------------- */
static void draw_glyph(NOVA_DC *d, Glyph *g, int pen_x, int base_y, COLORREF c, const RECT *clip)
{
    if (!g || !g->bmp) return;
    int ox = pen_x + g->x0 + d->org_x, oy = base_y + g->y0 + d->org_y;
    for (int y = 0; y < g->h; y++) {
        int dy = oy + y;
        if (clip && (dy < clip->top || dy >= clip->bottom)) continue;
        const unsigned char *row = g->bmp + (size_t)y * g->w;
        for (int x = 0; x < g->w; x++) {
            int dx = ox + x;
            if (!row[x] || (clip && (dx < clip->left || dx >= clip->right))) continue;
            blend(d, dx, dy, c, row[x]);
        }
    }
}

/* Draw @n characters with their left edge at @x and the cell's top at @y
 * (logical), honouring the DC's alignment, background mode and current
 * position; @dx: per-character advances (ExtTextOut), @clip: device rect */
static BOOL text_out(NOVA_DC *d, int x, int y, const WCHAR *t, int n, const INT *dx, const RECT *clip, int glyph_idx,
                     const RECT *opaque_rc, int height)
{
    GObj *font;
    Size *s = dc_size(d, &font);
    if (height) s = size_for(face_index(font), height);    /* scaled by the world transform */
    if (!s) return FALSE;
    int w = 0;
    if (dx) for (int i = 0; i < n; i++) w += dx[i];
    else w = text_width(s, t, n, glyph_idx, 0, 0);
    UINT al = d->text_align;
    if (al & 1 /* TA_UPDATECP */) { x = d->cx; y = d->cy; }
    if ((al & 6) == 6) x -= w / 2;                           /* TA_CENTER */
    else if (al & 2) x -= w;                                 /* TA_RIGHT */
    int base;
    if ((al & 24) == 24) base = y;                           /* TA_BASELINE */
    else if (al & 8) base = y - s->descent;                  /* TA_BOTTOM */
    else base = y + s->ascent;                               /* TA_TOP */
    if (al & 1) d->cx = (al & 6) == 6 ? d->cx : (al & 2) ? x : x + w;

    if (d->bk_mode == OPAQUE && !opaque_rc)
        fill(d, x, base - s->ascent, x + w, base + s->descent, d->bk_color);
    int pen = x;
    COLORREF c = d->text_color;
    for (int i = 0; i < n; i++) {
        UINT32 cp = t[i];
        int start = i;
        if (!glyph_idx && cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && t[i + 1] >= 0xDC00 && t[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (t[i + 1] - 0xDC00);
            i++;
        }
        Glyph *g = glyph(s, glyph_idx ? cp | 0x80000000u : cp);
        if (g) {
            if (font->italic && g->bmp) {                   /* slant: shear rows by their height */
                int ox = pen + g->x0 + d->org_x, oy = base + g->y0 + d->org_y;
                for (int yy = 0; yy < g->h; yy++) {
                    int sh = (-(g->y0 + yy)) / 4;
                    for (int xx = 0; xx < g->w; xx++) {
                        int px = ox + xx + sh, py = oy + yy;
                        if (clip && (px < clip->left || px >= clip->right || py < clip->top || py >= clip->bottom)) continue;
                        blend(d, px, py, c, g->bmp[yy * g->w + xx]);
                    }
                }
            } else {
                draw_glyph(d, g, pen, base, c, clip);
                if (font->weight >= 600 && !is_bold_face(s->fi)) draw_glyph(d, g, pen + 1, base, c, clip);
            }
        }
        pen += dx ? dx[start] + (i != start ? dx[i] : 0) : (g ? g->adv : 0);
    }
    int th = s->em / 14 + 1;
    if (font->underline) fill(d, x, base + 1 + s->descent / 3, pen, base + 1 + s->descent / 3 + th, c);
    if (font->strike) fill(d, x, base - s->ascent / 3, pen, base - s->ascent / 3 + th, c);
    return TRUE;
}

/* The DC's world transform (identity when none is set) */
static void xform_get_(NOVA_DC *d, XFORM *x)
{
    if (!d->xform[0] && !d->xform[1] && !d->xform[2] && !d->xform[3]) {
        x->eM11 = 1; x->eM12 = 0; x->eM21 = 0; x->eM22 = 1; x->eDx = 0; x->eDy = 0;
    } else memcpy(x, d->xform, sizeof(*x));
}

/* The LOGFONT height @font draws at on @d's device: scaled by a GM_ADVANCED
 * world transform that only scales and offsets; 0 when there is none */
static int scaled_height(NOVA_DC *d, GObj *font)
{
    XFORM xf;
    if (!d || d->gmode != GM_ADVANCED) return 0;
    xform_get_(d, &xf);
    if (xf.eM12 || xf.eM21 || xf.eM11 <= 0 || xf.eM22 <= 0 || (xf.eM11 == 1 && xf.eM22 == 1)) return 0;
    int fh = font->height ? font->height : -16;
    int h = (int)(fh * xf.eM22 + (fh < 0 ? -0.5f : 0.5f));
    return h ? h : fh < 0 ? -1 : 1;
}

static int is_complex(LPCWSTR s, int n);
GDIAPI BOOL TextOutW(HDC h, int x, int y, LPCWSTR s, int len)
{
    NOVA_DC *d = dc_of(h);
    if (!d || !s) return FALSE;
    if (len > 0 && is_complex(s, len)) return ExtTextOutW(h, x, y, 0, 0, s, (UINT)len, 0);
    return text_out(d, x, y, s, len, 0, 0, 0, 0, 0);
}

/* ANSI text: the code page is UTF-8 here */
static WCHAR *to_wide(LPCSTR s, int len, WCHAR *buf, int cap, int *out_n)
{
    if (len < 0) len = (int)strlen(s);
    int n = MultiByteToWideChar(CP_ACP, 0, s, len, 0, 0);
    WCHAR *w = n <= cap ? buf : t_alloc(sizeof(WCHAR) * (size_t)n);
    if (!w) { *out_n = 0; return buf; }
    *out_n = MultiByteToWideChar(CP_ACP, 0, s, len, w, n);
    return w;
}

GDIAPI BOOL TextOutA(HDC h, int x, int y, LPCSTR s, int len)
{
    if (!s) return FALSE;
    WCHAR buf[512];
    int n;
    WCHAR *w = to_wide(s, len, buf, 512, &n);
    BOOL r = TextOutW(h, x, y, w, n);
    if (w != buf) t_free(w);
    return r;
}

/* -----------------------------------------------------------------------
 * Complex scripts: as Windows' LPK does, text with right-to-left or
 * shaped characters goes through Uniscribe (usp10.dll), which picks a font
 * for each script, shapes the runs with HarfBuzz, orders them and draws
 * them back here as glyph indices
 * ----------------------------------------------------------------------- */
static int complex_char(WCHAR c)
{
    return (c >= 0x0590 && c <= 0x08FF) ||                  /* Hebrew, Arabic, Syriac, Thaana, N'Ko... */
           (c >= 0x0900 && c <= 0x0DFF) ||                  /* the Indic scripts */
           (c >= 0x0E00 && c <= 0x0FFF) ||                  /* Thai, Lao, Tibetan */
           (c >= 0x1000 && c <= 0x109F) || (c >= 0x1780 && c <= 0x18AF) ||   /* Myanmar, Khmer, Mongolian */
           (c >= 0x200C && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) ||
           (c >= 0xA8E0 && c <= 0xA8FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

static int is_complex(LPCWSTR s, int n)
{
    for (int i = 0; i < n; i++) if (complex_char(s[i])) return 1;
    return 0;
}

#define SSA_GLYPHS_   0x80
#define SSA_FALLBACK_ 0x20
#define SSA_RTL_      0x100
typedef HRESULT (WINAPI *SSAnalyse_)(HDC, const void *, int, int, int, DWORD, int, void *, void *, const int *,
                                    void *, const BYTE *, void **);
typedef HRESULT (WINAPI *SSOut_)(void *, int, int, UINT, const RECT *, int, int, BOOL);
typedef HRESULT (WINAPI *SSFree_)(void **);
typedef const SIZE *(WINAPI *SSSize_)(void *);
static struct { int tried; SSAnalyse_ analyse; SSOut_ out; SSFree_ free; SSSize_ size; } g_usp;

static int usp_ready(void)
{
    if (!g_usp.tried) {
        HMODULE m = LoadLibraryW(L"usp10.dll");
        if (m) {
            g_usp.analyse = (SSAnalyse_)GetProcAddress(m, "ScriptStringAnalyse");
            g_usp.out = (SSOut_)GetProcAddress(m, "ScriptStringOut");
            g_usp.free = (SSFree_)GetProcAddress(m, "ScriptStringFree");
            g_usp.size = (SSSize_)GetProcAddress(m, "ScriptString_pSize");
        }
        g_usp.tried = 1;
    }
    return g_usp.analyse && g_usp.out && g_usp.free && g_usp.size;
}

/* Analyse @s with Uniscribe: the analysis, or 0 */
static void *usp_analyse(HDC h, LPCWSTR s, int n, int rtl)
{
    void *ssa = 0;
    if (n <= 0 || !usp_ready()) return 0;
    DWORD flags = SSA_GLYPHS_ | SSA_FALLBACK_ | (rtl ? SSA_RTL_ : 0);
    if (FAILED(g_usp.analyse(h, s, n, n * 3 / 2 + 16, -1, flags, 0, 0, 0, 0, 0, 0, &ssa))) return 0;
    return ssa;
}

static int dc_rtl(NOVA_DC *d, UINT opts) { return (opts & 0x80 /* ETO_RTLREADING */) || (d->text_align & 256 /* TA_RTLREADING */); }

/* ExtTextOut for complex text; FALSE: Uniscribe is missing, draw it plainly */
static BOOL complex_text_out(HDC h, NOVA_DC *d, int x, int y, UINT opts, const RECT *rc, LPCWSTR s, int n, BOOL *ok)
{
    void *ssa = usp_analyse(h, s, n, dc_rtl(d, opts));
    if (!ssa) return FALSE;
    const SIZE *sz = g_usp.size(ssa);
    int w = sz ? sz->cx : 0;
    UINT al = d->text_align;
    if (al & 1 /* TA_UPDATECP */) { x = d->cx; y = d->cy; }
    if ((al & 6) == 6) x -= w / 2;                          /* TA_CENTER */
    else if (al & 2) x -= w;                                /* TA_RIGHT */
    /* the runs are placed left to right from x: no alignment of their own */
    d->text_align = al & 24;
    *ok = SUCCEEDED(g_usp.out(ssa, x, y, opts & 4 /* ETO_CLIPPED */, rc, 0, 0, FALSE));
    d->text_align = al;
    if (al & 1) d->cx = (al & 6) == 6 ? d->cx : (al & 2) ? x : x + w;
    g_usp.free(&ssa);
    return TRUE;
}

/* The width of complex text as Uniscribe lays it out, or -1 */
static int complex_width(HDC h, NOVA_DC *d, LPCWSTR s, int n)
{
    void *ssa = usp_analyse(h, s, n, dc_rtl(d, 0));
    if (!ssa) return -1;
    const SIZE *sz = g_usp.size(ssa);
    int w = sz ? sz->cx : -1;
    g_usp.free(&ssa);
    return w;
}

GDIAPI BOOL ExtTextOutW(HDC h, int x, int y, UINT opts, const RECT *rc, LPCWSTR s, UINT len, const INT *dx)
{
    NOVA_DC *d = dc_of(h);
    if (!d) return FALSE;
    /* GM_ADVANCED with a world transform (cairo draws text with a font 32
     * times its size, scaled back by the transform): scales and offsets are
     * applied here, to the origin, rectangle, advances and font size */
    XFORM xf;
    int height = 0;
    RECT trc;
    INT *tdx = 0;
    xform_get_(d, &xf);
    if (d->gmode == GM_ADVANCED && !xf.eM12 && !xf.eM21 && xf.eM11 > 0 && xf.eM22 > 0 &&
        (xf.eM11 != 1 || xf.eM22 != 1 || xf.eDx || xf.eDy)) {
        GObj *font;
        dc_size(d, &font);
        height = scaled_height(d, font);
        if (rc) {
            trc.left = (int)(rc->left * xf.eM11 + xf.eDx + 0.5f); trc.right = (int)(rc->right * xf.eM11 + xf.eDx + 0.5f);
            trc.top = (int)(rc->top * xf.eM22 + xf.eDy + 0.5f); trc.bottom = (int)(rc->bottom * xf.eM22 + xf.eDy + 0.5f);
            rc = &trc;
        }
        if (dx && s && len) {
            int step = opts & 0x2000 /* ETO_PDY */ ? 2 : 1;
            tdx = t_alloc(sizeof(INT) * len);
            if (tdx) {
                float pos = 0;
                for (UINT i = 0; i < len; i++) {
                    float next = pos + dx[step * i] * xf.eM11;
                    tdx[i] = (int)(next + 0.5f) - (int)(pos + 0.5f);
                    pos = next;
                }
                opts &= ~0x2000u;
            }
            dx = tdx;
        }
        x = (int)(x * xf.eM11 + xf.eDx + 0.5f);
        y = (int)(y * xf.eM22 + xf.eDy + 0.5f);
    }
    if (rc && (opts & 2 /* ETO_OPAQUE */)) fill(d, rc->left, rc->top, rc->right, rc->bottom, d->bk_color);
    if (!s || !len) { t_free(tdx); return TRUE; }
    RECT clip, *cp = 0;
    if (rc && (opts & 4 /* ETO_CLIPPED */)) {
        clip.left = rc->left + d->org_x; clip.top = rc->top + d->org_y;
        clip.right = rc->right + d->org_x; clip.bottom = rc->bottom + d->org_y;
        cp = &clip;
    }
    if (!(opts & (0x10 /* ETO_GLYPH_INDEX */ | 0x1000 /* ETO_IGNORELANGUAGE */)) && is_complex(s, (int)len)) {
        BOOL ok;
        if (!height && complex_text_out(h, d, x, y, opts, rc, s, (int)len, &ok)) return ok;
    }
    INT *pdx = 0;
    if (dx && (opts & 0x2000 /* ETO_PDY */)) {              /* x,y pairs: keep the x advances */
        pdx = t_alloc(sizeof(INT) * len);
        if (pdx) for (UINT i = 0; i < len; i++) pdx[i] = dx[2 * i];
    }
    BOOL r = text_out(d, x, y, s, (int)len, pdx ? pdx : dx, cp, (opts & 0x10 /* ETO_GLYPH_INDEX */) != 0,
                      rc && (opts & 2) ? rc : 0, height);
    t_free(pdx);
    t_free(tdx);
    return r;
}

GDIAPI BOOL ExtTextOutA(HDC h, int x, int y, UINT opts, const RECT *rc, LPCSTR s, UINT len, const INT *dx)
{
    if (!s || !len) return ExtTextOutW(h, x, y, opts, rc, 0, 0, 0);
    WCHAR buf[512];
    int n;
    WCHAR *w = to_wide(s, (int)len, buf, 512, &n);
    BOOL r = ExtTextOutW(h, x, y, opts, rc, w, (UINT)n, (UINT)n == len ? dx : 0);
    if (w != buf) t_free(w);
    return r;
}

typedef struct { int x, y; UINT n; LPCWSTR str; UINT flags; RECT rc; const INT *dx; } PolyTextW;
GDIAPI BOOL PolyTextOutW(HDC h, const void *items, int n)
{
    const PolyTextW *p = items;
    for (int i = 0; i < n; i++) ExtTextOutW(h, p[i].x, p[i].y, p[i].flags, &p[i].rc, p[i].str, p[i].n, p[i].dx);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Measuring
 * ----------------------------------------------------------------------- */
GDIAPI BOOL GetTextExtentPoint32W(HDC h, LPCWSTR s, int len, LPSIZE sz)
{
    NOVA_DC *d = dc_of(h);
    Size *z = dc_size(d, 0);
    if (!z || !sz) return FALSE;
    int cw = s && len > 0 && is_complex(s, len) ? complex_width(h, d, s, len) : -1;
    sz->cx = cw >= 0 ? cw : s && len > 0 ? text_width(z, s, len, 0, 0, 0) : 0;
    sz->cy = z->ascent + z->descent;
    return TRUE;
}

GDIAPI BOOL GetTextExtentPoint32A(HDC h, LPCSTR s, int len, LPSIZE sz)
{
    if (!s) return GetTextExtentPoint32W(h, 0, 0, sz);
    WCHAR buf[512];
    int n;
    WCHAR *w = to_wide(s, len, buf, 512, &n);
    BOOL r = GetTextExtentPoint32W(h, w, n, sz);
    if (w != buf) t_free(w);
    return r;
}
GDIAPI BOOL GetTextExtentPointW(HDC h, LPCWSTR s, int len, LPSIZE sz) { return GetTextExtentPoint32W(h, s, len, sz); }
GDIAPI BOOL GetTextExtentPointA(HDC h, LPCSTR s, int len, LPSIZE sz) { return GetTextExtentPoint32A(h, s, len, sz); }

GDIAPI BOOL GetTextExtentExPointW(HDC h, LPCWSTR s, int len, int max, LPINT fit, LPINT dx, LPSIZE sz)
{
    NOVA_DC *d = dc_of(h);
    Size *z = dc_size(d, 0);
    if (!z) return FALSE;
    INT tmp[256], *pos = dx ? dx : len <= 256 ? tmp : t_alloc(sizeof(INT) * (size_t)len);
    int w = s && len > 0 && pos ? text_width(z, s, len, 0, pos, 0) : 0;
    if (fit) {
        int k = 0;
        if (max < 0) k = len;
        else while (k < len && pos && pos[k] <= max) k++;
        *fit = k;
    }
    if (pos && pos != dx && pos != tmp) t_free(pos);
    if (sz) { sz->cx = w; sz->cy = z->ascent + z->descent; }
    return TRUE;
}

GDIAPI BOOL GetTextExtentExPointA(HDC h, LPCSTR s, int len, int max, LPINT fit, LPINT dx, LPSIZE sz)
{
    WCHAR buf[512];
    int n;
    WCHAR *w = to_wide(s ? s : "", s ? len : 0, buf, 512, &n);
    BOOL r = GetTextExtentExPointW(h, w, n, max, fit, dx, sz);   /* (dx per UTF-16 unit) */
    if (w != buf) t_free(w);
    return r;
}

GDIAPI BOOL GetTextExtentExPointI(HDC h, LPWORD gi, int len, int max, LPINT fit, LPINT dx, LPSIZE sz)
{
    NOVA_DC *d = dc_of(h);
    Size *z = dc_size(d, 0);
    if (!z) return FALSE;
    int w = 0, k = 0;
    for (int i = 0; i < len; i++) {
        Glyph *g = glyph(z, gi[i] | 0x80000000u);
        w += g ? g->adv : 0;
        if (dx) dx[i] = w;
        if (max < 0 || w <= max) k = i + 1;
    }
    if (fit) *fit = k;
    if (sz) { sz->cx = w; sz->cy = z->ascent + z->descent; }
    return TRUE;
}

GDIAPI BOOL GetTextExtentPointI(HDC h, LPWORD gi, int len, LPSIZE sz) { return GetTextExtentExPointI(h, gi, len, -1, 0, 0, sz); }

static void fill_metrics(Size *z, GObj *f, TEXTMETRICW *tm)
{
    memset(tm, 0, sizeof(*tm));
    tm->tmAscent = z->ascent;
    tm->tmDescent = z->descent;
    tm->tmHeight = z->ascent + z->descent;
    tm->tmInternalLeading = tm->tmHeight - z->em > 0 ? tm->tmHeight - z->em : 0;
    tm->tmExternalLeading = z->ext_gap;
    tm->tmAveCharWidth = z->ave;
    tm->tmMaxCharWidth = z->max;
    tm->tmWeight = f->weight;
    tm->tmItalic = (BYTE)f->italic;
    tm->tmUnderlined = (BYTE)f->underline;
    tm->tmStruckOut = (BYTE)f->strike;
    tm->tmFirstChar = 0x20; tm->tmLastChar = 0xFFFC; tm->tmDefaultChar = 0x1F; tm->tmBreakChar = ' ';
    int mono = z->fi == F_MONO || z->fi == F_MONO_BOLD;
    /* TMPF_FIXED_PITCH (bit 0) is set for *variable* pitch; TRUETYPE | VECTOR */
    tm->tmPitchAndFamily = (BYTE)((mono ? 0 : 1) | 2 | 4 | (mono ? 0x30 : 0x20));
    tm->tmCharSet = 0;
    tm->tmDigitizedAspectX = tm->tmDigitizedAspectY = 96;
}

GDIAPI BOOL GetTextMetricsW(HDC h, TEXTMETRICW *tm)
{
    GObj *f;
    Size *z = dc_size(dc_of(h), &f);
    if (!z || !tm) return FALSE;
    fill_metrics(z, f, tm);
    return TRUE;
}

GDIAPI BOOL GetTextMetricsA(HDC h, TEXTMETRICA *tm)
{
    TEXTMETRICW w;
    if (!GetTextMetricsW(h, &w)) return FALSE;
    memcpy(tm, &w, 44);                                     /* the LONGs */
    tm->tmFirstChar = 0x20; tm->tmLastChar = (char)0xFF; tm->tmDefaultChar = 0x1F; tm->tmBreakChar = ' ';
    tm->tmItalic = w.tmItalic; tm->tmUnderlined = w.tmUnderlined; tm->tmStruckOut = w.tmStruckOut;
    tm->tmPitchAndFamily = w.tmPitchAndFamily; tm->tmCharSet = w.tmCharSet;
    return TRUE;
}

GDIAPI int GetTextFaceW(HDC h, int n, LPWSTR out)
{
    GObj *f;
    NOVA_DC *d = dc_of(h);
    if (!dc_size(d, &f)) return 0;
    const WCHAR sys[] = L"System";
    const WCHAR *name = f->face[0] ? f->face : sys;
    int k = 0;
    while (name[k]) k++;
    if (!out) return k + 1;
    int m = k < n - 1 ? k : n - 1;
    for (int i = 0; i < m; i++) out[i] = name[i];
    if (n > 0) out[m] = 0;
    return m;
}

GDIAPI int GetTextFaceA(HDC h, int n, LPSTR out)
{
    WCHAR w[64];
    int k = GetTextFaceW(h, 64, w);
    if (!out) return k + 1;
    int m = WideCharToMultiByte(CP_ACP, 0, w, k, out, n - 1, 0, 0);
    if (m < 0) m = 0;
    if (n > 0) out[m] = 0;
    return m;
}

GDIAPI BOOL GetCharWidth32W(HDC h, UINT first, UINT last, LPINT out)
{
    Size *z = dc_size(dc_of(h), 0);
    if (!z) return FALSE;
    for (UINT c = first; c <= last; c++) { Glyph *g = glyph(z, c); out[c - first] = g ? g->adv : 0; }
    return TRUE;
}
GDIAPI BOOL GetCharWidthW(HDC h, UINT first, UINT last, LPINT out) { return GetCharWidth32W(h, first, last, out); }
GDIAPI BOOL GetCharWidth32A(HDC h, UINT first, UINT last, LPINT out) { return GetCharWidth32W(h, first, last, out); }
GDIAPI BOOL GetCharWidthA(HDC h, UINT first, UINT last, LPINT out) { return GetCharWidth32W(h, first, last, out); }
GDIAPI BOOL GetCharWidthI(HDC h, UINT first, UINT n, LPWORD gi, LPINT out)
{
    Size *z = dc_size(dc_of(h), 0);
    if (!z) return FALSE;
    for (UINT i = 0; i < n; i++) { Glyph *g = glyph(z, (gi ? gi[i] : first + i) | 0x80000000u); out[i] = g ? g->adv : 0; }
    return TRUE;
}

static void abc_of(Glyph *g, ABC *a)
{
    if (!g) { a->abcA = 0; a->abcB = 0; a->abcC = 0; return; }
    a->abcA = g->w ? g->x0 : 0;
    a->abcB = (UINT)(g->w ? g->w : g->adv);
    a->abcC = g->adv - a->abcA - (int)a->abcB;
}
GDIAPI BOOL GetCharABCWidthsW(HDC h, UINT first, UINT last, ABC *out)
{
    Size *z = dc_size(dc_of(h), 0);
    if (!z) return FALSE;
    for (UINT c = first; c <= last; c++) abc_of(glyph(z, c), &out[c - first]);
    return TRUE;
}
/* by glyph index: @gi's, or @n from @first */
GDIAPI BOOL GetCharABCWidthsI(HDC h, UINT first, UINT n, LPWORD gi, ABC *out)
{
    Size *z = dc_size(dc_of(h), 0);
    if (!z || !out) return FALSE;
    for (UINT i = 0; i < n; i++) abc_of(glyph(z, (gi ? gi[i] : first + i) | 0x80000000u), &out[i]);
    return TRUE;
}
GDIAPI BOOL GetCharABCWidthsA(HDC h, UINT first, UINT last, ABC *out) { return GetCharABCWidthsW(h, first, last, out); }


GDIAPI BOOL GetCharABCWidthsFloatW(HDC h, UINT first, UINT last, void *out)
{
    float *f = out;
    Size *z = dc_size(dc_of(h), 0);
    if (!z) return FALSE;
    for (UINT c = first; c <= last; c++) {
        Glyph *g = glyph(z, c);
        f[3 * (c - first)] = 0; f[3 * (c - first) + 1] = g ? g->adv : 0; f[3 * (c - first) + 2] = 0;
    }
    return TRUE;
}

GDIAPI DWORD GetGlyphIndicesW(HDC h, LPCWSTR s, int n, LPWORD out, DWORD flags)
{
    Size *z = dc_size(dc_of(h), 0);
    if (!z) return GDI_ERROR;
    for (int i = 0; i < n; i++) {
        int gi = stbtt_FindGlyphIndex(&z->f->info, s[i]);
        out[i] = (WORD)(gi || !(flags & 1 /* GGI_MARK_NONEXISTING_GLYPHS */) ? gi : 0xFFFF);
    }
    return (DWORD)n;
}

GDIAPI DWORD GetGlyphIndicesA(HDC h, LPCSTR s, int n, LPWORD out, DWORD flags)
{
    WCHAR buf[512];
    int k;
    WCHAR *w = to_wide(s, n, buf, 512, &k);
    DWORD r = GetGlyphIndicesW(h, w, k, out, flags);
    if (w != buf) t_free(w);
    return r;
}

/* GetCharacterPlacement: one glyph per character, left to right */
GDIAPI DWORD GetCharacterPlacementW(HDC h, LPCWSTR s, int n, int max, void *res, DWORD flags)
{
    (void)max; (void)flags;
    Size *z = dc_size(dc_of(h), 0);
    if (!z) return 0;
    BYTE *r = res;                                          /* GCP_RESULTSW */
    UINT *order = *(UINT **)(r + 16), *caret = *(UINT **)(r + 32);
    INT *dx = *(INT **)(r + 24);
    WCHAR *glyphs = *(WCHAR **)(r + 40);
    UINT nglyphs = *(UINT *)(r + 48);
    int w = 0;
    for (int i = 0; i < n; i++) {
        Glyph *g = glyph(z, s[i]);
        int adv = g ? g->adv : 0;
        if (order) order[i] = (UINT)i;
        if (caret) caret[i] = (UINT)w;
        if (dx) dx[i] = adv;
        if (glyphs && (UINT)i < nglyphs) glyphs[i] = (WCHAR)stbtt_FindGlyphIndex(&z->f->info, s[i]);
        w += adv;
    }
    *(UINT *)(r + 48) = (UINT)n;
    return (DWORD)(z->ascent + z->descent) << 16 | (DWORD)(w & 0xFFFF);
}

/* The font file itself (programs that shape text read its tables) */
GDIAPI DWORD GetFontData(HDC h, DWORD table, DWORD off, LPVOID buf, DWORD n)
{
    Size *z = dc_size(dc_of(h), 0);
    if (!z) return GDI_ERROR;
    const unsigned char *base = z->f->data;
    DWORD size = z->f->size;
    if (table) {
        UINT32 t = (table & 0xFF) << 24 | (table & 0xFF00) << 8 | (table >> 8 & 0xFF00) | table >> 24;   /* stored as the file's bytes */
        int tabs = base[4] << 8 | base[5];
        const unsigned char *e = 0;
        for (int i = 0; i < tabs; i++) {
            const unsigned char *d = base + 12 + 16 * i;
            UINT32 tag = (UINT32)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
            if (tag == t) { e = d; break; }
        }
        if (!e) return GDI_ERROR;
        DWORD to = (DWORD)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11], tl = (DWORD)e[12] << 24 | e[13] << 16 | e[14] << 8 | e[15];
        base += to;
        size = tl;
    }
    if (off > size) return GDI_ERROR;
    DWORD k = size - off;
    if (!buf) return k;
    if (n < k) k = n;
    memcpy(buf, base + off, k);
    return k;
}

GDIAPI int GetTextCharset(HDC h) { (void)h; return 0; }   /* ANSI_CHARSET */
GDIAPI int GetTextCharsetInfo(HDC h, void *sig, DWORD flags) { (void)h; (void)flags; if (sig) memset(sig, 0, 24); return 0; }
GDIAPI DWORD GetKerningPairsW(HDC h, DWORD n, void *pairs) { (void)h; (void)n; (void)pairs; return 0; }
GDIAPI DWORD GetFontLanguageInfo(HDC h) { (void)h; return 0; }
GDIAPI int SetTextCharacterExtra(HDC h, int extra) { (void)h; (void)extra; return 0; }
GDIAPI int GetTextCharacterExtra(HDC h) { (void)h; return 0; }
GDIAPI BOOL SetTextJustification(HDC h, int extra, int count) { (void)h; (void)extra; (void)count; return TRUE; }
GDIAPI DWORD SetMapperFlags(HDC h, DWORD f) { (void)h; (void)f; return 0; }

/* -----------------------------------------------------------------------
 * Enumerating fonts: the names programs look for, all TrueType
 * ----------------------------------------------------------------------- */
typedef int (CALLBACK *FONTENUMPROCW_)(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM);
typedef int (CALLBACK *FONTENUMPROCA_)(const LOGFONTA *, const TEXTMETRICA *, DWORD, LPARAM);
static const char *const g_families[] = {
    "Segoe UI", "MS Shell Dlg", "MS Shell Dlg 2", "Tahoma", "Arial", "Microsoft Sans Serif", "Verdana", "Calibri",
    "Times New Roman", "Inter", "Consolas", "Courier New", "Lucida Console", "DejaVu Sans Mono",
    "Noto Sans Arabic", "Noto Sans Devanagari", 0 };

static int enum_fonts(HDC h, const WCHAR *want, FONTENUMPROCW_ fn, LPARAM lp)
{
    int r = 1;
    for (int i = 0; g_families[i] && r; i++) {
        if (want && want[0] && !wieq(want, g_families[i])) continue;
        struct { LOGFONTW lf; WCHAR full[64], style[32], script[32]; } elf;
        memset(&elf, 0, sizeof(elf));
        for (int k = 0; g_families[i][k]; k++) elf.lf.lfFaceName[k] = elf.full[k] = (WCHAR)g_families[i][k];
        elf.style[0] = 'R'; elf.style[1] = 'e'; elf.style[2] = 'g';
        GObj f;
        memset(&f, 0, sizeof(f));
        memcpy(f.face, elf.lf.lfFaceName, sizeof(f.face));
        f.height = -16; f.weight = 400;
        Size *z = size_for(face_index(&f), -16);
        if (!z) break;
        struct { TEXTMETRICW tm; DWORD ntmFlags; UINT ntmSizeEM, ntmCellHeight, ntmAvgWidth; DWORD sig[6]; } ntm;
        memset(&ntm, 0, sizeof(ntm));
        fill_metrics(z, &f, &ntm.tm);
        ntm.ntmSizeEM = 2048;
        elf.lf.lfHeight = ntm.tm.tmHeight;
        elf.lf.lfWeight = 400;
        elf.lf.lfOutPrecision = 3; elf.lf.lfClipPrecision = 2; elf.lf.lfQuality = 1;
        int mono = z->fi == F_MONO || z->fi == F_MONO_BOLD;
        elf.lf.lfPitchAndFamily = (BYTE)((mono ? 1 : 2) | (mono ? 0x30 : 0x20));
        r = fn(&elf.lf, &ntm.tm, 4 /* TRUETYPE_FONTTYPE */, lp);
    }
    return r;
}

GDIAPI int EnumFontFamiliesExW(HDC h, LOGFONTW *want, FONTENUMPROCW_ fn, LPARAM lp, DWORD flags)
{
    (void)flags;
    return enum_fonts(h, want ? want->lfFaceName : 0, fn, lp);
}
GDIAPI int EnumFontFamiliesW(HDC h, LPCWSTR face_name, FONTENUMPROCW_ fn, LPARAM lp) { return enum_fonts(h, face_name, fn, lp); }
GDIAPI int EnumFontsW(HDC h, LPCWSTR face_name, FONTENUMPROCW_ fn, LPARAM lp) { return enum_fonts(h, face_name, fn, lp); }

typedef struct { FONTENUMPROCA_ fn; LPARAM lp; } EnumA;
static int CALLBACK enum_a(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    EnumA *e = (EnumA *)lp;
    struct { LOGFONTA lf; char full[64], style[32], script[32]; } a;
    memset(&a, 0, sizeof(a));
    memcpy(&a.lf, lf, 28);
    WideCharToMultiByte(CP_ACP, 0, lf->lfFaceName, -1, a.lf.lfFaceName, 32, 0, 0);
    memcpy(a.full, a.lf.lfFaceName, 32);
    TEXTMETRICA ta;
    memcpy(&ta, tm, 44);
    ta.tmFirstChar = 0x20; ta.tmLastChar = (char)0xFF; ta.tmDefaultChar = 0x1F; ta.tmBreakChar = ' ';
    ta.tmItalic = tm->tmItalic; ta.tmUnderlined = tm->tmUnderlined; ta.tmStruckOut = tm->tmStruckOut;
    ta.tmPitchAndFamily = tm->tmPitchAndFamily; ta.tmCharSet = tm->tmCharSet;
    return e->fn(&a.lf, &ta, type, e->lp);
}
GDIAPI int EnumFontFamiliesExA(HDC h, LOGFONTA *want, FONTENUMPROCA_ fn, LPARAM lp, DWORD flags)
{
    (void)flags;
    WCHAR w[32] = { 0 };
    if (want) MultiByteToWideChar(CP_ACP, 0, want->lfFaceName, -1, w, 32);
    EnumA e = { fn, lp };
    return enum_fonts(h, w, enum_a, (LPARAM)&e);
}
GDIAPI int EnumFontFamiliesA(HDC h, LPCSTR face_name, FONTENUMPROCA_ fn, LPARAM lp)
{
    WCHAR w[32] = { 0 };
    if (face_name) MultiByteToWideChar(CP_ACP, 0, face_name, -1, w, 32);
    EnumA e = { fn, lp };
    return enum_fonts(h, w, enum_a, (LPARAM)&e);
}
GDIAPI int EnumFontsA(HDC h, LPCSTR face_name, FONTENUMPROCA_ fn, LPARAM lp) { return EnumFontFamiliesA(h, face_name, fn, lp); }

/* Private fonts are accepted (their text draws with the built-in faces).
 * A bare file name is a font in the Windows Fonts folder, as on Windows
 * (setup programs register the fonts they copy there by name). */
GDIAPI int AddFontResourceExW(LPCWSTR f, DWORD fl, PVOID r)
{
    (void)fl; (void)r;
    if (!f || !*f) return 0;
    if (GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES) return 1;
    for (LPCWSTR c = f; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':') return 0;
    WCHAR path[MAX_PATH];
    UINT n = GetWindowsDirectoryW(path, MAX_PATH - 8);
    if (!n || n >= MAX_PATH - 8) return 0;
    memcpy(path + n, L"\\Fonts\\", 7 * sizeof(WCHAR));
    n += 7;
    for (; *f && n < MAX_PATH - 1; f++) path[n++] = *f;
    path[n] = 0;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
GDIAPI int AddFontResourceW(LPCWSTR f) { return AddFontResourceExW(f, 0, 0); }
GDIAPI int AddFontResourceA(LPCSTR f)
{
    WCHAR w[MAX_PATH];
    if (!f || !MultiByteToWideChar(CP_ACP, 0, f, -1, w, MAX_PATH)) return 0;
    return AddFontResourceExW(w, 0, 0);
}
GDIAPI BOOL RemoveFontResourceExW(LPCWSTR f, DWORD fl, PVOID r) { (void)f; (void)fl; (void)r; return TRUE; }
GDIAPI BOOL RemoveFontResourceW(LPCWSTR f) { (void)f; return TRUE; }
GDIAPI HANDLE AddFontMemResourceEx(PVOID p, DWORD n, PVOID r, DWORD *count) { (void)p; (void)n; (void)r; if (count) *count = 1; return (HANDLE)(ULONG_PTR)0xF0E1; }
GDIAPI BOOL RemoveFontMemResourceEx(HANDLE h) { (void)h; return TRUE; }

/* -----------------------------------------------------------------------
 * For user32 (the Windows 8x8-cell fallbacks it used to call)
 * ----------------------------------------------------------------------- */
__declspec(dllexport) void NovaGdiFill(HDC h, int l, int t, int r, int b, COLORREF c)
{
    NOVA_DC *d = dc_of(h);
    if (d) fill(d, l, t, r, b, c);
}
__declspec(dllexport) int NovaGdiCellW(void) { Size *z = size_for(F_SANS, -12); return z ? z->ave : 7; }
__declspec(dllexport) int NovaGdiCellH(void) { Size *z = size_for(F_SANS, -12); return z ? z->ascent + z->descent : 15; }
__declspec(dllexport) void NovaGdiChar(HDC h, int x, int y, char c) { WCHAR w = (WCHAR)(BYTE)c; TextOutW(h, x, y, &w, 1); }
int _fltused = 1;   /* the MSVC ABI marker for floating point use */

/* -----------------------------------------------------------------------
 * Outline metrics and the characters a font covers, read from its tables
 * ----------------------------------------------------------------------- */
static const unsigned char *font_table(Face *f, const char tag[4], DWORD *len)
{
    const unsigned char *b = f->data;
    int tabs = b[4] << 8 | b[5];
    for (int i = 0; i < tabs; i++) {
        const unsigned char *e = b + 12 + 16 * i;
        if (e[0] == (unsigned char)tag[0] && e[1] == (unsigned char)tag[1] && e[2] == (unsigned char)tag[2] &&
            e[3] == (unsigned char)tag[3]) {
            DWORD off = (DWORD)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11];
            DWORD n = (DWORD)e[12] << 24 | e[13] << 16 | e[14] << 8 | e[15];
            if (off > f->size || n > f->size - off) return 0;
            if (len) *len = n;
            return b + off;
        }
    }
    return 0;
}
static int be16(const unsigned char *t, DWORD len, DWORD off) { return t && off + 2 <= len ? (short)(t[off] << 8 | t[off + 1]) : 0; }
static UINT ube16(const unsigned char *t, DWORD len, DWORD off) { return t && off + 2 <= len ? (UINT)(t[off] << 8 | t[off + 1]) : 0; }

/* a name-table string (Windows platform, then any) as UTF-16 */
static int font_name(Face *f, int id, WCHAR *out, int cap)
{
    static const int tries[][3] = { { 3, 1, 0x409 }, { 3, 0, 0x409 }, { 3, 10, 0x409 } };
    for (int k = 0; k < 3; k++) {
        int len = 0;
        const char *s = stbtt_GetFontNameString(&f->info, &len, tries[k][0], tries[k][1], tries[k][2], id);
        if (!s) continue;
        int n = 0;
        for (int i = 0; i + 1 < len && n < cap - 1; i += 2) out[n++] = (WCHAR)((unsigned char)s[i] << 8 | (unsigned char)s[i + 1]);
        out[n] = 0;
        return n;
    }
    int len = 0;   /* Macintosh Roman */
    const char *s = stbtt_GetFontNameString(&f->info, &len, 1, 0, 0, id);
    int n = 0;
    for (int i = 0; s && i < len && n < cap - 1; i++) out[n++] = (unsigned char)s[i];
    out[n] = 0;
    return n;
}

#define OTM_NAME 128
static size_t wlen_(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
/* fills @o (sizeof(OUTLINETEXTMETRICW)) and the four names; returns FALSE without a font */
static BOOL outline_metrics(HDC h, OUTLINETEXTMETRICW *o, WCHAR names[4][OTM_NAME])
{
    GObj *fo;
    Size *z = dc_size(dc_of(h), &fo);
    if (!z) return FALSE;
    Face *f = z->f;
    memset(o, 0, sizeof(*o));
    fill_metrics(z, fo, &o->otmTextMetrics);
    DWORD hl = 0, hhl = 0, ol = 0, pl = 0;
    const unsigned char *head = font_table(f, "head", &hl), *hhea = font_table(f, "hhea", &hhl),
                        *os2 = font_table(f, "OS/2", &ol), *post = font_table(f, "post", &pl);
    float s = z->scale;
#define SC(v) ((int)((v) * s + ((v) < 0 ? -0.5f : 0.5f)))
    if (os2 && ol >= 42) memcpy(&o->otmPanoseNumber, os2 + 32, 10);
    o->otmfsSelection = ube16(os2, ol, 62);
    o->otmfsType = ube16(os2, ol, 8);
    o->otmsCharSlopeRise = be16(hhea, hhl, 18);
    o->otmsCharSlopeRun = be16(hhea, hhl, 20);
    if (post && pl >= 8) {
        int fixed = (int)((DWORD)post[4] << 24 | post[5] << 16 | post[6] << 8 | post[7]);
        o->otmItalicAngle = (int)((long long)fixed * 10 / 65536);
    }
    o->otmEMSquare = ube16(head, hl, 18);
    o->otmAscent = SC(be16(os2, ol, 68));
    o->otmDescent = SC(be16(os2, ol, 70));
    o->otmLineGap = (UINT)SC(be16(os2, ol, 72));
    o->otmsXHeight = (UINT)SC(be16(os2, ol, 86));
    o->otmsCapEmHeight = (UINT)SC(be16(os2, ol, 88));
    o->otmrcFontBox.left = SC(be16(head, hl, 36));
    o->otmrcFontBox.bottom = SC(be16(head, hl, 38));
    o->otmrcFontBox.right = SC(be16(head, hl, 40));
    o->otmrcFontBox.top = SC(be16(head, hl, 42));
    o->otmMacAscent = SC(be16(hhea, hhl, 4));
    o->otmMacDescent = SC(be16(hhea, hhl, 6));
    o->otmMacLineGap = (UINT)SC(be16(hhea, hhl, 8));
    o->otmusMinimumPPEM = ube16(head, hl, 46);
    o->otmptSubscriptSize.x = SC(be16(os2, ol, 10));
    o->otmptSubscriptSize.y = SC(be16(os2, ol, 12));
    o->otmptSubscriptOffset.x = SC(be16(os2, ol, 14));
    o->otmptSubscriptOffset.y = SC(be16(os2, ol, 16));
    o->otmptSuperscriptSize.x = SC(be16(os2, ol, 18));
    o->otmptSuperscriptSize.y = SC(be16(os2, ol, 20));
    o->otmptSuperscriptOffset.x = SC(be16(os2, ol, 22));
    o->otmptSuperscriptOffset.y = SC(be16(os2, ol, 24));
    o->otmsStrikeoutSize = (UINT)SC(be16(os2, ol, 26));
    o->otmsStrikeoutPosition = SC(be16(os2, ol, 28));
    o->otmsUnderscorePosition = SC(be16(post, pl, 8));
    o->otmsUnderscoreSize = SC(be16(post, pl, 10));
#undef SC
    if (!o->otmAscent && !o->otmDescent) {          /* no OS/2 table: the hhea values */
        o->otmAscent = o->otmMacAscent;
        o->otmDescent = o->otmMacDescent;
        o->otmLineGap = o->otmMacLineGap;
    }
    /* family, face (full name), style, full (unique) name, as Windows fills them */
    static const int ids[4] = { 1, 4, 2, 3 };
    for (int i = 0; i < 4; i++)
        if (!font_name(f, ids[i], names[i], OTM_NAME) && i == 0) GetTextFaceW(h, OTM_NAME, names[0]);
    return TRUE;
}

GDIAPI UINT GetOutlineTextMetricsW(HDC h, UINT size, OUTLINETEXTMETRICW *otm)
{
    OUTLINETEXTMETRICW o;
    WCHAR names[4][OTM_NAME];
    if (!outline_metrics(h, &o, names)) return 0;
    UINT need = sizeof(o), at[4];
    for (int i = 0; i < 4; i++) {
        at[i] = need;
        need += (UINT)(wlen_(names[i]) + 1) * sizeof(WCHAR);
    }
    o.otmSize = need;
    if (!otm) return need;
    if (size < sizeof(o)) return 0;
    for (int i = 0; i < 4; i++) {
        char **p = i == 0 ? &o.otmpFamilyName : i == 1 ? &o.otmpFaceName : i == 2 ? &o.otmpStyleName : &o.otmpFullName;
        *p = (char *)(UINT_PTR)at[i];
    }
    BYTE *out = (BYTE *)otm;
    memcpy(out, &o, sizeof(o));
    for (int i = 0; i < 4; i++) {
        UINT n = (UINT)(wlen_(names[i]) + 1) * sizeof(WCHAR);
        if (at[i] + n <= size) memcpy(out + at[i], names[i], n);
        else otm->otmSize = at[i];
    }
    return size < need ? size : need;
}

GDIAPI UINT GetOutlineTextMetricsA(HDC h, UINT size, OUTLINETEXTMETRICA *otm)
{
    OUTLINETEXTMETRICW w;
    OUTLINETEXTMETRICA a;
    WCHAR names[4][OTM_NAME];
    char an[4][OTM_NAME * 2];
    if (!outline_metrics(h, &w, names)) return 0;
    TEXTMETRICA tma;
    GetTextMetricsA(h, &tma);
    memset(&a, 0, sizeof(a));
    a.otmTextMetrics = tma;
    /* everything after the TEXTMETRIC lines up field for field */
    memcpy(&a.otmFiller, &w.otmFiller, (size_t)((char *)&w.otmpFamilyName - (char *)&w.otmFiller));
    UINT need = sizeof(a), at[4], len[4];
    for (int i = 0; i < 4; i++) {
        int n = WideCharToMultiByte(CP_ACP, 0, names[i], -1, an[i], (int)sizeof(an[i]), 0, 0);
        if (n <= 0) { an[i][0] = 0; n = 1; }
        len[i] = (UINT)n;
        at[i] = need;
        need += len[i];
    }
    a.otmSize = need;
    if (!otm) return need;
    if (size < sizeof(a)) return 0;
    a.otmpFamilyName = (char *)(UINT_PTR)at[0];
    a.otmpFaceName = (char *)(UINT_PTR)at[1];
    a.otmpStyleName = (char *)(UINT_PTR)at[2];
    a.otmpFullName = (char *)(UINT_PTR)at[3];
    BYTE *out = (BYTE *)otm;
    memcpy(out, &a, sizeof(a));
    for (int i = 0; i < 4; i++) {
        if (at[i] + len[i] <= size) memcpy(out + at[i], an[i], len[i]);
        else otm->otmSize = at[i];
    }
    return size < need ? size : need;
}

/* the BMP characters the selected font has glyphs for, as ranges */
GDIAPI DWORD GetFontUnicodeRanges(HDC h, LPGLYPHSET gs)
{
    Size *z = dc_size(dc_of(h), 0);
    if (!z) return 0;
    /* like Windows, a buffer is taken to be the size an earlier call with NULL returned */
    DWORD ranges = 0, glyphs = 0, cap = 0x8000;
    int in = 0;
    for (UINT32 c = 0; c <= 0x10000; c++) {
        int has = c < 0x10000 && !(c >= 0xD800 && c < 0xE000) && stbtt_FindGlyphIndex(&z->f->info, (int)c) != 0;
        if (has) {
            if (!in) {
                if (gs && ranges < cap) { gs->ranges[ranges].wcLow = (WCHAR)c; gs->ranges[ranges].cGlyphs = 0; }
                ranges++;
                in = 1;
            }
            if (gs && ranges - 1 < cap) gs->ranges[ranges - 1].cGlyphs++;
            glyphs++;
        } else in = 0;
    }
    DWORD need = (DWORD)(sizeof(GLYPHSET) + (ranges ? ranges - 1 : 0) * sizeof(WCRANGE));
    if (gs) {
        gs->cbThis = need;
        gs->flAccel = 0;
        gs->cGlyphsSupported = glyphs;
        gs->cRanges = ranges < cap ? ranges : cap;
    }
    return need;
}

/* -----------------------------------------------------------------------
 * GetGlyphOutline: a glyph's metrics, its coverage bitmap (GGO_BITMAP,
 * GGO_GRAY2/4/8_BITMAP) or its outline (GGO_NATIVE, quadratic splines in
 * 16.16 pixels).  Qt's GDI font engine measures and draws through it;
 * wglUseFontOutlines builds display lists from the outline.  The
 * transform (@mat) is taken as the identity.
 * ----------------------------------------------------------------------- */
typedef struct { UINT bbx, bby; LONG ox, oy; short incx, incy; } GGO_METRICS_;
typedef struct { WORD fract; short value; } FIXED_;

static void put_fx(BYTE *p, float v)
{
    LONG f = (LONG)(v * 65536.0f + (v < 0 ? -0.5f : 0.5f));
    memcpy(p, &f, 4);                           /* FIXED: fract, then value */
}

/* GGO_NATIVE: TTPOLYGONHEADER per contour, then one TTPOLYCURVE per segment */
static DWORD glyph_native(Size *z, int gi, DWORD size, BYTE *buf)
{
    stbtt_vertex *v = 0;
    int nv = stbtt_GetGlyphShape(&z->f->info, gi, &v);
    float sc = z->scale;
    DWORD need = 0, hdr = 0;
    for (int pass = 0; pass < 2; pass++) {
        DWORD at = 0;
        for (int i = 0; i < nv; i++) {
            if (v[i].type == STBTT_vmove) {
                if (pass && hdr != (DWORD)-1) { DWORD cb = at - hdr; memcpy(buf + hdr, &cb, 4); }
                hdr = at;
                if (pass) {
                    DWORD type = 24;                    /* TT_POLYGON_TYPE */
                    memcpy(buf + at + 4, &type, 4);
                    put_fx(buf + at + 8, v[i].x * sc);
                    put_fx(buf + at + 12, v[i].y * sc);
                }
                at += 16;
                continue;
            }
            int q = v[i].type == STBTT_vline ? 1 : v[i].type == STBTT_vcurve ? 2 : 3;
            WORD npt = (WORD)(q == 1 ? 1 : q == 2 ? 2 : 3);
            if (pass) {
                WORD t = (WORD)q;
                memcpy(buf + at, &t, 2);
                memcpy(buf + at + 2, &npt, 2);
                BYTE *pt = buf + at + 4;
                if (q == 2) { put_fx(pt, v[i].cx * sc); put_fx(pt + 4, v[i].cy * sc); pt += 8; }
                if (q == 3) {
                    put_fx(pt, v[i].cx * sc);  put_fx(pt + 4, v[i].cy * sc);
                    put_fx(pt + 8, v[i].cx1 * sc); put_fx(pt + 12, v[i].cy1 * sc);
                    pt += 16;
                }
                put_fx(pt, v[i].x * sc); put_fx(pt + 4, v[i].y * sc);
            }
            at += 4 + 8u * npt;
        }
        if (!pass) {
            need = at;
            if (!buf || !size) break;
            if (size < need) { need = (DWORD)-1; break; }
        } else if (nv) {
            DWORD cb = at - hdr;
            memcpy(buf + hdr, &cb, 4);
        }
    }
    if (v) stbtt_FreeShape(&z->f->info, v);
    return need;
}

GDIAPI DWORD GetGlyphOutlineW(HDC h, UINT c, UINT fmt, void *gm_out, DWORD size, void *buf, const void *mat)
{
    (void)mat;
    NOVA_DC *d = dc_of(h);
    GObj *font;
    Size *z = dc_size(d, &font);
    if (!z || !gm_out) return GDI_ERROR;
    int th = scaled_height(d, font);                    /* in device space, as Windows measures */
    if (th) z = size_for(face_index(font), th);
    if (!z) return GDI_ERROR;
    UINT kind = fmt & 0x7F;                          /* without GGO_GLYPH_INDEX, GGO_UNHINTED */
    UINT32 key = fmt & 0x80 ? (c | 0x80000000u) : c;
    Glyph *g = glyph(z, key);
    if (!g) return GDI_ERROR;
    GGO_METRICS_ gm;
    gm.bbx = g->w ? (UINT)g->w : 1;
    gm.bby = g->h ? (UINT)g->h : 1;
    gm.ox = g->w ? g->x0 : 0;
    gm.oy = g->h ? -g->y0 : 0;
    gm.incx = g->adv;
    gm.incy = 0;
    memcpy(gm_out, &gm, sizeof(gm));
    if (kind == 0) return 1;                         /* GGO_METRICS: non-zero on success */
    if (kind == 2) {                                 /* GGO_NATIVE */
        int gi = key & 0x80000000u ? (int)c : stbtt_FindGlyphIndex(&z->f->info, (int)c);
        return glyph_native(z, gi, size, buf);
    }
    if (kind != 1 && (kind < 4 || kind > 6)) return GDI_ERROR;
    if (!g->w || !g->h) return 0;                    /* blank glyph: no bitmap */
    DWORD pitch = kind == 1 ? (((DWORD)g->w + 31) / 32) * 4 : (((DWORD)g->w + 3) & ~3u);
    DWORD need = pitch * (DWORD)g->h;
    if (!buf || !size) return need;
    if (size < need) return GDI_ERROR;
    BYTE *o = buf;
    memset(o, 0, need);
    int levels = kind == 4 ? 4 : kind == 5 ? 16 : 64;
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++) {
            unsigned cov = g->bmp ? g->bmp[y * g->w + x] : 0;
            if (kind == 1) { if (cov >= 128) o[y * pitch + x / 8] |= (BYTE)(0x80 >> (x & 7)); }
            else o[y * pitch + x] = (BYTE)((cov * levels + 127) / 255);
        }
    return need;
}

GDIAPI DWORD GetGlyphOutlineA(HDC h, UINT c, UINT fmt, void *gm, DWORD size, void *buf, const void *mat)
{
    if (!(fmt & 0x80) && c < 0x100) {
        char ch = (char)c;
        WCHAR w = 0;
        MultiByteToWideChar(CP_ACP, 0, &ch, 1, &w, 1);
        c = w;
    }
    return GetGlyphOutlineW(h, c, fmt, gm, size, buf, mat);
}
