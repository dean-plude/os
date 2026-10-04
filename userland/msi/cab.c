/*
 * cab.c — Microsoft cabinet files: the container of the package's files
 *
 * A cabinet holds folders (compressed streams of CFDATA blocks, each up
 * to 32 KB uncompressed) and files at offsets within a folder.  A folder
 * may continue in the next cabinet of a set.  Compression is none, MSZIP
 * (deflate per block, with the history carried across blocks) or LZX.
 */
#include "msi_int.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static size_t rd_str(const uint8_t *p, size_t max, char *out, size_t cap)
{
    size_t n = 0;
    while (n < max && p[n]) n++;
    size_t c = n < cap - 1 ? n : cap - 1;
    memcpy(out, p, c);
    out[c] = '\0';
    return n + 1;
}

bool cab_open(Cab *c, const void *data, size_t size)
{
    memset(c, 0, sizeof(*c));
    const uint8_t *h = data;
    if (size < 36 || memcmp(h, "MSCF", 4)) return false;
    c->data = data;
    c->size = size;
    uint32_t coff_files = rd32(h + 16);
    c->nfolders = rd16(h + 26);
    c->nfiles   = rd16(h + 28);
    uint16_t flags = rd16(h + 30);
    size_t pos = 36;
    uint8_t reserve_folder = 0;
    if (flags & 4) {                              /* reserved areas */
        if (size < 40) return false;
        uint16_t hres = rd16(h + 36);
        reserve_folder = h[38];
        c->reserve_data = h[39];
        pos = 40 + hres;
    }
    char disk[64];                                /* disk names: not needed */
    if (flags & 1) { pos += rd_str(h + pos, size - pos, c->prev, sizeof(c->prev)); pos += rd_str(h + pos, size - pos, disk, sizeof(disk)); }
    if (flags & 2) { pos += rd_str(h + pos, size - pos, c->next, sizeof(c->next)); pos += rd_str(h + pos, size - pos, disk, sizeof(disk)); }

    c->folders = calloc((size_t)c->nfolders + 1, sizeof(CabFolder));
    c->files   = calloc((size_t)c->nfiles + 1, sizeof(CabFile));
    if (!c->folders || !c->files) { cab_close(c); return false; }
    for (int i = 0; i < c->nfolders; i++) {
        if (pos + 8 > size) { cab_close(c); return false; }
        c->folders[i].data_off = rd32(h + pos);
        c->folders[i].ndata    = rd16(h + pos + 4);
        c->folders[i].compress = rd16(h + pos + 6);
        pos += 8 + reserve_folder;
    }
    pos = coff_files;
    for (int i = 0; i < c->nfiles; i++) {
        if (pos + 16 > size) { cab_close(c); return false; }
        CabFile *f = &c->files[i];
        f->size       = rd32(h + pos);
        f->folder_off = rd32(h + pos + 4);
        uint16_t ifold = rd16(h + pos + 8);
        f->date    = rd16(h + pos + 10);
        f->time    = rd16(h + pos + 12);
        f->attribs = rd16(h + pos + 14);
        pos += 16;
        pos += rd_str(h + pos, size - pos, f->name, sizeof(f->name));
        if (ifold == 0xFFFD) { f->folder = -1; }                                  /* from prev */
        else if (ifold == 0xFFFE) { f->folder = c->nfolders - 1; f->continued_next = true; }
        else if (ifold == 0xFFFF) { f->folder = -1; f->continued_next = true; }
        else f->folder = ifold;
    }
    return true;
}

void cab_close(Cab *c)
{
    free(c->folders);
    free(c->files);
    memset(c, 0, sizeof(*c));
}

/* -----------------------------------------------------------------------
 * Folder reader
 * ----------------------------------------------------------------------- */
static bool reader_setup(CabReader *r, const Cab *cab, int folder)
{
    if (folder < 0 || folder >= cab->nfolders) { snprintf(r->error, sizeof(r->error), "bad folder"); return false; }
    r->cab = cab;
    r->folder = folder;
    r->next_data = cab->folders[folder].data_off;
    r->blocks_left = cab->folders[folder].ndata;
    return true;
}

bool cab_reader_start(CabReader *r, const Cab *cab, int folder)
{
    memset(r, 0, sizeof(*r));
    if (!reader_setup(r, cab, folder)) return false;
    uint16_t tc = cab->folders[folder].compress;
    r->method = tc & 0xF;
    r->out = malloc(32768 + 64);
    if (!r->out) { snprintf(r->error, sizeof(r->error), "out of memory"); return false; }
    if (r->method == 1) {
        r->window_size = 32768;
        r->window = calloc(1, r->window_size);
    } else if (r->method == 3) {
        int bits = (tc >> 8) & 0x1F;
        r->lzx = lzx_init(bits, r->error);
        if (!r->lzx) return false;
    } else if (r->method != 0) {
        snprintf(r->error, sizeof(r->error), "unsupported compression %d", r->method);
        return false;
    }
    return true;
}

