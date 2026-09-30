/*
 * cfb.c — reading an OLE compound file (the container of .msi packages)
 *
 * Streams are found by their MSI names: the directory names are UTF-16
 * in a "compressed" encoding (two 6-bit characters per code unit in the
 * 0x3800-0x47FF range, one in 0x4800-0x483F, and 0x4840 marking a table),
 * decoded here to ASCII with '!' standing for the table marker.
 */
#include "msi_int.h"

#define ENDOFCHAIN 0xFFFFFFFEu
#define FREESECT   0xFFFFFFFFu

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static const uint8_t *sector(const Cfb *c, uint32_t n)
{
    size_t off = ((size_t)n + 1) << c->sector_shift;
    if (n >= 0xFFFFFFFAu || off + ((size_t)1 << c->sector_shift) > c->size) return NULL;
    return c->data + off;
}

/* Read a chain of sectors into a malloc'd buffer of @len bytes */
static uint8_t *read_chain(const Cfb *c, uint32_t start, size_t len)
{
    size_t ssz = (size_t)1 << c->sector_shift;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) return NULL;
    size_t done = 0;
    uint32_t s = start;
    size_t guard = 0;
    while (done < len) {
        const uint8_t *p = sector(c, s);
        if (!p || guard++ > c->fat_entries) { free(buf); return NULL; }
        size_t n = len - done < ssz ? len - done : ssz;
        memcpy(buf + done, p, n);
        done += n;
        s = s < c->fat_entries ? c->fat[s] : ENDOFCHAIN;
    }
    return buf;
}

static uint8_t *read_mini_chain(const Cfb *c, uint32_t start, size_t len)
{
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) return NULL;
    size_t done = 0;
    uint32_t s = start, guard = 0;
    while (done < len) {
        size_t off = (size_t)s * 64;
        if (s >= c->minifat_entries || off + 64 > c->ministream_size || guard++ > c->minifat_entries) {
            free(buf);
            return NULL;
        }
        size_t n = len - done < 64 ? len - done : 64;
        memcpy(buf + done, c->ministream + off, n);
        done += n;
        s = c->minifat[s];
    }
    return buf;
}

static const uint8_t *dir_entry(const Cfb *c, int i)
{
    /* directory entries are 128 bytes in the directory chain */
    size_t ssz = (size_t)1 << c->sector_shift;
    size_t per = ssz / 128;
    uint32_t s = c->dir_start;
    size_t guard = 0;
    for (size_t k = 0; k < (size_t)i / per; k++) {
        if (s >= c->fat_entries || guard++ > c->fat_entries) return NULL;
        s = c->fat[s];
    }
    const uint8_t *p = sector(c, s);
    return p ? p + ((size_t)i % per) * 128 : NULL;
}

