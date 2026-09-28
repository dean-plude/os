/*
 * png.c — PNG decoder with a built-in inflate (see png.h)
 *
 * The inflater follows the structure of Mark Adler's "puff" (canonical
 * Huffman decoding one bit at a time): small and easy to check, and fast
 * enough for icon-sized images.
 */

#include "png.h"
#include "../lib/string.h"
#include "../mm/vmm.h"

/* -----------------------------------------------------------------------
 * Inflate
 * ----------------------------------------------------------------------- */
#define MAXBITS  15
#define MAXLCODES 286
#define MAXDCODES 30

typedef struct { UINT16 count[MAXBITS + 1]; UINT16 sym[288]; } Huff;

typedef struct {
    const UINT8 *in, *end;
    UINT32 bits;
    int    nbits;
    bool   err;
    UINT8 *out;
    size_t olen, ocap;
    Huff   lit, dist, cl;
} Inflate;

static UINT32 getbits(Inflate *s, int n)
{
    while (s->nbits < n) {
        if (s->in >= s->end) { s->err = true; return 0; }
        s->bits |= (UINT32)*s->in++ << s->nbits;
        s->nbits += 8;
    }
    UINT32 v = s->bits & ((1u << n) - 1);
    s->bits >>= n;
    s->nbits -= n;
    return v;
}

/* Build canonical Huffman tables from code lengths; false if the lengths
 * are over-subscribed (an incomplete code is allowed, as in zlib). */
static bool build(Huff *h, const UINT8 *len, int n)
{
    UINT16 offs[MAXBITS + 1];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return true;          /* no codes: ok, fails on use */
    int left = 1;
    for (int l = 1; l <= MAXBITS; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return false;
    }
    offs[1] = 0;
    for (int l = 1; l < MAXBITS; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (UINT16)i;
    return true;
}

static int decode(Inflate *s, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= MAXBITS; l++) {
        code |= (int)getbits(s, 1);
        if (s->err) return -1;
        int count = h->count[l];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const UINT16 lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const UINT8  lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const UINT16 dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                  8193, 12289, 16385, 24577 };
static const UINT8  dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool put(Inflate *s, UINT8 b)
{
    if (s->olen >= s->ocap) return false;
    s->out[s->olen++] = b;
    return true;
}

static bool codes(Inflate *s)
{
    for (;;) {
        int sym = decode(s, &s->lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (!put(s, (UINT8)sym)) return false;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            UINT32 len = lbase[sym] + getbits(s, lext[sym]);
            int ds = decode(s, &s->dist);
            if (ds < 0 || ds >= 30) return false;
            UINT32 dist = dbase[ds] + getbits(s, dext[ds]);
            if (s->err || dist > s->olen || s->olen + len > s->ocap) return false;
            for (UINT32 i = 0; i < len; i++, s->olen++)
                s->out[s->olen] = s->out[s->olen - dist];
        }
    }
}

static bool stored(Inflate *s)
{
    s->bits = 0;                                  /* to a byte boundary */
    s->nbits = 0;
    if (s->end - s->in < 4) return false;
    UINT32 len  = s->in[0] | (s->in[1] << 8);
    UINT32 nlen = s->in[2] | (s->in[3] << 8);
    s->in += 4;
    if (len != (~nlen & 0xFFFF) || (size_t)(s->end - s->in) < len) return false;
    if (s->olen + len > s->ocap) return false;
    memcpy(s->out + s->olen, s->in, len);
    s->olen += len;
    s->in += len;
    return true;
}

static bool fixed(Inflate *s)
{
    UINT8 len[288];
    int i = 0;
    for (; i < 144; i++) len[i] = 8;
    for (; i < 256; i++) len[i] = 9;
    for (; i < 280; i++) len[i] = 7;
    for (; i < 288; i++) len[i] = 8;
    build(&s->lit, len, 288);
    for (i = 0; i < 30; i++) len[i] = 5;
    build(&s->dist, len, 30);
    return codes(s);
}

static bool dynamic(Inflate *s)
{
    static const UINT8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    UINT8 len[MAXLCODES + MAXDCODES];
    int nlen = (int)getbits(s, 5) + 257, ndist = (int)getbits(s, 5) + 1, ncode = (int)getbits(s, 4) + 4;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) return false;
    int i = 0;
    for (; i < ncode; i++) len[order[i]] = (UINT8)getbits(s, 3);
    for (; i < 19; i++) len[order[i]] = 0;
    if (s->err || !build(&s->cl, len, 19)) return false;

    for (i = 0; i < nlen + ndist;) {
        int sym = decode(s, &s->cl);
        if (sym < 0) return false;
        if (sym < 16) { len[i++] = (UINT8)sym; continue; }
        UINT8 v = 0;
        int rep;
        if (sym == 16) {
            if (i == 0) return false;
            v = len[i - 1];
            rep = 3 + (int)getbits(s, 2);
        } else if (sym == 17) {
            rep = 3 + (int)getbits(s, 3);
        } else {
            rep = 11 + (int)getbits(s, 7);
        }
        if (s->err || i + rep > nlen + ndist) return false;
        while (rep--) len[i++] = v;
    }
    if (len[256] == 0) return false;              /* no end-of-block code */
    if (!build(&s->lit, len, nlen) || !build(&s->dist, len + nlen, ndist)) return false;
    return codes(s);
}

