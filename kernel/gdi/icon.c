/*
 * icon.c — .ico files and PE icon resources (see icon.h)
 */

#include "icon.h"
#include "png.h"
#include "../lib/string.h"
#include "../mm/vmm.h"

static UINT16 le16(const UINT8 *p) { return (UINT16)(p[0] | p[1] << 8); }
static UINT32 le32(const UINT8 *p) { return (UINT32)p[0] | p[1] << 8 | p[2] << 16 | (UINT32)p[3] << 24; }

/* -----------------------------------------------------------------------
 * .ico parsing
 * ----------------------------------------------------------------------- */
GdiIcon *IconLoad(const void *data, size_t len)
{
    const UINT8 *d = data;
    if (!d || len < 6 || len > 0x7FFFFFFF || le16(d) != 0 || (le16(d + 2) != 1 && le16(d + 2) != 2))
        return NULL;
    int n = le16(d + 4);
    if (n == 0 || 6 + (size_t)n * 16 > len) return NULL;

    GdiIcon *ic = kzalloc(sizeof(*ic));
    if (!ic) return NULL;
    ic->data = kmalloc(len);
    if (!ic->data) { kfree(ic); return NULL; }
    memcpy(ic->data, d, len);
    ic->len = (UINT32)len;

    for (int i = 0; i < n && ic->n < ICON_MAX_IMAGES; i++) {
        const UINT8 *e = d + 6 + i * 16;
        UINT32 sz = le32(e + 8), off = le32(e + 12);
        if (off > len || sz > len - off || sz < 16) continue;
        IconImage *im = &ic->img[ic->n];
        im->w = e[0] ? e[0] : 256;
        im->h = e[1] ? e[1] : 256;
        im->off = off;
        im->len = sz;
        im->png = PngIsPng(d + off, sz);
        int bpp = le16(e + 6);
        if (im->png) {
            bpp = 32;
        } else if (!bpp && sz >= 16) {
            bpp = le16(d + off + 14);             /* from the DIB header */
            if (!bpp) bpp = e[2] ? (e[2] <= 2 ? 1 : e[2] <= 16 ? 4 : 8) : 8;
        }
        im->bpp = bpp;
        ic->n++;
    }
    if (!ic->n) { IconFree(ic); return NULL; }
    return ic;
}

void IconFree(GdiIcon *ic)
{
    if (!ic) return;
    for (int i = 0; i < ic->n; i++) kfree(ic->img[i].px);
    kfree(ic->data);
    kfree(ic);
}

