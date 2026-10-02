/*
 * font.c — font files for NovaOS DirectWrite: OpenType tables, metrics,
 * names, glyph outlines and an anti-aliasing rasterizer
 *
 * Outlines and the scanline rasterizer come from stb_truetype (public
 * domain).  Glyphs are flattened in font units, transformed by the full
 * 2x2 matrix plus offset into device space, then rasterized, so any
 * rotation, shear or scale DirectWrite callers ask for is honoured.
 */
#include "dwrite_int.h"


/* -----------------------------------------------------------------------
 * stb_truetype, with what it needs from a C library
 * ----------------------------------------------------------------------- */
static double t_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static double t_fabs(double x) { return x < 0 ? -x : x; }
static double t_fmod(double x, double y) { return y ? x - (double)(long long)(x / y) * y : 0; }
static double t_pow(double x, double y)
{
    double r = 1;     /* only stb_truetype's signed-distance code uses pow; DirectWrite does not */
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
static double t_acos(double x) { return 1.5707963267949 - x; }   /* SDF only */

#define STBTT_ifloor(x)   ((int)dw_floor(x))
#define STBTT_iceil(x)    ((int)dw_ceil(x))
#define STBTT_sqrt(x)     t_sqrt(x)
#define STBTT_pow(x, y)   t_pow(x, y)
#define STBTT_fmod(x, y)  t_fmod(x, y)
#define STBTT_cos(x)      t_cos(x)
#define STBTT_acos(x)     t_acos(x)
#define STBTT_fabs(x)     t_fabs(x)
#define STBTT_malloc(x, u) ((void)(u), dw_alloc(x))
#define STBTT_free(x, u)   ((void)(u), dw_free(x))
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

_Static_assert(sizeof(GlyphVertex) == sizeof(stbtt_vertex), "GlyphVertex is stbtt_vertex");

static UINT16 be16(const BYTE *p) { return (UINT16)(p[0] << 8 | p[1]); }
static INT16  bes16(const BYTE *p) { return (INT16)be16(p); }
static UINT32 be32(const BYTE *p) { return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 | p[3]; }

/* -----------------------------------------------------------------------
 * Font files
 * ----------------------------------------------------------------------- */
FontData *font_data_from_bytes(BYTE *bytes, UINT32 size)
{
    if (size < 12) { dw_free(bytes); return NULL; }
    UINT32 type = FILE_UNKNOWN, face = FACE_UNKNOWN, n = 1;
    UINT32 tag = be32(bytes);
    if (tag == 0x74746366 /* ttcf */) {
        n = be32(bytes + 8);
        if (n == 0 || n > 4096 || 12 + n * 4 > size) { dw_free(bytes); return NULL; }
        UINT32 first = be32(bytes + 12);
        BOOL cff = first + 4 <= size && be32(bytes + first) == 0x4F54544F;
        type = FILE_COLLECTION;
        face = FACE_COLLECTION;
        (void)cff;
    } else if (tag == 0x00010000 || tag == 0x74727565 /* true */) {
        type = FILE_TRUETYPE; face = FACE_TRUETYPE;
    } else if (tag == 0x4F54544F /* OTTO */) {
        type = FILE_CFF; face = FACE_CFF;
    } else {
        dw_free(bytes);
        return NULL;
    }
    FontData *d = dw_zalloc(sizeof(*d));
    if (!d) { dw_free(bytes); return NULL; }
    d->ref = 1;
    d->bytes = bytes;
    d->size = size;
    d->file_type = type;
    d->face_type = face;
    d->num_faces = n;
    d->faces = dw_zalloc(n * sizeof(FaceData *));
    InitializeSRWLock(&d->lock);
    return d;
}

FontData *font_data_load_path(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD hi = 0, size = GetFileSize(h, &hi);
    BYTE *buf = (hi || size == INVALID_FILE_SIZE || size < 12) ? NULL : dw_alloc(size);
    DWORD got = 0;
    BOOL ok = buf && ReadFile(h, buf, size, &got, NULL) && got == size;
    CloseHandle(h);
    if (!ok) { dw_free(buf); return NULL; }
    return font_data_from_bytes(buf, size);
}

void font_data_addref(FontData *d) { InterlockedIncrement(&d->ref); }

void font_data_release(FontData *d)
{
    if (!d || InterlockedDecrement(&d->ref)) return;
    for (UINT32 i = 0; i < d->num_faces; i++)
        if (d->faces[i]) { dw_free(d->faces[i]->info); dw_free(d->faces[i]); }
    dw_free(d->faces);
    dw_free(d->bytes);
    dw_free(d);
}

const BYTE *sfnt_table(const FaceData *f, const char tag[4], UINT32 *size)
{
    const BYTE *b = f->file->bytes;
    UINT32 total = f->file->size, dir = f->offset;
    if (dir + 12 > total) return NULL;
    UINT32 n = be16(b + dir + 4);
    for (UINT32 i = 0; i < n; i++) {
        const BYTE *r = b + dir + 12 + i * 16;
        if (r + 16 > b + total) break;
        if (memcmp(r, tag, 4)) continue;
        UINT32 off = be32(r + 8), len = be32(r + 12);
        if (off > total || len > total - off) return NULL;
        if (size) *size = len;
        return b + off;
    }
    return NULL;
}

int sfnt_name(const FaceData *f, int id, WCHAR *out, int cap)
{
    UINT32 size;
    const BYTE *t = sfnt_table(f, "name", &size);
    if (!t || size < 6 || cap < 1) return 0;
    UINT32 count = be16(t + 2), strings = be16(t + 4);
    const BYTE *best = NULL;
    int best_rank = 0;
    for (UINT32 i = 0; i < count && 6 + i * 12 + 12 <= size; i++) {
        const BYTE *r = t + 6 + i * 12;
        if (be16(r + 6) != id) continue;
        UINT16 plat = be16(r), enc = be16(r + 2), lang = be16(r + 4);
        int rank = 0;
        if (plat == 3 && (enc == 1 || enc == 10 || enc == 0)) rank = lang == 0x409 ? 4 : 3;
        else if (plat == 0) rank = 2;
        else if (plat == 1 && enc == 0) rank = lang == 0 ? 1 : 0;
        if (rank > best_rank) { best_rank = rank; best = r; }
    }
    if (!best) return 0;
    UINT32 len = be16(best + 8), off = strings + be16(best + 10);
    if (off > size || len > size - off) return 0;
    const BYTE *s = t + off;
    int n = 0;
    if (best_rank >= 2) {
        for (UINT32 i = 0; i + 1 < len && n < cap - 1; i += 2) out[n++] = (WCHAR)be16(s + i);
    } else {
        for (UINT32 i = 0; i < len && n < cap - 1; i++) out[n++] = s[i] < 0x80 ? s[i] : '?';
    }
    out[n] = 0;
    return n;
}

/* -----------------------------------------------------------------------
 * Faces: metrics from head, hhea, OS/2 and post
 * ----------------------------------------------------------------------- */
static void face_init_metrics(FaceData *f)
{
    stbtt_fontinfo *info = f->info;
    UINT32 n;
    const BYTE *head = sfnt_table(f, "head", &n);
    f->upem = head && n >= 54 ? be16(head + 18) : 2048;
    if (f->upem < 16) f->upem = 2048;
    DW_FONT_METRICS1 *m = &f->metrics;
    m->m.designUnitsPerEm = f->upem;
    if (head && n >= 54) {
        m->glyphBoxLeft = bes16(head + 36); m->glyphBoxBottom = bes16(head + 38);
        m->glyphBoxRight = bes16(head + 40); m->glyphBoxTop = bes16(head + 42);
    }
    f->num_glyphs = (UINT16)info->numGlyphs;

    INT16 asc = (INT16)(f->upem * 8 / 10), desc = (INT16)(f->upem * 2 / 10), gap = 0;
    const BYTE *hhea = sfnt_table(f, "hhea", &n);
    if (hhea && n >= 36) {
        asc = bes16(hhea + 4); desc = (INT16)-bes16(hhea + 6); gap = bes16(hhea + 8);
        f->caret.slopeRise = bes16(hhea + 18);
        f->caret.slopeRun = bes16(hhea + 20);
        f->caret.offset = bes16(hhea + 22);
    } else {
        f->caret.slopeRise = 1;
    }
    f->hhea_ascent = asc;
    f->hhea_descent = desc;
    m->m.ascent = (UINT16)asc;
    m->m.descent = (UINT16)desc;
    m->m.lineGap = gap;

    f->weight = 400; f->stretch = 5; f->style = STYLE_NORMAL;
    const BYTE *os2 = sfnt_table(f, "OS/2", &n);
    UINT16 caph = 0, xh = 0;
    if (os2 && n >= 78) {
        UINT16 sel = be16(os2 + 62);
        f->weight = be16(os2 + 4);
        f->stretch = be16(os2 + 6);
        if (f->weight < 1 || f->weight > 1000) f->weight = 400;
        if (f->weight < 10) f->weight *= 100;      /* old fonts store 1..9 */
        if (f->stretch < 1 || f->stretch > 9) f->stretch = 5;
        f->style = (sel & 0x200) ? STYLE_OBLIQUE : (sel & 1) ? STYLE_ITALIC : STYLE_NORMAL;
        memcpy(f->panose, os2 + 32, 10);
        m->subscriptSizeX = bes16(os2 + 10); m->subscriptSizeY = bes16(os2 + 12);
        m->subscriptPositionX = bes16(os2 + 14); m->subscriptPositionY = (INT16)-bes16(os2 + 16);
        m->superscriptSizeX = bes16(os2 + 18); m->superscriptSizeY = bes16(os2 + 20);
        m->superscriptPositionX = bes16(os2 + 22); m->superscriptPositionY = bes16(os2 + 24);
        m->m.strikethroughThickness = (UINT16)bes16(os2 + 26);
        m->m.strikethroughPosition = bes16(os2 + 28);
        INT16 tasc = bes16(os2 + 68), tdesc = bes16(os2 + 70), tgap = bes16(os2 + 72);
        UINT16 wasc = be16(os2 + 74), wdesc = be16(os2 + 76);
        if (sel & 0x80) {                           /* USE_TYPO_METRICS */
            m->m.ascent = (UINT16)tasc; m->m.descent = (UINT16)-tdesc; m->m.lineGap = tgap;
            m->hasTypographicMetrics = TRUE;
        } else if (wasc || wdesc) {
            m->m.ascent = wasc; m->m.descent = wdesc;
            int g = (asc + desc + gap) - (wasc + wdesc);
            m->m.lineGap = (INT16)(g > 0 ? g : 0);
        }
        if (be16(os2) >= 2 && n >= 90) { xh = (UINT16)bes16(os2 + 86); caph = (UINT16)bes16(os2 + 88); }
        f->symbol = FALSE;
    }
    if (head && n >= 46 && !os2) {
        UINT16 mac = be16(head + 44);
        if (mac & 1) f->weight = 700;
        if (mac & 2) f->style = STYLE_ITALIC;
    }
    int x0, y0, x1, y1;
    if (!caph) { int g = stbtt_FindGlyphIndex(info, 'H'); caph = g && stbtt_GetGlyphBox(info, g, &x0, &y0, &x1, &y1) ? (UINT16)y1 : (UINT16)(m->m.ascent * 7 / 10); }
    if (!xh)   { int g = stbtt_FindGlyphIndex(info, 'x'); xh = g && stbtt_GetGlyphBox(info, g, &x0, &y0, &x1, &y1) ? (UINT16)y1 : (UINT16)(caph * 2 / 3); }
    m->m.capHeight = caph;
    m->m.xHeight = xh;
    if (!m->m.strikethroughThickness) m->m.strikethroughThickness = (UINT16)(f->upem / 20);
    if (!m->m.strikethroughPosition) m->m.strikethroughPosition = (INT16)(xh / 2);

    const BYTE *post = sfnt_table(f, "post", &n);
    if (post && n >= 16) {
        m->m.underlinePosition = bes16(post + 8);
        m->m.underlineThickness = (UINT16)bes16(post + 10);
        f->mono = be32(post + 12) != 0;
    }
    if (!m->m.underlineThickness) m->m.underlineThickness = (UINT16)(f->upem / 20);
    if (!m->m.underlinePosition) m->m.underlinePosition = (INT16)-(f->upem / 10);
    if (f->panose[0] == 2 && f->panose[3] == 9) f->mono = TRUE;

    /* a symbol font maps its glyphs through a (3,0) cmap */
    const BYTE *cmap = sfnt_table(f, "cmap", &n);
    if (cmap && n >= 4) {
        UINT32 subs = be16(cmap + 2);
        BOOL uni = FALSE, sym = FALSE;
        for (UINT32 i = 0; i < subs && 4 + i * 8 + 8 <= n; i++) {
            UINT16 plat = be16(cmap + 4 + i * 8), enc = be16(cmap + 6 + i * 8);
            if (plat == 0 || (plat == 3 && (enc == 1 || enc == 10))) uni = TRUE;
            if (plat == 3 && enc == 0) sym = TRUE;
        }
        f->symbol = sym && !uni;
    }
    f->has_kerning = sfnt_table(f, "kern", NULL) || sfnt_table(f, "GPOS", NULL);
}

FaceData *font_face_data(FontData *d, UINT32 index)
{
    if (!d || index >= d->num_faces) return NULL;
    AcquireSRWLockExclusive(&d->lock);
    FaceData *f = d->faces[index];
    if (!f) {
        int off = stbtt_GetFontOffsetForIndex(d->bytes, (int)index);
        stbtt_fontinfo *info = dw_zalloc(sizeof(*info));
        f = dw_zalloc(sizeof(*f));
        if (off >= 0 && info && f && stbtt_InitFont(info, d->bytes, off)) {
            f->file = d;
            f->index = index;
            f->offset = (UINT32)off;
            f->info = info;
            face_init_metrics(f);
            d->faces[index] = f;
        } else {
            dw_free(info);
            dw_free(f);
            f = NULL;
        }
    }
    ReleaseSRWLockExclusive(&d->lock);
    return f;
}

/* -----------------------------------------------------------------------
 * Glyphs
 * ----------------------------------------------------------------------- */
UINT16 face_glyph_index(const FaceData *f, UINT32 cp)
{
    int g = stbtt_FindGlyphIndex(f->info, (int)cp);
    if (!g && f->symbol && cp < 0x100) g = stbtt_FindGlyphIndex(f->info, (int)(0xF000 | cp));
    return (UINT16)g;
}

void face_glyph_metrics(const FaceData *f, UINT16 glyph, DW_GLYPH_METRICS *m)
{
    int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphHMetrics(f->info, glyph, &adv, &lsb);
    int asc = f->metrics.m.ascent, desc = f->metrics.m.descent;
    m->advanceWidth = (UINT32)adv;
    m->advanceHeight = (UINT32)(asc + desc);
    m->verticalOriginY = asc;
    if (stbtt_GetGlyphBox(f->info, glyph, &x0, &y0, &x1, &y1) && x1 > x0) {
        m->leftSideBearing = x0;
        m->rightSideBearing = adv - x1;
        m->topSideBearing = asc - y1;
        m->bottomSideBearing = desc + y0;
    } else {
        m->leftSideBearing = 0;
        m->rightSideBearing = adv;
        m->topSideBearing = asc;
        m->bottomSideBearing = desc;
    }
}

int face_kern(const FaceData *f, UINT16 a, UINT16 b)
{
    return f->has_kerning ? stbtt_GetGlyphKernAdvance(f->info, a, b) : 0;
}

static UINT32 add_range(DW_UNICODE_RANGE *out, UINT32 cap, UINT32 n, UINT32 first, UINT32 last)
{
    if (n && out && n <= cap && out[n - 1].last + 1 >= first) {
        if (last > out[n - 1].last) out[n - 1].last = last;
        return n;
    }
    if (out && n < cap) { out[n].first = first; out[n].last = last; }
    return n + 1;
}

UINT32 face_unicode_ranges(const FaceData *f, DW_UNICODE_RANGE *out, UINT32 cap)
{
    UINT32 size, n = 0;
    const BYTE *cmap = sfnt_table(f, "cmap", &size);
    if (!cmap || size < 4) return 0;
    const BYTE *fmt4 = NULL, *fmt12 = NULL;
    UINT32 subs = be16(cmap + 2);
    for (UINT32 i = 0; i < subs && 4 + i * 8 + 8 <= size; i++) {
        UINT16 plat = be16(cmap + 4 + i * 8), enc = be16(cmap + 6 + i * 8);
        UINT32 off = be32(cmap + 8 + i * 8);
        if (off + 8 > size || !(plat == 0 || (plat == 3 && (enc == 1 || enc == 10 || enc == 0)))) continue;
        UINT16 format = be16(cmap + off);
        if (format == 12 && !fmt12) fmt12 = cmap + off;
        if (format == 4 && !fmt4) fmt4 = cmap + off;
    }
    const BYTE *end = cmap + size;
    if (fmt12 && fmt12 + 16 <= end) {
        UINT32 groups = be32(fmt12 + 12);
        for (UINT32 i = 0; i < groups && fmt12 + 16 + i * 12 + 12 <= end; i++)
            n = add_range(out, cap, n, be32(fmt12 + 16 + i * 12), be32(fmt12 + 20 + i * 12));
    } else if (fmt4 && fmt4 + 14 <= end) {
        UINT32 segs = be16(fmt4 + 6) / 2;
        const BYTE *ends = fmt4 + 14, *starts = fmt4 + 16 + segs * 2;
        for (UINT32 i = 0; i < segs && starts + i * 2 + 2 <= end; i++) {
            UINT32 a = be16(starts + i * 2), b = be16(ends + i * 2);
            if (a == 0xFFFF) continue;
            n = add_range(out, cap, n, a, b);
        }
    }
    return n;
}

int face_glyph_shape(const FaceData *f, UINT16 glyph, GlyphVertex **verts)
{
    stbtt_vertex *v = NULL;
    int n = stbtt_GetGlyphShape(f->info, glyph, &v);
    *verts = (GlyphVertex *)v;
    return n;
}

void face_free_shape(GlyphVertex *verts) { dw_free(verts); }

/* -----------------------------------------------------------------------
 * Rasterizing
 * ----------------------------------------------------------------------- */
typedef struct { stbtt__point *pts; int *lens; int contours; int total; float x0, y0, x1, y1; } Flat;

static int flatten(const FaceData *f, UINT16 glyph, const DW_MATRIX *m, Flat *fl)
{
    memset(fl, 0, sizeof(*fl));
    stbtt_vertex *v = NULL;
    int nv = stbtt_GetGlyphShape(f->info, glyph, &v);
    if (nv <= 0) return 0;
    /* allowed error: a third of a pixel, in font units */
    double s1 = t_sqrt((double)m->m11 * m->m11 + (double)m->m12 * m->m12);
    double s2 = t_sqrt((double)m->m21 * m->m21 + (double)m->m22 * m->m22);
    double scale = s1 > s2 ? s1 : s2;
    if (scale <= 0) { dw_free(v); return 0; }
    fl->pts = stbtt_FlattenCurves(v, nv, (float)(0.35 / scale), &fl->lens, &fl->contours, NULL);
    dw_free(v);
    if (!fl->pts) return 0;
    for (int i = 0; i < fl->contours; i++) fl->total += fl->lens[i];
    fl->x0 = fl->y0 = 1e30f;
    fl->x1 = fl->y1 = -1e30f;
    for (int i = 0; i < fl->total; i++) {
        float x = fl->pts[i].x, y = fl->pts[i].y;
        float dx = x * m->m11 + y * m->m21 + m->dx;
        float dy = x * m->m12 + y * m->m22 + m->dy;
        fl->pts[i].x = dx;
        fl->pts[i].y = dy;
        if (dx < fl->x0) fl->x0 = dx;
        if (dx > fl->x1) fl->x1 = dx;
        if (dy < fl->y0) fl->y0 = dy;
        if (dy > fl->y1) fl->y1 = dy;
    }
    return fl->total > 0;
}

static void flat_free(Flat *fl)
{
    dw_free(fl->pts);
    dw_free(fl->lens);
}

void face_glyph_bounds(const FaceData *f, UINT16 glyph, const DW_MATRIX *m, int r[4])
{
    Flat fl;
    r[0] = r[1] = r[2] = r[3] = 0;
    if (!flatten(f, glyph, m, &fl)) return;
    r[0] = (int)dw_floor(fl.x0);
    r[1] = (int)dw_floor(fl.y0);
    r[2] = (int)dw_ceil(fl.x1);
    r[3] = (int)dw_ceil(fl.y1);
    if (r[2] == r[0]) r[2]++;
    if (r[3] == r[1]) r[3]++;
    flat_free(&fl);
}

void face_raster_glyph(const FaceData *f, UINT16 glyph, const DW_MATRIX *m,
                       BYTE *buf, int w, int h, int ox, int oy)
{
    Flat fl;
    if (!flatten(f, glyph, m, &fl)) return;
    int x0 = (int)dw_floor(fl.x0), y0 = (int)dw_floor(fl.y0);
    int gw = (int)dw_ceil(fl.x1) - x0 + 1, gh = (int)dw_ceil(fl.y1) - y0 + 1;
    BYTE *tmp = gw > 0 && gh > 0 && gw < 8192 && gh < 8192 ? dw_zalloc((SIZE_T)gw * gh) : NULL;
    if (tmp) {
        stbtt__bitmap bm = { gw, gh, gw, tmp };
        stbtt__rasterize(&bm, fl.pts, fl.lens, fl.contours, 1, 1, 0, 0, x0, y0, 0, NULL);
        for (int y = 0; y < gh; y++) {
            int dy = y0 + y - oy;
            if (dy < 0 || dy >= h) continue;
            for (int x = 0; x < gw; x++) {
                int dx = x0 + x - ox;
                if (dx < 0 || dx >= w || !tmp[y * gw + x]) continue;
                int v = buf[dy * w + dx] + tmp[y * gw + x];
                buf[dy * w + dx] = (BYTE)(v > 255 ? 255 : v);
            }
        }
        dw_free(tmp);
    }
    flat_free(&fl);
}
