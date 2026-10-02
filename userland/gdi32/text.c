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
                     const RECT *opaque_rc)
{
    GObj *font;
    Size *s = dc_size(d, &font);
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

static int is_complex(LPCWSTR s, int n);
GDIAPI BOOL TextOutW(HDC h, int x, int y, LPCWSTR s, int len)
{
    NOVA_DC *d = dc_of(h);
    if (!d || !s) return FALSE;
    if (len > 0 && is_complex(s, len)) return ExtTextOutW(h, x, y, 0, 0, s, (UINT)len, 0);
    return text_out(d, x, y, s, len, 0, 0, 0, 0);
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
    if (rc && (opts & 2 /* ETO_OPAQUE */)) fill(d, rc->left, rc->top, rc->right, rc->bottom, d->bk_color);
    if (!s || !len) return TRUE;
    RECT clip, *cp = 0;
    if (rc && (opts & 4 /* ETO_CLIPPED */)) {
        clip.left = rc->left + d->org_x; clip.top = rc->top + d->org_y;
        clip.right = rc->right + d->org_x; clip.bottom = rc->bottom + d->org_y;
        cp = &clip;
    }
    if (!(opts & (0x10 /* ETO_GLYPH_INDEX */ | 0x1000 /* ETO_IGNORELANGUAGE */)) && is_complex(s, (int)len)) {
        BOOL ok;
        if (complex_text_out(h, d, x, y, opts, rc, s, (int)len, &ok)) return ok;
    }
    INT *pdx = 0;
    if (dx && (opts & 0x2000 /* ETO_PDY */)) {              /* x,y pairs: keep the x advances */
        pdx = t_alloc(sizeof(INT) * len);
        if (pdx) for (UINT i = 0; i < len; i++) pdx[i] = dx[2 * i];
    }
    BOOL r = text_out(d, x, y, s, (int)len, pdx ? pdx : dx, cp, (opts & 0x10 /* ETO_GLYPH_INDEX */) != 0,
                      rc && (opts & 2) ? rc : 0);
    t_free(pdx);
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

GDIAPI BOOL GetCharABCWidthsW(HDC h, UINT first, UINT last, ABC *out)
{
    Size *z = dc_size(dc_of(h), 0);
    if (!z) return FALSE;
    for (UINT c = first; c <= last; c++) {
        Glyph *g = glyph(z, c);
        ABC *a = &out[c - first];
        if (!g) { a->abcA = 0; a->abcB = 0; a->abcC = 0; continue; }
        a->abcA = g->w ? g->x0 : 0;
        a->abcB = (UINT)(g->w ? g->w : g->adv);
        a->abcC = g->adv - a->abcA - (int)a->abcB;
    }
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

/* Private fonts are accepted (their text draws with the built-in faces) */
GDIAPI int AddFontResourceExW(LPCWSTR f, DWORD fl, PVOID r) { (void)fl; (void)r; return GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES; }
GDIAPI int AddFontResourceW(LPCWSTR f) { return AddFontResourceExW(f, 0, 0); }
GDIAPI int AddFontResourceA(LPCSTR f) { return GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES; }
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