/* Decode a DIB image: BITMAPINFOHEADER, palette, XOR bitmap, AND mask */
static bool decode_dib(IconImage *im, const UINT8 *d, UINT32 len)
{
    if (len < 40) return false;
    UINT32 hs = le32(d);
    int w = (int)le32(d + 4), h2 = (int)le32(d + 8);
    int bpp = le16(d + 14);
    UINT32 comp = le32(d + 16), used = le32(d + 32);
    if (hs < 40 || hs > len || w <= 0 || w > 1024 || h2 <= 0 || h2 > 2048) return false;
    if (comp != 0 && !(comp == 3 && (bpp == 16 || bpp == 32))) return false;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return false;
    int h = h2 / 2;                               /* the height counts both masks */
    if (h <= 0) return false;

    UINT32 pal_off = hs + (comp == 3 ? 12 : 0);   /* BI_BITFIELDS masks follow the header */
    int ncol = bpp <= 8 ? (used && used <= (1u << bpp) ? (int)used : 1 << bpp) : 0;
    UINT32 xor_off = pal_off + (UINT32)ncol * 4;
    UINT32 xstride = (UINT32)((w * bpp + 31) / 32) * 4, astride = (UINT32)((w + 31) / 32) * 4;
    UINT32 and_off = xor_off + xstride * (UINT32)h;
    if (xor_off > len || and_off > len) return false;
    bool have_and = and_off + astride * (UINT32)h <= len;

    UINT32 *px = kmalloc((size_t)w * h * 4);
    if (!px) return false;
    bool any_alpha = false;
    for (int y = 0; y < h; y++) {
        const UINT8 *row = d + xor_off + (UINT32)(h - 1 - y) * xstride;   /* bottom-up */
        UINT32 *o = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            UINT32 b, g, r, a = 255;
            if (bpp <= 8) {
                int bit = x * bpp;
                int v = (row[bit >> 3] >> (8 - bpp - (bit & 7))) & ((1 << bpp) - 1);
                if (v >= ncol) v = 0;
                const UINT8 *c = d + pal_off + v * 4;
                b = c[0]; g = c[1]; r = c[2];
            } else if (bpp == 16) {                /* 5-5-5 */
                UINT32 v = le16(row + 2 * x);
                r = (v >> 10 & 31) * 255 / 31; g = (v >> 5 & 31) * 255 / 31; b = (v & 31) * 255 / 31;
            } else if (bpp == 24) {
                b = row[3 * x]; g = row[3 * x + 1]; r = row[3 * x + 2];
            } else {
                b = row[4 * x]; g = row[4 * x + 1]; r = row[4 * x + 2]; a = row[4 * x + 3];
                if (a) any_alpha = true;
            }
            o[x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    /* Without an alpha channel (or an all-zero one, as old 32-bit icons
     * have), transparency comes from the AND mask. */
    if (bpp < 32 || !any_alpha) {
        for (int y = 0; y < h; y++) {
            const UINT8 *m = have_and ? d + and_off + (UINT32)(h - 1 - y) * astride : NULL;
            UINT32 *o = px + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                bool clear = m && (m[x >> 3] >> (7 - (x & 7)) & 1);
                o[x] = (o[x] & 0xFFFFFF) | (clear ? 0 : 0xFF000000u);
            }
        }
    }
    im->px = px;
    im->pw = w;
    im->ph = h;
    return true;
}

static bool ensure(GdiIcon *ic, IconImage *im)
{
    if (im->px) return true;
    if (im->bad) return false;
    const UINT8 *d = ic->data + im->off;
    bool ok = im->png ? PngDecode(d, im->len, &im->px, &im->pw, &im->ph)
                      : decode_dib(im, d, im->len);
    if (!ok) im->bad = true;
    return ok;
}

bool IconDecode(GdiIcon *ic, int i)
{
    return ic && i >= 0 && i < ic->n && ensure(ic, &ic->img[i]);
}

/* Best image for a target size in device pixels: the smallest that is at
 * least that big (downscaling looks better than upscaling), deepest colour
 * first; else the largest. */
static IconImage *pick(GdiIcon *ic, int target)
{
    for (int tries = 0; tries < ic->n; tries++) {
        IconImage *best = NULL;
        for (int i = 0; i < ic->n; i++) {
            IconImage *im = &ic->img[i];
            if (im->bad) continue;
            if (!best) { best = im; continue; }
            bool big = im->w >= target, bbig = best->w >= target;
            if (big != bbig) { if (big) best = im; continue; }
            if (im->w != best->w) {
                if (big ? im->w < best->w : im->w > best->w) best = im;
                continue;
            }
            if (im->bpp > best->bpp) best = im;
        }
        if (!best) return NULL;
        if (ensure(ic, best)) return best;        /* else it is now marked bad */
    }
    return NULL;
}

bool IconDraw(GdiIcon *ic, int x, int y, int size)
{
    if (!ic || size <= 0) return false;
    IconImage *im = pick(ic, size * GdiScale());
    if (!im) return false;
    /* keep the aspect ratio (icons are square; pictures need not be) */
    int w = size, h = size;
    if (im->pw > im->ph) h = size * im->ph / im->pw;
    else if (im->ph > im->pw) w = size * im->pw / im->ph;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    GdiDrawImage(RECT(x + (size - w) / 2, y + (size - h) / 2, w, h), im->px, im->pw, im->ph);
    return true;
}

bool IconDrawFit(GdiIcon *ic, int i, GdiRect box)
{
    if (!ic || i < 0 || i >= ic->n || !ensure(ic, &ic->img[i])) return false;
    IconImage *im = &ic->img[i];
    int sc = GdiScale();
    /* logical size at one image pixel per device pixel, then shrink to fit */
    int w = (im->pw + sc - 1) / sc, h = (im->ph + sc - 1) / sc;
    if (w > box.w) { h = h * box.w / w; w = box.w; }
    if (h > box.h) { w = w * box.h / h; h = box.h; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    GdiRect r = RECT(box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h);
    if (w * sc == im->pw && h * sc == im->ph) GdiDrawImageDevice(r.x, r.y, im->px, im->pw, im->ph);
    else                                      GdiDrawImage(r, im->px, im->pw, im->ph);
    return true;
}

GdiIcon *IconFromPng(const void *png, size_t len)
{
    const UINT8 *p = png;
    if (!PngIsPng(png, len) || len < 24 || len > 0x7FFFFFF0) return NULL;
    UINT32 w = (UINT32)p[16] << 24 | p[17] << 16 | p[18] << 8 | p[19];
    UINT32 h = (UINT32)p[20] << 24 | p[21] << 16 | p[22] << 8 | p[23];
    UINT8 *ico = kmalloc(22 + len);
    if (!ico) return NULL;
    memset(ico, 0, 22);
    ico[2] = 1; ico[4] = 1;
    ico[6] = w < 256 ? (UINT8)w : 0;
    ico[7] = h < 256 ? (UINT8)h : 0;
    ico[10] = 1; ico[12] = 32;
    UINT32 n = (UINT32)len;
    ico[14] = (UINT8)n; ico[15] = (UINT8)(n >> 8); ico[16] = (UINT8)(n >> 16); ico[17] = (UINT8)(n >> 24);
    ico[18] = 22;
    memcpy(ico + 22, png, len);
    GdiIcon *ic = IconLoad(ico, 22 + len);
    kfree(ico);
    return ic;
}

bool IconDrawImage(GdiIcon *ic, int i, int x, int y)
{
    if (!ic || i < 0 || i >= ic->n || !ensure(ic, &ic->img[i])) return false;
    IconImage *im = &ic->img[i];
    GdiDrawImageDevice(x, y, im->px, im->pw, im->ph);
    return true;
}

/* -----------------------------------------------------------------------
 * PE resources: RT_GROUP_ICON (14) lists the images, each an RT_ICON (3)
 * ----------------------------------------------------------------------- */
typedef struct {
    const UINT8 *d;
    size_t len;
    UINT32 rsrc_off;          /* file offset of the resource section data */
    UINT32 rsrc_rva, rsrc_size;
    const UINT8 *sec;         /* section table */
    int nsec;
} Pe;

/* File offset of @rva, or 0 if it is not in a section's raw data */
static UINT32 rva_to_off(const Pe *pe, UINT32 rva, UINT32 size)
{
    for (int i = 0; i < pe->nsec; i++) {
        const UINT8 *s = pe->sec + i * 40;
        UINT32 va = le32(s + 12), vsz = le32(s + 8), raw = le32(s + 20), rsz = le32(s + 16);
        UINT32 span = vsz > rsz ? rsz : vsz;
        if (rva >= va && rva - va < span && size <= span - (rva - va)) {
            UINT32 off = raw + (rva - va);
            if (off < pe->len && size <= pe->len - off) return off;
        }
    }
    return 0;
}

/* Resource directory at @dir (offset within .rsrc): entry with integer id
 * @id, or the first entry if id < 0.  Returns the entry's target offset
 * with the high "subdirectory" bit, or 0. */
static UINT32 res_find(const Pe *pe, UINT32 dir, int id)
{
    if (dir + 16 > pe->rsrc_size) return 0;
    const UINT8 *p = pe->d + pe->rsrc_off + dir;
    int named = le16(p + 12), ids = le16(p + 14), total = named + ids;
    if (dir + 16 + (UINT32)total * 8 > pe->rsrc_size) return 0;
    for (int i = 0; i < total; i++) {
        const UINT8 *e = p + 16 + i * 8;
        UINT32 name = le32(e);
        if (id < 0 || (!(name & 0x80000000u) && (int)(name & 0xFFFF) == id))
            return le32(e + 4) ? le32(e + 4) : 0;
    }
    return 0;
}

/* Data of the resource type/id (first language); NULL if absent */
static const UINT8 *res_data(const Pe *pe, int type, int id, UINT32 *size)
{
    UINT32 t = res_find(pe, 0, type);
    if (!(t & 0x80000000u)) return NULL;
    UINT32 n = res_find(pe, t & 0x7FFFFFFF, id);
    if (!(n & 0x80000000u)) return NULL;
    UINT32 l = res_find(pe, n & 0x7FFFFFFF, -1);  /* any language */
    if (!l || (l & 0x80000000u) || l + 16 > pe->rsrc_size) return NULL;
    const UINT8 *de = pe->d + pe->rsrc_off + l;
    UINT32 rva = le32(de), sz = le32(de + 4);
    UINT32 off = rva_to_off(pe, rva, sz);
    if (!off) return NULL;
    *size = sz;
    return pe->d + off;
}

GdiIcon *IconFromPe(const void *image, size_t len)
{
    Pe pe = { .d = image, .len = len };
    const UINT8 *d = image;
    if (!d || len < 64 || d[0] != 'M' || d[1] != 'Z') return NULL;
    UINT32 nt = le32(d + 60);
    if (nt > len || len - nt < 24 || memcmp(d + nt, "PE\0\0", 4)) return NULL;
    int nsec = le16(d + nt + 6), optsz = le16(d + nt + 20);
    const UINT8 *opt = d + nt + 24;
    if ((size_t)(opt - d) + optsz > len) return NULL;
    int magic = le16(opt);
    UINT32 dd = magic == 0x20B ? 112 : magic == 0x10B ? 96 : 0;
    if (!dd || (UINT32)optsz < dd + 3 * 8) return NULL;
    pe.rsrc_rva = le32(opt + dd + 2 * 8);         /* data directory 2: resources */
    pe.rsrc_size = le32(opt + dd + 2 * 8 + 4);
    pe.sec = opt + optsz;
    pe.nsec = nsec;
    if ((size_t)(pe.sec - d) + (size_t)nsec * 40 > len || !pe.rsrc_rva || pe.rsrc_size < 16) return NULL;
    pe.rsrc_off = rva_to_off(&pe, pe.rsrc_rva, pe.rsrc_size);
    if (!pe.rsrc_off) return NULL;

    /* The first icon group */
    UINT32 t = res_find(&pe, 0, 14);
    if (!(t & 0x80000000u)) return NULL;
    UINT32 g = res_find(&pe, t & 0x7FFFFFFF, -1);
    if (!(g & 0x80000000u)) return NULL;
    UINT32 l = res_find(&pe, g & 0x7FFFFFFF, -1);
    if (!l || (l & 0x80000000u) || l + 16 > pe.rsrc_size) return NULL;
    const UINT8 *de = pe.d + pe.rsrc_off + l;
    UINT32 gsz = le32(de + 4), goff = rva_to_off(&pe, le32(de), gsz);
    if (!goff || gsz < 6) return NULL;
    const UINT8 *grp = d + goff;
    int n = le16(grp + 4);
    if (n <= 0 || 6 + (UINT32)n * 14 > gsz) return NULL;
    if (n > ICON_MAX_IMAGES) n = ICON_MAX_IMAGES;

    /* Rebuild it as an .ico: directory, then each RT_ICON's data */
    const UINT8 *parts[ICON_MAX_IMAGES];
    UINT32 sizes[ICON_MAX_IMAGES], total = 6 + (UINT32)n * 16;
    for (int i = 0; i < n; i++) {
        parts[i] = res_data(&pe, 3, le16(grp + 6 + i * 14 + 12), &sizes[i]);
        if (!parts[i]) sizes[i] = 0;
        total += sizes[i];
    }
    UINT8 *ico = kzalloc(total);
    if (!ico) return NULL;
    ico[2] = 1;
    ico[4] = (UINT8)n; ico[5] = (UINT8)(n >> 8);
    UINT32 off = 6 + (UINT32)n * 16;
    for (int i = 0; i < n; i++) {
        UINT8 *e = ico + 6 + i * 16;
        memcpy(e, grp + 6 + i * 14, 8);           /* w, h, colours, 0, planes, bpp */
        e[8] = (UINT8)sizes[i]; e[9] = (UINT8)(sizes[i] >> 8);
        e[10] = (UINT8)(sizes[i] >> 16); e[11] = (UINT8)(sizes[i] >> 24);
        e[12] = (UINT8)off; e[13] = (UINT8)(off >> 8); e[14] = (UINT8)(off >> 16); e[15] = (UINT8)(off >> 24);
        if (sizes[i]) memcpy(ico + off, parts[i], sizes[i]);
        off += sizes[i];
    }
    GdiIcon *ic = IconLoad(ico, total);           /* skips the missing images */
    kfree(ico);
    return ic;
}
