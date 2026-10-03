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
 *
 * Writing (NtfsEnableWrite and below) covers what a desktop needs:
 * replacing a file's contents, creating files and directories, renaming
 * and deleting them.  Clusters come from $Bitmap and records from the
 * MFT's $BITMAP (the MFT grows when it is full); records 0-3 are mirrored
 * to $MFTMirr.  A directory's $I30 index is rebuilt as a whole on each
 * change (root, then leaf blocks and separators, sorted by the $UpCase
 * collation), which keeps the B+ tree valid without node-by-node
 * rebalancing.  There is no journalling: when writing starts, $LogFile is
 * reset (filled with 0xFF, as an empty log), so Windows finds a clean
 * volume and starts a new log.  Files with an $ATTRIBUTE_LIST, compressed,
 * sparse or encrypted data are left alone.
 */

#include "ntfs.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"

#define AT_STANDARD_INFO     0x10u
#define AT_ATTRIBUTE_LIST    0x20u
#define AT_FILE_NAME         0x30u
#define AT_VOLUME_NAME       0x60u
#define AT_VOLUME_INFORMATION 0x70u
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
    /* writing */
    bool      rw;
    Stream    cbm;                         /* $Bitmap's $DATA, and it in memory */
    UINT8    *cmap;
    UINT64    clusters, hint, nfree;              /* (nfree: clusters free) */
    Stream    mbm;                         /* $MFT's $BITMAP, and it in memory */
    UINT8    *mmap;
    UINT64    mmap_bytes;
    UINT16   *upcase;                      /* $UpCase */
    UINT32    upcase_n;
    UINT64    mirr_lcn;                    /* $MFTMirr */
    UINT32    mirr_n;
    struct SecEnt *sec;                    /* $Secure's descriptors (as $SII lists them), once read */
    UINT32    nsec;
    bool      sec_read;
};

static UINT16 rd16(const UINT8 *p) { UINT16 v; memcpy(&v, p, 2); return v; }
static void   wr16(UINT8 *p, UINT16 v) { memcpy(p, &v, 2); }
static void   wr32(UINT8 *p, UINT32 v) { memcpy(p, &v, 4); }
static void   wr64(UINT8 *p, UINT64 v) { memcpy(p, &v, 8); }
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
    if (v->rw && v->dev->flush && !v->dev->gone) v->dev->flush(v->dev);
    stream_free(&v->cbm);
    stream_free(&v->mbm);
    kfree(v->cmap);
    kfree(v->mmap);
    kfree(v->upcase);
    kfree(v->sec);
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

/* ===========================================================================
 * Writing
 * ========================================================================= */

#define AT_BITMAP            0xB0u
#define REC_IN_USE           0x0001u
#define REC_IS_DIR           0x0002u
#define FIRST_FREE_REC       24            /* 16-23 are reserved for the system */
#define MFT_GROW             64            /* records added when the MFT is full */
#define FN_DIR_FLAG          0x10000000u   /* $FILE_NAME flags: a directory */

static UINT64 (*g_now)(void);
void NtfsSetClock(UINT64 (*now)(void)) { g_now = now; }
static UINT64 now_ft(void) { return g_now ? g_now() : 0; }

static inline UINT32 align8(UINT32 x) { return (x + 7) & ~7u; }

/* Write @len bytes at volume byte offset @off (sector-aligned both) */
static bool dev_write(NtfsVol *v, UINT64 off, UINT64 len, const void *buf)
{
    const UINT8 *p = buf;
    UINT64 lba = v->base + off / BLOCK_SECTOR, n = len / BLOCK_SECTOR;
    while (n) {
        UINT32 k = n > 128 ? 128 : (UINT32)n;
        if (!v->dev->write(v->dev, lba, k, p)) return false;
        lba += k; p += (UINT64)k * BLOCK_SECTOR; n -= k;
    }
    return true;
}

/* Stream bytes [off, off+len) to the disk, inside allocated (not sparse) runs */
static bool write_raw(NtfsVol *v, const Stream *s, UINT64 off, const UINT8 *in, UINT64 len)
{
    while (len) {
        UINT64 vcn = off / v->cluster, at = off % v->cluster, count;
        UINT64 lcn = map_vcn(s, vcn, &count);
        if (!count || lcn == SPARSE) return false;
        UINT64 span = count * v->cluster - at;
        if (span > len) span = len;
        UINT64 disk = lcn * v->cluster + at, done = 0;
        while (done < span) {
            UINT64 pos = disk + done, head = pos % BLOCK_SECTOR, want = span - done;
            if (!head && want >= BLOCK_SECTOR) {
                UINT64 k = want - want % BLOCK_SECTOR;
                if (!dev_write(v, pos, k, in + done)) return false;
                done += k;
            } else {                                             /* read, change, write a sector */
                UINT64 k = BLOCK_SECTOR - head;
                if (k > want) k = want;
                if (!dev_read(v, pos - head, BLOCK_SECTOR, v->bounce)) return false;
                memcpy(v->bounce + head, in + done, k);
                if (!dev_write(v, pos - head, BLOCK_SECTOR, v->bounce)) return false;
                done += k;
            }
        }
        off += span; in += span; len -= span;
    }
    return true;
}

/* Protect a multi-sector record with its update sequence, write it with
 * @put, then take the protection off again */
static void usa_apply(UINT8 *rec, UINT32 size)
{
    UINT16 off = rd16(rec + 4), count = rd16(rec + 6);
    UINT16 usn = (UINT16)(rd16(rec + off) + 1);
    if (usn == 0 || usn == 0xFFFF) usn = 1;
    wr16(rec + off, usn);
    for (UINT32 i = 1; i < count && i * BLOCK_SECTOR <= size; i++) {
        UINT8 *end = rec + i * BLOCK_SECTOR - 2;
        memcpy(rec + off + 2 * i, end, 2);
        wr16(end, usn);
    }
}

static void usa_undo(UINT8 *rec, UINT32 size)
{
    UINT16 off = rd16(rec + 4), count = rd16(rec + 6);
    for (UINT32 i = 1; i < count && i * BLOCK_SECTOR <= size; i++)
        memcpy(rec + i * BLOCK_SECTOR - 2, rec + off + 2 * i, 2);
}

static bool write_record(NtfsVol *v, UINT64 n, UINT8 *rec)
{
    usa_apply(rec, v->rec_size);
    bool ok = write_raw(v, &v->mft, n * v->rec_size, rec, v->rec_size);
    if (ok && n < v->mirr_n)
        ok = dev_write(v, v->mirr_lcn * v->cluster + n * v->rec_size, v->rec_size, rec);
    usa_undo(rec, v->rec_size);
    return ok;
}

/* A record whether or not it is in use; false if it was never formatted */
static bool read_record_any(NtfsVol *v, UINT64 n, UINT8 *rec)
{
    UINT64 off = n * v->rec_size;
    if (off + v->rec_size > v->mft.size || !stream_read(v, &v->mft, off, rec, v->rec_size)) return false;
    return fixup(rec, v->rec_size, "FILE");
}

/* ---- bitmaps ---- */

static inline bool bit(const UINT8 *m, UINT64 i) { return (m[i >> 3] >> (i & 7)) & 1; }
static inline void set_bit(UINT8 *m, UINT64 i, bool on)
{
    if (on) m[i >> 3] |= (UINT8)(1u << (i & 7));
    else    m[i >> 3] &= (UINT8)~(1u << (i & 7));
}

/* Write back the sectors of a bitmap stream holding bits [first, last] */
static bool flush_bits(NtfsVol *v, const Stream *s, const UINT8 *map, UINT64 bytes, UINT64 first, UINT64 last)
{
    UINT64 a = (first >> 3) & ~(UINT64)(BLOCK_SECTOR - 1), b = (last >> 3) + 1;
    b = (b + BLOCK_SECTOR - 1) & ~(UINT64)(BLOCK_SECTOR - 1);
    if (b > bytes) b = bytes;
    if (s->resident) return false;
    return write_raw(v, s, a, map + a, b - a);
}

static void free_runs(NtfsVol *v, const Run *r, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++) {
        if (r[i].lcn == SPARSE) continue;
        for (UINT64 c = 0; c < r[i].len; c++)
            if (bit(v->cmap, r[i].lcn + c)) { set_bit(v->cmap, r[i].lcn + c, false); v->nfree++; }
        flush_bits(v, &v->cbm, v->cmap, v->cbm.size, r[i].lcn, r[i].lcn + r[i].len - 1);
    }
}

/* Allocate @count clusters as runs appended to @s from VCN @vcn: one
 * extent if a free one is long enough, else the free extents in order */
static bool alloc_clusters(NtfsVol *v, Stream *s, UINT64 vcn, UINT64 count)
{
    UINT64 total = v->clusters;
    UINT32 first_new = s->nruns;
    UINT64 start = 0, len = 0, found = SPARSE;
    /* Right after the stream's last extent, so it stays in one piece */
    if (s->nruns && s->runs[s->nruns - 1].lcn != SPARSE && s->runs[s->nruns - 1].vcn + s->runs[s->nruns - 1].len == vcn) {
        UINT64 at = s->runs[s->nruns - 1].lcn + s->runs[s->nruns - 1].len, k = 0;
        while (k < count && at + k < total && !bit(v->cmap, at + k)) k++;
        if (k == count) found = at;
    }
    /* Else a single extent, from the hint on */
    for (UINT64 pass = 0, c = v->hint; pass < total && found == SPARSE; pass++, c++) {
        if (c >= total) { c = 0; len = 0; }
        if (bit(v->cmap, c)) { len = 0; continue; }
        if (!len) start = c;
        if (++len == count) found = start;
    }
    UINT64 left = count;
    if (found != SPARSE) {
        if (!add_run(s, vcn, found, count)) return false;
        left = 0;
    } else {
        for (UINT64 c = 0; c < total && left; ) {
            if (bit(v->cmap, c)) { c++; continue; }
            UINT64 n = 0;
            while (c + n < total && n < left && !bit(v->cmap, c + n)) n++;
            if (!add_run(s, vcn, c, n)) { s->nruns = first_new; return false; }
            vcn += n; left -= n; c += n;
        }
    }
    if (left) { s->nruns = first_new; return false; }           /* the volume is full */
    for (UINT32 i = first_new; i < s->nruns; i++) {
        for (UINT64 c = 0; c < s->runs[i].len; c++) set_bit(v->cmap, s->runs[i].lcn + c, true);
        v->nfree -= s->runs[i].len;
        if (!flush_bits(v, &v->cbm, v->cmap, v->cbm.size, s->runs[i].lcn, s->runs[i].lcn + s->runs[i].len - 1))
            return false;
        v->hint = s->runs[i].lcn + s->runs[i].len;
    }
    /* merge with the run before when it continues it */
    if (first_new > 0 && s->nruns > first_new) {
        Run *p = &s->runs[first_new - 1], *q = &s->runs[first_new];
        if (p->lcn != SPARSE && p->lcn + p->len == q->lcn && p->vcn + p->len == q->vcn) {
            p->len += q->len;
            memmove(q, q + 1, (s->nruns - first_new - 1) * sizeof(Run));
            s->nruns--;
        }
    }
    return true;
}

static int signed_bytes(INT64 x)
{
    int n = 1;
    while (n < 8 && !(x >= -(1LL << (8 * n - 1)) && x < (1LL << (8 * n - 1)))) n++;
    return n;
}

/* Mapping pairs for @n runs; bytes written (with the terminator), or -1 */
static int encode_runs(const Run *r, UINT32 n, UINT8 *out, int cap)
{
    int o = 0;
    INT64 prev = 0;
    for (UINT32 i = 0; i < n; i++) {
        int nl = signed_bytes((INT64)r[i].len), no = 0;
        INT64 delta = 0;
        if (r[i].lcn != SPARSE) { delta = (INT64)r[i].lcn - prev; no = signed_bytes(delta); prev = (INT64)r[i].lcn; }
        if (o + 1 + nl + no + 1 > cap) return -1;
        out[o++] = (UINT8)(nl | no << 4);
        for (int k = 0; k < nl; k++) out[o++] = (UINT8)(r[i].len >> (8 * k));
        for (int k = 0; k < no; k++) out[o++] = (UINT8)((UINT64)delta >> (8 * k));
    }
    out[o++] = 0;
    return o;
}

/* ---- attributes in a record buffer ---- */

static UINT32 rec_used(const UINT8 *rec) { return rd32(rec + 0x18); }

static UINT8 *find_attr(UINT8 *rec, UINT32 size, UINT32 type, const UINT16 *name, UINT32 nlen)
{
    for (const UINT8 *a = NULL; (a = next_attr(rec, size, a, type)); )
        if (name_eq(a, name, nlen)) return (UINT8 *)a;
    return NULL;
}

static void remove_attr(UINT8 *rec, UINT8 *a)
{
    UINT32 len = rd32(a + 4), used = rec_used(rec), at = (UINT32)(a - rec);
    memmove(a, a + len, used - at - len);
    wr32(rec + 0x18, used - len);
}

static UINT16 up(NtfsVol *v, UINT16 c);

/* Attributes of one type are kept in the order of their names (upcased,
 * then exactly; no name first) */
static int attr_name_cmp(NtfsVol *v, const UINT8 *a, const UINT8 *b)
{
    UINT32 na = a[9], nb = b[9], n = na < nb ? na : nb;
    const UINT8 *pa = a + rd16(a + 0xA), *pb = b + rd16(b + 0xA);
    for (int exact = 0; exact < 2; exact++) {
        for (UINT32 i = 0; i < n; i++) {
            UINT16 x = rd16(pa + 2 * i), y = rd16(pb + 2 * i);
            if (!exact) { x = up(v, x); y = up(v, y); }
            if (x != y) return x < y ? -1 : 1;
        }
        if (na != nb) return na < nb ? -1 : 1;
    }
    return 0;
}

/* Insert attribute @attr (@len bytes, instance filled in) in type order;
 * NULL if the record has no room */
static UINT8 *insert_attr(NtfsVol *v, UINT8 *rec, const UINT8 *attr, UINT32 len)
{
    UINT32 used = rec_used(rec);
    if (used + len > v->rec_size) return NULL;
    UINT32 type = rd32(attr), off = rd16(rec + 0x14);
    while (off + 8 <= used) {
        UINT32 t = rd32(rec + off);
        if (t == AT_END || t > type) break;
        if (t == type && attr_name_cmp(v, rec + off, attr) > 0) break;   /* (same type: by name) */
        off += rd32(rec + off + 4);
    }
    memmove(rec + off + len, rec + off, used - off);
    memcpy(rec + off, attr, len);
    UINT16 inst = rd16(rec + 0x28);
    wr16(rec + off + 0xE, inst);
    wr16(rec + 0x28, (UINT16)(inst + 1));
    wr32(rec + 0x18, used + len);
    return rec + off;
}

