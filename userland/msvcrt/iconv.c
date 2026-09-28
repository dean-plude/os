/* msvcrt: iconv for the character sets the web mostly uses: UTF-8,
 * UTF-16 and UTF-32 (either byte order), ISO-8859-1, Windows-1252 and
 * US-ASCII.  "//TRANSLIT" and "//IGNORE" suffixes are accepted; with
 * either, characters the target can't hold become '?'. */
#define NOVA_BUILD_MSVCRT
#include <iconv.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

enum { CS_UTF8, CS_UTF16LE, CS_UTF16BE, CS_UTF16, CS_UTF32LE, CS_UTF32BE, CS_UTF32, CS_LATIN1, CS_CP1252, CS_ASCII };

typedef struct {
    int from, to;
    int lossy;          /* //TRANSLIT or //IGNORE */
    int bom_done;       /* UTF-16/32 output: BOM written; input: BOM checked */
    int in_be;          /* UTF-16/32 input byte order (after the BOM) */
} Conv;

static const unsigned short cp1252_hi[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

static int charset(const char *name, int *lossy)
{
    char n[32];
    int k = 0;
    for (const char *p = name; *p && k < 31; p++) {
        if (*p == '/') {
            if (!strncmp(p, "//TRANSLIT", 10) || !strncmp(p, "//IGNORE", 8)) *lossy = 1;
            break;
        }
        if (*p != '-' && *p != '_') n[k++] = (char)toupper((unsigned char)*p);
    }
    n[k] = 0;
    static const struct { const char *n; int cs; } names[] = {
        {"UTF8", CS_UTF8}, {"UTF16LE", CS_UTF16LE}, {"UTF16BE", CS_UTF16BE}, {"UTF16", CS_UTF16},
        {"UCS2", CS_UTF16}, {"UCS2LE", CS_UTF16LE}, {"UCS2BE", CS_UTF16BE},
        {"UTF32LE", CS_UTF32LE}, {"UTF32BE", CS_UTF32BE}, {"UTF32", CS_UTF32},
        {"UCS4", CS_UTF32BE}, {"UCS4LE", CS_UTF32LE}, {"UCS4BE", CS_UTF32BE}, {"UCS4INTERNAL", CS_UTF32LE},
        {"WCHART", CS_UTF16LE},
        {"ISO88591", CS_LATIN1}, {"LATIN1", CS_LATIN1}, {"L1", CS_LATIN1}, {"ISOIR100", CS_LATIN1},
        {"CP819", CS_LATIN1}, {"IBM819", CS_LATIN1}, {"ISO885915", CS_CP1252},
        {"WINDOWS1252", CS_CP1252}, {"CP1252", CS_CP1252},
        {"USASCII", CS_ASCII}, {"ASCII", CS_ASCII}, {"ANSIX3.41968", CS_ASCII}, {"646", CS_ASCII},
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(n, names[i].n)) return names[i].cs;
    return -1;
}

iconv_t iconv_open(const char *to, const char *from)
{
    int lossy = 0;
    int t = charset(to, &lossy), f = charset(from, &lossy);
    if (t < 0 || f < 0) { errno = EINVAL; return (iconv_t)-1; }
    Conv *c = calloc(1, sizeof(*c));
    if (!c) { errno = ENOMEM; return (iconv_t)-1; }
    c->from = f;
    c->to = t;
    c->lossy = lossy;
    c->in_be = f == CS_UTF16BE || f == CS_UTF32BE || f == CS_UTF16 || f == CS_UTF32;
    return c;
}

int iconv_close(iconv_t cd)
{
    if (cd == (iconv_t)-1 || !cd) { errno = EBADF; return -1; }
    free(cd);
    return 0;
}

/* Decode one character: bytes used (>0), 0 if the input is incomplete,
 * -1 if it is invalid. */
static int decode(Conv *c, const unsigned char *p, size_t n, unsigned *cp)
{
    switch (c->from) {
    case CS_UTF8: {
        unsigned b = p[0];
        if (b < 0x80) { *cp = b; return 1; }
        int len = b >= 0xF0 && b < 0xF5 ? 4 : b >= 0xE0 ? 3 : b >= 0xC2 && b < 0xE0 ? 2 : 0;
        if (!len || b >= 0xF5) return -1;
        if (n < (size_t)len) {
            for (size_t i = 1; i < n; i++) if ((p[i] & 0xC0) != 0x80) return -1;
            return 0;
        }
        unsigned v = b & (0x7F >> len);
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) return -1;
            v = (v << 6) | (p[i] & 0x3F);
        }
        if ((len == 3 && v < 0x800) || (len == 4 && (v < 0x10000 || v > 0x10FFFF)) ||
            (v >= 0xD800 && v < 0xE000)) return -1;
        *cp = v;
        return len;
    }
    case CS_UTF16LE: case CS_UTF16BE: case CS_UTF16: {
        if (n < 2) return 0;
        int be = c->from == CS_UTF16LE ? 0 : c->in_be;
        unsigned u = be ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]);
        if (u >= 0xDC00 && u < 0xE000) return -1;
        if (u >= 0xD800 && u < 0xDC00) {
            if (n < 4) return 0;
            unsigned l = be ? (p[2] << 8 | p[3]) : (p[3] << 8 | p[2]);
            if (l < 0xDC00 || l >= 0xE000) return -1;
            *cp = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
            return 4;
        }
        *cp = u;
        return 2;
    }
    case CS_UTF32LE: case CS_UTF32BE: case CS_UTF32: {
        if (n < 4) return 0;
        int be = c->from == CS_UTF32LE ? 0 : c->in_be;
        unsigned v = be ? ((unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3])
                        : ((unsigned)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]);
        if (v > 0x10FFFF || (v >= 0xD800 && v < 0xE000)) return -1;
        *cp = v;
        return 4;
    }
    case CS_LATIN1:
        *cp = p[0];
        return 1;
    case CS_CP1252:
        *cp = p[0] >= 0x80 && p[0] < 0xA0 ? cp1252_hi[p[0] - 0x80] : p[0];
        return 1;
    default:   /* ASCII */
        if (p[0] >= 0x80) return -1;
        *cp = p[0];
        return 1;
    }
}

