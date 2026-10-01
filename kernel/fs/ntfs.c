/*
 * ntfs.c — NTFS, read-only (see ntfs.h)
 *
 * Structures follow the on-disk format as documented by the Linux-NTFS
 * project ("NTFS Documentation", Richard Russon and Yuval Fledel).  Every
 * MFT record and index block is checked (magic, update sequence, bounds)
 * before it is used, so a damaged volume reads as errors, not crashes.
 *
 * An attribute's data is a Stream: a resident value, or a list of runs
 * gathered from every MFT record that holds a piece of it (through the
 * $ATTRIBUTE_LIST of a file too big or too fragmented for one record).
 */

#include "ntfs.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"

#define AT_STANDARD_INFO     0x10u
#define AT_ATTRIBUTE_LIST    0x20u
#define AT_FILE_NAME         0x30u
#define AT_VOLUME_NAME       0x60u
#define AT_DATA              0x80u
#define AT_INDEX_ROOT        0x90u
#define AT_INDEX_ALLOCATION  0xA0u
#define AT_END               0xFFFFFFFFu

#define ATTR_COMPRESSED      0x00FFu
#define ATTR_ENCRYPTED       0x4000u

#define MFT_FIRST_USER       16            /* records below are metafiles */
#define REF_MASK             0x0000FFFFFFFFFFFFull
#define LZNT1_CHUNK          4096
#define MAX_DEPTH            24            /* index B+ tree levels we follow */
#define BOUNCE               (64u * 1024u)

typedef struct { UINT64 vcn, lcn, len; } Run;        /* lcn == ~0: sparse */
#define SPARSE (~0ull)

typedef struct {
    bool    resident;
    UINT8  *value;                         /* resident: a copy of the value */
    Run    *runs;
    UINT32  nruns, cap;
    UINT64  size, init_size;               /* data size; bytes past init_size read as 0 */
    UINT32  cu_clusters;                   /* compression unit; 0 if not compressed */
    UINT16  flags;
} Stream;

struct NtfsVol {
    BlockDev *dev;
    UINT64    base;                        /* LBA of the boot sector */
    UINT32    cluster;                     /* bytes */
    UINT32    rec_size;                    /* MFT record bytes */
    UINT32    idx_size;                    /* index block bytes */
    UINT64    total;                       /* bytes */
    Stream    mft;                         /* $MFT's $DATA */
    UINT8    *bounce;
    char      label[64];
};

static UINT16 rd16(const UINT8 *p) { UINT16 v; memcpy(&v, p, 2); return v; }
static UINT32 rd32(const UINT8 *p) { UINT32 v; memcpy(&v, p, 4); return v; }
static UINT64 rd64(const UINT8 *p) { UINT64 v; memcpy(&v, p, 8); return v; }

/* ---------------------------------------------------------------------------
 * The disk
 * ------------------------------------------------------------------------- */
/* Read @len bytes at volume byte offset @off (sector-aligned both) */
static bool dev_read(NtfsVol *v, UINT64 off, UINT64 len, void *buf)
{
    UINT8 *p = buf;
    UINT64 lba = v->base + off / BLOCK_SECTOR, n = len / BLOCK_SECTOR;
    while (n) {
        UINT32 k = n > 128 ? 128 : (UINT32)n;
        if (!v->dev->read(v->dev, lba, k, p)) return false;
        lba += k; p += (UINT64)k * BLOCK_SECTOR; n -= k;
    }
    return true;
}