/* A resident attribute into @out; its length */
static UINT32 make_resident(UINT8 *out, UINT32 type, const UINT16 *name, UINT32 nlen,
                            const void *val, UINT32 vlen, UINT8 rflags)
{
    UINT32 voff = align8(0x18 + 2 * nlen), len = align8(voff + vlen);
    memset(out, 0, len);
    wr32(out, type);
    wr32(out + 4, len);
    out[8] = 0;
    out[9] = (UINT8)nlen;
    wr16(out + 0xA, 0x18);
    for (UINT32 i = 0; i < nlen; i++) wr16(out + 0x18 + 2 * i, name[i]);
    wr32(out + 0x10, vlen);
    wr16(out + 0x14, (UINT16)voff);
    out[0x16] = rflags;
    if (vlen) memcpy(out + voff, val, vlen);
    return len;
}

/* A non-resident attribute into @out (@cap bytes); its length, 0 if the runs don't fit */
static UINT32 make_nonresident(NtfsVol *v, UINT8 *out, UINT32 cap, UINT32 type, const UINT16 *name, UINT32 nlen,
                               const Stream *s, UINT64 alloc, UINT64 size)
{
    UINT32 mp = align8(0x40 + 2 * nlen);
    if (mp >= cap) return 0;
    memset(out, 0, cap);
    int n = encode_runs(s->runs, s->nruns, out + mp, (int)(cap - mp));
    if (n < 0) return 0;
    UINT32 len = align8(mp + (UINT32)n);
    if (len > cap) return 0;
    wr32(out, type);
    wr32(out + 4, len);
    out[8] = 1;
    out[9] = (UINT8)nlen;
    wr16(out + 0xA, 0x40);
    for (UINT32 i = 0; i < nlen; i++) wr16(out + 0x40 + 2 * i, name[i]);
    wr64(out + 0x10, 0);
    wr64(out + 0x18, alloc / v->cluster - 1);                  /* (all ones when empty) */
    wr16(out + 0x20, (UINT16)mp);
    wr64(out + 0x28, alloc);
    wr64(out + 0x30, size);
    wr64(out + 0x38, size);
    return len;
}

/* ---- names ---- */

/* UTF-8 → UTF-16; the number of units, or -1 if too long or not valid */
static int utf8_to_utf16(const char *s, UINT16 *out, int cap)
{
    int n = 0;
    const UINT8 *p = (const UINT8 *)s;
    while (*p) {
        UINT32 c = *p++;
        int more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (more < 0) return -1;
        if (more) c &= 0x3Fu >> more;
        for (int i = 0; i < more; i++) {
            if ((*p & 0xC0) != 0x80) return -1;
            c = c << 6 | (*p++ & 0x3F);
        }
        if (c >= 0x10000) {
            if (n + 2 > cap) return -1;
            c -= 0x10000;
            out[n++] = (UINT16)(0xD800 + (c >> 10));
            out[n++] = (UINT16)(0xDC00 + (c & 0x3FF));
        } else {
            if (n + 1 > cap) return -1;
            out[n++] = (UINT16)c;
        }
    }
    return n;
}

static UINT16 up(NtfsVol *v, UINT16 c)
{
    if (c < v->upcase_n) return v->upcase[c];
    return c >= 'a' && c <= 'z' ? (UINT16)(c - 32) : c;
}