/* Encode one character: bytes written (>0), 0 if it doesn't fit, -1 if
 * the target charset can't represent it. */
static int encode(Conv *c, unsigned cp, unsigned char *o, size_t n)
{
    switch (c->to) {
    case CS_UTF8:
        if (cp < 0x80) { if (n < 1) return 0; o[0] = (unsigned char)cp; return 1; }
        if (cp < 0x800) { if (n < 2) return 0; o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 0x3F); return 2; }
        if (cp < 0x10000) {
            if (n < 3) return 0;
            o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | ((cp >> 6) & 0x3F); o[2] = 0x80 | (cp & 0x3F);
            return 3;
        }
        if (n < 4) return 0;
        o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | ((cp >> 12) & 0x3F);
        o[2] = 0x80 | ((cp >> 6) & 0x3F); o[3] = 0x80 | (cp & 0x3F);
        return 4;
    case CS_UTF16LE: case CS_UTF16BE: case CS_UTF16: {
        int be = c->to != CS_UTF16LE;
        unsigned u[2], k = 1;
        if (cp >= 0x10000) { u[0] = 0xD800 + ((cp - 0x10000) >> 10); u[1] = 0xDC00 + ((cp - 0x10000) & 0x3FF); k = 2; }
        else u[0] = cp;
        if (n < 2 * k) return 0;
        for (unsigned i = 0; i < k; i++) {
            o[2 * i + (be ? 0 : 1)] = (unsigned char)(u[i] >> 8);
            o[2 * i + (be ? 1 : 0)] = (unsigned char)u[i];
        }
        return 2 * k;
    }
    case CS_UTF32LE: case CS_UTF32BE: case CS_UTF32: {
        if (n < 4) return 0;
        int be = c->to != CS_UTF32LE;
        for (int i = 0; i < 4; i++) o[be ? i : 3 - i] = (unsigned char)(cp >> (24 - 8 * i));
        return 4;
    }
    case CS_LATIN1:
        if (cp > 0xFF) return -1;
        if (n < 1) return 0;
        o[0] = (unsigned char)cp;
        return 1;
    case CS_CP1252:
        if (cp >= 0x80 && cp < 0xA0) return -1;
        if (cp > 0xFF) {
            for (int i = 0; i < 32; i++) if (cp1252_hi[i] == cp && cp != 0xFFFD) {
                if (n < 1) return 0;
                o[0] = (unsigned char)(0x80 + i);
                return 1;
            }
            return -1;
        }
        if (n < 1) return 0;
        o[0] = (unsigned char)cp;
        return 1;
    default:
        if (cp >= 0x80) return -1;
        if (n < 1) return 0;
        o[0] = (unsigned char)cp;
        return 1;
    }
}

size_t iconv(iconv_t cd, char **in, size_t *inleft, char **out, size_t *outleft)
{
    Conv *c = cd;
    if (cd == (iconv_t)-1 || !cd) { errno = EBADF; return (size_t)-1; }
    if (!in || !*in) {                 /* reset the shift state */
        c->bom_done = 0;
        c->in_be = c->from == CS_UTF16BE || c->from == CS_UTF32BE || c->from == CS_UTF16 || c->from == CS_UTF32;
        return 0;
    }
    size_t lossy = 0;
    const unsigned char *p = (const unsigned char *)*in;
    unsigned char *o = (unsigned char *)*out;
    size_t n = *inleft, room = *outleft;
    /* UTF-16/UTF-32 with no stated byte order: honour a leading BOM */
    if (!c->bom_done && (c->from == CS_UTF16 || c->from == CS_UTF32)) {
        int w = c->from == CS_UTF16 ? 2 : 4;
        if (n >= (size_t)w) {
            if (w == 2 && p[0] == 0xFF && p[1] == 0xFE) { c->in_be = 0; p += 2; n -= 2; }
            else if (w == 2 && p[0] == 0xFE && p[1] == 0xFF) { c->in_be = 1; p += 2; n -= 2; }
            else if (w == 4 && p[0] == 0xFF && p[1] == 0xFE && !p[2] && !p[3]) { c->in_be = 0; p += 4; n -= 4; }
            else if (w == 4 && !p[0] && !p[1] && p[2] == 0xFE && p[3] == 0xFF) { c->in_be = 1; p += 4; n -= 4; }
            c->bom_done = 1;
        }
    }
    size_t r = 0;
    while (n) {
        unsigned cp;
        int used = decode(c, p, n, &cp);
        if (used == 0) { errno = EINVAL; r = (size_t)-1; break; }
        if (used < 0) {
            if (!c->lossy) { errno = EILSEQ; r = (size_t)-1; break; }
            used = 1;
            cp = '?';
            lossy++;
        }
        int put = encode(c, cp, o, room);
        if (put < 0) {
            if (!c->lossy) { errno = EILSEQ; r = (size_t)-1; break; }
            put = encode(c, '?', o, room);
            lossy++;
        }
        if (put == 0) { errno = E2BIG; r = (size_t)-1; break; }
        p += used; n -= used;
        o += put; room -= put;
    }
    *in = (char *)p;
    *inleft = n;
    *out = (char *)o;
    *outleft = room;
    return r == (size_t)-1 ? r : lossy;
}