long ZlibInflate(const void *src, size_t len, void *dst, size_t cap)
{
    const UINT8 *p = src;
    if (len < 2 || (p[0] & 0x0F) != 8 || ((p[0] << 8) | p[1]) % 31 || (p[1] & 0x20))
        return -1;                                /* not deflate / preset dictionary */
    Inflate *s = kzalloc(sizeof(*s));
    if (!s) return -1;
    s->in = p + 2;
    s->end = p + len;
    s->out = dst;
    s->ocap = cap;
    bool ok = true;
    int last;
    do {
        last = (int)getbits(s, 1);
        int type = (int)getbits(s, 2);
        if (s->err) { ok = false; break; }
        if (type == 0)      ok = stored(s);
        else if (type == 1) ok = fixed(s);
        else if (type == 2) ok = dynamic(s);
        else                ok = false;
    } while (ok && !last);
    long n = ok && !s->err ? (long)s->olen : -1;
    kfree(s);
    return n;
}

/* -----------------------------------------------------------------------
 * PNG
 * ----------------------------------------------------------------------- */
static const UINT8 SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

static UINT32 be32(const UINT8 *p) { return (UINT32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool PngIsPng(const void *data, size_t len)
{
    return len >= 8 && memcmp(data, SIG, 8) == 0;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

/* Undo the per-row filters of one (sub)image in place: rows of 1 + rb bytes */
static bool unfilter(UINT8 *d, int rows, size_t rb, int bpp)
{
    UINT8 *prev = NULL;
    for (int y = 0; y < rows; y++) {
        UINT8 f = d[0], *r = d + 1;
        for (size_t i = 0; i < rb; i++) {
            int a = i >= (size_t)bpp ? r[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
            switch (f) {
            case 0: break;
            case 1: r[i] = (UINT8)(r[i] + a); break;
            case 2: r[i] = (UINT8)(r[i] + b); break;
            case 3: r[i] = (UINT8)(r[i] + ((a + b) >> 1)); break;
            case 4: r[i] = (UINT8)(r[i] + paeth(a, b, c)); break;
            default: return false;
            }
        }
        prev = r;
        d += 1 + rb;
    }
    return true;
}

typedef struct {
    int w, h, depth, ctype, chans;
    UINT8 pal[256][4];
    int npal;
    bool trns;
    UINT16 tr, tg, tb;                            /* tRNS key colour (gray: tr) */
} PngInfo;

/* Sample @i of a row at bit depth @d (raw value) */
static UINT32 sample(const UINT8 *row, size_t i, int d)
{
    if (d == 8)  return row[i];
    if (d == 16) return (UINT32)row[2 * i] << 8 | row[2 * i + 1];
    size_t bit = i * d;
    return (row[bit >> 3] >> (8 - d - (bit & 7))) & ((1u << d) - 1);
}

static UINT8 to8(UINT32 v, int d)
{
    if (d == 8)  return (UINT8)v;
    if (d == 16) return (UINT8)(v >> 8);
    return (UINT8)(v * 255 / ((1u << d) - 1));
}

static UINT32 pixel(const PngInfo *pi, const UINT8 *row, int x)
{
    int d = pi->depth;
    size_t s = (size_t)x * pi->chans;
    UINT32 r, g, b, a = 255;
    switch (pi->ctype) {
    case 0: {
        UINT32 v = sample(row, s, d);
        r = g = b = to8(v, d);
        if (pi->trns && v == pi->tr) a = 0;
        break; }
    case 2: {
        UINT32 vr = sample(row, s, d), vg = sample(row, s + 1, d), vb = sample(row, s + 2, d);
        r = to8(vr, d); g = to8(vg, d); b = to8(vb, d);
        if (pi->trns && vr == pi->tr && vg == pi->tg && vb == pi->tb) a = 0;
        break; }
    case 3: {
        UINT32 v = sample(row, s, d);
        if ((int)v >= pi->npal) return 0;
        r = pi->pal[v][0]; g = pi->pal[v][1]; b = pi->pal[v][2]; a = pi->pal[v][3];
        break; }
    case 4:
        r = g = b = to8(sample(row, s, d), d);
        a = to8(sample(row, s + 1, d), d);
        break;
    default:                                      /* 6: RGBA */
        r = to8(sample(row, s, d), d);     g = to8(sample(row, s + 1, d), d);
        b = to8(sample(row, s + 2, d), d); a = to8(sample(row, s + 3, d), d);
        break;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

bool PngDecode(const void *data, size_t len, UINT32 **out, int *w, int *h)
{
    const UINT8 *p = data, *end = p + len;
    if (!PngIsPng(data, len)) return false;
    p += 8;

    PngInfo pi;
    memset(&pi, 0, sizeof(pi));
    int interlace = 0;
    bool have_hdr = false;
    UINT8 *z = NULL;                              /* concatenated IDAT */
    size_t zlen = 0, zcap = 0;
    bool ok = false;

    while (end - p >= 12) {
        UINT32 clen = be32(p);
        const UINT8 *type = p + 4, *cd = p + 8;
        if (clen > (size_t)(end - cd) - 4) break;     /* truncated */
        if (!memcmp(type, "IHDR", 4) && clen >= 13) {
            pi.w = (int)be32(cd); pi.h = (int)be32(cd + 4);
            pi.depth = cd[8]; pi.ctype = cd[9]; interlace = cd[12];
            static const int ch[7] = { 1, 0, 3, 1, 2, 0, 4 };
            if (pi.w <= 0 || pi.h <= 0 || pi.w > PNG_MAX_DIM || pi.h > PNG_MAX_DIM ||
                (INT64)pi.w * pi.h > PNG_MAX_PIXELS ||
                pi.ctype > 6 || !ch[pi.ctype] || cd[10] || cd[11] || interlace > 1)
                goto done;
            pi.chans = ch[pi.ctype];
            int dp = pi.depth;
            bool dok = pi.ctype == 3 ? (dp == 1 || dp == 2 || dp == 4 || dp == 8)
                     : pi.ctype == 0 ? (dp == 1 || dp == 2 || dp == 4 || dp == 8 || dp == 16)
                     : (dp == 8 || dp == 16);
            if (!dok) goto done;
            have_hdr = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            pi.npal = (int)(clen / 3 > 256 ? 256 : clen / 3);
            for (int i = 0; i < pi.npal; i++) {
                pi.pal[i][0] = cd[3 * i]; pi.pal[i][1] = cd[3 * i + 1];
                pi.pal[i][2] = cd[3 * i + 2]; pi.pal[i][3] = 255;
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            if (pi.ctype == 3) {
                for (UINT32 i = 0; i < clen && i < 256; i++) pi.pal[i][3] = cd[i];
            } else if (pi.ctype == 0 && clen >= 2) {
                pi.trns = true; pi.tr = (UINT16)(cd[0] << 8 | cd[1]);
            } else if (pi.ctype == 2 && clen >= 6) {
                pi.trns = true;
                pi.tr = (UINT16)(cd[0] << 8 | cd[1]); pi.tg = (UINT16)(cd[2] << 8 | cd[3]);
                pi.tb = (UINT16)(cd[4] << 8 | cd[5]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (zlen + clen > zcap) {
                size_t nc = (zlen + clen) * 2;
                UINT8 *nz = kmalloc(nc);
                if (!nz) goto done;
                if (z) { memcpy(nz, z, zlen); kfree(z); }
                z = nz; zcap = nc;
            }
            memcpy(z + zlen, cd, clen);
            zlen += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        p = cd + clen + 4;                        /* skip the CRC */
    }
    if (!have_hdr || !z || (pi.ctype == 3 && !pi.npal)) goto done;

    int bits = pi.chans * pi.depth, bpp = bits >= 8 ? bits / 8 : 1;
    static const int ax[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = interlace ? 7 : 1;

    size_t raw_len = 0;                           /* filtered data size, all passes */
    for (int ps = 0; ps < passes; ps++) {
        int pw = interlace ? (pi.w - ax[ps] + dx[ps] - 1) / dx[ps] : pi.w;
        int ph = interlace ? (pi.h - ay[ps] + dy[ps] - 1) / dy[ps] : pi.h;
        if (pw > 0 && ph > 0) raw_len += (size_t)ph * (1 + ((size_t)pw * bits + 7) / 8);
    }
    UINT8 *raw = kmalloc(raw_len);
    UINT32 *img = kzalloc((size_t)pi.w * pi.h * 4);
    if (!raw || !img || ZlibInflate(z, zlen, raw, raw_len) != (long)raw_len) {
        kfree(raw); kfree(img);
        goto done;
    }

    UINT8 *d = raw;
    ok = true;
    for (int ps = 0; ps < passes && ok; ps++) {
        int pw = interlace ? (pi.w - ax[ps] + dx[ps] - 1) / dx[ps] : pi.w;
        int ph = interlace ? (pi.h - ay[ps] + dy[ps] - 1) / dy[ps] : pi.h;
        if (pw <= 0 || ph <= 0) continue;
        size_t rb = ((size_t)pw * bits + 7) / 8;
        if (!unfilter(d, ph, rb, bpp)) { ok = false; break; }
        for (int y = 0; y < ph; y++) {
            const UINT8 *row = d + (size_t)y * (1 + rb) + 1;
            int oy = interlace ? ay[ps] + y * dy[ps] : y;
            for (int x = 0; x < pw; x++) {
                int ox = interlace ? ax[ps] + x * dx[ps] : x;
                img[(size_t)oy * pi.w + ox] = pixel(&pi, row, x);
            }
        }
        d += (size_t)ph * (1 + rb);
    }
    kfree(raw);
    if (!ok) { kfree(img); goto done; }
    *out = img;
    *w = pi.w;
    *h = pi.h;

done:
    kfree(z);
    return ok;
}