/* $FILE_NAME collation: upcased first, then exactly */
static int collate(NtfsVol *v, const UINT8 *k1, const UINT8 *k2)
{
    UINT32 n1 = k1[0x40], n2 = k2[0x40], n = n1 < n2 ? n1 : n2;
    for (UINT32 i = 0; i < n; i++) {
        UINT16 a = up(v, rd16(k1 + 0x42 + 2 * i)), b = up(v, rd16(k2 + 0x42 + 2 * i));
        if (a != b) return a < b ? -1 : 1;
    }
    if (n1 != n2) return n1 < n2 ? -1 : 1;
    for (UINT32 i = 0; i < n; i++) {
        UINT16 a = rd16(k1 + 0x42 + 2 * i), b = rd16(k2 + 0x42 + 2 * i);
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

/* ---- directory indexes ---- */

typedef struct { UINT8 *e; UINT32 len; } Ent;            /* an index entry in leaf form */

typedef struct {
    NtfsVol *v;
    Stream   alloc;
    UINT32   block, vcn_unit;
    Ent     *ents;
    UINT32   n, cap;
    UINT16   minkey;                       /* shortest valid key */
    bool     error;
} Collect;

static bool collect_add(Collect *c, const UINT8 *e)
{
    UINT16 klen = rd16(e + 0xA);
    UINT32 len = rd16(e + 8) - ((rd16(e + 0xC) & 1) ? 8u : 0u);
    if (klen < c->minkey || 0x10 + (UINT32)klen > len || len > 0x10 + 0x42 + 2 * 255 + 8) return false;
    if (c->n == c->cap) {
        UINT32 cap = c->cap ? c->cap * 2 : 64;
        Ent *ne = kmalloc(cap * sizeof(Ent));
        if (!ne) return false;
        if (c->n) memcpy(ne, c->ents, c->n * sizeof(Ent));
        kfree(c->ents);
        c->ents = ne;
        c->cap = cap;
    }
    UINT8 *copy = kzalloc(len);
    if (!copy) return false;
    memcpy(copy, e, len);
    wr16(copy + 8, (UINT16)len);
    wr16(copy + 0xC, 0);                                       /* leaf form: no subnode */
    c->ents[c->n++] = (Ent){ copy, len };
    return true;
}

static void collect_node(Collect *c, const UINT8 *hdr, UINT32 avail, int depth);

static void collect_child(Collect *c, UINT64 vcn, int depth)
{
    if (depth >= MAX_DEPTH) { c->error = true; return; }
    UINT64 off = vcn * c->vcn_unit;
    if (off + c->block > c->alloc.size) { c->error = true; return; }
    UINT8 *b = kmalloc(c->block);
    if (!b) { c->error = true; return; }
    if (!stream_read(c->v, &c->alloc, off, b, c->block) || !fixup(b, c->block, "INDX")) c->error = true;
    else collect_node(c, b + 0x18, c->block - 0x18, depth + 1);
    kfree(b);
}

static void collect_node(Collect *c, const UINT8 *hdr, UINT32 avail, int depth)
{
    UINT32 first = rd32(hdr), used = rd32(hdr + 4);
    if (used > avail) used = avail;
    for (UINT32 o = first; !c->error && o + 0x10 <= used; ) {
        const UINT8 *e = hdr + o;
        UINT16 elen = rd16(e + 8), flags = rd16(e + 0xC);
        if (elen < 0x10 || o + elen > used) { c->error = true; return; }
        if (flags & 1) collect_child(c, rd64(e + elen - 8), depth);
        if (flags & 2) return;
        if (!collect_add(c, e)) { c->error = true; return; }
        o += elen;
    }
}

static void ents_free(Ent *e, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++) kfree(e[i].e);
    kfree(e);
}

/* Every entry of index @name (4 characters) of record @mft, in order (@rec
 * is the record), in leaf form */
static bool load_index_named(NtfsVol *v, UINT64 mft, UINT8 *rec, const UINT16 *name, Ent **out, UINT32 *n)
{
    Collect c;
    memset(&c, 0, sizeof(c));
    c.v = v;
    c.minkey = name == I30 ? 0x42 : 4;
    UINT8 *root = find_attr(rec, v->rec_size, AT_INDEX_ROOT, name, 4);
    if (!root || root[8]) return false;
    const UINT8 *r = root + rd16(root + 0x14);
    UINT32 rlen = rd32(root + 0x10);
    if (rlen < 0x20) return false;
    c.block = rd32(r + 8);
    c.vcn_unit = c.block >= v->cluster ? v->cluster : 512;
    if ((r[0x1C] & 1) && !open_stream(v, mft, AT_INDEX_ALLOCATION, name, 4, &c.alloc)) return false;
    collect_node(&c, r + 0x10, rlen - 0x10, 0);
    stream_free(&c.alloc);
    if (c.error) { ents_free(c.ents, c.n); return false; }
    *out = c.ents;
    *n = c.n;
    return true;
}

/* Every entry of directory @dir's index, in order (@rec is its record) */
static bool load_index(NtfsVol *v, UINT64 dir, UINT8 *rec, Ent **out, UINT32 *n)
{
    return load_index_named(v, dir, rec, I30, out, n);
}

/* An entry with a subnode pointer (internal form) */
static Ent with_sub(const Ent *e, UINT64 vcn)
{
    Ent r = { kzalloc(e->len + 8), e->len + 8 };
    if (!r.e) return r;
    memcpy(r.e, e->e, e->len);
    wr16(r.e + 8, (UINT16)r.len);
    wr16(r.e + 0xC, 1);
    wr64(r.e + r.len - 8, vcn);
    return r;
}

/* Lay out entries @es (each with or without a subnode) followed by an END
 * entry (with @last_sub, unless it is ~0) at @hdr */
static UINT32 put_entries(UINT8 *hdr, UINT32 first, const Ent *es, UINT32 n, UINT64 last_sub)
{
    UINT32 o = first;
    for (UINT32 i = 0; i < n; i++) { memcpy(hdr + o, es[i].e, es[i].len); o += es[i].len; }
    UINT8 *end = hdr + o;
    bool sub = last_sub != ~0ull;
    memset(end, 0, sub ? 0x18 : 0x10);
    wr16(end + 8, sub ? 0x18 : 0x10);
    wr16(end + 0xC, (UINT16)(2 | (sub ? 1 : 0)));
    if (sub) wr64(end + 0x10, last_sub);
    return o + (sub ? 0x18 : 0x10);
}

typedef struct {
    UINT8  **blocks;                       /* built index blocks */
    UINT32   n, cap;
} Blocks;

/* Pack one level of @items (leaf entries, or entries with subnodes) into
 * blocks.  Returns the separators for the level above (each pointing at
 * the block before it) and, in *trail, the last block.  @trail_in is the
 * pointer after the last item (internal levels), ~0 for leaves. */
static bool pack_level(NtfsVol *v, UINT32 bsize, Blocks *b, Ent *items, UINT32 n, UINT64 trail_in,
                       Ent **seps, UINT32 *nseps, UINT64 *trail)
{
    bool leaf = trail_in == ~0ull;
    UINT32 cap = bsize - 0x40 - (leaf ? 0x10 : 0x18);
    Ent *out = kmalloc((n + 1) * sizeof(Ent));
    if (!out) return false;
    UINT32 no = 0, start = 0, used = 0;
    for (UINT32 i = 0; i <= n; i++) {
        bool last = i == n;
        if (!last && used + items[i].len <= cap) { used += items[i].len; continue; }
        /* close the block: items[start .. i) (all of them when @last) */
        UINT32 end = i, sep = ~0u;
        if (!last) { sep = i - 1; end = i - 1; }                 /* its last item goes up */
        if (!last && end <= start) { kfree(out); return false; } /* (an entry larger than a block) */
        UINT8 *blk = kzalloc(bsize);
        if (!blk) { kfree(out); return false; }
        if (b->n == b->cap) {
            UINT32 nc = b->cap ? b->cap * 2 : 8;
            UINT8 **nb = kmalloc(nc * sizeof(UINT8 *));
            if (!nb) { kfree(blk); kfree(out); return false; }
            if (b->n) memcpy(nb, b->blocks, b->n * sizeof(UINT8 *));
            kfree(b->blocks);
            b->blocks = nb;
            b->cap = nc;
        }
        UINT64 vcn_self = b->n;                                  /* block number for now */
        b->blocks[b->n++] = blk;
        UINT64 end_sub = leaf ? ~0ull : last ? trail_in : rd64(items[sep].e + items[sep].len - 8);
        memcpy(blk, "INDX", 4);
        wr16(blk + 4, 0x28);
        wr16(blk + 6, (UINT16)(bsize / BLOCK_SECTOR + 1));
        wr64(blk + 0x10, vcn_self);                              /* (a block number; made a VCN when written) */
        UINT8 *hdr = blk + 0x18;
        UINT32 len = put_entries(hdr, 0x28, items + start, end - start, end_sub);
        wr32(hdr, 0x28);
        wr32(hdr + 4, len);
        wr32(hdr + 8, bsize - 0x18);
        hdr[0xC] = leaf ? 0 : 1;
        if (last) { *trail = vcn_self; break; }
        /* the separator, pointing at this block */
        Ent base = items[sep];
        if (!leaf) { base.len -= 8; }                            /* drop its old subnode */
        Ent s = with_sub(&base, vcn_self);
        if (!s.e) { kfree(out); return false; }
        wr16(s.e + 0xC, 1);
        out[no++] = s;
        start = i;
        used = items[i].len;
    }
    *seps = out;
    *nseps = no;
    return true;
}

/* Turn block numbers stored in entries and headers into VCNs */
static void blocks_to_vcns(UINT8 *hdr, UINT32 vcn_per_block)
{
    UINT32 first = rd32(hdr), used = rd32(hdr + 4);
    for (UINT32 o = first; o + 0x10 <= used; ) {
        UINT8 *e = hdr + o;
        UINT16 elen = rd16(e + 8), flags = rd16(e + 0xC);
        if (flags & 1) wr64(e + elen - 8, rd64(e + elen - 8) * vcn_per_block);
        if (flags & 2) break;
        o += elen;
    }
}

static const UINT8 g_end_leaf[0x10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 2, 0, 0, 0 };

/* Write index @name of record @dir anew from @ents (sorted, leaf form).
 * @rec is the record, updated and written. */
static bool write_index_named(NtfsVol *v, UINT64 dir, UINT8 *rec, const UINT16 *name, Ent *ents, UINT32 n)
{
    UINT8 *root = find_attr(rec, v->rec_size, AT_INDEX_ROOT, name, 4);
    if (!root || root[8]) return false;
    UINT8 rhead[0x10];
    memcpy(rhead, root + rd16(root + 0x14), 0x10);             /* type, collation, block size, clusters */
    UINT32 bsize = rd32(rhead + 8);
    UINT32 vcn_unit = bsize >= v->cluster ? v->cluster : 512;
    UINT32 vpb = bsize / vcn_unit;

    /* The existing allocation (kept, and grown if needed) */
    Stream alloc;
    memset(&alloc, 0, sizeof(alloc));
    UINT8 *aa = find_attr(rec, v->rec_size, AT_INDEX_ALLOCATION, name, 4);
    if (aa && !open_stream(v, dir, AT_INDEX_ALLOCATION, name, 4, &alloc)) return false;
    UINT8 *ba = find_attr(rec, v->rec_size, AT_BITMAP, name, 4);
    if (ba && ba[8]) { stream_free(&alloc); return false; }     /* (a non-resident index bitmap: not handled) */

    /* Room for the root: the record without the root and index attributes */
    UINT32 other = rec_used(rec) - rd32(root + 4) - (aa ? rd32(aa + 4) : 0) - (ba ? rd32(ba + 4) : 0);
    UINT32 root_fixed = 0x20 + 0x20;                             /* header + "$I30", root header, index header */
    UINT32 total = 0;
    for (UINT32 i = 0; i < n; i++) total += ents[i].len;

    Blocks b = { NULL, 0, 0 };
    Ent *top = ents, *level = NULL;
    UINT32 ntop = n;
    UINT64 trail = ~0ull;
    bool large = false, ok = false;
    /* ($Secure holds two indexes and a stream that grows: each index root
     * keeps to a quarter of the record) */
    UINT32 cap = name == I30 ? v->rec_size : v->rec_size / 4;
    if (other + root_fixed + total + 0x10 > v->rec_size || root_fixed + total + 0x10 > cap) {
        large = true;
        INT32 room = (INT32)v->rec_size - (INT32)(other + root_fixed + 0x18 + 0x80 + 0x30);  /* (the allocation and bitmap attributes) */
        if (room > (INT32)cap - (INT32)(root_fixed + 0x18)) room = (INT32)cap - (INT32)(root_fixed + 0x18);
        if (room < 0) goto out;
        Ent *items = ents;
        UINT32 nitems = n;
        UINT64 t_in = ~0ull;
        for (int guard = 0; guard < MAX_DEPTH; guard++) {
            Ent *seps; UINT32 nseps; UINT64 t;
            if (!pack_level(v, bsize, &b, items, nitems, t_in, &seps, &nseps, &t)) goto out;
            if (level) ents_free(level, nitems);
            level = seps;
            items = seps; nitems = nseps; t_in = t;
            UINT32 sz = 0;
            for (UINT32 i = 0; i < nseps; i++) sz += seps[i].len;
            if (sz <= (UINT32)room) break;
        }
        top = items; ntop = nitems; trail = t_in;
    }

    /* Clusters for the blocks */
    UINT64 need = (UINT64)b.n * bsize;
    if (large && alloc.size < need) {
        UINT64 have = alloc.resident ? 0 : alloc.size;
        UINT64 more = (need - have + v->cluster - 1) / v->cluster;
        UINT64 slack = (have / 4 + bsize - 1) / bsize * bsize / v->cluster;   /* (grow by a quarter more: fewer pieces) */
        if ((have + (more + slack) * v->cluster) / bsize <= 512) more += slack;
        if (alloc.resident) { stream_free(&alloc); }
        if (!alloc_clusters(v, &alloc, have / v->cluster, more)) goto out;
        alloc.size = alloc.init_size = have + more * v->cluster;
    }
    for (UINT32 i = 0; i < b.n; i++) {
        UINT8 *blk = b.blocks[i];
        wr64(blk + 0x10, (UINT64)i * vpb);
        blocks_to_vcns(blk + 0x18, vpb);
        usa_apply(blk, bsize);
        bool w = write_raw(v, &alloc, (UINT64)i * bsize, blk, bsize);
        usa_undo(blk, bsize);
        if (!w) goto out;
    }

    /* The root */
    UINT32 rv_len = 0x20;
    for (UINT32 i = 0; i < ntop; i++) rv_len += top[i].len;
    rv_len += large ? 0x18 : 0x10;
    UINT8 *rv = kzalloc(rv_len);
    if (!rv) goto out;
    memcpy(rv, rhead, 0x10);
    UINT8 *ih = rv + 0x10;
    UINT32 used = put_entries(ih, 0x10, top, ntop, large ? trail : ~0ull);
    wr32(ih, 0x10);
    wr32(ih + 4, used);
    wr32(ih + 8, used);
    ih[0xC] = large ? 1 : 0;
    if (large) blocks_to_vcns(ih, vpb);

    UINT8 *buf = kmalloc(v->rec_size);
    if (!buf) { kfree(rv); goto out; }
    /* drop the old root and index attributes, then add the new ones */
    remove_attr(rec, find_attr(rec, v->rec_size, AT_INDEX_ROOT, name, 4));
    if ((aa = find_attr(rec, v->rec_size, AT_INDEX_ALLOCATION, name, 4))) remove_attr(rec, aa);
    if ((ba = find_attr(rec, v->rec_size, AT_BITMAP, name, 4))) remove_attr(rec, ba);
    UINT32 len = make_resident(buf, AT_INDEX_ROOT, name, 4, rv, rv_len, 0);
    kfree(rv);
    bool fit = insert_attr(v, rec, buf, len) != NULL;
    if (fit && large) {
        len = make_nonresident(v, buf, v->rec_size / 2, AT_INDEX_ALLOCATION, name, 4, &alloc, alloc.size, alloc.size);
        fit = len && insert_attr(v, rec, buf, len);
        UINT64 nblk = alloc.size / bsize;
        UINT8 bm[64];
        UINT32 bml = (UINT32)((nblk + 63) / 64 * 8);
        if (bml > sizeof(bm)) fit = false;
        if (fit) {
            memset(bm, 0, sizeof(bm));
            for (UINT32 i = 0; i < b.n; i++) set_bit(bm, i, true);
            len = make_resident(buf, AT_BITMAP, name, 4, bm, bml, 0);
            fit = insert_attr(v, rec, buf, len) != NULL;
        }
    } else if (fit && !alloc.resident && alloc.nruns) {
        free_runs(v, alloc.runs, alloc.nruns);                   /* small again: give the blocks back */
    }
    kfree(buf);
    if (!fit) { kprintf("[NTFS] record %llu: its index does not fit\n", (unsigned long long)dir); goto out; }
    ok = write_record(v, dir, rec);
out:
    for (UINT32 i = 0; i < b.n; i++) kfree(b.blocks[i]);
    kfree(b.blocks);
    if (level) ents_free(level, ntop);
    stream_free(&alloc);
    return ok;
}

static bool write_index(NtfsVol *v, UINT64 dir, UINT8 *rec, Ent *ents, UINT32 n)
{
    return write_index_named(v, dir, rec, I30, ents, n);
}

/* ---- enabling writes ---- */

/* Windows left the volume as it would find it again: no transactions
 * waiting in $LogFile, no chkdsk pending, not hibernated (Fast Startup
 * hibernates too).  Writing to it otherwise would lose Windows's changes,
 * so it stays read-only, as ntfs-3g refuses it. */
static bool hiber_one(const NtfsEntry *e, void *ctx)
{
    static const char want[] = "hiberfil.sys";
    for (int i = 0; ; i++) {
        char a = e->name[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return true;
        if (!a) break;
    }
    *(UINT64 *)ctx = e->mft;
    return false;
}

static const char *unclean(NtfsVol *v)
{
    Stream st;
    if (open_stream(v, 3, AT_VOLUME_INFORMATION, NULL, 0, &st)) {
        bool dirty = st.resident && st.size >= 12 && (rd16((const UINT8 *)st.value + 10) & 0x0001);
        stream_free(&st);
        if (dirty) return "marked dirty (chkdsk is pending)";
    }
    if (!open_stream(v, 2, AT_DATA, NULL, 0, &st)) return "no $LogFile";
    UINT8 *p = kmalloc(512);
    bool ok = p && st.size >= 4096 && stream_read(v, &st, 0, p, 512);
    stream_free(&st);
    const char *why = ok ? NULL : "$LogFile unreadable";
    if (ok && !memcmp(p, "RSTR", 4)) {
        UINT16 ra = rd16(p + 0x18);
        if (ra + 0x10 > 512) why = "$LogFile unreadable";
        else if (rd16(p + ra + 0x0C) != 0xFFFF && !(rd16(p + ra + 0x0E) & 0x0002))
            why = "$LogFile has transactions Windows has not finished";
    } else if (ok) {
        for (int i = 0; i < 512 && !why; i++)                 /* all 0xFF: an emptied log */
            if (p[i] != 0xFF) why = "$LogFile not recognised";
    }
    kfree(p);
    if (why) return why;
    UINT64 hib = 0;
    NtfsList(v, NTFS_ROOT, hiber_one, &hib);
    char sig[4] = { 0 };
    if (hib && NtfsRead(v, hib, 0, sig, 4)) {
        for (int i = 0; i < 4; i++) if (sig[i] >= 'A' && sig[i] <= 'Z') sig[i] = (char)(sig[i] + 32);
        if (!memcmp(sig, "hibr", 4)) return "Windows is hibernated on it (Fast Startup)";
    }
    return NULL;
}

bool NtfsEnableWrite(NtfsVol *v)
{
    if (v->rw) return true;
    const char *why = unclean(v);
    if (why) {
        kprintf("[NTFS] %s: \"%s\" stays read-only: %s\n", v->dev->name, v->label, why);
        return false;
    }
    /* $Bitmap */
    if (!open_stream(v, 6, AT_DATA, NULL, 0, &v->cbm) || v->cbm.resident) goto fail;
    v->clusters = v->total / v->cluster;
    if (v->cbm.size < (v->clusters + 7) / 8 || v->cbm.size > (512u << 20)) goto fail;
    v->cmap = kmalloc(v->cbm.size);
    if (!v->cmap || !stream_read(v, &v->cbm, 0, v->cmap, v->cbm.size)) goto fail;
    /* $MFT's $BITMAP */
    if (!open_stream(v, 0, AT_BITMAP, NULL, 0, &v->mbm) || v->mbm.resident) goto fail;
    v->mmap_bytes = v->mbm.size;
    v->mmap = kmalloc(v->mmap_bytes ? v->mmap_bytes : 8);
    if (!v->mmap || !stream_read(v, &v->mbm, 0, v->mmap, v->mmap_bytes)) goto fail;
    /* $UpCase */
    Stream uc;
    if (!open_stream(v, 10, AT_DATA, NULL, 0, &uc)) goto fail;
    v->upcase_n = (UINT32)(uc.size / 2);
    if (v->upcase_n > 65536) v->upcase_n = 65536;
    v->upcase = kmalloc(v->upcase_n * 2 + 2);
    bool ucok = v->upcase && stream_read(v, &uc, 0, v->upcase, (UINT64)v->upcase_n * 2);
    stream_free(&uc);
    if (!ucok) goto fail;
    /* $MFTMirr: where, and how many records */
    Stream mm;
    if (!open_stream(v, 1, AT_DATA, NULL, 0, &mm) || mm.resident || !mm.nruns) { stream_free(&mm); goto fail; }
    v->mirr_lcn = mm.runs[0].lcn;
    v->mirr_n = (UINT32)(mm.size / v->rec_size);
    if (v->mirr_n > 4) v->mirr_n = 4;
    if (mm.runs[0].len * v->cluster < (UINT64)v->mirr_n * v->rec_size) v->mirr_n = 0;
    stream_free(&mm);
    /* $LogFile: an empty log (all 0xFF), so nothing is replayed over our writes */
    Stream lf;
    if (!open_stream(v, 2, AT_DATA, NULL, 0, &lf) || lf.resident) { stream_free(&lf); goto fail; }
    UINT8 *ff = kmalloc(BOUNCE);
    bool lok = ff != NULL;
    if (ff) memset(ff, 0xFF, BOUNCE);
    for (UINT64 o = 0; lok && o < lf.size; o += BOUNCE)
        lok = write_raw(v, &lf, o, ff, lf.size - o < BOUNCE ? lf.size - o : BOUNCE);
    kfree(ff);
    stream_free(&lf);
    if (!lok) goto fail;
    v->hint = 0;
    v->nfree = 0;
    for (UINT64 c = 0; c < v->clusters; c++) if (!bit(v->cmap, c)) v->nfree++;
    v->rw = true;
    kprintf("[NTFS] %s: \"%s\" is writable (%llu of %llu clusters free)\n", v->dev->name, v->label,
            (unsigned long long)NtfsFreeBytes(v) / v->cluster, (unsigned long long)v->clusters);
    return true;
fail:
    kprintf("[NTFS] %s: \"%s\" stays read-only (its metadata could not be set up for writing)\n",
            v->dev->name, v->label);
    stream_free(&v->cbm); stream_free(&v->mbm);
    kfree(v->cmap); kfree(v->mmap); kfree(v->upcase);
    v->cmap = v->mmap = NULL; v->upcase = NULL;
    return false;
}

bool NtfsWritable(const NtfsVol *v) { return v->rw; }

UINT64 NtfsFreeBytes(NtfsVol *v) { return v->cmap ? v->nfree * v->cluster : 0; }

bool NtfsSync(NtfsVol *v)
{
    return !v->rw || !v->dev->flush || v->dev->flush(v->dev);
}

/* ---- the MFT ---- */

static bool grow_mft(NtfsVol *v)
{
    UINT8 *rec = kmalloc(v->rec_size), *buf = kmalloc(v->rec_size);
    bool ok = false;
    if (!rec || !buf || !read_record(v, 0, rec) || next_attr(rec, v->rec_size, NULL, AT_ATTRIBUTE_LIST)) goto out;
    UINT8 *da = find_attr(rec, v->rec_size, AT_DATA, NULL, 0);
    UINT8 *bm = find_attr(rec, v->rec_size, AT_BITMAP, NULL, 0);
    if (!da || !da[8] || !bm || !bm[8]) goto out;
    UINT64 nrec = v->mft.size / v->rec_size, want = nrec + MFT_GROW;
    UINT64 alloc = rd64(da + 0x28), size = want * v->rec_size;
    Stream s = v->mft;                                            /* (runs shared until replaced) */
    Stream grown;
    memset(&grown, 0, sizeof(grown));
    for (UINT32 i = 0; i < s.nruns; i++) if (!add_run(&grown, s.runs[i].vcn, s.runs[i].lcn, s.runs[i].len)) goto out;
    if (size > alloc) {
        UINT64 more = (size - alloc + v->cluster - 1) / v->cluster;
        if (!alloc_clusters(v, &grown, alloc / v->cluster, more)) { stream_free(&grown); goto out; }
        alloc += more * v->cluster;
    }
    /* the MFT bitmap must cover the new records */
    UINT64 bm_need = (want + 63) / 64 * 8;
    if (bm_need > rd64(bm + 0x28)) { stream_free(&grown); goto out; }   /* (its clusters would have to grow too) */

    /* format the new records (not in use) */
    grown.size = grown.init_size = size;
    grown.resident = false;
    Stream old = v->mft;
    v->mft = grown;
    for (UINT64 r = nrec; r < want; r++) {
        memset(buf, 0, v->rec_size);
        memcpy(buf, "FILE", 4);
        wr16(buf + 4, 0x30);
        wr16(buf + 6, (UINT16)(v->rec_size / BLOCK_SECTOR + 1));
        wr16(buf + 0x10, 1);
        wr16(buf + 0x14, (UINT16)align8(0x30 + 2 * (v->rec_size / BLOCK_SECTOR + 1)));
        wr32(buf + rd16(buf + 0x14), AT_END);
        wr32(buf + 0x18, rd16(buf + 0x14) + 8);
        wr32(buf + 0x1C, v->rec_size);
        wr32(buf + 0x2C, (UINT32)r);
        if (!write_record(v, r, buf)) { v->mft = old; stream_free(&grown); goto out; }
    }
    /* record 0: the new $DATA, and the bitmap's size */
    UINT32 len = make_nonresident(v, buf, v->rec_size / 2, AT_DATA, NULL, 0, &v->mft, alloc, size);
    if (!len) { v->mft = old; stream_free(&grown); goto out; }
    remove_attr(rec, da);
    if (!insert_attr(v, rec, buf, len)) { v->mft = old; stream_free(&grown); goto out; }
    bm = find_attr(rec, v->rec_size, AT_BITMAP, NULL, 0);
    UINT64 bm_old = rd64(bm + 0x30);
    if (bm_need > bm_old) {
        wr64(bm + 0x30, bm_need);
        wr64(bm + 0x38, bm_need);
        UINT8 *nm = kzalloc(bm_need);
        if (!nm) { v->mft = old; stream_free(&grown); goto out; }
        memcpy(nm, v->mmap, v->mmap_bytes);
        kfree(v->mmap);
        v->mmap = nm;
        v->mmap_bytes = bm_need;
        v->mbm.size = v->mbm.init_size = bm_need;
        if (!write_raw(v, &v->mbm, 0, v->mmap, bm_need)) { v->mft = old; stream_free(&grown); goto out; }
    }
    if (!write_record(v, 0, rec)) { v->mft = old; stream_free(&grown); goto out; }
    stream_free(&old);
    ok = true;
    kprintf("[NTFS] %s: the MFT grew to %llu records\n", v->dev->name, (unsigned long long)want);
out:
    kfree(rec);
    kfree(buf);
    return ok;
}

/* A free MFT record number (marked in use in the bitmap), or 0 */
static UINT64 alloc_record(NtfsVol *v)
{
    for (int tries = 0; tries < 2; tries++) {
        UINT64 nrec = v->mft.size / v->rec_size;
        for (UINT64 r = FIRST_FREE_REC; r < nrec && r < v->mmap_bytes * 8; r++) {
            if (bit(v->mmap, r)) continue;
            set_bit(v->mmap, r, true);
            if (!flush_bits(v, &v->mbm, v->mmap, v->mmap_bytes, r, r)) { set_bit(v->mmap, r, false); return 0; }
            return r;
        }
        if (!grow_mft(v)) return 0;
    }
    return 0;
}

static void free_record_bit(NtfsVol *v, UINT64 r)
{
    set_bit(v->mmap, r, false);
    flush_bits(v, &v->mbm, v->mmap, v->mmap_bytes, r, r);
}

/* ---- files ---- */

/* A $FILE_NAME value for @name in directory @parent (reference with sequence) */
static UINT32 make_fn(UINT8 *out, UINT64 parent_ref, const UINT16 *name, UINT32 nlen, bool dir, UINT64 t,
                      UINT64 alloc, UINT64 size)
{
    UINT32 len = 0x42 + 2 * nlen;
    memset(out, 0, len);
    wr64(out, parent_ref);
    wr64(out + 0x08, t); wr64(out + 0x10, t); wr64(out + 0x18, t); wr64(out + 0x20, t);
    wr64(out + 0x28, alloc);
    wr64(out + 0x30, size);
    wr32(out + 0x38, dir ? FN_DIR_FLAG : 0x20u);
    out[0x40] = (UINT8)nlen;
    out[0x41] = 1;                                               /* Win32 namespace */
    for (UINT32 i = 0; i < nlen; i++) wr16(out + 0x42 + 2 * i, name[i]);
    return len;
}

/* An index entry for file @ref (with sequence) with key @fn */
static Ent make_entry(UINT64 ref, const UINT8 *fn, UINT32 fnlen)
{
    Ent e = { NULL, 0x10 + align8(fnlen) };
    e.e = kzalloc(e.len);
    if (!e.e) return e;
    wr64(e.e, ref);
    wr16(e.e + 8, (UINT16)e.len);
    wr16(e.e + 0xA, (UINT16)fnlen);
    memcpy(e.e + 0x10, fn, fnlen);
    return e;
}

static UINT64 seq_ref(const UINT8 *rec, UINT64 n) { return n | (UINT64)rd16(rec + 0x10) << 48; }

/* Insert @e into the sorted @ents (taking it over); false if the name is there */
static bool ents_insert(NtfsVol *v, Ent **ents, UINT32 *n, Ent e)
{
    UINT32 i = 0;
    while (i < *n) {
        int c = collate(v, (*ents)[i].e + 0x10, e.e + 0x10);
        if (c == 0) return false;
        if (c > 0) break;
        i++;
    }
    Ent *ne = kmalloc((*n + 1) * sizeof(Ent));
    if (!ne) return false;
    if (i) memcpy(ne, *ents, i * sizeof(Ent));
    ne[i] = e;
    if (*n > i) memcpy(ne + i + 1, *ents + i, (*n - i) * sizeof(Ent));
    kfree(*ents);
    *ents = ne;
    (*n)++;
    return true;
}

/* Is there an entry named @name (case-insensitively) in @ents? */
static bool ents_has(NtfsVol *v, const Ent *ents, UINT32 n, const UINT16 *name, UINT32 nlen)
{
    for (UINT32 i = 0; i < n; i++) {
        const UINT8 *k = ents[i].e + 0x10;
        if (k[0x40] != nlen) continue;
        bool same = true;
        for (UINT32 j = 0; j < nlen && same; j++) same = up(v, rd16(k + 0x42 + 2 * j)) == up(v, name[j]);
        if (same) return true;
    }
    return false;
}

static void touch_si(UINT8 *rec, UINT32 size, UINT64 t, bool data)
{
    UINT8 *si = find_attr(rec, size, AT_STANDARD_INFO, NULL, 0);
    if (!si || si[8] || rd32(si + 0x10) < 0x30) return;
    UINT8 *val = si + rd16(si + 0x14);
    if (data) wr64(val + 0x08, t);
    wr64(val + 0x10, t);
    wr64(val + 0x18, t);
}

/* Refresh the copy of record @rec's (@mft's) sizes, times and attributes
 * in the entries its directories hold for it */
static bool update_dir_entries(NtfsVol *v, UINT64 mft, const UINT8 *rec)
{
    UINT64 alloc = 0, size = 0, ct = 0, mt = 0;
    UINT32 attrs = 0;
    bool is_dir = rd16(rec + 0x16) & REC_IS_DIR;
    const UINT8 *si = next_attr(rec, v->rec_size, NULL, AT_STANDARD_INFO);
    if (si && !si[8] && rd32(si + 0x10) >= 0x30) {
        const UINT8 *val = si + rd16(si + 0x14);
        ct = rd64(val); mt = rd64(val + 8);
        attrs = rd32(val + 0x20);
    }
    const UINT8 *da = is_dir ? NULL : find_attr((UINT8 *)rec, v->rec_size, AT_DATA, NULL, 0);
    if (da && da[8]) { alloc = rd64(da + 0x28); size = rd64(da + 0x30); }
    else if (da) { size = rd32(da + 0x10); alloc = align8((UINT32)size); }
    bool ok = true;
    for (const UINT8 *fa = NULL; (fa = next_attr(rec, v->rec_size, fa, AT_FILE_NAME)); ) {
        UINT64 dir = rd64(fa + rd16(fa + 0x14)) & REF_MASK;
        UINT8 *drec = kmalloc(v->rec_size);
        Ent *ents = NULL;
        UINT32 n = 0;
        bool good = drec && read_record(v, dir, drec) && load_index(v, dir, drec, &ents, &n), any = false;
        for (UINT32 i = 0; good && i < n; i++) {
            if ((rd64(ents[i].e) & REF_MASK) != mft) continue;
            UINT8 *k = ents[i].e + 0x10;
            wr64(k + 0x08, ct); wr64(k + 0x10, mt); wr64(k + 0x18, mt); wr64(k + 0x20, mt);
            wr64(k + 0x28, alloc);
            wr64(k + 0x30, size);
            wr32(k + 0x38, (attrs & 0x37FFu & ~0x10u) | (is_dir ? FN_DIR_FLAG : 0));
            any = true;
        }
        if (good && any) good = write_index(v, dir, drec, ents, n);
        if (ents) ents_free(ents, n);
        kfree(drec);
        ok = ok && good;
    }
    return ok;
}

bool NtfsCanWrite(NtfsVol *v, UINT64 mft)
{
    UINT8 *rec = kmalloc(v->rec_size);
    bool ok = v->rw && mft >= MFT_FIRST_USER && rec && read_record(v, mft, rec) &&
              !(rd16(rec + 0x16) & REC_IS_DIR) && !next_attr(rec, v->rec_size, NULL, AT_ATTRIBUTE_LIST);
    const UINT8 *da = ok ? find_attr(rec, v->rec_size, AT_DATA, NULL, 0) : NULL;
    ok = da && !(rd16(da + 0xC) & (ATTR_COMPRESSED | ATTR_ENCRYPTED | 0x8000));
    kfree(rec);
    return ok;
}

bool NtfsWriteFile(NtfsVol *v, UINT64 mft, const void *data, UINT64 len)
{
    if (!v->rw || mft < MFT_FIRST_USER) return false;
    UINT8 *rec = kmalloc(v->rec_size), *attr = kmalloc(v->rec_size), *keep = kmalloc(v->rec_size);
    Stream ns;
    memset(&ns, 0, sizeof(ns));
    bool ok = false;
    if (!rec || !attr || !keep || !read_record(v, mft, rec)) goto out;
    if ((rd16(rec + 0x16) & REC_IS_DIR) || next_attr(rec, v->rec_size, NULL, AT_ATTRIBUTE_LIST)) goto out;
    UINT8 *da = find_attr(rec, v->rec_size, AT_DATA, NULL, 0);
    if (!da || (rd16(da + 0xC) & (ATTR_COMPRESSED | ATTR_ENCRYPTED | 0x8000))) goto out;
    Stream old;
    memset(&old, 0, sizeof(old));
    if (da[8] && (!stream_start(&old, da) || !decode_runs(v, &old, da))) { stream_free(&old); goto out; }
    UINT32 keep_len = rd32(da + 4);
    memcpy(keep, da, keep_len);
    remove_attr(rec, da);

    UINT64 alloc = align8((UINT32)(len & 0xFFFF));             /* (resident: the value's size, 8-aligned) */
    UINT32 alen = 0;
    bool placed = false;
    if (len + 0x18 + 8 <= v->rec_size - rec_used(rec)) {
        alen = make_resident(attr, AT_DATA, NULL, 0, data, (UINT32)len, 0);
        placed = insert_attr(v, rec, attr, alen) != NULL;
    }
    if (!placed) {
        UINT64 clusters = (len + v->cluster - 1) / v->cluster;
        alloc = clusters * v->cluster;
        if (clusters && !alloc_clusters(v, &ns, 0, clusters)) {
            insert_attr(v, rec, keep, keep_len);
            stream_free(&old);
            goto out;
        }
        ns.size = ns.init_size = alloc;
        bool w = true;
        UINT64 full = len - len % v->cluster;
        if (full) w = write_raw(v, &ns, 0, data, full);
        if (w && len > full) {                                   /* the last cluster, zero-padded */
            UINT8 *tail = kzalloc(v->cluster);
            w = tail != NULL;
            if (w) { memcpy(tail, (const UINT8 *)data + full, len - full); w = write_raw(v, &ns, full, tail, v->cluster); }
            kfree(tail);
        }
        alen = w ? make_nonresident(v, attr, v->rec_size - rec_used(rec), AT_DATA, NULL, 0, &ns, alloc, len) : 0;
        if (!alen || !insert_attr(v, rec, attr, alen)) {
            free_runs(v, ns.runs, ns.nruns);
            insert_attr(v, rec, keep, keep_len);
            stream_free(&old);
            goto out;
        }
    }
    UINT64 t = now_ft();
    touch_si(rec, v->rec_size, t, true);
    if (!write_record(v, mft, rec)) { stream_free(&old); goto out; }
    if (old.nruns) free_runs(v, old.runs, old.nruns);
    stream_free(&old);
    (void)alloc;
    ok = update_dir_entries(v, mft, rec);                        /* (the directory entries carry the size too) */
out:
    stream_free(&ns);
    kfree(rec); kfree(attr); kfree(keep);
    return ok;
}

bool NtfsCreate(NtfsVol *v, UINT64 dir, const char *name, bool is_dir, UINT64 *out)
{
    if (!v->rw) return false;
    UINT16 wname[255];
    int nlen = utf8_to_utf16(name, wname, 255);
    if (nlen <= 0) return false;
    for (int i = 0; i < nlen; i++)
        if (wname[i] < 0x20 || wname[i] == '/' || wname[i] == '\\' || wname[i] == ':') return false;

    UINT8 *drec = kmalloc(v->rec_size), *rec = kmalloc(v->rec_size), *attr = kmalloc(v->rec_size);
    UINT8 fn[0x42 + 2 * 255];
    Ent *ents = NULL;
    UINT32 n = 0;
    UINT64 r = 0;
    bool ok = false;
    if (!drec || !rec || !attr || !read_record(v, dir, drec) || !(rd16(drec + 0x16) & REC_IS_DIR)) goto out;
    if (!load_index(v, dir, drec, &ents, &n) || ents_has(v, ents, n, wname, (UINT32)nlen)) goto out;
    if (!(r = alloc_record(v))) goto out;

    /* the new record */
    UINT16 seq = 1;
    if (read_record_any(v, r, rec)) { seq = (UINT16)(rd16(rec + 0x10) + 1); if (!seq) seq = 1; }
    memset(rec, 0, v->rec_size);
    memcpy(rec, "FILE", 4);
    wr16(rec + 4, 0x30);
    wr16(rec + 6, (UINT16)(v->rec_size / BLOCK_SECTOR + 1));
    wr16(rec + 0x10, seq);
    wr16(rec + 0x12, 1);
    UINT16 aoff = (UINT16)align8(0x30 + 2 * (v->rec_size / BLOCK_SECTOR + 1));
    wr16(rec + 0x14, aoff);
    wr16(rec + 0x16, (UINT16)(REC_IN_USE | (is_dir ? REC_IS_DIR : 0)));
    wr32(rec + aoff, AT_END);
    wr32(rec + 0x18, aoff + 8u);
    wr32(rec + 0x1C, v->rec_size);
    wr32(rec + 0x2C, (UINT32)r);

    UINT64 t = now_ft();
    UINT8 si[0x48];
    memset(si, 0, sizeof(si));
    wr64(si, t); wr64(si + 8, t); wr64(si + 0x10, t); wr64(si + 0x18, t);
    wr32(si + 0x20, is_dir ? 0 : 0x20u);
    const UINT8 *psi = next_attr(drec, v->rec_size, NULL, AT_STANDARD_INFO);
    if (psi && !psi[8] && rd32(psi + 0x10) >= 0x48)
        wr32(si + 0x34, rd32(psi + rd16(psi + 0x14) + 0x34));    /* the directory's security descriptor */
    UINT32 len = make_resident(attr, AT_STANDARD_INFO, NULL, 0, si, sizeof(si), 0);
    insert_attr(v, rec, attr, len);
    UINT32 fnlen = make_fn(fn, seq_ref(drec, dir), wname, (UINT32)nlen, is_dir, t, 0, 0);
    len = make_resident(attr, AT_FILE_NAME, NULL, 0, fn, fnlen, 1);
    insert_attr(v, rec, attr, len);
    if (is_dir) {
        UINT8 rv[0x30];
        memset(rv, 0, sizeof(rv));
        UINT8 *pr = find_attr(drec, v->rec_size, AT_INDEX_ROOT, I30, 4);
        memcpy(rv, pr + rd16(pr + 0x14), 0x10);                   /* same index geometry as the parent */
        wr32(rv + 0x10, 0x10);
        wr32(rv + 0x14, 0x20);
        wr32(rv + 0x18, 0x20);
        memcpy(rv + 0x20, g_end_leaf, 0x10);
        len = make_resident(attr, AT_INDEX_ROOT, I30, 4, rv, 0x30, 0);
    } else {
        len = make_resident(attr, AT_DATA, NULL, 0, NULL, 0, 0);
    }
    if (!insert_attr(v, rec, attr, len) || !write_record(v, r, rec)) goto undo;

    Ent e = make_entry(seq_ref(rec, r), fn, fnlen);
    if (!e.e) goto undo;
    if (!ents_insert(v, &ents, &n, e)) { kfree(e.e); goto undo; }
    touch_si(drec, v->rec_size, t, true);
    if (!write_index(v, dir, drec, ents, n)) goto undo;
    *out = r;
    ok = true;
    goto out;
undo:
    wr16(rec + 0x16, 0);
    write_record(v, r, rec);
    free_record_bit(v, r);
out:
    if (ents) ents_free(ents, n);
    kfree(drec); kfree(rec); kfree(attr);
    return ok;
}

/* Does the $FILE_NAME value @val spell @name (case-insensitively)? */
static bool fn_is(NtfsVol *v, const UINT8 *val, const UINT16 *name, UINT32 nlen)
{
    if (val[0x40] != nlen) return false;
    for (UINT32 j = 0; j < nlen; j++) if (up(v, rd16(val + 0x42 + 2 * j)) != up(v, name[j])) return false;
    return true;
}

/* Is the $FILE_NAME @val one of @mft's names in @dir that @name
 * (NULL: any name) takes with it?  A DOS alias goes with its long name. */
static bool fn_of(NtfsVol *v, const UINT8 *val, UINT64 dir, const UINT16 *name, UINT32 nlen)
{
    if ((rd64(val) & REF_MASK) != dir) return false;
    return !name || val[0x41] == 2 || fn_is(v, val, name, nlen);
}

/* Drop the entries of @ents that refer to record @mft by @name (NULL: every one) */
static void ents_drop_name(NtfsVol *v, Ent *ents, UINT32 *n, UINT64 mft, UINT64 dir, const UINT16 *name, UINT32 nlen)
{
    UINT32 o = 0;
    for (UINT32 i = 0; i < *n; i++) {
        if ((rd64(ents[i].e) & REF_MASK) == mft && fn_of(v, ents[i].e + 0x10, dir, name, nlen)) { kfree(ents[i].e); continue; }
        ents[o++] = ents[i];
    }
    *n = o;
}

/* Remove the names @mft has in @dir (@name only, if given) from @dir's index */
static bool unlink_from(NtfsVol *v, UINT64 dir, UINT64 mft, const UINT16 *name, UINT32 nlen)
{
    UINT8 *drec = kmalloc(v->rec_size);
    Ent *ents = NULL;
    UINT32 n = 0;
    bool ok = drec && read_record(v, dir, drec) && load_index(v, dir, drec, &ents, &n);
    if (ok) {
        ents_drop_name(v, ents, &n, mft, dir, name, nlen);
        touch_si(drec, v->rec_size, now_ft(), true);
        ok = write_index(v, dir, drec, ents, n);
    }
    if (ents) ents_free(ents, n);
    kfree(drec);
    return ok;
}

/* The record's link count: one per $FILE_NAME, DOS aliases included */
static void count_links(NtfsVol *v, UINT8 *rec)
{
    UINT16 k = 0;
    for (const UINT8 *x = NULL; (x = next_attr(rec, v->rec_size, x, AT_FILE_NAME)); ) k++;
    wr16(rec + 0x12, k);
}

bool NtfsDelete(NtfsVol *v, UINT64 dir, UINT64 mft, const char *name)
{
    if (!v->rw || mft < MFT_FIRST_USER) return false;
    UINT16 wname[255];
    int nlen = 0;
    if (name && (nlen = utf8_to_utf16(name, wname, 255)) <= 0) return false;
    UINT8 *rec = kmalloc(v->rec_size);
    bool ok = false;
    if (!rec || !read_record(v, mft, rec) || next_attr(rec, v->rec_size, NULL, AT_ATTRIBUTE_LIST)) goto out;
    bool is_dir = rd16(rec + 0x16) & REC_IS_DIR;
    if (is_dir) {                                                /* only an empty one */
        Ent *ents = NULL;
        UINT32 n = 0;
        if (!load_index(v, mft, rec, &ents, &n)) goto out;
        ents_free(ents, n);
        if (n) goto out;
    }
    /* Its names elsewhere (hard links) keep it alive */
    UINT32 names = 0, here = 0;
    for (const UINT8 *fa = NULL; (fa = next_attr(rec, v->rec_size, fa, AT_FILE_NAME)); ) {
        const UINT8 *val = fa + rd16(fa + 0x14);
        if (val[0x41] == 2) continue;                            /* a DOS alias */
        names++;
        if (fn_of(v, val, dir, name ? wname : NULL, (UINT32)nlen)) here++;
    }
    if (!here) goto out;
    if (!unlink_from(v, dir, mft, name ? wname : NULL, (UINT32)nlen)) goto out;
    if (names > here) {                                          /* drop those names only: the others keep the file */
        UINT8 *fa;
        while ((fa = (UINT8 *)next_attr(rec, v->rec_size, NULL, AT_FILE_NAME))) {
            UINT8 *k = NULL;
            for (const UINT8 *x = NULL; (x = next_attr(rec, v->rec_size, x, AT_FILE_NAME)); )
                if (fn_of(v, x + rd16(x + 0x14), dir, name ? wname : NULL, (UINT32)nlen)) { k = (UINT8 *)x; break; }
            if (!k) break;
            remove_attr(rec, k);
        }
        count_links(v, rec);
        ok = write_record(v, mft, rec);
        goto out;
    }
    /* Free its clusters, then the record */
    for (const UINT8 *a = NULL; (a = next_attr(rec, v->rec_size, a, 0)); ) {
        if (!a[8]) continue;
        Stream s;
        memset(&s, 0, sizeof(s));
        if (stream_start(&s, a) && decode_runs(v, &s, a)) free_runs(v, s.runs, s.nruns);
        stream_free(&s);
    }
    wr16(rec + 0x16, 0);
    UINT16 seq = (UINT16)(rd16(rec + 0x10) + 1);
    wr16(rec + 0x10, seq ? seq : 1);
    ok = write_record(v, mft, rec);
    if (ok) free_record_bit(v, mft);
out:
    kfree(rec);
    return ok;
}

bool NtfsRename(NtfsVol *v, UINT64 old_dir, UINT64 mft, const char *old_name, UINT64 new_dir, const char *name)
{
    if (!v->rw || mft < MFT_FIRST_USER) return false;
    UINT16 wname[255], wold[255];
    int nlen = utf8_to_utf16(name, wname, 255), olen = 0;
    if (nlen <= 0) return false;
    if (old_name && (olen = utf8_to_utf16(old_name, wold, 255)) <= 0) return false;
    const UINT16 *oname = old_name ? wold : NULL;
    UINT8 *rec = kmalloc(v->rec_size), *drec = kmalloc(v->rec_size), *attr = kmalloc(v->rec_size);
    UINT8 fn[0x42 + 2 * 255];
    Ent *ents = NULL;
    UINT32 n = 0;
    bool ok = false;
    if (!rec || !drec || !attr || !read_record(v, mft, rec) || !read_record(v, new_dir, drec)) goto out;
    if (!(rd16(drec + 0x16) & REC_IS_DIR)) goto out;
    bool is_dir = rd16(rec + 0x16) & REC_IS_DIR;
    /* the names it has in @old_dir (@old_name and its alias, if given) go */
    UINT64 alloc = 0, size = 0;
    for (;;) {
        UINT8 *k = NULL;
        for (const UINT8 *x = NULL; (x = next_attr(rec, v->rec_size, x, AT_FILE_NAME)); )
            if (fn_of(v, x + rd16(x + 0x14), old_dir, oname, (UINT32)olen)) { k = (UINT8 *)x; break; }
        if (!k) break;
        const UINT8 *val = k + rd16(k + 0x14);
        alloc = rd64(val + 0x28);
        size = rd64(val + 0x30);
        remove_attr(rec, k);
    }
    const UINT8 *da = next_attr(rec, v->rec_size, NULL, AT_DATA);
    if (da && da[8]) { alloc = rd64(da + 0x28); size = rd64(da + 0x30); }
    else if (da) { alloc = 0; size = rd32(da + 0x10); }
    UINT64 t = now_ft();
    UINT32 fnlen = make_fn(fn, seq_ref(drec, new_dir), wname, (UINT32)nlen, is_dir, t, alloc, size);
    UINT32 len = make_resident(attr, AT_FILE_NAME, NULL, 0, fn, fnlen, 1);
    if (!insert_attr(v, rec, attr, len)) goto out;
    count_links(v, rec);

    /* the new name must be free in @new_dir (the file itself may hold it: a case change) */
    if (!load_index(v, new_dir, drec, &ents, &n)) goto out;
    if (new_dir == old_dir) ents_drop_name(v, ents, &n, mft, old_dir, oname, (UINT32)olen);
    if (ents_has(v, ents, n, wname, (UINT32)nlen)) goto out;
    touch_si(rec, v->rec_size, t, false);
    if (!write_record(v, mft, rec)) goto out;
    if (new_dir != old_dir && !unlink_from(v, old_dir, mft, oname, (UINT32)olen)) goto out;
    if (new_dir != old_dir) {                                    /* (the record of @new_dir is unchanged so far) */
        ents_free(ents, n);
        ents = NULL; n = 0;
        if (!read_record(v, new_dir, drec) || !load_index(v, new_dir, drec, &ents, &n)) goto out;
    }
    Ent e = make_entry(seq_ref(rec, mft), fn, fnlen);
    if (!e.e || !ents_insert(v, &ents, &n, e)) { kfree(e.e); goto out; }
    touch_si(drec, v->rec_size, t, true);
    ok = write_index(v, new_dir, drec, ents, n);
out:
    if (ents) ents_free(ents, n);
    kfree(rec); kfree(drec); kfree(attr);
    return ok;
}

/* Another name for file @mft: a $FILE_NAME in its record and an entry in
 * @dir's index (a hard link; directories have one name) */
bool NtfsLink(NtfsVol *v, UINT64 mft, UINT64 dir, const char *name)
{
    if (!v->rw || mft < MFT_FIRST_USER) return false;
    UINT16 wname[255];
    int nlen = utf8_to_utf16(name, wname, 255);
    if (nlen <= 0) return false;
    for (int i = 0; i < nlen; i++)
        if (wname[i] < 0x20 || wname[i] == '/' || wname[i] == '\\' || wname[i] == ':') return false;
    UINT8 *rec = kmalloc(v->rec_size), *drec = kmalloc(v->rec_size), *attr = kmalloc(v->rec_size);
    UINT8 fn[0x42 + 2 * 255];
    Ent *ents = NULL;
    UINT32 n = 0;
    bool ok = false;
    if (!rec || !drec || !attr || !read_record(v, mft, rec) || !read_record(v, dir, drec)) goto out;
    if (!(rd16(drec + 0x16) & REC_IS_DIR) || (rd16(rec + 0x16) & REC_IS_DIR)) goto out;
    if (next_attr(rec, v->rec_size, NULL, AT_ATTRIBUTE_LIST)) goto out;
    if (!load_index(v, dir, drec, &ents, &n) || ents_has(v, ents, n, wname, (UINT32)nlen)) goto out;
    UINT64 alloc = 0, size = 0;
    const UINT8 *da = next_attr(rec, v->rec_size, NULL, AT_DATA);
    if (da && da[8]) { alloc = rd64(da + 0x28); size = rd64(da + 0x30); }
    else if (da) size = rd32(da + 0x10);
    UINT64 t = now_ft();
    UINT32 fnlen = make_fn(fn, seq_ref(drec, dir), wname, (UINT32)nlen, false, t, alloc, size);
    UINT32 len = make_resident(attr, AT_FILE_NAME, NULL, 0, fn, fnlen, 1);
    if (!insert_attr(v, rec, attr, len)) goto out;
    count_links(v, rec);
    touch_si(rec, v->rec_size, t, false);
    if (!write_record(v, mft, rec)) goto out;
    Ent e = make_entry(seq_ref(rec, mft), fn, fnlen);
    if (!e.e || !ents_insert(v, &ents, &n, e)) { kfree(e.e); goto out; }
    touch_si(drec, v->rec_size, t, true);
    ok = write_index(v, dir, drec, ents, n);
out:
    if (ents) ents_free(ents, n);
    kfree(rec); kfree(drec); kfree(attr);
    return ok;
}

UINT32 NtfsLinks(NtfsVol *v, UINT64 mft)
{
    UINT8 *rec = kmalloc(v->rec_size);
    UINT32 k = 0;
    if (rec && read_record(v, mft, rec))
        for (const UINT8 *x = NULL; (x = next_attr(rec, v->rec_size, x, AT_FILE_NAME)); )
            if (x[rd16(x + 0x14) + 0x41] != 2) k++;              /* (not a DOS alias) */
    kfree(rec);
    return k;
}

/* ---------------------------------------------------------------------------
 * Formatting
 *
 * A new volume laid out as Windows and mkntfs lay theirs out: 4 KiB
 * clusters, 1 KiB records, 4 KiB index blocks; the system files in
 * records 0-11 ($MFT ... $Extend), 12-15 reserved, $Quota, $ObjId and
 * $Reparse in 24-26 under $Extend.  Security descriptors are kept in
 * $Secure: 0x100 for the system files (read-only to SYSTEM and
 * Administrators), 0x101 for $Secure and $Extend, and 0x102 for the root
 * directory and what is made under it (full control for SYSTEM,
 * Administrators and whoever creates a file, change for Authenticated
 * Users, read and run for Users, all inherited, as Windows sets C:\).  $LogFile starts empty (all 0xFF).
 * ------------------------------------------------------------------------- */
#include "ntfs_upcase.h"

#define FMT_RECORDS     64                 /* the MFT's first records */
#define SD_SYSTEM       0x100u
#define SD_EXTEND       0x101u
#define SD_ROOT         0x102u
#define SDS_MIRROR      0x40000u           /* $SDS keeps each 256 KiB twice */

static const UINT8 g_sid_system[]  = { 1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0 };                 /* S-1-5-18 */
static const UINT8 g_sid_admins[]  = { 1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 2, 0, 0 }; /* S-1-5-32-544 */
static const UINT8 g_sid_users[]   = { 1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x21, 2, 0, 0 }; /* S-1-5-32-545 */
static const UINT8 g_sid_authusr[] = { 1, 1, 0, 0, 0, 0, 0, 5, 11, 0, 0, 0 };                 /* S-1-5-11 */
static const UINT8 g_sid_creator[] = { 1, 1, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0 };                  /* S-1-3-0: CREATOR OWNER */

typedef struct { UINT8 flags; UINT32 mask; const UINT8 *sid; UINT32 sid_len; } FmtAce;

/* A self-relative security descriptor: owner and group Administrators, a DACL of @aces */
static UINT32 build_sd(UINT8 *out, const FmtAce *aces, int n)
{
    UINT32 o = 0x14, acl = o;
    o += 8;
    for (int i = 0; i < n; i++) {
        UINT32 sz = 8 + aces[i].sid_len;
        out[o] = 0;                                              /* ACCESS_ALLOWED_ACE_TYPE */
        out[o + 1] = aces[i].flags;
        wr16(out + o + 2, (UINT16)sz);
        wr32(out + o + 4, aces[i].mask);
        memcpy(out + o + 8, aces[i].sid, aces[i].sid_len);
        o += sz;
    }
    out[acl] = 2; out[acl + 1] = 0;                              /* ACL_REVISION */
    wr16(out + acl + 2, (UINT16)(o - acl));
    wr16(out + acl + 4, (UINT16)n);
    wr16(out + acl + 6, 0);
    UINT32 owner = o;
    memcpy(out + o, g_sid_admins, sizeof(g_sid_admins)); o += sizeof(g_sid_admins);
    UINT32 group = o;
    memcpy(out + o, g_sid_admins, sizeof(g_sid_admins)); o += sizeof(g_sid_admins);
    out[0] = 1; out[1] = 0;
    wr16(out + 2, 0x8004);                                       /* SE_SELF_RELATIVE | SE_DACL_PRESENT */
    wr32(out + 4, owner);
    wr32(out + 8, group);
    wr32(out + 0xC, 0);
    wr32(out + 0x10, acl);
    return o;
}

/* The hash $Secure indexes descriptors by */
static UINT32 sd_hash(const UINT8 *sd, UINT32 len)
{
    UINT32 h = 0;
    for (UINT32 i = 0; i + 4 <= len; i += 4) h = ((h << 3) | (h >> 29)) + rd32(sd + i);
    return h;
}

/* A view index's root ($SDH, $SII, $O, $Q, $R): @entries already laid out */
static UINT32 view_root(UINT8 *out, UINT32 collation, UINT32 bsize, const UINT8 *entries, UINT32 elen)
{
    memset(out, 0, 0x20 + elen + 0x10);
    wr32(out, 0);                                                /* indexes no attribute */
    wr32(out + 4, collation);
    wr32(out + 8, bsize);
    out[0xC] = 1;
    wr32(out + 0x10, 0x10);
    wr32(out + 0x14, 0x10 + elen + 0x10);
    wr32(out + 0x18, 0x10 + elen + 0x10);
    if (elen) memcpy(out + 0x20, entries, elen);
    memcpy(out + 0x20 + elen, g_end_leaf, 0x10);
    return 0x20 + elen + 0x10;
}

/* A view index entry: @key then @data, at @out; its length */
static UINT32 view_entry(UINT8 *out, const void *key, UINT16 klen, const void *data, UINT16 dlen, UINT32 pad)
{
    UINT32 doff = 0x10 + klen, len = align8(doff + dlen + pad);
    memset(out, 0, len);
    wr16(out, (UINT16)doff);
    wr16(out + 2, dlen);
    wr16(out + 8, (UINT16)len);
    wr16(out + 0xA, klen);
    memcpy(out + 0x10, key, klen);
    memcpy(out + doff, data, dlen);
    return len;
}

/* A record header for record @r: in use with @flags (0: free) */
static void fmt_record(NtfsVol *v, UINT8 *rec, UINT64 r, UINT16 flags, UINT16 seq, UINT16 links)
{
    memset(rec, 0, v->rec_size);
    memcpy(rec, "FILE", 4);
    wr16(rec + 4, 0x30);
    wr16(rec + 6, (UINT16)(v->rec_size / BLOCK_SECTOR + 1));
    wr16(rec + 0x10, seq);
    wr16(rec + 0x12, links);
    UINT16 aoff = (UINT16)align8(0x30 + 2 * (v->rec_size / BLOCK_SECTOR + 1));
    wr16(rec + 0x14, aoff);
    wr16(rec + 0x16, flags);
    wr32(rec + aoff, AT_END);
    wr32(rec + 0x18, aoff + 8u);
    wr32(rec + 0x1C, v->rec_size);
    wr32(rec + 0x2C, (UINT32)r);
}

static void fmt_si(NtfsVol *v, UINT8 *rec, UINT64 t, UINT32 attrs, UINT32 sec)
{
    UINT8 si[0x48], a[0x80];
    memset(si, 0, sizeof(si));
    wr64(si, t); wr64(si + 8, t); wr64(si + 0x10, t); wr64(si + 0x18, t);
    wr32(si + 0x20, attrs);
    wr32(si + 0x34, sec);
    insert_attr(v, rec, a, make_resident(a, AT_STANDARD_INFO, NULL, 0, si, sizeof(si), 0));
}

/* The $FILE_NAME of a system file in @parent (Win32 and DOS name); its value in @fn */
static UINT32 fmt_fn(NtfsVol *v, UINT8 *rec, UINT8 *fn, UINT64 parent_ref, const char *name, UINT32 flags,
                     UINT64 t, UINT64 alloc, UINT64 size)
{
    UINT16 w[32];
    int n = utf8_to_utf16(name, w, 32);
    UINT32 len = make_fn(fn, parent_ref, w, (UINT32)n, false, t, alloc, size);
    wr32(fn + 0x38, flags);
    fn[0x41] = 3;
    UINT8 a[0x100];
    insert_attr(v, rec, a, make_resident(a, AT_FILE_NAME, NULL, 0, fn, len, 1));
    return len;
}

/* A non-resident attribute for @s (all of it initialised) */
static bool fmt_nonres(NtfsVol *v, UINT8 *rec, UINT32 type, const UINT16 *name, UINT32 nlen, const Stream *s,
                       UINT64 size)
{
    UINT8 a[0x100];
    UINT64 alloc = 0;
    for (UINT32 i = 0; i < s->nruns; i++) alloc += s->runs[i].len * v->cluster;
    UINT32 len = make_nonresident(v, a, sizeof(a), type, name, nlen, s, alloc, size);
    return len && insert_attr(v, rec, a, len);
}

/* @n clusters at @lcn into @s (marked used) */
static bool fmt_take(NtfsVol *v, Stream *s, UINT64 lcn, UINT64 n)
{
    for (UINT64 c = 0; c < n; c++) set_bit(v->cmap, lcn + c, true);
    v->nfree -= n;
    return add_run(s, 0, lcn, n);
}

static const struct { const char *name; UINT32 type, flags; INT64 min, max; } g_attrdef[] = {
    { "$STANDARD_INFORMATION", 0x10,  0x40, 48, 72 },
    { "$ATTRIBUTE_LIST",       0x20,  0x80, 0, -1 },
    { "$FILE_NAME",            0x30,  0x42, 68, 578 },
    { "$OBJECT_ID",            0x40,  0x40, 0, 256 },
    { "$SECURITY_DESCRIPTOR",  0x50,  0x80, 0, -1 },
    { "$VOLUME_NAME",          0x60,  0x40, 2, 256 },
    { "$VOLUME_INFORMATION",   0x70,  0x40, 12, 12 },
    { "$DATA",                 0x80,  0x00, 0, -1 },
    { "$INDEX_ROOT",           0x90,  0x40, 0, -1 },
    { "$INDEX_ALLOCATION",     0xA0,  0x80, 0, -1 },
    { "$BITMAP",               0xB0,  0x80, 0, -1 },
    { "$REPARSE_POINT",        0xC0,  0x80, 0, 16384 },
    { "$EA_INFORMATION",       0xD0,  0x40, 8, 8 },
    { "$EA",                   0xE0,  0x00, 0, 65536 },
    { "$LOGGED_UTILITY_STREAM", 0x100, 0x80, 0, 65536 },
};

static const UINT16 g_name_sds[] = { '$', 'S', 'D', 'S' }, g_name_sdh[] = { '$', 'S', 'D', 'H' },
                    g_name_sii[] = { '$', 'S', 'I', 'I' }, g_name_bad[] = { '$', 'B', 'a', 'd' },
                    g_name_o[] = { '$', 'O' }, g_name_q[] = { '$', 'Q' }, g_name_r[] = { '$', 'R' };

/* Write @len bytes of @data (zero-padded to whole clusters) to @s from its start */
static bool fmt_write(NtfsVol *v, const Stream *s, const void *data, UINT64 len)
{
    UINT64 full = len - len % v->cluster;
    if (full && !write_raw(v, s, 0, data, full)) return false;
    if (len == full) return true;
    UINT8 *tail = kzalloc(v->cluster);
    if (!tail) return false;
    memcpy(tail, (const UINT8 *)data + full, len - full);
    bool ok = write_raw(v, s, full, tail, v->cluster);
    kfree(tail);
    return ok;
}

bool NtfsFormat(BlockDev *dev, UINT64 lba, UINT64 sectors, const char *label, UINT64 serial)
{
    const UINT32 cluster = 4096, rec_size = 1024, bsize = 4096;
    if (sectors < (32ull << 20) / BLOCK_SECTOR || lba + sectors > dev->sectors) return false;
    UINT64 clusters = (sectors - 1) * BLOCK_SECTOR / cluster;    /* (the last sector: the backup boot sector) */
    NtfsVol *v = kzalloc(sizeof(NtfsVol));
    UINT8 *rec = kmalloc(rec_size), *buf = NULL, *big = NULL;
    bool ok = false;
    Ent *ents = NULL;
    UINT32 nents = 0;
    Stream s_log, s_ad, s_up, s_sds, s_bad, s_boot, s_mirr;
    memset(&s_log, 0, sizeof(Stream)); memset(&s_ad, 0, sizeof(Stream)); memset(&s_up, 0, sizeof(Stream));
    memset(&s_sds, 0, sizeof(Stream)); memset(&s_bad, 0, sizeof(Stream)); memset(&s_boot, 0, sizeof(Stream));
    memset(&s_mirr, 0, sizeof(Stream));
    if (!v || !rec || !(v->bounce = kmalloc(BOUNCE)) || !(buf = kzalloc(64 * 1024))) goto out;
    v->dev = dev; v->base = lba; v->cluster = cluster; v->rec_size = rec_size; v->idx_size = bsize;
    v->total = clusters * cluster;
    v->clusters = clusters;
    v->nfree = clusters;
    v->cbm.size = v->cbm.init_size = (clusters + 63) / 64 * 8;
    if (!(v->cmap = kzalloc(v->cbm.size + cluster))) goto out;
    v->upcase_n = 65536;
    if (!(v->upcase = kmalloc(65536 * 2))) goto out;
    for (UINT32 c = 0; c < 65536; c++) v->upcase[c] = (UINT16)c;
    for (UINT32 i = 0; i < sizeof(g_ntfs_upcase) / sizeof(g_ntfs_upcase[0]); i++)
        for (UINT32 c = g_ntfs_upcase[i].first; c <= g_ntfs_upcase[i].last; c += g_ntfs_upcase[i].stride)
            v->upcase[c] = (UINT16)(c + g_ntfs_upcase[i].delta);

    /* Where things go: $Boot 0-1, the MFT bitmap 2, the MFT from 4,
     * $MFTMirr and $LogFile in the middle, the rest after the MFT */
    UINT64 mft_lcn = 4, mft_clusters = FMT_RECORDS * rec_size / cluster;
    UINT64 log_bytes = v->total / 100;
    if (log_bytes < (2u << 20)) log_bytes = 2u << 20;
    if (log_bytes > (64u << 20)) log_bytes = 64u << 20;
    UINT64 log_clusters = log_bytes / cluster, mirr_lcn = clusters / 2;
    if (!fmt_take(v, &s_boot, 0, 2) || !fmt_take(v, &v->mbm, 2, 1) || !fmt_take(v, &v->mft, mft_lcn, mft_clusters) ||
        !fmt_take(v, &s_mirr, mirr_lcn, 1) || !fmt_take(v, &s_log, mirr_lcn + 1, log_clusters)) goto out;
    v->hint = mft_lcn + mft_clusters;
    UINT64 bm_clusters = (v->cbm.size + cluster - 1) / cluster;
    UINT32 sds_used = 0;
    UINT8 *sds = kzalloc(SDS_MIRROR + 4096);
    if (!sds) goto out;
    if (!alloc_clusters(v, &v->cbm, 0, bm_clusters) || !alloc_clusters(v, &s_ad, 0, 1) ||
        !alloc_clusters(v, &s_up, 0, 65536 * 2 / cluster)) { kfree(sds); goto out; }
    v->mft.size = v->mft.init_size = (UINT64)FMT_RECORDS * rec_size;
    v->mbm.size = v->mbm.init_size = FMT_RECORDS / 8;
    v->mmap_bytes = cluster;
    v->mmap = kzalloc(cluster);
    v->mirr_lcn = mirr_lcn;
    v->mirr_n = 4;

    /* $Secure's descriptors */
    static const FmtAce sys_aces[] = {
        { 0, 0x120089, g_sid_system, sizeof(g_sid_system) }, { 0, 0x120089, g_sid_admins, sizeof(g_sid_admins) } };
    static const FmtAce ext_aces[] = {
        { 0, 0x12019F, g_sid_system, sizeof(g_sid_system) }, { 0, 0x12019F, g_sid_admins, sizeof(g_sid_admins) } };
    static const FmtAce root_aces[] = {
        { 3, 0x1F01FF, g_sid_system, sizeof(g_sid_system) }, { 3, 0x1F01FF, g_sid_admins, sizeof(g_sid_admins) },
        { 0xB, 0x10000000, g_sid_creator, sizeof(g_sid_creator) },     /* (inherit only: GENERIC_ALL) */
        { 3, 0x1301BF, g_sid_authusr, sizeof(g_sid_authusr) }, { 3, 0x1200A9, g_sid_users, sizeof(g_sid_users) } };
    struct { const FmtAce *a; int n; } sdl[3] = { { sys_aces, 2 }, { ext_aces, 2 }, { root_aces, 5 } };
    UINT8 sii[3 * 0x30], sdh[3 * 0x38];
    UINT32 sii_len = 0, sdh_len = 0;
    UINT32 hashes[3], offs[3], lens[3];
    for (int i = 0; i < 3; i++) {
        UINT8 *e = sds + sds_used;
        UINT32 n = build_sd(e + 0x14, sdl[i].a, sdl[i].n);
        hashes[i] = sd_hash(e + 0x14, n);
        offs[i] = sds_used;
        lens[i] = 0x14 + n;
        wr32(e, hashes[i]);
        wr32(e + 4, SD_SYSTEM + (UINT32)i);
        wr64(e + 8, sds_used);
        wr32(e + 0x10, lens[i]);
        sds_used = (sds_used + lens[i] + 15) & ~15u;
    }
    for (int i = 0; i < 3; i++) {                                /* $SII by id; $SDH by hash, then id */
        UINT32 id = SD_SYSTEM + (UINT32)i;
        sii_len += view_entry(sii + sii_len, &id, 4, sds + offs[i], 0x14, 0);
    }
    int order[3] = { 0, 1, 2 };
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (hashes[order[j]] < hashes[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int k = 0; k < 3; k++) {
        int i = order[k];
        UINT32 key[2] = { hashes[i], SD_SYSTEM + (UINT32)i };
        UINT32 at = sdh_len;
        sdh_len += view_entry(sdh + sdh_len, key, 8, sds + offs[i], 0x14, 4);
        wr16(sdh + at + 0x10 + 8 + 0x14, 'I');                   /* (the padding Windows writes: "II") */
        wr16(sdh + at + 0x10 + 8 + 0x16, 'I');
    }
    UINT64 sds_size = SDS_MIRROR + sds_used;
    memcpy(sds + SDS_MIRROR, sds, sds_used);
    if (!alloc_clusters(v, &s_sds, 0, (sds_size + cluster - 1) / cluster)) { kfree(sds); goto out; }

    /* $Bad: every cluster, sparse */
    if (!add_run(&s_bad, 0, SPARSE, clusters)) { kfree(sds); goto out; }

    /* The data of the system files */
    UINT64 t = now_ft();
    bool w = fmt_write(v, &s_sds, sds, sds_size);
    kfree(sds);
    big = kmalloc(65536 * 2 > 2560 ? 65536 * 2 : 2560);
    if (!w || !big) goto out;
    memcpy(big, v->upcase, 65536 * 2);
    if (!fmt_write(v, &s_up, big, 65536 * 2)) goto out;
    memset(big, 0, 2560);
    for (UINT32 i = 0; i < sizeof(g_attrdef) / sizeof(g_attrdef[0]); i++) {
        UINT8 *e = big + 160 * i;
        for (int k = 0; g_attrdef[i].name[k]; k++) wr16(e + 2 * k, (UINT8)g_attrdef[i].name[k]);
        wr32(e + 128, g_attrdef[i].type);
        wr32(e + 140, g_attrdef[i].flags);
        wr64(e + 144, (UINT64)g_attrdef[i].min);
        wr64(e + 152, (UINT64)g_attrdef[i].max);
    }
    if (!fmt_write(v, &s_ad, big, 2560)) goto out;
    memset(buf, 0xFF, 64 * 1024);
    for (UINT64 o = 0; o < log_clusters * cluster; o += 64 * 1024)
        if (!write_raw(v, &s_log, o, buf, log_clusters * cluster - o < 64 * 1024 ? log_clusters * cluster - o : 64 * 1024))
            goto out;

    /* The records */
    UINT64 root_ref = 5 | (5ull << 48);
    UINT8 fn[0x42 + 64];
    struct { UINT64 r; const char *name; } names[12];
    UINT32 nnames = 0;
    for (UINT64 r = 0; r < FMT_RECORDS; r++) {
        bool used = r < 16 || (r >= 24 && r <= 26);
        UINT16 seq = r == 0 ? 1 : r < 16 ? (UINT16)r : 1;
        UINT16 rflags = !used ? 0 : r == 5 || r == 11 ? 3 : r == 9 ? 9 : r >= 24 ? 0xD : 1;
        fmt_record(v, rec, r, rflags, seq, used && (r < 12 || r >= 24) ? 1 : 0);
        if (used) {
            UINT32 sec = r == 5 ? SD_ROOT : r == 9 || r == 11 || r >= 24 ? SD_EXTEND : SD_SYSTEM;
            UINT32 sattr = r == 9 ? 0x20000006u : r >= 24 ? 0x20000026u : 0x6u;
            fmt_si(v, rec, t, sattr, sec);
        }
        const char *name = NULL;
        UINT32 fflags = 0x6;
        UINT64 alloc = 0, size = 0;
        switch (r) {
        case 0:  name = "$MFT"; alloc = size = (UINT64)FMT_RECORDS * rec_size; break;
        case 1:  name = "$MFTMirr"; alloc = size = cluster; break;
        case 2:  name = "$LogFile"; alloc = size = log_clusters * cluster; break;
        case 3:  name = "$Volume"; break;
        case 4:  name = "$AttrDef"; alloc = cluster; size = 2560; break;
        case 5:  name = "."; fflags = 0x10000006u; break;
        case 6:  name = "$Bitmap"; alloc = bm_clusters * cluster; size = v->cbm.size; break;
        case 7:  name = "$Boot"; alloc = size = 2 * cluster; break;
        case 8:  name = "$BadClus"; break;
        case 9:  name = "$Secure"; fflags = 0x20000006u; break;
        case 10: name = "$UpCase"; alloc = size = 65536 * 2; break;
        case 11: name = "$Extend"; fflags = 0x10000006u; break;
        case 24: name = "$Quota"; fflags = 0x20000026u; break;
        case 25: name = "$ObjId"; fflags = 0x20000026u; break;
        case 26: name = "$Reparse"; fflags = 0x20000026u; break;
        }
        UINT32 fnlen = 0;
        if (name) fnlen = fmt_fn(v, rec, fn, r >= 24 ? (11 | (11ull << 48)) : root_ref, name, fflags, t, alloc, size);
        UINT8 a[0x200];
        bool good = true;
        switch (r) {
        case 0:  good = fmt_nonres(v, rec, AT_DATA, NULL, 0, &v->mft, v->mft.size) &&
                        fmt_nonres(v, rec, AT_BITMAP, NULL, 0, &v->mbm, v->mbm.size); break;
        case 1:  good = fmt_nonres(v, rec, AT_DATA, NULL, 0, &s_mirr, cluster); break;
        case 2:  good = fmt_nonres(v, rec, AT_DATA, NULL, 0, &s_log, log_clusters * cluster); break;
        case 3: {
            UINT16 wl[64];
            int ln = label ? utf8_to_utf16(label, wl, 32) : 0;
            if (ln < 0) ln = 0;
            insert_attr(v, rec, a, make_resident(a, AT_VOLUME_NAME, NULL, 0, wl, 2u * (UINT32)ln, 0));
            UINT8 vi[12] = { 0 };
            vi[8] = 3; vi[9] = 1;                                /* NTFS 3.1, not dirty */
            insert_attr(v, rec, a, make_resident(a, AT_VOLUME_INFORMATION, NULL, 0, vi, 12, 0));
            insert_attr(v, rec, a, make_resident(a, AT_DATA, NULL, 0, NULL, 0, 0));
            break;
        }
        case 4:  good = fmt_nonres(v, rec, AT_DATA, NULL, 0, &s_ad, 2560); break;
        case 6:  good = fmt_nonres(v, rec, AT_DATA, NULL, 0, &v->cbm, v->cbm.size); break;
        case 7:  good = fmt_nonres(v, rec, AT_DATA, NULL, 0, &s_boot, 2 * cluster); break;
        case 8:
            insert_attr(v, rec, a, make_resident(a, AT_DATA, NULL, 0, NULL, 0, 0));
            good = fmt_nonres(v, rec, AT_DATA, g_name_bad, 4, &s_bad, clusters * cluster);
            break;
        case 9: {
            good = fmt_nonres(v, rec, AT_DATA, g_name_sds, 4, &s_sds, sds_size);
            UINT8 rv[0x200];
            UINT32 n = view_root(rv, 0x12, bsize, sdh, sdh_len);
            good = good && insert_attr(v, rec, a, make_resident(a, AT_INDEX_ROOT, g_name_sdh, 4, rv, n, 0));
            n = view_root(rv, 0x10, bsize, sii, sii_len);
            good = good && insert_attr(v, rec, a, make_resident(a, AT_INDEX_ROOT, g_name_sii, 4, rv, n, 0));
            break;
        }
        case 10: good = fmt_nonres(v, rec, AT_DATA, NULL, 0, &s_up, 65536 * 2); break;
        case 5: case 11: {                                       /* empty for now: filled in below */
            UINT8 rv[0x30];
            memset(rv, 0, sizeof(rv));
            wr32(rv, AT_FILE_NAME);
            wr32(rv + 4, 1);                                     /* COLLATION_FILE_NAME */
            wr32(rv + 8, bsize);
            rv[0xC] = 1;
            wr32(rv + 0x10, 0x10); wr32(rv + 0x14, 0x20); wr32(rv + 0x18, 0x20);
            memcpy(rv + 0x20, g_end_leaf, 0x10);
            good = insert_attr(v, rec, a, make_resident(a, AT_INDEX_ROOT, I30, 4, rv, 0x30, 0)) != NULL;
            break;
        }
        case 12: case 13: case 14: case 15:
            insert_attr(v, rec, a, make_resident(a, AT_DATA, NULL, 0, NULL, 0, 0));
            break;
        case 24: {                                               /* $Quota: the default limits, and Administrators */
            UINT8 rv[0x200], e[0xC0], q[0x40];
            UINT32 el = 0, owner = 0x100;
            el += view_entry(e + el, g_sid_admins, sizeof(g_sid_admins), &owner, 4, 0);
            UINT32 n = view_root(rv, 0x11, bsize, e, el);
            good = insert_attr(v, rec, a, make_resident(a, AT_INDEX_ROOT, g_name_o, 2, rv, n, 0)) != NULL;
            memset(q, 0, sizeof(q));
            wr32(q, 2); wr32(q + 4, 1);                          /* version 2, default limits */
            wr64(q + 0x10, t);
            wr64(q + 0x18, ~0ull); wr64(q + 0x20, ~0ull);        /* no threshold, no limit */
            el = 0;
            UINT32 id = 1;
            el += view_entry(e + el, &id, 4, q, 0x30, 0);
            memcpy(q + 0x30, g_sid_admins, sizeof(g_sid_admins));
            id = 0x100;
            el += view_entry(e + el, &id, 4, q, 0x40, 0);
            n = view_root(rv, 0x10, bsize, e, el);
            good = good && insert_attr(v, rec, a, make_resident(a, AT_INDEX_ROOT, g_name_q, 2, rv, n, 0));
            break;
        }
        case 25: case 26: {
            UINT8 rv[0x40];
            UINT32 n = view_root(rv, 0x13, bsize, NULL, 0);
            good = insert_attr(v, rec, a, make_resident(a, AT_INDEX_ROOT, r == 25 ? g_name_o : g_name_r, 2, rv, n, 0))
                   != NULL;
            break;
        }
        }
        if (!good || !write_record(v, r, rec)) goto out;
        if (used) set_bit(v->mmap, r, true);
        if (name && r < 24 && r != 5) { names[nnames].r = r; names[nnames].name = name; nnames++; }
        (void)fnlen;
    }

    /* The root's and $Extend's indexes */
    for (int pass = 0; pass < 2; pass++) {
        UINT64 dir = pass ? 11 : 5;
        nents = 0;
        ents = NULL;
        for (UINT64 r = 0; r < FMT_RECORDS; r++) {
            bool child = pass ? r >= 24 && r <= 26 : (r < 12);
            if (!child || !read_record_any(v, r, rec)) continue;
            const UINT8 *fa = next_attr(rec, rec_size, NULL, AT_FILE_NAME);
            if (!fa) continue;
            Ent e = make_entry(seq_ref(rec, r), fa + rd16(fa + 0x14), rd32(fa + 0x10));
            if (!e.e || !ents_insert(v, &ents, &nents, e)) { kfree(e.e); goto out; }
        }
        if (!read_record_any(v, dir, rec) || !write_index(v, dir, rec, ents, nents)) goto out;
        ents_free(ents, nents);
        ents = NULL;
    }
    (void)names;

    /* The bitmaps, then the boot sectors */
    if (!fmt_write(v, &v->cbm, v->cmap, v->cbm.size) || !write_raw(v, &v->mbm, 0, v->mmap, cluster)) goto out;
    memset(buf, 0, BLOCK_SECTOR);
    UINT8 *bs = buf;
    bs[0] = 0xEB; bs[1] = 0x52; bs[2] = 0x90;
    memcpy(bs + 3, "NTFS    ", 8);
    wr16(bs + 0x0B, BLOCK_SECTOR);
    bs[0x0D] = (UINT8)(cluster / BLOCK_SECTOR);
    bs[0x15] = 0xF8;
    wr16(bs + 0x18, 63);
    wr16(bs + 0x1A, 255);
    wr32(bs + 0x1C, (UINT32)lba);
    wr32(bs + 0x24, 0x00800080);
    wr64(bs + 0x28, sectors - 1);
    wr64(bs + 0x30, mft_lcn);
    wr64(bs + 0x38, mirr_lcn);
    bs[0x40] = 0xF6;                                             /* records: 2^10 bytes */
    bs[0x44] = 1;                                                /* index blocks: 1 cluster */
    wr64(bs + 0x48, serial);
    bs[0x54] = 0xF4; bs[0x55] = 0xEB; bs[0x56] = 0xFD;           /* (not bootable: hlt; jmp $-1) */
    bs[510] = 0x55; bs[511] = 0xAA;
    if (!dev->write(dev, lba, 1, bs) || !dev->write(dev, lba + sectors - 1, 1, bs)) goto out;
    ok = !dev->flush || dev->flush(dev);
    kprintf("[NTFS] %s: formatted \"%s\" at LBA %llu, %llu MiB\n", dev->name, label ? label : "",
            (unsigned long long)lba, (unsigned long long)(v->total >> 20));
out:
    if (ents) ents_free(ents, nents);
    stream_free(&s_log); stream_free(&s_ad); stream_free(&s_up); stream_free(&s_sds); stream_free(&s_bad);
    stream_free(&s_boot); stream_free(&s_mirr);
    if (v) {
        stream_free(&v->cbm); stream_free(&v->mbm); stream_free(&v->mft);
        kfree(v->cmap); kfree(v->mmap); kfree(v->upcase); kfree(v->bounce);
        kfree(v);
    }
    kfree(rec); kfree(buf); kfree(big);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Security descriptors and file information
 *
 * NTFS 3 keeps each distinct security descriptor once, in $Secure: its
 * $SDS stream holds them (every 256 KiB twice, the copy right after), the
 * $SII index finds one by id and $SDH by hash.  A file's $STANDARD_INFORMATION
 * names its descriptor by id.
 * ------------------------------------------------------------------------- */
#define SECURE_MFT 9

struct SecEnt { UINT32 id, hash, len; UINT64 off; };     /* len: with the 0x14-byte header */

static bool sec_load(NtfsVol *v)
{
    if (v->sec_read) return true;
    UINT8 *rec = kmalloc(v->rec_size);
    Ent *ents = NULL;
    UINT32 n = 0;
    bool ok = rec && read_record(v, SECURE_MFT, rec) && load_index_named(v, SECURE_MFT, rec, g_name_sii, &ents, &n);
    struct SecEnt *se = ok && n ? kmalloc(n * sizeof(struct SecEnt)) : NULL;
    if (ok && n && !se) ok = false;
    UINT32 k = 0;
    for (UINT32 i = 0; ok && i < n; i++) {
        const UINT8 *e = ents[i].e;
        UINT16 doff = rd16(e), dlen = rd16(e + 2);
        if (dlen < 0x14 || doff + dlen > ents[i].len) continue;
        const UINT8 *d = e + doff;
        se[k++] = (struct SecEnt){ rd32(d + 4), rd32(d), rd32(d + 0x10), rd64(d + 8) };
    }
    if (ents) ents_free(ents, n);
    kfree(rec);
    if (!ok) { kfree(se); return false; }
    v->sec = se;
    v->nsec = k;
    v->sec_read = true;
    return true;
}

static const struct SecEnt *sec_find(NtfsVol *v, UINT32 id)
{
    if (!sec_load(v)) return NULL;
    for (UINT32 i = 0; i < v->nsec; i++)
        if (v->sec[i].id == id) return &v->sec[i];
    return NULL;
}

UINT32 NtfsSecurityId(NtfsVol *v, UINT64 mft)
{
    UINT8 *rec = kmalloc(v->rec_size);
    UINT32 id = 0;
    if (rec && read_record(v, mft, rec)) {
        const UINT8 *si = next_attr(rec, v->rec_size, NULL, AT_STANDARD_INFO);
        if (si && !si[8] && rd32(si + 0x10) >= 0x48) id = rd32(si + rd16(si + 0x14) + 0x34);
    }
    kfree(rec);
    return id;
}

bool NtfsSecurityById(NtfsVol *v, UINT32 id, void *buf, UINT32 cap, UINT32 *len)
{
    const struct SecEnt *e = sec_find(v, id);
    if (!e || e->len < 0x14 + 0x14) return false;
    *len = e->len - 0x14;
    if (*len > cap) return false;
    Stream sds;
    if (!open_stream(v, SECURE_MFT, AT_DATA, g_name_sds, 4, &sds)) return false;
    bool ok = e->off + e->len <= sds.size && stream_read(v, &sds, e->off + 0x14, buf, *len);
    stream_free(&sds);
    return ok;
}

/* $SII's key order (a ULONG), and $SDH's (hash, then id) */
static int sec_order(const UINT8 *k1, const UINT8 *k2, bool by_hash)
{
    for (int i = 0; i < (by_hash ? 2 : 1); i++) {
        UINT32 a = rd32(k1 + 4 * i), b = rd32(k2 + 4 * i);
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

static bool sec_insert(Ent **ents, UINT32 *n, Ent e, bool by_hash)
{
    UINT32 i = 0;
    while (i < *n && sec_order((*ents)[i].e + 0x10, e.e + 0x10, by_hash) < 0) i++;
    Ent *ne = kmalloc((*n + 1) * sizeof(Ent));
    if (!ne) return false;
    if (i) memcpy(ne, *ents, i * sizeof(Ent));
    ne[i] = e;
    if (*n > i) memcpy(ne + i + 1, *ents + i, (*n - i) * sizeof(Ent));
    kfree(*ents);
    *ents = ne;
    (*n)++;
    return true;
}

static Ent sec_entry(const void *key, UINT16 klen, const UINT8 *hdr, UINT32 pad)
{
    Ent e = { kzalloc(0x60), 0 };
    if (e.e) e.len = view_entry(e.e, key, klen, hdr, 0x14, pad);
    if (e.e && pad) { wr16(e.e + 0x10 + klen + 0x14, 'I'); wr16(e.e + 0x10 + klen + 0x16, 'I'); }   /* ($SDH: as Windows pads) */
    return e;
}

UINT32 NtfsAddSecurity(NtfsVol *v, const void *sd, UINT32 len)
{
    if (!v->rw || len < 0x14 || len > 0x10000 || !sec_load(v)) return 0;
    UINT32 hash = sd_hash(sd, len), id = 0x100;
    UINT64 end = 0;
    UINT8 *cmp = kmalloc(len);
    if (!cmp) return 0;
    for (UINT32 i = 0; i < v->nsec; i++) {                      /* already there? */
        const struct SecEnt *e = &v->sec[i];
        UINT32 got;
        if (e->hash == hash && e->len == len + 0x14 && NtfsSecurityById(v, e->id, cmp, len, &got) && !memcmp(cmp, sd, len)) {
            kfree(cmp);
            return e->id;
        }
        if (e->id >= id) id = e->id + 1;
        if (e->off + e->len > end) end = e->off + e->len;
    }
    kfree(cmp);

    /* where it goes: after the last, not across a 256 KiB block (each is followed by its copy) */
    UINT32 elen = 0x14 + len;
    UINT64 off = (end + 15) & ~15ull;
    if (off % (2 * SDS_MIRROR) + elen > SDS_MIRROR) off = (off / (2 * SDS_MIRROR) + 1) * (2 * SDS_MIRROR);
    UINT64 size = off + SDS_MIRROR + ((elen + 15) & ~15u);
    UINT8 *ent = kzalloc((elen + 15) & ~15u), *rec = kmalloc(v->rec_size), *attr = kmalloc(v->rec_size);
    Stream sds;
    memset(&sds, 0, sizeof(sds));
    Ent *sii = NULL, *sdh = NULL;
    UINT32 nsii = 0, nsdh = 0;
    UINT64 grown_from = ~0ull;                                    /* (the first new cluster of $SDS, on failure) */
    bool ok = false, kept = false;
    if (!ent || !rec || !attr || !read_record(v, SECURE_MFT, rec)) goto out;
    if (!open_stream(v, SECURE_MFT, AT_DATA, g_name_sds, 4, &sds) || sds.resident) goto out;
    wr32(ent, hash); wr32(ent + 4, id); wr64(ent + 8, off); wr32(ent + 0x10, elen);
    memcpy(ent + 0x14, sd, len);

    /* grow $SDS, zeroing what it gains */
    UINT64 have = sds.nruns ? (sds.runs[sds.nruns - 1].vcn + sds.runs[sds.nruns - 1].len) * v->cluster : 0;
    grown_from = have / v->cluster;
    if (size > have) {
        UINT64 more = (size - have + 0xFFFF) / 0x10000 * 0x10000 / v->cluster;   /* (64 KiB at a time) */
        if (!more) more = 1;
        if (!alloc_clusters(v, &sds, have / v->cluster, more)) goto out;
        UINT8 *z = kzalloc(v->cluster);
        bool zw = z != NULL;
        for (UINT64 c = 0; zw && c < more; c++) zw = write_raw(v, &sds, have + c * v->cluster, z, v->cluster);
        kfree(z);
        if (!zw) goto out;
        have += more * v->cluster;
    }
    if (!write_raw(v, &sds, off, ent, elen) || !write_raw(v, &sds, off + SDS_MIRROR, ent, elen)) goto out;
    UINT8 *old = find_attr(rec, v->rec_size, AT_DATA, g_name_sds, 4);
    if (!old) goto out;
    if (size > sds.size) {
        UINT32 old_len = rd32(old + 4);
        UINT8 *keep = kmalloc(old_len);
        if (!keep) goto out;
        memcpy(keep, old, old_len);
        remove_attr(rec, old);
        UINT32 alen = make_nonresident(v, attr, v->rec_size - rec_used(rec), AT_DATA, g_name_sds, 4, &sds, have, size);
        if (!alen || !insert_attr(v, rec, attr, alen)) { insert_attr(v, rec, keep, old_len); kfree(keep); goto out; }
        kfree(keep);
        if (!write_record(v, SECURE_MFT, rec)) goto out;
    }
    kept = true;

    /* the indexes */
    UINT8 key[8];
    wr32(key, hash); wr32(key + 4, id);
    if (!load_index_named(v, SECURE_MFT, rec, g_name_sii, &sii, &nsii) ||
        !load_index_named(v, SECURE_MFT, rec, g_name_sdh, &sdh, &nsdh)) goto out;
    Ent e1 = sec_entry(key + 4, 4, ent, 0), e2 = sec_entry(key, 8, ent, 4);
    if (!e1.e || !e2.e) { kfree(e1.e); kfree(e2.e); goto out; }
    if (!sec_insert(&sii, &nsii, e1, false)) { kfree(e1.e); kfree(e2.e); goto out; }
    if (!sec_insert(&sdh, &nsdh, e2, true)) { kfree(e2.e); goto out; }
    if (!write_index_named(v, SECURE_MFT, rec, g_name_sii, sii, nsii) ||
        !write_index_named(v, SECURE_MFT, rec, g_name_sdh, sdh, nsdh)) goto out;
    struct SecEnt *ns = kmalloc((v->nsec + 1) * sizeof(struct SecEnt));
    if (ns) {
        if (v->nsec) memcpy(ns, v->sec, v->nsec * sizeof(struct SecEnt));
        ns[v->nsec++] = (struct SecEnt){ id, hash, elen, off };
        kfree(v->sec);
        v->sec = ns;
    } else {
        v->sec_read = false;                                     /* (read again next time) */
        kfree(v->sec);
        v->sec = NULL;
        v->nsec = 0;
    }
    ok = true;
out:
    if (!kept && grown_from != ~0ull) {                          /* give back the clusters $SDS did not keep */
        for (UINT32 i = 0; i < sds.nruns; i++) {
            Run r = sds.runs[i];
            if (r.lcn == SPARSE || r.vcn + r.len <= grown_from) continue;
            if (r.vcn < grown_from) { r.lcn += grown_from - r.vcn; r.len -= grown_from - r.vcn; r.vcn = grown_from; }
            free_runs(v, &r, 1);
        }
    }
    if (sii) ents_free(sii, nsii);
    if (sdh) ents_free(sdh, nsdh);
    stream_free(&sds);
    kfree(ent); kfree(rec); kfree(attr);
    return ok ? id : 0;
}

bool NtfsSetSecurityId(NtfsVol *v, UINT64 mft, UINT32 id)
{
    if (!v->rw || (mft < MFT_FIRST_USER && mft != NTFS_ROOT) || (id && !sec_find(v, id))) return false;
    UINT8 *rec = kmalloc(v->rec_size);
    bool ok = false;
    if (rec && read_record(v, mft, rec)) {
        UINT8 *si = find_attr(rec, v->rec_size, AT_STANDARD_INFO, NULL, 0);
        if (si && !si[8] && rd32(si + 0x10) >= 0x48) {
            UINT8 *val = si + rd16(si + 0x14);
            if (rd32(val + 0x34) == id) ok = true;
            else { wr32(val + 0x34, id); ok = write_record(v, mft, rec); }
        }
    }
    kfree(rec);
    return ok;
}

bool NtfsSetInfo(NtfsVol *v, UINT64 mft, UINT64 ctime, UINT64 mtime, UINT32 attrs)
{
    if (!v->rw || mft < MFT_FIRST_USER) return false;
    UINT8 *rec = kmalloc(v->rec_size);
    bool ok = false;
    if (rec && read_record(v, mft, rec)) {
        UINT8 *si = find_attr(rec, v->rec_size, AT_STANDARD_INFO, NULL, 0);
        if (si && !si[8] && rd32(si + 0x10) >= 0x30) {
            UINT8 *val = si + rd16(si + 0x14);
            UINT32 a = (rd32(val + 0x20) & ~0x27u) | (attrs & 0x27u);    /* read-only, hidden, system, archive */
            if (ctime) wr64(val, ctime);
            if (mtime) { wr64(val + 8, mtime); wr64(val + 0x10, mtime); }
            wr32(val + 0x20, a);
            ok = write_record(v, mft, rec) && update_dir_entries(v, mft, rec);
        }
    }
    kfree(rec);
    return ok;
}

typedef struct { const char *name; UINT64 mft; bool dir, found; } Lookup;

static int fold(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static bool lookup_one(const NtfsEntry *e, void *ctx)
{
    Lookup *l = ctx;
    const char *a = e->name, *b = l->name;
    while (*a && fold((UINT8)*a) == fold((UINT8)*b)) { a++; b++; }
    if (*a || *b) return true;
    l->mft = e->mft;
    l->dir = e->dir;
    l->found = true;
    return false;
}

bool NtfsLookup(NtfsVol *v, UINT64 dir, const char *name, UINT64 *mft, bool *is_dir)
{
    Lookup l = { name, 0, false, false };
    if (!NtfsList(v, dir, lookup_one, &l) && !l.found) return false;
    if (l.found) { *mft = l.mft; if (is_dir) *is_dir = l.dir; }
    return l.found;
}