/* Undo the update sequence of a multi-sector record (FILE or INDX) */
static bool fixup(UINT8 *rec, UINT32 size, const char *magic)
{
    if (memcmp(rec, magic, 4)) return false;
    UINT16 off = rd16(rec + 4), count = rd16(rec + 6);
    if (count < 2 || count - 1u != size / BLOCK_SECTOR || off + 2u * count > size || (off & 1)) return false;
    UINT16 usn = rd16(rec + off);
    for (UINT32 i = 1; i < count; i++) {
        UINT8 *end = rec + i * BLOCK_SECTOR - 2;
        if (rd16(end) != usn) return false;                     /* a torn write */
        memcpy(end, rec + off + 2 * i, 2);
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Attributes in an MFT record
 * ------------------------------------------------------------------------- */
/* The next attribute after @a (NULL: the first) of @type, or NULL */
static const UINT8 *next_attr(const UINT8 *rec, UINT32 size, const UINT8 *a, UINT32 type)
{
    UINT32 off = a ? (UINT32)(a - rec) + rd32(a + 4) : rd16(rec + 0x14);
    UINT32 used = rd32(rec + 0x18);
    if (used > size) used = size;
    while (off + 16 <= used) {
        const UINT8 *p = rec + off;
        UINT32 t = rd32(p), len = rd32(p + 4);
        if (t == AT_END || len < 16 || off + len > used || (len & 7)) return NULL;
        if (type == 0 || t == type) {
            bool nr = p[8];
            if (nr ? len >= 0x40 : len >= 0x18 && rd16(p + 0x14) + (UINT64)rd32(p + 0x10) <= len)
                if (p[9] == 0 || rd16(p + 0xA) + 2u * p[9] <= len)
                    return p;
        }
        off += len;
    }
    return NULL;
}

static bool name_eq(const UINT8 *a, const UINT16 *name, UINT32 name_len)
{
    if (a[9] != name_len) return false;
    for (UINT32 i = 0; i < name_len; i++)
        if (rd16(a + rd16(a + 0xA) + 2 * i) != name[i]) return false;
    return true;
}

/* An MFT record, read and checked */
static bool read_record(NtfsVol *v, UINT64 n, UINT8 *rec);

/* ---------------------------------------------------------------------------
 * Streams
 * ------------------------------------------------------------------------- */
static void stream_free(Stream *s)
{
    kfree(s->value);
    kfree(s->runs);
    memset(s, 0, sizeof(*s));
}

static bool add_run(Stream *s, UINT64 vcn, UINT64 lcn, UINT64 len)
{
    if (s->nruns == s->cap) {
        UINT32 cap = s->cap ? s->cap * 2 : 16;
        Run *r = kmalloc(cap * sizeof(Run));
        if (!r) return false;
        if (s->nruns) memcpy(r, s->runs, s->nruns * sizeof(Run));
        kfree(s->runs);
        s->runs = r;
        s->cap = cap;
    }
    s->runs[s->nruns++] = (Run){ vcn, lcn, len };
    return true;
}

/* Decode a non-resident attribute's mapping pairs into @s's runs */
static bool decode_runs(NtfsVol *v, Stream *s, const UINT8 *a)
{
    UINT32 alen = rd32(a + 4);
    UINT32 off = rd16(a + 0x20);
    UINT64 vcn = rd64(a + 0x10), last = rd64(a + 0x18);
    INT64 lcn = 0;
    UINT64 clusters = v->total / v->cluster;
    while (off < alen && a[off]) {
        UINT8 h = a[off++];
        UINT32 nl = h & 0xF, no = h >> 4;
        if (!nl || nl > 8 || no > 8 || off + nl + no > alen) return false;
        UINT64 len = 0;
        for (UINT32 i = 0; i < nl; i++) len |= (UINT64)a[off + i] << (8 * i);
        off += nl;
        INT64 delta = 0;
        if (no) {
            for (UINT32 i = 0; i < no; i++) delta |= (INT64)((UINT64)a[off + i] << (8 * i));
            if (no < 8 && (a[off + no - 1] & 0x80)) delta |= (INT64)(~0ull << (8 * no));   /* sign-extend */
            off += no;
            lcn += delta;
            if (lcn < 0 || (UINT64)lcn >= clusters || len > clusters - (UINT64)lcn) return false;
        }
        if (!len || vcn + len < vcn) return false;
        if (!add_run(s, vcn, no ? (UINT64)lcn : SPARSE, len)) return false;
        vcn += len;
    }
    return vcn == last + 1 || (vcn == 0 && last == ~0ull);   /* the runs cover exactly [lowest, highest] */
}

/* Take the sizes and kind of a stream from its first attribute piece */
static bool stream_start(Stream *s, const UINT8 *a)
{
    s->flags = rd16(a + 0xC);
    if (s->flags & ATTR_ENCRYPTED) return false;
    if (!a[8]) {
        UINT32 len = rd32(a + 0x10);
        s->resident = true;
        s->size = s->init_size = len;
        s->value = kmalloc(len ? len : 1);
        if (!s->value) return false;
        memcpy(s->value, a + rd16(a + 0x14), len);
        return true;
    }
    if (rd64(a + 0x10) != 0) return false;                       /* not the first piece */
    s->size = rd64(a + 0x30);
    s->init_size = rd64(a + 0x38);
    if (s->init_size > s->size) s->init_size = s->size;
    if (s->flags & ATTR_COMPRESSED) {
        if (a[0x22] == 0 || a[0x22] > 8) return false;
        s->cu_clusters = 1u << a[0x22];
    }
    return true;
}

static bool stream_read(NtfsVol *v, Stream *s, UINT64 off, void *buf, UINT64 len);

/* Open the stream of attribute @type named @name in file @mft */
static bool open_stream(NtfsVol *v, UINT64 mft, UINT32 type, const UINT16 *name, UINT32 name_len, Stream *s)
{
    memset(s, 0, sizeof(*s));
    UINT8 *rec = kmalloc(v->rec_size);
    if (!rec) return false;
    bool ok = false;
    if (!read_record(v, mft, rec)) goto out;

    const UINT8 *al = next_attr(rec, v->rec_size, NULL, AT_ATTRIBUTE_LIST);
    if (!al) {                                                   /* all in this record, in VCN order */
        bool started = false;
        for (const UINT8 *a = NULL; (a = next_attr(rec, v->rec_size, a, type)); ) {
            if (!name_eq(a, name, name_len)) continue;
            if (!started) {
                if (!stream_start(s, a)) goto out;
                started = true;
                if (s->resident) break;
            }
            if (!a[8] || !decode_runs(v, s, a)) goto out;
        }
        ok = started;
        goto out;
    }

    /* The attribute list says which records hold the pieces, in VCN order */
    Stream ls;
    memset(&ls, 0, sizeof(ls));
    if (!stream_start(&ls, al) || (!ls.resident && !decode_runs(v, &ls, al)) || ls.size > 1u << 20) {
        stream_free(&ls);
        goto out;
    }
    UINT8 *list = kmalloc(ls.size ? ls.size : 1);
    if (!list || !stream_read(v, &ls, 0, list, ls.size)) { kfree(list); stream_free(&ls); goto out; }
    UINT32 n = (UINT32)ls.size;
    stream_free(&ls);
    UINT8 *other = kmalloc(v->rec_size);
    bool found = false, bad = !other;
    for (UINT32 o = 0; !bad && o + 0x1A <= n; ) {
        const UINT8 *e = list + o;
        UINT16 elen = rd16(e + 4);
        if (elen < 0x1A || o + elen > n) break;
        o += elen;
        if (rd32(e) != type || e[6] != name_len || e[7] + 2u * name_len > elen) continue;
        bool same = true;
        for (UINT32 i = 0; i < name_len && same; i++) same = rd16(e + e[7] + 2 * i) == name[i];
        if (!same) continue;
        UINT64 ref = rd64(e + 0x10) & REF_MASK, low = rd64(e + 8);
        UINT16 inst = rd16(e + 0x18);
        const UINT8 *src = rec;
        if (ref != mft) {
            if (!read_record(v, ref, other) || (rd64(other + 0x20) & REF_MASK) != mft) { bad = true; break; }
            src = other;
        }
        const UINT8 *a = NULL;
        while ((a = next_attr(src, v->rec_size, a, type)))
            if (rd16(a + 0xE) == inst && name_eq(a, name, name_len)) break;
        if (!a) { bad = true; break; }
        if (!found) {
            if (low != 0 || !stream_start(s, a)) { bad = true; break; }
            found = true;
            if (s->resident) break;
        }
        if (!a[8] || rd64(a + 0x10) != low || !decode_runs(v, s, a)) { bad = true; break; }
    }
    kfree(other);
    kfree(list);
    ok = found && !bad;
out:
    kfree(rec);
    if (!ok) stream_free(s);
    return ok;
}

/* The LCN of cluster @vcn and how many clusters from there are contiguous */
static UINT64 map_vcn(const Stream *s, UINT64 vcn, UINT64 *count)
{
    UINT32 lo = 0, hi = s->nruns;
    while (lo < hi) {                                            /* runs are sorted by VCN */
        UINT32 mid = (lo + hi) / 2;
        const Run *r = &s->runs[mid];
        if (vcn < r->vcn) hi = mid;
        else if (vcn >= r->vcn + r->len) lo = mid + 1;
        else {
            *count = r->len - (vcn - r->vcn);
            return r->lcn == SPARSE ? SPARSE : r->lcn + (vcn - r->vcn);
        }
    }
    *count = 0;
    return SPARSE;                                               /* past the runs: unallocated */
}

/* Raw (uncompressed) stream bytes [off, off+len), all below init_size */
static bool read_raw(NtfsVol *v, const Stream *s, UINT64 off, UINT8 *out, UINT64 len)
{
    while (len) {
        UINT64 vcn = off / v->cluster, in = off % v->cluster, count;
        UINT64 lcn = map_vcn(s, vcn, &count);
        UINT64 span = count ? count * v->cluster - in : v->cluster - in;
        if (span > len) span = len;
        if (lcn == SPARSE) {
            memset(out, 0, span);
        } else {
            UINT64 disk = lcn * v->cluster + in;
            UINT64 done = 0;
            while (done < span) {
                UINT64 pos = disk + done, head = pos % BLOCK_SECTOR;
                UINT64 want = span - done;
                if (!head && want >= BLOCK_SECTOR) {             /* whole sectors: straight into @out */
                    UINT64 k = want - want % BLOCK_SECTOR;
                    if (!dev_read(v, pos, k, out + done)) return false;
                    done += k;
                } else {                                         /* a partial sector: via the bounce buffer */
                    UINT64 k = BLOCK_SECTOR - head;
                    if (k > want) k = want;
                    if (!dev_read(v, pos - head, BLOCK_SECTOR, v->bounce)) return false;
                    memcpy(out + done, v->bounce + head, k);
                    done += k;
                }
            }
        }
        off += span; out += span; len -= span;
    }
    return true;
}

/* LZNT1: decompress @n bytes at @in into @out (@cap bytes, zero-filled past the data) */
static bool lznt1(const UINT8 *in, UINT32 n, UINT8 *out, UINT32 cap)
{
    UINT32 ip = 0, op = 0;
    memset(out, 0, cap);
    while (ip + 2 <= n && op < cap) {
        UINT16 h = rd16(in + ip);
        if (!h) break;                                           /* the end of the data */
        UINT32 size = (h & 0xFFFu) + 1;
        ip += 2;
        if (ip + size > n) return false;
        UINT32 cstart = op, cend = op + LZNT1_CHUNK;
        if (cend > cap) cend = cap;
        if (!(h & 0x8000)) {                                     /* stored */
            if (size > cend - op) size = cend - op;
            memcpy(out + op, in + ip, size);
        } else {
            UINT32 p = ip, end = ip + size;
            while (p < end && op < cend) {
                UINT8 tags = in[p++];
                for (int b = 0; b < 8 && p < end && op < cend; b++, tags >>= 1) {
                    if (!(tags & 1)) { out[op++] = in[p++]; continue; }
                    if (p + 2 > end) return false;
                    UINT16 t = rd16(in + p);
                    p += 2;
                    UINT32 lmask = 0xFFF, dshift = 12;
                    for (UINT32 i = op - cstart - 1; i >= 0x10 && op > cstart; i >>= 1) { lmask >>= 1; dshift--; }
                    UINT32 dist = (t >> dshift) + 1, cnt = (t & lmask) + 3;
                    if (dist > op - cstart) return false;
                    for (UINT32 k = 0; k < cnt && op < cend; k++, op++) out[op] = out[op - dist];
                }
            }
        }
        ip += size;
        op = cstart + LZNT1_CHUNK;                                /* each chunk stands for 4 KiB */
    }
    return true;
}

/* One compression unit of a compressed stream, decompressed into @out */
static bool read_cu(NtfsVol *v, const Stream *s, UINT64 cu, UINT8 *out)
{
    UINT64 first = cu * s->cu_clusters, alloc = 0;
    UINT32 cu_bytes = s->cu_clusters * v->cluster;
    for (UINT64 c = first; c < first + s->cu_clusters; ) {      /* allocated clusters lead, sparse ones trail */
        UINT64 count, lcn = map_vcn(s, c, &count);
        if (!count) break;
        if (count > first + s->cu_clusters - c) count = first + s->cu_clusters - c;
        if (lcn == SPARSE) break;
        alloc += count;
        c += count;
    }
    if (alloc == 0) { memset(out, 0, cu_bytes); return true; }  /* all sparse: zeros */
    if (alloc == s->cu_clusters)                                 /* stored uncompressed */
        return read_raw(v, s, first * v->cluster, out, cu_bytes);
    UINT8 *cbuf = kmalloc(alloc * v->cluster);
    if (!cbuf) return false;
    bool ok = read_raw(v, s, first * v->cluster, cbuf, alloc * v->cluster) &&
              lznt1(cbuf, (UINT32)(alloc * v->cluster), out, cu_bytes);
    kfree(cbuf);
    return ok;
}

static bool stream_read(NtfsVol *v, Stream *s, UINT64 off, void *buf, UINT64 len)
{
    UINT8 *out = buf;
    if (off > s->size || len > s->size - off) return false;
    if (s->resident) { memcpy(out, s->value + off, len); return true; }
    UINT64 valid = s->init_size > off ? s->init_size - off : 0;   /* past init_size: zeros */
    if (valid < len) memset(out + valid, 0, len - valid);
    if (valid > len) valid = len;
    if (!s->cu_clusters) return read_raw(v, s, off, out, valid);

    UINT32 cu_bytes = s->cu_clusters * v->cluster;
    UINT8 *unit = kmalloc(cu_bytes);
    if (!unit) return false;
    bool ok = true;
    for (UINT64 done = 0; ok && done < valid; ) {
        UINT64 pos = off + done, cu = pos / cu_bytes, in = pos % cu_bytes;
        UINT64 k = cu_bytes - in;
        if (k > valid - done) k = valid - done;
        ok = read_cu(v, s, cu, unit);
        if (ok) memcpy(out + done, unit + in, k);
        done += k;
    }
    kfree(unit);
    return ok;
}

/* ---------------------------------------------------------------------------
 * MFT records
 * ------------------------------------------------------------------------- */
static bool read_record(NtfsVol *v, UINT64 n, UINT8 *rec)
{
    UINT64 off = n * v->rec_size;
    if (n > REF_MASK || off / v->rec_size != n || off + v->rec_size > v->mft.size) return false;
    if (!stream_read(v, &v->mft, off, rec, v->rec_size)) return false;
    if (!fixup(rec, v->rec_size, "FILE") || !(rd16(rec + 0x16) & 1)) return false;   /* in use */
    UINT16 aoff = rd16(rec + 0x14);
    return aoff >= 0x18 && aoff < v->rec_size && rd32(rec + 0x18) <= v->rec_size;
}

/* ---------------------------------------------------------------------------
 * Mounting
 * ------------------------------------------------------------------------- */
static void utf16_to_utf8(const UINT8 *s, UINT32 n, char *out, UINT32 cap)
{
    UINT32 o = 0;
    for (UINT32 i = 0; i < n; i++) {
        UINT32 c = rd16(s + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {          /* a surrogate pair */
            UINT32 lo = rd16(s + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
        UINT32 need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (o + need >= cap) break;
        if (need == 1) out[o++] = (char)c;
        else if (need == 2) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (need == 3) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                              out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
               out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = '\0';
}

/* Record size from a boot-sector "clusters per" byte: negative means 2^-n bytes */
static UINT32 per_size(INT8 c, UINT32 cluster)
{
    if (c > 0) return c <= 64 ? (UINT32)c * cluster : 0;
    return c <= -9 && c >= -20 ? 1u << -c : 0;
}

NtfsVol *NtfsMount(BlockDev *dev, UINT64 lba)
{
    UINT8 *bs = kmalloc(BLOCK_SECTOR);
    if (!bs) return NULL;
    if (lba >= dev->sectors || !dev->read(dev, lba, 1, bs) || memcmp(bs + 3, "NTFS    ", 8) ||
        bs[510] != 0x55 || bs[511] != 0xAA) {
        kfree(bs);
        return NULL;
    }
    UINT16 bps = rd16(bs + 0x0B);
    UINT8 spc = bs[0x0D];
    UINT64 total = rd64(bs + 0x28) * bps, mft_lcn = rd64(bs + 0x30);
    INT8 crec = (INT8)bs[0x40], cidx = (INT8)bs[0x44];
    kfree(bs);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc) return NULL;
    UINT32 cluster = spc <= 0x80 ? bps * spc : bps << (256 - spc);   /* spc > 0x80: 2^-n sectors (big clusters) */
    if (cluster < 512 || cluster > (2u << 20) || (cluster & (cluster - 1))) return NULL;
    UINT32 rec = per_size(crec, cluster), idx = per_size(cidx, cluster);
    if (rec < 1024 || rec > 65536 || (rec & (rec - 1)) || idx < 512 || idx > 65536 || (idx & (idx - 1))) return NULL;
    if (total > (dev->sectors - lba) * BLOCK_SECTOR) total = (dev->sectors - lba) * BLOCK_SECTOR;
    if (mft_lcn >= total / cluster) return NULL;

    NtfsVol *v = kzalloc(sizeof(NtfsVol));
    UINT8 *r = kmalloc(rec);
    if (!v || !r || !(v->bounce = kmalloc(BOUNCE))) goto fail;
    v->dev = dev; v->base = lba; v->cluster = cluster; v->rec_size = rec; v->idx_size = idx; v->total = total;

    /* $MFT record 0, read straight from the disk, describes the MFT; its
     * first extent holds the records an attribute list could point to */
    if (rec % BLOCK_SECTOR || !dev_read(v, mft_lcn * cluster, rec, r) || !fixup(r, rec, "FILE")) goto fail;
    const UINT8 *a = next_attr(r, rec, NULL, AT_DATA);
    while (a && a[9]) a = next_attr(r, rec, a, AT_DATA);        /* the unnamed $DATA */
    if (!a || !a[8] || !stream_start(&v->mft, a) || !decode_runs(v, &v->mft, a)) goto fail;
    if (next_attr(r, rec, NULL, AT_ATTRIBUTE_LIST)) {            /* a fragmented MFT: gather every piece */
        Stream full;
        if (!open_stream(v, 0, AT_DATA, NULL, 0, &full)) goto fail;
        stream_free(&v->mft);
        v->mft = full;
    }
    kfree(r);
    r = NULL;

    Stream vn;                                                   /* the label: $Volume's $VOLUME_NAME */
    if (open_stream(v, 3, AT_VOLUME_NAME, NULL, 0, &vn)) {
        if (vn.resident) utf16_to_utf8(vn.value, (UINT32)(vn.size / 2), v->label, sizeof(v->label));
        stream_free(&vn);
    }
    kprintf("[NTFS] %s: volume \"%s\" at LBA %llu, %llu MiB, %u-byte clusters\n", dev->name, v->label,
            (unsigned long long)lba, (unsigned long long)(total >> 20), cluster);
    return v;
fail:
    kfree(r);
    if (v) { stream_free(&v->mft); kfree(v->bounce); kfree(v); }
    return NULL;
}

void NtfsUnmount(NtfsVol *v)
{
    if (!v) return;
    stream_free(&v->mft);
    kfree(v->bounce);
    kfree(v);
}

const char *NtfsLabel(const NtfsVol *v) { return v->label; }
UINT64 NtfsTotalBytes(const NtfsVol *v) { return v->total; }

/* ---------------------------------------------------------------------------
 * Directories: the $I30 index, a B+ tree of $FILE_NAME keys
 * ------------------------------------------------------------------------- */
static const UINT16 I30[4] = { '$', 'I', '3', '0' };

typedef struct {
    NtfsVol *v;
    UINT64   dir;
    Stream   alloc;                                              /* $INDEX_ALLOCATION */
    UINT32   block;                                              /* index block bytes */
    UINT32   vcn_unit;                                           /* bytes per index VCN */
    bool   (*fn)(const NtfsEntry *e, void *ctx);
    void    *ctx;
    bool     stop, error;
} Walk;

static void emit(Walk *w, const UINT8 *e, UINT32 elen)
{
    UINT16 klen = rd16(e + 0xA);
    if (klen < 0x42 || 0x10u + klen > elen) return;
    const UINT8 *fn = e + 0x10;
    UINT32 nlen = fn[0x40];
    if (0x42 + 2 * nlen > klen || fn[0x41] == 2) return;         /* the DOS 8.3 alias of another entry */
    UINT64 mft = rd64(e) & REF_MASK;
    if (mft < MFT_FIRST_USER || mft == w->dir) return;           /* metafiles ($MFT, ...) and "." */
    NtfsEntry ent;
    memset(&ent, 0, sizeof(ent));
    utf16_to_utf8(fn + 0x42, nlen, ent.name, sizeof(ent.name));
    UINT32 flags = rd32(fn + 0x38);
    ent.mft = mft;
    ent.dir = (flags & 0x10000000u) != 0;
    ent.size = ent.dir ? 0 : rd64(fn + 0x30);
    ent.ctime = rd64(fn + 0x08);
    ent.mtime = rd64(fn + 0x10);
    ent.attrs = (flags & 0x37FFu) | (ent.dir ? 0x10u : 0);       /* FILE_ATTRIBUTE_*; 0x10: DIRECTORY */
    if (!ent.name[0]) return;
    if (!w->fn(&ent, w->ctx)) w->stop = true;
}

static void walk_node(Walk *w, const UINT8 *hdr, UINT32 avail, int depth);

static void walk_child(Walk *w, UINT64 vcn, int depth)
{
    if (depth >= MAX_DEPTH || !w->alloc.size) { w->error = true; return; }
    UINT64 off = vcn * w->vcn_unit;
    if (off / w->vcn_unit != vcn || off + w->block > w->alloc.size) { w->error = true; return; }
    UINT8 *b = kmalloc(w->block);
    if (!b) { w->error = true; return; }
    if (!stream_read(w->v, &w->alloc, off, b, w->block) || !fixup(b, w->block, "INDX")) w->error = true;
    else walk_node(w, b + 0x18, w->block - 0x18, depth + 1);
    kfree(b);
}

/* In order: each entry's subtree (the smaller keys), then the entry */
static void walk_node(Walk *w, const UINT8 *hdr, UINT32 avail, int depth)
{
    UINT32 first = rd32(hdr), used = rd32(hdr + 4);
    if (used > avail) used = avail;
    for (UINT32 o = first; !w->stop && !w->error && o + 0x10 <= used; ) {
        const UINT8 *e = hdr + o;
        UINT16 elen = rd16(e + 8), flags = rd16(e + 0xC);
        if (elen < 0x10 || o + elen > used || ((flags & 1) && elen < 0x18)) { w->error = true; return; }
        if (flags & 1) walk_child(w, rd64(e + elen - 8), depth);
        if (w->stop || w->error) return;
        if (flags & 2) return;                                   /* the last entry carries no key */
        emit(w, e, elen);
        o += elen;
    }
}

bool NtfsList(NtfsVol *v, UINT64 dir, bool (*fn)(const NtfsEntry *e, void *ctx), void *ctx)
{
    Stream root;
    if (!open_stream(v, dir, AT_INDEX_ROOT, I30, 4, &root)) return false;
    Walk w;
    memset(&w, 0, sizeof(w));
    w.v = v; w.dir = dir; w.fn = fn; w.ctx = ctx;
    bool ok = root.resident && root.size >= 0x20;
    if (ok) {
        const UINT8 *r = root.value;
        w.block = rd32(r + 8);
        if (w.block < 512 || w.block > 65536 || w.block % BLOCK_SECTOR) ok = false;
        w.vcn_unit = w.block >= v->cluster ? v->cluster : 512;
        if (ok && (r[0x1C] & 1) && !open_stream(v, dir, AT_INDEX_ALLOCATION, I30, 4, &w.alloc)) ok = false;
        if (ok) walk_node(&w, r + 0x10, (UINT32)root.size - 0x10, 0);
    }
    stream_free(&w.alloc);
    stream_free(&root);
    return ok && !w.error;
}

/* ---------------------------------------------------------------------------
 * File data
 * ------------------------------------------------------------------------- */
bool NtfsSize(NtfsVol *v, UINT64 mft, UINT64 *size)
{
    Stream s;
    if (!open_stream(v, mft, AT_DATA, NULL, 0, &s)) return false;
    *size = s.size;
    stream_free(&s);
    return true;
}

bool NtfsRead(NtfsVol *v, UINT64 mft, UINT64 off, void *buf, UINT64 len)
{
    Stream s;
    if (!open_stream(v, mft, AT_DATA, NULL, 0, &s)) return false;
    bool ok = stream_read(v, &s, off, buf, len);
    stream_free(&s);
    return ok;
}
