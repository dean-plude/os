/*
 * lzx.c — the LZX decompressor used by cabinets
 *
 * LZX is LZ77 with Huffman-coded literals, match lengths and positions
 * (the "main" tree), a length tree for long matches, and an "aligned
 * offset" tree; three recently used offsets are kept; blocks are verbatim,
 * aligned or uncompressed.  The input is 16-bit little-endian words read
 * MSB first.  Output comes in 32 KB frames (one per CFDATA block); the
 * bit stream is realigned after each frame, and x86 call operands (E8)
 * are translated back per frame when the folder asked for it.
 */
#include "msi_int.h"

#define LZX_MIN_MATCH        2
#define LZX_NUM_CHARS        256
#define LZX_PRETREE_NUM      20
#define LZX_PRETREE_MAXBITS  16
#define LZX_MAIN_MAXBITS     16
#define LZX_LENGTH_NUM       249
#define LZX_LENGTH_MAXBITS   16
#define LZX_ALIGNED_NUM      8
#define LZX_ALIGNED_MAXBITS  7
#define LZX_MAX_SLOTS        50

#define BT_VERBATIM     1
#define BT_ALIGNED      2
#define BT_UNCOMPRESSED 3

typedef struct {
    uint8_t  lens[LZX_NUM_CHARS + LZX_MAX_SLOTS * 8];
    uint16_t count[17];
    uint16_t symbol[LZX_NUM_CHARS + LZX_MAX_SLOTS * 8];
    int      nsym;
} Tree;

typedef struct {
    uint32_t window_size;
    uint8_t *window;
    uint32_t window_pos;
    uint32_t R0, R1, R2;
    int      num_slots;               /* position slots for this window */
    int      main_elements;
    bool     header_read;
    int      intel_filesize;
    int      block_type;
    uint32_t block_length, block_remaining;
    uint32_t frame_pos;               /* output bytes so far (for E8) */
    Tree     pretree, maintree, lengthtree, alignedtree;
    /* bit reader */
    const uint8_t *in;
    uint32_t in_len, in_pos;
    uint32_t bitbuf;
    int      bitcnt;
    bool     overrun;
} Lzx;

static const uint32_t position_base[LZX_MAX_SLOTS + 1] = {
    0,1,2,3,4,6,8,12,16,24,32,48,64,96,128,192,256,384,512,768,1024,1536,2048,3072,4096,6144,
    8192,12288,16384,24576,32768,49152,65536,98304,131072,196608,262144,393216,524288,655360,
    786432,917504,1048576,1179648,1310720,1441792,1572864,1703936,1835008,1966080,2097152 };
static const uint8_t extra_bits[LZX_MAX_SLOTS + 1] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,
    17,17,17,17,17,17,17,17,17,17,17,17,17,17,17 };

static void ensure(Lzx *z, int n)
{
    while (z->bitcnt < n) {
        if (z->in_pos + 1 >= z->in_len) {
            if (z->in_pos < z->in_len) { /* a lone byte: pad */ z->in_pos = z->in_len; }
            z->bitbuf |= 0;                   /* zeros */
            z->bitcnt += 16;
            z->overrun = z->overrun || z->in_pos >= z->in_len;
            /* keep reading zeros; the caller checks overrun at frame end */
            if (z->bitcnt >= n) return;
            continue;
        }
        uint32_t w = (uint32_t)(z->in[z->in_pos] | z->in[z->in_pos + 1] << 8);
        z->in_pos += 2;
        z->bitbuf |= w << (16 - z->bitcnt);
        z->bitcnt += 16;
    }
}

static uint32_t peek(Lzx *z, int n) { ensure(z, n); return z->bitbuf >> (32 - n); }
static void     drop(Lzx *z, int n) { z->bitbuf <<= n; z->bitcnt -= n; }
static uint32_t bits(Lzx *z, int n) { if (!n) return 0; uint32_t v = peek(z, n); drop(z, n); return v; }
static void     align16(Lzx *z) { int r = z->bitcnt & 15; if (r) drop(z, r); }

static bool tree_build(Tree *t, int nsym)
{
    t->nsym = nsym;
    memset(t->count, 0, sizeof(t->count));
    for (int i = 0; i < nsym; i++) t->count[t->lens[i]]++;
    t->count[0] = 0;
    uint16_t offs[18];
    offs[1] = 0;
    int left = 1;
    for (int len = 1; len <= 16; len++) {
        left = (left << 1) - t->count[len];
        if (left < 0) return false;             /* over-subscribed */
        offs[len + 1] = (uint16_t)(offs[len] + t->count[len]);
    }
    for (int i = 0; i < nsym; i++) if (t->lens[i]) t->symbol[offs[t->lens[i]]++] = (uint16_t)i;
    return true;
}