bool cab_reader_continue(CabReader *r, const Cab *next)
{
    if (next->nfolders < 1) { snprintf(r->error, sizeof(r->error), "next cabinet has no folders"); return false; }
    if ((next->folders[0].compress & 0xF) != r->method) {
        snprintf(r->error, sizeof(r->error), "compression changes between cabinets");
        return false;
    }
    return reader_setup(r, next, 0);
}

bool cab_reader_next(CabReader *r)
{
    if (r->blocks_left <= 0) return false;
    const Cab *c = r->cab;
    size_t pos = r->next_data;
    if (pos + 8 > c->size) { snprintf(r->error, sizeof(r->error), "truncated cabinet"); return false; }
    uint16_t cb = rd16(c->data + pos + 4), cbu = rd16(c->data + pos + 6);
    pos += 8 + c->reserve_data;
    if (pos + cb > c->size || cbu > 32768) { snprintf(r->error, sizeof(r->error), "bad data block"); return false; }
    const uint8_t *in = c->data + pos;
    r->next_data = (uint32_t)(pos + cb);
    r->blocks_left--;
    r->out_len = 0;
    uint32_t in_len = cb;
    if ((cbu == 0 && r->blocks_left == 0) || r->partial_len) {
        /* a block split across cabinets: its first bytes end this cabinet
         * (uncompressed size 0), the rest begin the next one */
        uint8_t *p = realloc(r->partial, (size_t)r->partial_len + cb);
        if (!p) { snprintf(r->error, sizeof(r->error), "out of memory"); return false; }
        memcpy(p + r->partial_len, in, cb);
        r->partial = p;
        r->partial_len += cb;
        if (cbu == 0) return false;               /* cab_reader_continue() next */
        in = p;
        in_len = r->partial_len;
        r->partial_len = 0;
    }
    switch (r->method) {
    case 0:
        if (in_len != cbu) { snprintf(r->error, sizeof(r->error), "bad stored block"); return false; }
        memcpy(r->out, in, in_len);
        r->out_len = in_len;
        return true;
    case 1: {
        int n = mszip_block(in, in_len, r->window, r->window_size, &r->window_pos, r->out, cbu, r->error);
        if (n < 0) return false;
        if ((uint32_t)n != cbu) { snprintf(r->error, sizeof(r->error), "MSZIP block size mismatch"); return false; }
        r->out_len = (uint32_t)n;
        return true; }
    case 3: {
        int n = lzx_block(r->lzx, in, in_len, r->out, cbu, r->error);
        if (n < 0) return false;
        r->out_len = cbu;
        return true; }
    }
    return false;
}

void cab_reader_end(CabReader *r)
{
    free(r->partial);
    free(r->window);
    free(r->out);
    if (r->lzx) lzx_free(r->lzx);
    memset(r, 0, sizeof(*r));
}

/* -----------------------------------------------------------------------
 * MSZIP: "CK" + a raw deflate stream per block; back references may reach
 * into the previous blocks of the folder (kept in a 32 KB window)
 * ----------------------------------------------------------------------- */
typedef struct {
    const uint8_t *in;
    uint32_t       in_len, in_pos;
    uint32_t       bitbuf;
    int            bitcnt;
    uint8_t       *win;               /* 32 KB history ring */
    uint32_t       win_pos;           /* next write position (mod 32768) */
    uint8_t       *out;
    uint32_t       out_len, out_cap;
    bool           overrun;
} Inflate;