bool cfb_open(Cfb *c, const void *data, size_t size)
{
    memset(c, 0, sizeof(*c));
    const uint8_t *h = data;
    static const uint8_t sig[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    if (size < 512 || memcmp(h, sig, 8)) return false;
    c->data = data;
    c->size = size;
    c->sector_shift  = rd16(h + 0x1E);
    if (c->sector_shift != 9 && c->sector_shift != 12) return false;
    c->nfat          = rd32(h + 0x2C);
    c->dir_start     = rd32(h + 0x30);
    c->mini_cutoff   = rd32(h + 0x38);
    c->minifat_start = rd32(h + 0x3C);
    c->nminifat      = rd32(h + 0x40);
    c->difat_start   = rd32(h + 0x44);
    c->ndifat        = rd32(h + 0x48);
    size_t ssz = (size_t)1 << c->sector_shift, per = ssz / 4;

    /* the FAT: its sector numbers come from the DIFAT (109 in the header,
     * the rest in DIFAT sectors) */
    if (c->nfat > 65536) return false;
    c->fat_entries = (uint32_t)(c->nfat * per);
    c->fat = malloc(c->fat_entries * 4 + 4);
    if (!c->fat) return false;
    uint32_t got = 0;
    for (uint32_t i = 0; i < 109 && got < c->nfat; i++, got++) {
        const uint8_t *p = sector(c, rd32(h + 0x4C + 4 * i));
        if (!p) { cfb_close(c); return false; }
        for (size_t k = 0; k < per; k++) c->fat[got * per + k] = rd32(p + 4 * k);
    }
    uint32_t ds = c->difat_start;
    for (uint32_t n = 0; got < c->nfat && n < c->ndifat; n++) {
        const uint8_t *dp = sector(c, ds);
        if (!dp) { cfb_close(c); return false; }
        for (size_t i = 0; i + 1 < per && got < c->nfat; i++, got++) {
            const uint8_t *p = sector(c, rd32(dp + 4 * i));
            if (!p) { cfb_close(c); return false; }
            for (size_t k = 0; k < per; k++) c->fat[got * per + k] = rd32(p + 4 * k);
        }
        ds = rd32(dp + 4 * (per - 1));
    }

    /* mini FAT */
    if (c->nminifat && c->nminifat < 65536) {
        c->minifat_entries = (uint32_t)(c->nminifat * per);
        uint8_t *raw = read_chain(c, c->minifat_start, c->minifat_entries * 4);
        if (!raw) { cfb_close(c); return false; }
        c->minifat = malloc(c->minifat_entries * 4);
        if (!c->minifat) { free(raw); cfb_close(c); return false; }
        for (uint32_t i = 0; i < c->minifat_entries; i++) c->minifat[i] = rd32(raw + 4 * i);
        free(raw);
    }

    /* directory: count entries; the root holds the mini stream */
    uint32_t s = c->dir_start;
    size_t guard = 0;
    while (s < c->fat_entries && sector(c, s) && guard++ < c->fat_entries) {
        c->nentries += (int)(ssz / 128);
        s = c->fat[s];
    }
    const uint8_t *root = dir_entry(c, 0);
    if (!root) { cfb_close(c); return false; }
    size_t rsize = c->sector_shift == 9 ? rd32(root + 0x78) : (size_t)(rd32(root + 0x78) | ((unsigned long long)rd32(root + 0x7C) << 32));
    if (rsize && rsize < c->size) {
        c->ministream = read_chain(c, rd32(root + 0x74), rsize);
        c->ministream_size = c->ministream ? rsize : 0;
    }
    return true;
}

void cfb_close(Cfb *c)
{
    free(c->fat);
    free(c->minifat);
    free(c->ministream);
    memset(c, 0, sizeof(*c));
}

static char mime2utf(int x)
{
    if (x < 10) return (char)('0' + x);
    if (x < 36) return (char)('A' + x - 10);
    if (x < 62) return (char)('a' + x - 36);
    return x == 62 ? '.' : '_';
}

static void decode_name(const uint8_t *raw, int nbytes, char *out, int cap)
{
    int n = 0;
    for (int i = 0; i + 1 < nbytes && n < cap - 3; i += 2) {
        unsigned ch = rd16(raw + i);
        if (!ch) break;
        if (ch >= 0x3800 && ch < 0x4840) {
            if (ch >= 0x4800) {
                out[n++] = mime2utf((int)(ch - 0x4800));
            } else {
                ch -= 0x3800;
                out[n++] = mime2utf((int)(ch & 0x3F));
                out[n++] = mime2utf((int)((ch >> 6) & 0x3F));
            }
        } else if (ch == 0x4840) {
            out[n++] = '!';
        } else {
            out[n++] = ch < 128 ? (char)ch : '?';
        }
    }
    out[n] = '\0';
}

bool cfb_entry(const Cfb *c, int i, char *name, int cap, bool *is_stream)
{
    if (i < 0 || i >= c->nentries) return false;
    const uint8_t *e = dir_entry(c, i);
    if (!e) return false;
    int type = e[0x42];
    if (type == 0) { name[0] = '\0'; *is_stream = false; return true; }
    int nlen = rd16(e + 0x40);
    if (nlen > 64) nlen = 64;
    decode_name(e, nlen - 2 > 0 ? nlen - 2 : 0, name, cap);
    *is_stream = type == 2;
    return true;
}

void *cfb_read(const Cfb *c, const char *name, size_t *size)
{
    char n[128];
    bool stream;
    for (int i = 0; cfb_entry(c, i, n, sizeof(n), &stream); i++) {
        if (!stream || strcmp(n, name)) continue;
        const uint8_t *e = dir_entry(c, i);
        size_t len = rd32(e + 0x78);
        uint32_t start = rd32(e + 0x74);
        void *buf = len < c->mini_cutoff ? read_mini_chain(c, start, len) : read_chain(c, start, len);
        if (buf && size) *size = len;
        return buf;
    }
    return NULL;
}