static int tree_decode(Lzx *z, const Tree *t)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 16; len++) {
        code |= (int)bits(z, 1);
        int count = t->count[len];
        if (code - count < first) return t->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* Read the code lengths for symbols [first, last) of @t, delta-coded
 * through the pretree */
static bool read_lengths(Lzx *z, Tree *t, int first, int last)
{
    for (int i = 0; i < LZX_PRETREE_NUM; i++) z->pretree.lens[i] = (uint8_t)bits(z, 4);
    if (!tree_build(&z->pretree, LZX_PRETREE_NUM)) return false;
    int x = first;
    while (x < last) {
        int sym = tree_decode(z, &z->pretree);
        if (sym < 0) return false;
        if (sym == 17) {
            int run = (int)bits(z, 4) + 4;
            while (run-- && x < last) t->lens[x++] = 0;
        } else if (sym == 18) {
            int run = (int)bits(z, 5) + 20;
            while (run-- && x < last) t->lens[x++] = 0;
        } else if (sym == 19) {
            int run = (int)bits(z, 1) + 4;
            int s = tree_decode(z, &z->pretree);
            if (s < 0) return false;
            int v = t->lens[x] - s;
            if (v < 0) v += 17;
            while (run-- && x < last) t->lens[x++] = (uint8_t)v;
        } else {
            int v = t->lens[x] - sym;
            if (v < 0) v += 17;
            t->lens[x++] = (uint8_t)v;
        }
    }
    return true;
}

void *lzx_init(int window_bits, char *err)
{
    if (window_bits < 15 || window_bits > 21) { snprintf(err, 96, "LZX: bad window size"); return NULL; }
    Lzx *z = calloc(1, sizeof(Lzx));
    if (!z) { snprintf(err, 96, "out of memory"); return NULL; }
    z->window_size = 1u << window_bits;
    z->window = calloc(1, z->window_size);
    if (!z->window) { free(z); snprintf(err, 96, "out of memory"); return NULL; }
    /* position slots: 30 for 2^15, +2 per doubling up to 2^20, 50 at 2^21 */
    int slots = window_bits == 21 ? 50 : window_bits == 20 ? 42 : (window_bits - 15) * 2 + 30;
    z->num_slots = slots;
    z->main_elements = LZX_NUM_CHARS + slots * 8;
    lzx_reset(z);
    return z;
}

void lzx_reset(void *st)
{
    Lzx *z = st;
    z->R0 = z->R1 = z->R2 = 1;
    z->header_read = false;
    z->block_remaining = 0;
    z->block_type = 0;
    memset(z->maintree.lens, 0, sizeof(z->maintree.lens));
    memset(z->lengthtree.lens, 0, sizeof(z->lengthtree.lens));
}

void lzx_free(void *st)
{
    Lzx *z = st;
    if (!z) return;
    free(z->window);
    free(z);
}

int lzx_block(void *st, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len, char *err)
{
    Lzx *z = st;
    z->in = in;
    z->in_len = in_len;
    z->in_pos = 0;
    z->bitbuf = 0;
    z->bitcnt = 0;
    z->overrun = false;
    uint32_t start = z->window_pos;
    uint32_t todo = out_len;
    uint32_t wmask = z->window_size - 1;

    if (!z->header_read) {
        if (bits(z, 1)) { z->intel_filesize = (int)((bits(z, 16) << 16) | bits(z, 16)); }
        else z->intel_filesize = 0;
        z->header_read = true;
    }

    while (todo) {
        if (!z->block_remaining) {
            if (z->block_type == BT_UNCOMPRESSED) {
                /* an odd-length uncompressed block is padded */
                if (z->block_length & 1) z->in_pos++;
                z->bitbuf = 0; z->bitcnt = 0;
            }
            z->block_type = (int)bits(z, 3);
            z->block_length = (bits(z, 16) << 8) | bits(z, 8);
            z->block_remaining = z->block_length;
            switch (z->block_type) {
            case BT_ALIGNED:
                for (int i = 0; i < LZX_ALIGNED_NUM; i++) z->alignedtree.lens[i] = (uint8_t)bits(z, 3);
                if (!tree_build(&z->alignedtree, LZX_ALIGNED_NUM)) { snprintf(err, 96, "LZX: bad aligned tree"); return -1; }
                /* fall through */
            case BT_VERBATIM:
                if (!read_lengths(z, &z->maintree, 0, LZX_NUM_CHARS) ||
                    !read_lengths(z, &z->maintree, LZX_NUM_CHARS, z->main_elements) ||
                    !tree_build(&z->maintree, z->main_elements)) { snprintf(err, 96, "LZX: bad main tree"); return -1; }
                if (!read_lengths(z, &z->lengthtree, 0, LZX_LENGTH_NUM) ||
                    !tree_build(&z->lengthtree, LZX_LENGTH_NUM)) { snprintf(err, 96, "LZX: bad length tree"); return -1; }
                break;
            case BT_UNCOMPRESSED:
                /* realign to 16 bits, then R0-R2 follow as raw dwords */
                if (z->bitcnt > 16) z->in_pos -= 2;      /* give back the lookahead word */
                z->bitbuf = 0; z->bitcnt = 0;
                if (z->in_pos + 12 > z->in_len) { snprintf(err, 96, "LZX: truncated block header"); return -1; }
                z->R0 = (uint32_t)(in[z->in_pos] | in[z->in_pos+1] << 8 | in[z->in_pos+2] << 16 | (uint32_t)in[z->in_pos+3] << 24);
                z->R1 = (uint32_t)(in[z->in_pos+4] | in[z->in_pos+5] << 8 | in[z->in_pos+6] << 16 | (uint32_t)in[z->in_pos+7] << 24);
                z->R2 = (uint32_t)(in[z->in_pos+8] | in[z->in_pos+9] << 8 | in[z->in_pos+10] << 16 | (uint32_t)in[z->in_pos+11] << 24);
                z->in_pos += 12;
                break;
            default:
                snprintf(err, 96, "LZX: bad block type %d", z->block_type);
                return -1;
            }
        }
        uint32_t n = todo < z->block_remaining ? todo : z->block_remaining;
        z->block_remaining -= n;
        todo -= n;

        if (z->block_type == BT_UNCOMPRESSED) {
            if (z->in_pos + n > z->in_len) { snprintf(err, 96, "LZX: truncated uncompressed block"); return -1; }
            for (uint32_t i = 0; i < n; i++) { z->window[z->window_pos] = in[z->in_pos++]; z->window_pos = (z->window_pos + 1) & wmask; }
            continue;
        }
        while (n) {
            int sym = tree_decode(z, &z->maintree);
            if (sym < 0) { snprintf(err, 96, "LZX: bad main code"); return -1; }
            if (sym < LZX_NUM_CHARS) {
                z->window[z->window_pos] = (uint8_t)sym;
                z->window_pos = (z->window_pos + 1) & wmask;
                n--;
                continue;
            }
            sym -= LZX_NUM_CHARS;
            uint32_t len = (uint32_t)(sym & 7);
            if (len == 7) {
                int l = tree_decode(z, &z->lengthtree);
                if (l < 0) { snprintf(err, 96, "LZX: bad length code"); return -1; }
                len += (uint32_t)l;
            }
            len += LZX_MIN_MATCH;
            int slot = sym >> 3;
            uint32_t off;
            if (slot == 0)      off = z->R0;
            else if (slot == 1) { off = z->R1; z->R1 = z->R0; z->R0 = off; }
            else if (slot == 2) { off = z->R2; z->R2 = z->R0; z->R0 = off; }
            else {
                int eb = extra_bits[slot];
                if (z->block_type == BT_ALIGNED && eb >= 3) {
                    uint32_t verb = bits(z, eb - 3) << 3;
                    int a = tree_decode(z, &z->alignedtree);
                    if (a < 0) { snprintf(err, 96, "LZX: bad aligned code"); return -1; }
                    off = position_base[slot] + verb + (uint32_t)a;
                } else {
                    off = position_base[slot] + bits(z, eb);
                }
                off -= 2;
                z->R2 = z->R1; z->R1 = z->R0; z->R0 = off;
            }
            if (len > n) { snprintf(err, 96, "LZX: match crosses frame"); return -1; }
            if (off > z->window_size) { snprintf(err, 96, "LZX: match too far"); return -1; }
            for (uint32_t i = 0; i < len; i++) {
                z->window[z->window_pos] = z->window[(z->window_pos - off) & wmask];
                z->window_pos = (z->window_pos + 1) & wmask;
            }
            n -= len;
        }
    }
    if (z->overrun && z->in_pos > z->in_len) { snprintf(err, 96, "LZX: input overrun"); return -1; }

    /* the frame's bytes, then E8 translation */
    for (uint32_t i = 0; i < out_len; i++) out[i] = z->window[(start + i) & wmask];
    if (z->intel_filesize && out_len > 10 && z->frame_pos < 0x40000000u) {
        uint32_t end = out_len - 10;
        for (uint32_t i = 0; i < end; i++) {
            if (out[i] != 0xE8) continue;
            int32_t cur = (int32_t)(z->frame_pos + i);
            int32_t abs = (int32_t)(out[i+1] | out[i+2] << 8 | out[i+3] << 16 | (uint32_t)out[i+4] << 24);
            if (abs >= -cur && abs < z->intel_filesize) {
                int32_t rel = abs >= 0 ? abs - cur : abs + z->intel_filesize;
                out[i+1] = (uint8_t)rel; out[i+2] = (uint8_t)(rel >> 8);
                out[i+3] = (uint8_t)(rel >> 16); out[i+4] = (uint8_t)(rel >> 24);
            }
            i += 4;
        }
    }
    z->frame_pos += out_len;
    /* the bit stream realigns after every frame */
    align16(z);
    return (int)out_len;
}