static int getbits(Inflate *z, int n)
{
    while (z->bitcnt < n) {
        if (z->in_pos >= z->in_len) { z->overrun = true; return 0; }
        z->bitbuf |= (uint32_t)z->in[z->in_pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    int v = (int)(z->bitbuf & ((1u << n) - 1));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

typedef struct { uint16_t count[16]; uint16_t symbol[320]; } Huff;

static int huff_build(Huff *h, const uint8_t *lengths, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    uint16_t offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    return 0;
}

static int huff_decode(Inflate *z, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbits(z, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (z->overrun) return -1;
    }
    return -1;
}

static bool put(Inflate *z, uint8_t b)
{
    if (z->out_len >= z->out_cap) return false;
    z->out[z->out_len++] = b;
    z->win[z->win_pos] = b;
    z->win_pos = (z->win_pos + 1) & 32767;
    return true;
}

static const uint16_t LBASE[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEXT[]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DEXT[]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static bool inflate_codes(Inflate *z, const Huff *lit, const Huff *dist, char *err)
{
    for (;;) {
        int sym = huff_decode(z, lit);
        if (sym < 0) { snprintf(err, 96, "MSZIP: bad literal code"); return false; }
        if (sym < 256) { if (!put(z, (uint8_t)sym)) { snprintf(err, 96, "MSZIP: output overflow"); return false; } continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) { snprintf(err, 96, "MSZIP: bad length code"); return false; }
        int len = LBASE[sym] + getbits(z, LEXT[sym]);
        int ds = huff_decode(z, dist);
        if (ds < 0 || ds >= 30) { snprintf(err, 96, "MSZIP: bad distance code"); return false; }
        uint32_t d = DBASE[ds] + (uint32_t)getbits(z, DEXT[ds]);
        if (d > 32768) { snprintf(err, 96, "MSZIP: distance too far"); return false; }
        for (int i = 0; i < len; i++) {
            uint8_t b = z->win[(z->win_pos - d) & 32767];
            if (!put(z, b)) { snprintf(err, 96, "MSZIP: output overflow"); return false; }
        }
        if (z->overrun) { snprintf(err, 96, "MSZIP: input overrun"); return false; }
    }
}

static bool inflate_fixed(Inflate *z, char *err)
{
    static Huff lit, dist;
    static bool built;
    if (!built) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&dist, l, 30);
        built = true;
    }
    return inflate_codes(z, &lit, &dist, err);
}

static bool inflate_dynamic(Inflate *z, char *err)
{
    static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    int nlen = getbits(z, 5) + 257, ndist = getbits(z, 5) + 1, ncode = getbits(z, 4) + 4;
    if (nlen > 286 || ndist > 30) { snprintf(err, 96, "MSZIP: bad code counts"); return false; }
    uint8_t lengths[320];
    memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)getbits(z, 3);
    Huff lencode;
    huff_build(&lencode, lengths, 19);
    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = huff_decode(z, &lencode);
        if (sym < 0) { snprintf(err, 96, "MSZIP: bad code length code"); return false; }
        if (sym < 16) { lengths[idx++] = (uint8_t)sym; continue; }
        int len = 0, rep;
        if (sym == 16) { if (!idx) { snprintf(err, 96, "MSZIP: repeat without previous"); return false; } len = lengths[idx - 1]; rep = 3 + getbits(z, 2); }
        else if (sym == 17) rep = 3 + getbits(z, 3);
        else rep = 11 + getbits(z, 7);
        if (idx + rep > nlen + ndist) { snprintf(err, 96, "MSZIP: too many lengths"); return false; }
        while (rep--) lengths[idx++] = (uint8_t)len;
    }
    Huff lit, dist;
    huff_build(&lit, lengths, nlen);
    huff_build(&dist, lengths + nlen, ndist);
    return inflate_codes(z, &lit, &dist, err);
}

int mszip_block(const uint8_t *in, uint32_t in_len, uint8_t *window, uint32_t window_size,
                uint32_t *window_pos, uint8_t *out, uint32_t out_cap, char *err)
{
    (void)window_size;
    if (in_len < 2 || in[0] != 'C' || in[1] != 'K') { snprintf(err, 96, "MSZIP: bad block signature"); return -1; }
    Inflate z;
    memset(&z, 0, sizeof(z));
    z.in = in + 2;
    z.in_len = in_len - 2;
    z.win = window;
    z.win_pos = *window_pos;
    z.out = out;
    z.out_cap = out_cap;
    int last;
    do {
        last = getbits(&z, 1);
        int type = getbits(&z, 2);
        bool ok;
        if (type == 0) {
            z.bitbuf = 0; z.bitcnt = 0;           /* byte align */
            if (z.in_pos + 4 > z.in_len) { snprintf(err, 96, "MSZIP: truncated stored block"); return -1; }
            uint32_t len = (uint32_t)(z.in[z.in_pos] | z.in[z.in_pos + 1] << 8);
            z.in_pos += 4;
            if (z.in_pos + len > z.in_len) { snprintf(err, 96, "MSZIP: truncated stored block"); return -1; }
            for (uint32_t i = 0; i < len; i++)
                if (!put(&z, z.in[z.in_pos + i])) { snprintf(err, 96, "MSZIP: output overflow"); return -1; }
            z.in_pos += len;
            ok = true;
        } else if (type == 1) {
            ok = inflate_fixed(&z, err);
        } else if (type == 2) {
            ok = inflate_dynamic(&z, err);
        } else {
            snprintf(err, 96, "MSZIP: bad block type");
            return -1;
        }
        if (!ok) return -1;
    } while (!last);
    *window_pos = z.win_pos;
    return (int)z.out_len;
}
