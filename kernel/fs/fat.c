/*
 * fat.c — FAT16/FAT32 with long file names (see fat.h)
 *
 * Layout and algorithms follow Microsoft's "FAT: General Overview of
 * On-Disk Format" (the FAT32 specification, 1.03).  Metadata (FAT and
 * directory sectors) goes through a small write-back sector cache that
 * FatSync writes out, mirroring FAT sectors to every FAT copy; file data
 * moves straight between the caller's buffer and the disk.
 *
 * Writes are ordered so that a crash leaves each file as it was or as it
 * was last written, never half of each: a file's data reaches the disk
 * before the FAT links that chain it, and those before the directory
 * entry that points at it (a dirty directory sector is written only after
 * every dirty FAT sector); a replaced or deleted file's clusters are not
 * freed, so not reused, until FatSync has put the entries that no longer
 * use them on the disk.
 *
 * A crash in the middle of that can still leave clusters marked as used
 * that no file reaches.  The volume's clean-shutdown bit (FAT[1], as
 * Windows keeps it) says whether that can have happened: it is cleared on
 * the disk before the first FAT or directory sector of a change is
 * written, and set again once FatSync has flushed everything.  FatReclaim
 * frees what no file or directory reaches on a volume mounted with it
 * clear.
 */

#include "fat.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../hal/rtc.h"

#define CACHE_N 64

typedef struct {
    UINT32 lba;                 /* volume-relative */
    UINT32 age;
    bool   valid, dirty;
    UINT8  data[BLOCK_SECTOR];
} CacheEnt;

struct FatVol {
    BlockDev *dev;
    UINT64    base;
    int       type;             /* 16 or 32 */
    UINT32    spc, cluster_bytes;
    UINT32    reserved, nfats, fat_sectors;
    UINT32    root_entries, root_start, root_sectors;
    UINT32    data_start, clusters, total;
    UINT32    root_cluster;     /* FAT32 */
    UINT32    fsinfo;           /* FAT32: FSInfo sector, 0 if none */
    UINT32    next_free;
    INT64     free_count;       /* -1: not counted yet */
    char      label[12];
    UINT32    clock;
    UINT32    stamp;            /* FatSetStamp */
    UINT32   *pending;          /* chains to free at the next FatSync (replaced or deleted files) */
    UINT32    npending, pending_cap;
    CacheEnt  cache[CACHE_N];
    UINT8    *cbuf;             /* one cluster */
    /* the last position found in a directory's cluster chain */
    UINT32    memo_dir, memo_k, memo_cluster;
    bool      clean_on_disk;    /* FAT[1]'s clean-shutdown bit is set on the disk */
    bool      clean_ok;         /* FatSync may set it (it was set at mount, or FatReclaim ran) */
};

/* ---------------------------------------------------------------------------
 * Sector cache
 * ------------------------------------------------------------------------- */
static bool dev_read(FatVol *v, UINT32 lba, UINT32 n, void *buf) { return v->dev->read(v->dev, v->base + lba, n, buf); }
static bool dev_write(FatVol *v, UINT32 lba, UINT32 n, const void *buf) { return v->dev->write(v->dev, v->base + lba, n, buf); }

static bool is_fat_sector(FatVol *v, UINT32 lba) { return lba >= v->reserved && lba < v->reserved + v->fat_sectors; }

static bool cache_writeback(FatVol *v, CacheEnt *c);

/* FAT[1]'s clean-shutdown bit (FAT32 bit 27, FAT16 bit 15) */
static UINT32 clean_bit(FatVol *v) { return v->type == 32 ? 0x08000000u : 0x8000u; }

/* Set or clear the clean-shutdown bit on the disk, in every FAT copy, and
 * flush.  Goes around the cache (a cached copy of the sector is updated
 * and written whole), so it never evicts a sector in the middle of a
 * write-back. */
static bool set_clean(FatVol *v, bool clean)
{
    UINT8 raw[BLOCK_SECTOR], *d = raw;
    CacheEnt *c = NULL;
    for (int i = 0; i < CACHE_N; i++)
        if (v->cache[i].valid && v->cache[i].lba == v->reserved) c = &v->cache[i];
    if (c) d = c->data;
    else if (!dev_read(v, v->reserved, 1, raw)) return false;
    UINT32 off = v->type == 32 ? 4 : 2, bit = clean_bit(v);
    if (v->type == 32) {
        UINT32 *p = (UINT32 *)(d + off);
        *p = clean ? *p | bit : *p & ~bit;
    } else {
        UINT16 *p = (UINT16 *)(d + off);
        *p = (UINT16)(clean ? *p | bit : *p & ~bit);
    }
    bool ok = true;
    for (UINT32 k = 0; k < v->nfats; k++) ok = dev_write(v, v->reserved + k * v->fat_sectors, 1, d) && ok;
    if (v->dev->flush) ok = v->dev->flush(v->dev) && ok;
    if (ok && c) c->dirty = false;
    if (ok) v->clean_on_disk = clean;
    return ok;
}

/* Every dirty FAT sector to the disk (before any directory sector: the
 * chains an entry points at are on the disk before the entry) */
static bool fat_writeback(FatVol *v)
{
    bool ok = true;
    for (int i = 0; i < CACHE_N; i++)
        if (v->cache[i].valid && v->cache[i].dirty && is_fat_sector(v, v->cache[i].lba)) ok = cache_writeback(v, &v->cache[i]) && ok;
    return ok;
}

static bool cache_writeback(FatVol *v, CacheEnt *c)
{
    if (!c->valid || !c->dirty) return true;
    /* the first metadata write of a change: the volume is not clean until FatSync says so */
    if (v->clean_on_disk && c->lba != v->fsinfo && !set_clean(v, false)) return false;
    if (!c->valid || !c->dirty) return true;              /* (set_clean wrote FAT[1]'s sector) */
    if (!is_fat_sector(v, c->lba) && !fat_writeback(v)) return false;
    bool ok = dev_write(v, c->lba, 1, c->data);
    if (is_fat_sector(v, c->lba))                                              /* mirror the FAT */
        for (UINT32 k = 1; k < v->nfats; k++) ok = dev_write(v, c->lba + k * v->fat_sectors, 1, c->data) && ok;
    if (ok) c->dirty = false;
    return ok;
}

/* The cached copy of sector @lba; @load false skips reading it (it will be overwritten). */
static CacheEnt *cache_get(FatVol *v, UINT32 lba, bool load)
{
    CacheEnt *victim = &v->cache[0];
    for (int i = 0; i < CACHE_N; i++) {
        CacheEnt *c = &v->cache[i];
        if (c->valid && c->lba == lba) { c->age = ++v->clock; return c; }
        if (!c->valid) victim = c;
        else if (victim->valid && c->age < victim->age) victim = c;
    }
    if (!cache_writeback(v, victim)) return NULL;
    victim->valid = false;
    if (load && !dev_read(v, lba, 1, victim->data)) return NULL;
    if (!load) memset(victim->data, 0, BLOCK_SECTOR);
    victim->lba = lba;
    victim->valid = true;
    victim->dirty = false;
    victim->age = ++v->clock;
    return victim;
}

/* Drop cached sectors in [lba, lba+n) without writing them (the range is being overwritten). */
static void cache_invalidate(FatVol *v, UINT32 lba, UINT32 n)
{
    for (int i = 0; i < CACHE_N; i++)
        if (v->cache[i].valid && v->cache[i].lba >= lba && v->cache[i].lba < lba + n) v->cache[i].valid = false;
}

static void free_chain(FatVol *v, UINT32 c);

bool FatSync(FatVol *v)
{
    bool ok = true;
    for (int i = 0; i < CACHE_N; i++) ok = cache_writeback(v, &v->cache[i]) && ok;
    if (v->npending) {                    /* the entries are on the disk: free what they let go of */
        if (v->dev->flush) ok = v->dev->flush(v->dev) && ok;
        if (ok) {
            for (UINT32 i = 0; i < v->npending; i++) free_chain(v, v->pending[i]);
            v->npending = 0;
            for (int i = 0; i < CACHE_N; i++) ok = cache_writeback(v, &v->cache[i]) && ok;
        }
    }
    if (v->fsinfo) {
        CacheEnt *c = cache_get(v, v->fsinfo, true);
        if (c && *(UINT32 *)c->data == 0x41615252u) {
            *(UINT32 *)(c->data + 488) = v->free_count >= 0 ? (UINT32)v->free_count : 0xFFFFFFFFu;
            *(UINT32 *)(c->data + 492) = v->next_free;
            c->dirty = true;
            ok = cache_writeback(v, c) && ok;
        }
    }
    if (v->dev->flush) ok = v->dev->flush(v->dev) && ok;
    /* everything is on the disk: mark the volume clean again */
    if (ok && !v->clean_on_disk && v->clean_ok && !v->npending) ok = set_clean(v, true);
    return ok;
}

/* ---------------------------------------------------------------------------
 * The FAT
 * ------------------------------------------------------------------------- */
static UINT32 eoc(FatVol *v) { return v->type == 32 ? 0x0FFFFFFFu : 0xFFFFu; }
static bool is_end(FatVol *v, UINT32 x) { return x < 2 || x >= (v->type == 32 ? 0x0FFFFFF7u : 0xFFF7u); }
static UINT32 clus_lba(FatVol *v, UINT32 c) { return v->data_start + (c - 2) * v->spc; }

static UINT32 fat_get(FatVol *v, UINT32 c)
{
    UINT32 off = v->type == 32 ? c * 4 : c * 2;
    CacheEnt *e = cache_get(v, v->reserved + off / BLOCK_SECTOR, true);
    if (!e) return eoc(v);
    off %= BLOCK_SECTOR;
    return v->type == 32 ? *(UINT32 *)(e->data + off) & 0x0FFFFFFFu : *(UINT16 *)(e->data + off);
}

static bool fat_set(FatVol *v, UINT32 c, UINT32 val)
{
    UINT32 off = v->type == 32 ? c * 4 : c * 2;
    CacheEnt *e = cache_get(v, v->reserved + off / BLOCK_SECTOR, true);
    if (!e) return false;
    off %= BLOCK_SECTOR;
    if (v->type == 32) {
        UINT32 *p = (UINT32 *)(e->data + off);
        *p = (*p & 0xF0000000u) | (val & 0x0FFFFFFFu);
    } else {
        *(UINT16 *)(e->data + off) = (UINT16)val;
    }
    e->dirty = true;
    return true;
}

static void count_free(FatVol *v)
{
    if (v->free_count >= 0) return;
    INT64 n = 0;
    for (UINT32 c = 2; c < v->clusters + 2; c++) if (!fat_get(v, c)) n++;
    v->free_count = n;
}

/* A free cluster, marked end-of-chain and linked after @prev (0: none). */
static UINT32 alloc_cluster(FatVol *v, UINT32 prev)
{
    UINT32 n = v->clusters, c = v->next_free;
    if (c < 2 || c >= n + 2) c = 2;
    for (UINT32 i = 0; i < n; i++, c++) {
        if (c >= n + 2) c = 2;
        if (fat_get(v, c)) continue;
        if (!fat_set(v, c, eoc(v))) return 0;
        if (prev && !fat_set(v, prev, c)) return 0;
        v->next_free = c + 1;
        if (v->free_count > 0) v->free_count--;
        return c;
    }
    return 0;
}

/* Free chain @c at the next FatSync (an entry pointed at it until now) */
static void free_later(FatVol *v, UINT32 c)
{
    if (c < 2) return;
    if (v->npending == v->pending_cap) {
        UINT32 cap = v->pending_cap ? 2 * v->pending_cap : 32;
        UINT32 *np = kmalloc(sizeof(UINT32) * cap);
        if (!np) { free_chain(v, c); return; }            /* (out of memory: free it now, as before) */
        if (v->npending) memcpy(np, v->pending, sizeof(UINT32) * v->npending);
        kfree(v->pending);
        v->pending = np;
        v->pending_cap = cap;
    }
    v->pending[v->npending++] = c;
}

static void free_chain(FatVol *v, UINT32 c)
{
    for (UINT32 guard = 0; !is_end(v, c) && guard <= v->clusters; guard++) {
        UINT32 next = fat_get(v, c);
        fat_set(v, c, 0);
        cache_invalidate(v, clus_lba(v, c), v->spc);
        if (v->free_count >= 0) v->free_count++;
        if (c < v->next_free) v->next_free = c;
        c = next;
    }
    v->memo_dir = 0;
}

/* ---------------------------------------------------------------------------
 * Directory entries
 * ------------------------------------------------------------------------- */
static bool fixed_root(FatVol *v, UINT32 dir) { return dir == FAT_ROOT && v->type == 16; }
static UINT32 first_cluster(FatVol *v, UINT32 dir) { return dir == FAT_ROOT ? v->root_cluster : dir; }

static bool zero_cluster(FatVol *v, UINT32 c)
{
    for (UINT32 s = 0; s < v->spc; s++) {
        CacheEnt *e = cache_get(v, clus_lba(v, c) + s, false);
        if (!e) return false;
        e->dirty = true;
    }
    return true;
}

/* Entry @idx of @dir: a pointer into the cache (valid until the next
 * cache use) and the entry's sector in *ce.  With @extend, the directory
 * grows by a zeroed cluster when @idx is past its end. */
static UINT8 *dir_entry(FatVol *v, UINT32 dir, UINT32 idx, bool extend, CacheEnt **ce)
{
    UINT32 lba;
    if (fixed_root(v, dir)) {
        if (idx >= v->root_entries) return NULL;
        lba = v->root_start + idx / (BLOCK_SECTOR / 32);
    } else {
        UINT32 per = v->cluster_bytes / 32, k = idx / per, c, i = 0;
        if (v->memo_dir == dir && v->memo_k <= k) { c = v->memo_cluster; i = v->memo_k; }
        else c = first_cluster(v, dir);
        for (; i < k; i++) {
            UINT32 next = fat_get(v, c);
            if (is_end(v, next)) {
                if (!extend) return NULL;
                next = alloc_cluster(v, c);
                if (!next || !zero_cluster(v, next)) return NULL;
            }
            c = next;
        }
        v->memo_dir = dir;
        v->memo_k = k;
        v->memo_cluster = c;
        lba = clus_lba(v, c) + (idx % per) * 32 / BLOCK_SECTOR;
    }
    CacheEnt *e = cache_get(v, lba, true);
    if (!e) return NULL;
    *ce = e;
    return e->data + (idx * 32) % BLOCK_SECTOR;
}

static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static bool name_eq(const char *a, const char *b)
{
    while (*a && up(*a) == up(*b)) { a++; b++; }
    return !*a && !*b;
}

static UINT8 lfn_sum(const UINT8 *sfn)
{
    UINT8 s = 0;
    for (int i = 0; i < 11; i++) s = (UINT8)(((s & 1) << 7) + (s >> 1) + sfn[i]);
    return s;
}

/* UTF-8 <-> UCS-2 (the Basic Multilingual Plane; others become '_') */
static int utf8_to_ucs2(const char *s, UINT16 *out, int cap)
{
    int n = 0;
    const UINT8 *p = (const UINT8 *)s;
    while (*p && n < cap) {
        UINT32 c = *p++;
        if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) c = (c & 0x1F) << 6 | (*p++ & 0x3F);
        else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
            c = (c & 0x0F) << 12 | (UINT32)(p[0] & 0x3F) << 6 | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0x80) {
            c = '_';
            while ((*p & 0xC0) == 0x80) p++;
        }
        out[n++] = (UINT16)c;
    }
    return n;
}

static void ucs2_to_utf8(const UINT16 *s, int n, char *out, int cap)
{
    int k = 0;
    for (int i = 0; i < n && s[i]; i++) {
        UINT16 c = s[i];
        if (c < 0x80) { if (k + 1 >= cap) break; out[k++] = (char)c; }
        else if (c < 0x800) { if (k + 2 >= cap) break; out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
        else { if (k + 3 >= cap) break; out[k++] = (char)(0xE0 | c >> 12); out[k++] = (char)(0x80 | (c >> 6 & 0x3F)); out[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[k] = '\0';
}

static void sfn_to_name(const UINT8 *e, char *out)
{
    int k = 0;
    bool lower_base = e[12] & 0x08, lower_ext = e[12] & 0x10;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
        out[k++] = lower_base && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (e[8] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
            out[k++] = lower_ext && e[i] >= 'A' && e[i] <= 'Z' ? (char)(e[i] + 32) : (char)e[i];
    }
    out[k] = '\0';
}

static UINT32 entry_cluster(FatVol *v, const UINT8 *e)
{
    UINT32 c = *(const UINT16 *)(e + 26);
    if (v->type == 32) c |= (UINT32)*(const UINT16 *)(e + 20) << 16;
    return c;
}

/* Walks @dir, calling @fn with each complete entry (8.3 ones included). */
static bool dir_walk(FatVol *v, UINT32 dir, bool (*fn)(FatVol *, const FatEntry *, const UINT8 *, void *), void *ctx)
{
    UINT16 lfn[260];
    int lfn_parts = 0;
    UINT8 lfn_chk = 0;
    UINT32 lfn_first = 0;
    FatEntry fe;
    for (UINT32 idx = 0;; idx++) {
        CacheEnt *ce;
        UINT8 *e = dir_entry(v, dir, idx, false, &ce);
        if (!e || e[0] == 0x00) return true;
        if (e[0] == 0xE5) { lfn_parts = 0; continue; }
        if (e[11] == 0x0F) {
            int ord = e[0] & 0x1F;
            if (e[0] & 0x40) {
                memset(lfn, 0, sizeof(lfn));
                lfn_parts = ord;
                lfn_chk = e[13];
                lfn_first = idx;
            } else if (!lfn_parts || e[13] != lfn_chk) {
                lfn_parts = 0;
                continue;
            }
            if (ord < 1 || ord > 20) { lfn_parts = 0; continue; }
            UINT16 *dst = lfn + (ord - 1) * 13;
            for (int i = 0; i < 5; i++) dst[i] = *(UINT16 *)(e + 1 + 2 * i);
            for (int i = 0; i < 6; i++) dst[5 + i] = *(UINT16 *)(e + 14 + 2 * i);
            for (int i = 0; i < 2; i++) dst[11 + i] = *(UINT16 *)(e + 28 + 2 * i);
            continue;
        }
        if (e[11] & 0x08) { lfn_parts = 0; continue; }       /* volume label */
        bool dot = e[0] == '.';
        memset(&fe, 0, sizeof(fe));
        if (lfn_parts && lfn_chk == lfn_sum(e)) {
            for (int i = 0; i < 260; i++) if (lfn[i] == 0xFFFF) lfn[i] = 0;
            ucs2_to_utf8(lfn, 260, fe.name, FAT_NAME_MAX);
            fe.first_index = lfn_first;
        } else {
            sfn_to_name(e, fe.name);
            fe.first_index = idx;
        }
        lfn_parts = 0;
        if (dot) continue;
        fe.dir = (e[11] & 0x10) != 0;
        fe.cluster = entry_cluster(v, e);
        if (fe.dir && fe.cluster == 0) fe.cluster = FAT_ROOT;
        fe.size = *(UINT32 *)(e + 28);
        fe.wtime = (UINT32)*(UINT16 *)(e + 24) << 16 | *(UINT16 *)(e + 22);
        fe.attr = e[11];
        fe.dir_cluster = dir;
        fe.index = idx;
        if (!fn(v, &fe, e, ctx)) return true;
    }
}

typedef struct { bool (*fn)(const FatEntry *, void *); void *ctx; } ListCtx;
static bool list_cb(FatVol *v, const FatEntry *e, const UINT8 *raw, void *ctx)
{
    (void)v; (void)raw;
    ListCtx *l = ctx;
    return l->fn(e, l->ctx);
}

bool FatList(FatVol *v, UINT32 dir, bool (*fn)(const FatEntry *e, void *ctx), void *ctx)
{
    ListCtx l = { fn, ctx };
    return dir_walk(v, dir, list_cb, &l);
}

typedef struct { const char *name; FatEntry *out; bool found; } FindCtx;
static bool find_cb(FatVol *v, const FatEntry *e, const UINT8 *raw, void *ctx)
{
    FindCtx *f = ctx;
    char sfn[13];
    (void)v;
    sfn_to_name(raw, sfn);
    if (name_eq(e->name, f->name) || name_eq(sfn, f->name)) {
        *f->out = *e;
        f->found = true;
        return false;
    }
    return true;
}

bool FatLookup(FatVol *v, UINT32 dir, const char *name, FatEntry *out)
{
    FatEntry tmp;
    FindCtx f = { name, out ? out : &tmp, false };
    dir_walk(v, dir, find_cb, &f);
    return f.found;
}

bool FatLookupPath(FatVol *v, const char *path, FatEntry *out)
{
    memset(out, 0, sizeof(*out));
    out->dir = true;
    out->cluster = FAT_ROOT;
    char part[FAT_NAME_MAX];
    while (*path) {
        while (*path == '\\' || *path == '/') path++;
        if (!*path) break;
        int n = 0;
        while (*path && *path != '\\' && *path != '/' && n < FAT_NAME_MAX - 1) part[n++] = *path++;
        part[n] = '\0';
        if (!out->dir || !FatLookup(v, out->cluster, part, out)) return false;
    }
    return true;
}

/* ---- creating entries ---- */
static bool sfn_char_ok(char c)
{
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return c && strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

/* true if @name is already a valid upper-case 8.3 name; its 11-byte form in @sfn */
static bool exact_sfn(const char *name, UINT8 *sfn)
{
    memset(sfn, ' ', 11);
    int i = 0, k = 0;
    for (; name[i] && name[i] != '.'; i++) {
        if (k >= 8 || !sfn_char_ok(name[i])) return false;
        sfn[k++] = (UINT8)name[i];
    }
    if (!k) return false;
    if (name[i] == '.') {
        i++;
        for (k = 8; name[i]; i++) {
            if (k >= 11 || !sfn_char_ok(name[i])) return false;
            sfn[k++] = (UINT8)name[i];
        }
        if (k == 8) return false;
    }
    return true;
}

#define ALIAS_MAX 131072                      /* ~1 .. ~131071 */
typedef struct { const UINT8 *base, *ext; int nb, ne; UINT8 *used; } AliasCtx;

/* Marks N when @raw is "BASIS~N.EXT" for this basis (cut to fit the tail) */
static bool alias_cb(FatVol *v, const FatEntry *e, const UINT8 *raw, void *ctx)
{
    AliasCtx *a = ctx;
    (void)v; (void)e;
    if (memcmp(raw + 8, a->ext, (size_t)a->ne)) return true;
    for (int i = a->ne; i < 3; i++) if (raw[8 + i] != ' ') return true;
    int t = 0;
    while (t < 8 && raw[t] != '~') t++;
    if (t == 8 || t > a->nb || memcmp(raw, a->base, (size_t)t)) return true;
    UINT32 n = 0;
    int i = t + 1, digits = 0;
    for (; i < 8 && raw[i] >= '0' && raw[i] <= '9'; i++, digits++) n = n * 10 + (UINT32)(raw[i] - '0');
    for (; i < 8; i++) if (raw[i] != ' ') return true;
    if (!digits || n >= ALIAS_MAX) return true;
    int tl = 1 + digits, keep = a->nb + tl > 8 ? 8 - tl : a->nb;
    if (keep != t) return true;
    a->used[n / 8] |= (UINT8)(1u << (n % 8));
    return true;
}

/* The "BASIS~N.EXT" alias of a long name, unique in @dir */
static bool make_alias(FatVol *v, UINT32 dir, const char *name, UINT8 *sfn)
{
    UINT8 base[8], ext[3];
    int nb = 0, ne = 0;
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    for (const char *p = name; *p && p != dot; p++) {
        char c = up(*p);
        if (c == ' ' || c == '.') continue;
        if (nb < 8) base[nb++] = (UINT8)(sfn_char_ok(c) ? c : '_');
    }
    if (dot)
        for (const char *p = dot + 1; *p && ne < 3; p++) {
            char c = up(*p);
            if (c == ' ') continue;
            ext[ne++] = (UINT8)(sfn_char_ok(c) ? c : '_');
        }
    if (!nb) base[nb++] = '_';
    /* one walk marks the ~N tails already taken for this basis and
     * extension (a walk per candidate made big directories quadratic) */
    AliasCtx a = { base, ext, nb, ne, kzalloc(ALIAS_MAX / 8) };
    if (!a.used) return false;
    dir_walk(v, dir, alias_cb, &a);
    UINT32 n = 1;
    while (n < ALIAS_MAX && (a.used[n / 8] & (1u << (n % 8)))) n++;
    kfree(a.used);
    if (n >= ALIAS_MAX) return false;
    char tail[8];
    int tl = ksnprintf(tail, sizeof(tail), "~%u", n);
    int keep = nb + tl > 8 ? 8 - tl : nb;
    memset(sfn, ' ', 11);
    memcpy(sfn, base, (size_t)keep);
    memcpy(sfn + keep, tail, (size_t)tl);
    memcpy(sfn + 8, ext, (size_t)ne);
    return true;
}

void FatSetStamp(FatVol *v, UINT32 dos_time) { v->stamp = dos_time; }

static void stamp(FatVol *v, UINT8 *e, bool create)
{
    RtcTime t;
    rtc_read(&t);
    UINT16 date = (UINT16)((t.year >= 1980 ? t.year - 1980 : 0) << 9 | t.month << 5 | t.day);
    UINT16 time = (UINT16)(t.hour << 11 | t.minute << 5 | t.second / 2);
    if (v->stamp) { date = (UINT16)(v->stamp >> 16); time = (UINT16)v->stamp; }
    if (create) {
        *(UINT16 *)(e + 14) = time;
        *(UINT16 *)(e + 16) = date;
    }
    *(UINT16 *)(e + 18) = date;               /* last access */
    *(UINT16 *)(e + 22) = time;
    *(UINT16 *)(e + 24) = date;
}

static void set_cluster(FatVol *v, UINT8 *e, UINT32 c)
{
    *(UINT16 *)(e + 26) = (UINT16)c;
    *(UINT16 *)(e + 20) = v->type == 32 ? (UINT16)(c >> 16) : 0;
}

static bool create_entry(FatVol *v, UINT32 dir, const char *name, UINT8 attr, UINT32 cluster, UINT32 size)
{
    UINT8 sfn[11];
    UINT16 lname[256];
    int nl = 0, parts = 0;
    if (!exact_sfn(name, sfn)) {
        nl = utf8_to_ucs2(name, lname, 255);
        if (!nl || !make_alias(v, dir, name, sfn)) return false;
        parts = (nl + 12) / 13;
    }
    /* find (parts + 1) free entries in a row, growing the directory if need be */
    UINT32 need = (UINT32)parts + 1, run = 0, start = 0;
    for (UINT32 idx = 0;; idx++) {
        CacheEnt *ce;
        UINT8 *e = dir_entry(v, dir, idx, true, &ce);
        if (!e) return false;                            /* full (FAT16 root) or disk full */
        if (e[0] == 0x00 || e[0] == 0xE5) {
            if (!run++) start = idx;
            if (run == need) break;
        } else run = 0;
    }
    UINT8 chk = lfn_sum(sfn);
    for (int p = parts; p >= 1; p--) {
        CacheEnt *ce;
        UINT8 *e = dir_entry(v, dir, start + (UINT32)(parts - p), true, &ce);
        if (!e) return false;
        memset(e, 0, 32);
        e[0] = (UINT8)(p | (p == parts ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = chk;
        for (int i = 0; i < 13; i++) {
            int ci = (p - 1) * 13 + i;
            UINT16 ch = ci < nl ? lname[ci] : ci == nl ? 0 : 0xFFFF;
            int off = i < 5 ? 1 + 2 * i : i < 11 ? 14 + 2 * (i - 5) : 28 + 2 * (i - 11);
            *(UINT16 *)(e + off) = ch;
        }
        ce->dirty = true;
    }
    CacheEnt *ce;
    UINT8 *e = dir_entry(v, dir, start + (UINT32)parts, true, &ce);
    if (!e) return false;
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    if (e[0] == 0xE5) e[0] = 0x05;
    e[11] = attr;
    stamp(v, e, true);
    set_cluster(v, e, cluster);
    *(UINT32 *)(e + 28) = size;
    ce->dirty = true;
    return true;
}

static void erase_entry(FatVol *v, const FatEntry *fe)
{
    for (UINT32 i = fe->first_index; i <= fe->index; i++) {
        CacheEnt *ce;
        UINT8 *e = dir_entry(v, fe->dir_cluster, i, false, &ce);
        if (!e) return;
        e[0] = 0xE5;
        ce->dirty = true;
    }
}

/* ---------------------------------------------------------------------------
 * Files and directories
 * ------------------------------------------------------------------------- */
bool FatRead(FatVol *v, const FatEntry *fe, void *buf)
{
    UINT8 *out = buf;
    UINT32 left = fe->size, c = fe->cluster;
    while (left) {
        if (is_end(v, c) || c >= v->clusters + 2) return false;
        /* a run of consecutive clusters in one transfer */
        UINT32 run = 1, last = c;
        while ((UINT64)run * v->cluster_bytes < left) {
            UINT32 n = fat_get(v, last);
            if (n != last + 1) break;
            last = n;
            run++;
        }
        UINT32 bytes = run * v->cluster_bytes;
        if (bytes <= left) {
            if (!dev_read(v, clus_lba(v, c), run * v->spc, out)) return false;
        } else {
            if (run > 1 && !dev_read(v, clus_lba(v, c), (run - 1) * v->spc, out)) return false;
            if (!dev_read(v, clus_lba(v, last), v->spc, v->cbuf)) return false;
            memcpy(out + (run - 1) * v->cluster_bytes, v->cbuf, left - (run - 1) * v->cluster_bytes);
            bytes = left;
        }
        out += bytes;
        left -= bytes;
        c = fat_get(v, last);
    }
    return true;
}

/* A new chain holding @data; its first cluster (0 for an empty file) in *first. */
static bool write_chain(FatVol *v, const UINT8 *data, UINT32 len, UINT32 *first)
{
    *first = 0;
    UINT32 need = (len + v->cluster_bytes - 1) / v->cluster_bytes;
    count_free(v);
    if (need > v->free_count) return false;
    UINT32 prev = 0, run_start = 0, run_len = 0, off = 0;
    for (UINT32 i = 0; i < need; i++) {
        UINT32 c = alloc_cluster(v, prev);
        if (!c) { if (*first) free_chain(v, *first); *first = 0; return false; }
        if (!*first) *first = c;
        if (run_len && c == run_start + run_len && run_len < 64) run_len++;
        else {
            if (run_len) {                    /* write the previous run */
                cache_invalidate(v, clus_lba(v, run_start), run_len * v->spc);
                if (!dev_write(v, clus_lba(v, run_start), run_len * v->spc, data + off)) goto fail;
                off += run_len * v->cluster_bytes;
            }
            run_start = c;
            run_len = 1;
        }
        prev = c;
    }
    if (run_len) {
        UINT32 bytes = len - off, full = bytes / v->cluster_bytes;
        cache_invalidate(v, clus_lba(v, run_start), run_len * v->spc);
        if (full && !dev_write(v, clus_lba(v, run_start), full * v->spc, data + off)) goto fail;
        if (bytes % v->cluster_bytes) {
            memset(v->cbuf, 0, v->cluster_bytes);
            memcpy(v->cbuf, data + off + full * v->cluster_bytes, bytes % v->cluster_bytes);
            if (!dev_write(v, clus_lba(v, run_start + full), v->spc, v->cbuf)) goto fail;
        }
    }
    return true;
fail:
    free_chain(v, *first);
    *first = 0;
    return false;
}

bool FatWriteFile(FatVol *v, UINT32 dir, const char *name, const void *data, UINT32 len)
{
    FatEntry fe;
    bool exists = FatLookup(v, dir, name, &fe);
    if (exists && fe.dir) return false;
    UINT32 first, need = (len + v->cluster_bytes - 1) / v->cluster_bytes;
    count_free(v);
    if (need > v->free_count && v->npending) FatSync(v);   /* (frees what earlier writes let go of) */
    if (exists && need > v->free_count) {                  /* no room for both: free the old contents first */
        free_chain(v, fe.cluster);
        CacheEnt *ce;
        UINT8 *e = dir_entry(v, dir, fe.index, false, &ce);
        if (!e) return false;
        set_cluster(v, e, 0);
        *(UINT32 *)(e + 28) = 0;
        ce->dirty = true;
    }
    if (!write_chain(v, data, len, &first)) return false;
    if (!exists) {
        if (create_entry(v, dir, name, 0x20, first, len)) return true;
        free_chain(v, first);
        return false;
    }
    CacheEnt *ce;
    UINT8 *e = dir_entry(v, dir, fe.index, false, &ce);
    if (!e) { free_chain(v, first); return false; }
    UINT32 old = entry_cluster(v, e);                      /* (0 if freed above) */
    set_cluster(v, e, first);
    *(UINT32 *)(e + 28) = len;
    e[11] |= 0x20;                                         /* archive */
    stamp(v, e, false);
    ce->dirty = true;
    free_later(v, old);                                    /* (the old contents stay until the entry is on the disk) */
    return true;
}

bool FatMkdir(FatVol *v, UINT32 dir, const char *name, UINT32 *out)
{
    FatEntry fe;
    if (FatLookup(v, dir, name, &fe)) {
        if (!fe.dir) return false;
        if (out) *out = fe.cluster;
        return true;
    }
    UINT32 c = alloc_cluster(v, 0);
    if (!c) return false;
    if (!zero_cluster(v, c)) { free_chain(v, c); return false; }
    CacheEnt *ce = cache_get(v, clus_lba(v, c), true);
    if (!ce) { free_chain(v, c); return false; }
    UINT8 *e = ce->data;
    memset(e, ' ', 11);
    e[0] = '.';
    e[11] = 0x10;
    stamp(v, e, true);
    set_cluster(v, e, c);
    e += 32;
    memset(e, ' ', 11);
    e[0] = e[1] = '.';
    e[11] = 0x10;
    stamp(v, e, true);
    set_cluster(v, e, dir == FAT_ROOT ? 0 : dir);
    ce->dirty = true;
    if (!create_entry(v, dir, name, 0x10, c, 0)) { free_chain(v, c); return false; }
    if (out) *out = c;
    return true;
}

bool FatMkdirPath(FatVol *v, const char *path, UINT32 *out)
{
    UINT32 dir = FAT_ROOT;
    char part[FAT_NAME_MAX];
    while (*path) {
        while (*path == '\\' || *path == '/') path++;
        if (!*path) break;
        int n = 0;
        while (*path && *path != '\\' && *path != '/' && n < FAT_NAME_MAX - 1) part[n++] = *path++;
        part[n] = '\0';
        if (!FatMkdir(v, dir, part, &dir)) return false;
    }
    if (out) *out = dir;
    return true;
}

typedef struct { char names[16][FAT_NAME_MAX]; int n; } Batch;
static bool batch_cb(const FatEntry *e, void *ctx)
{
    Batch *b = ctx;
    strncpy(b->names[b->n], e->name, FAT_NAME_MAX - 1);
    b->names[b->n][FAT_NAME_MAX - 1] = '\0';
    return ++b->n < 16;
}

static bool delete_entry(FatVol *v, const FatEntry *fe, int depth)
{
    if (fe->dir) {
        if (depth > 32) return false;
        Batch *b = kmalloc(sizeof(Batch));
        if (!b) return false;
        for (;;) {                                         /* children, a batch at a time */
            b->n = 0;
            FatList(v, fe->cluster, batch_cb, b);
            if (!b->n) break;
            for (int i = 0; i < b->n; i++) {
                FatEntry ce;
                if (!FatLookup(v, fe->cluster, b->names[i], &ce) || !delete_entry(v, &ce, depth + 1)) { kfree(b); return false; }
            }
        }
        kfree(b);
    }
    /* the directory's own entry may have moved in the cache: find it again */
    CacheEnt *c;
    UINT8 *e = dir_entry(v, fe->dir_cluster, fe->index, false, &c);
    if (!e) return false;
    erase_entry(v, fe);
    if (fe->cluster != FAT_ROOT) free_later(v, fe->cluster);
    return true;
}

bool FatDelete(FatVol *v, UINT32 dir, const char *name)
{
    FatEntry fe;
    if (!FatLookup(v, dir, name, &fe)) return false;
    return delete_entry(v, &fe, 0);
}

/* ---------------------------------------------------------------------------
 * Reclaiming clusters no file reaches (after a crash in the middle of a change)
 * ------------------------------------------------------------------------- */
typedef struct {
    FatVol *v;
    UINT8  *fat;                /* the whole FAT, as read from the disk */
    UINT8  *seen;               /* a bit per cluster: reached from the root */
    UINT32  n;                  /* clusters + 2 */
    UINT32  crossed;
} Reach;

static UINT32 reach_get(Reach *r, UINT32 c)
{
    return r->v->type == 32 ? ((UINT32 *)r->fat)[c] & 0x0FFFFFFFu : ((UINT16 *)r->fat)[c];
}
static bool seen(Reach *r, UINT32 c) { return r->seen[c >> 3] & (1u << (c & 7)); }

/* Mark the chain from @c as reached.  A chain that runs into a cluster
 * already reached (two chains sharing clusters, or a loop) stops there:
 * the shared clusters stay in use, and the crossing is counted. */
static void reach_chain(Reach *r, UINT32 c)
{
    while (c >= 2 && c < r->n) {
        if (seen(r, c)) { r->crossed++; return; }
        r->seen[c >> 3] |= (UINT8)(1u << (c & 7));
        c = reach_get(r, c);
        if (is_end(r->v, c)) return;
    }
}

/* The entries in @len bytes of a directory: files' chains are marked, new
 * directories are marked and pushed.  1 at the end-of-directory entry, 0
 * if the directory may go on, -1 if the stack could not grow. */
static int reach_entries(Reach *r, const UINT8 *buf, UINT32 len, UINT32 **stack, UINT32 *depth, UINT32 *cap)
{
    for (UINT32 off = 0; off + 32 <= len; off += 32) {
        const UINT8 *e = buf + off;
        if (e[0] == 0x00) return 1;
        if (e[0] == 0xE5 || e[0] == '.' || e[11] == 0x0F || (e[11] & 0x08)) continue;
        UINT32 c = entry_cluster(r->v, e);
        if (c < 2 || c >= r->n) continue;
        if (!(e[11] & 0x10)) { reach_chain(r, c); continue; }
        if (seen(r, c)) { r->crossed++; continue; }            /* (a directory reached twice: not walked again) */
        reach_chain(r, c);
        if (*depth == *cap) {
            UINT32 ncap = *cap ? 2 * *cap : 256, *ns = kmalloc(sizeof(UINT32) * ncap);
            if (!ns) return -1;
            if (*depth) memcpy(ns, *stack, sizeof(UINT32) * *depth);
            kfree(*stack);
            *stack = ns;
            *cap = ncap;
        }
        (*stack)[(*depth)++] = c;
    }
    return 0;
}

bool FatReclaim(FatVol *v, bool force, FatReclaimInfo *info)
{
    memset(info, 0, sizeof(*info));
    if (v->clean_ok && !force) return true;
    for (int i = 0; i < CACHE_N; i++)
        if (!cache_writeback(v, &v->cache[i])) return false;
    Reach r = { v, NULL, NULL, v->clusters + 2, 0 };
    UINT32 esz = v->type == 32 ? 4 : 2, secs = (r.n * esz + BLOCK_SECTOR - 1) / BLOCK_SECTOR;
    if (secs > v->fat_sectors) return false;
    UINT32 blen = v->cluster_bytes > v->root_sectors * BLOCK_SECTOR ? v->cluster_bytes : v->root_sectors * BLOCK_SECTOR;
    UINT32 *stack = NULL, depth = 0, cap = 0;
    r.fat = kmalloc((UINT64)secs * BLOCK_SECTOR);
    r.seen = kzalloc((r.n + 7) / 8);
    UINT8 *buf = kmalloc(blen), *dirty = kzalloc((secs + 7) / 8);
    bool ok = r.fat && r.seen && buf && dirty;
    for (UINT32 s = 0; ok && s < secs; s += 128)
        ok = dev_read(v, v->reserved + s, secs - s < 128 ? secs - s : 128, r.fat + (UINT64)s * BLOCK_SECTOR);
    /* everything reachable from the root directory */
    if (ok && v->type == 16) {
        ok = dev_read(v, v->root_start, v->root_sectors, buf) &&
             reach_entries(&r, buf, v->root_sectors * BLOCK_SECTOR, &stack, &depth, &cap) >= 0;
    } else if (ok) {
        reach_chain(&r, v->root_cluster);
        stack = kmalloc(sizeof(UINT32) * 256);
        cap = stack ? 256 : 0;
        ok = stack != NULL;
        if (ok) stack[depth++] = v->root_cluster;
    }
    while (ok && depth) {
        UINT32 c = stack[--depth];
        for (UINT32 guard = 0; guard < r.n && c >= 2 && c < r.n; guard++) {
            if (!(ok = dev_read(v, clus_lba(v, c), v->spc, buf))) break;
            int end = reach_entries(&r, buf, v->cluster_bytes, &stack, &depth, &cap);
            if (end) { ok = end > 0; break; }
            c = reach_get(&r, c);
            if (is_end(v, c)) break;
        }
    }
    /* free what was not reached (bad clusters and reserved values stay) */
    UINT32 bad = v->type == 32 ? 0x0FFFFFF7u : 0xFFF7u, free = 0;
    for (UINT32 c = 2; ok && c < r.n; c++) {
        UINT32 x = reach_get(&r, c);
        if (!x) { free++; continue; }
        if (seen(&r, c) || x == bad || (x >= r.n && x < bad)) continue;
        if (v->type == 32) ((UINT32 *)r.fat)[c] &= 0xF0000000u;
        else ((UINT16 *)r.fat)[c] = 0;
        dirty[c * esz / BLOCK_SECTOR / 8] |= (UINT8)(1u << (c * esz / BLOCK_SECTOR % 8));
        info->reclaimed++;
        free++;
    }
    if (ok && info->reclaimed) {
        if (v->clean_on_disk && (ok = set_clean(v, false))) {     /* (a forced scan of a clean volume) */
            if (v->type == 32) ((UINT32 *)r.fat)[1] &= ~clean_bit(v);
            else ((UINT16 *)r.fat)[1] &= (UINT16)~clean_bit(v);
        }
        for (UINT32 s = 0; ok && s < secs; s++) {
            if (!(dirty[s / 8] & (1u << (s % 8)))) continue;
            for (UINT32 k = 0; ok && k < v->nfats; k++)
                ok = dev_write(v, v->reserved + k * v->fat_sectors + s, 1, r.fat + (UINT64)s * BLOCK_SECTOR);
        }
        cache_invalidate(v, v->reserved, v->fat_sectors);
        v->memo_dir = 0;
        v->next_free = 2;
    }
    if (ok) {
        v->free_count = free;
        v->clean_ok = true;
        ok = FatSync(v);                                   /* (sets the clean-shutdown bit) */
    }
    info->scanned = true;
    info->crossed = r.crossed;
    info->free = free;
    kfree(stack);
    kfree(dirty);
    kfree(buf);
    kfree(r.seen);
    kfree(r.fat);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Volumes
 * ------------------------------------------------------------------------- */
const char *FatLabel(const FatVol *v) { return v->label; }
int FatType(const FatVol *v) { return v->type; }
BlockDev *FatDevice(const FatVol *v) { return v->dev; }
UINT32 FatClusterBytes(const FatVol *v) { return v->cluster_bytes; }
UINT64 FatTotalBytes(const FatVol *v) { return (UINT64)v->clusters * v->cluster_bytes; }
UINT64 FatFreeBytes(FatVol *v) { count_free(v); return (UINT64)v->free_count * v->cluster_bytes; }

static void trim_label(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
}

FatVol *FatMount(BlockDev *dev, UINT64 lba)
{
    UINT8 *bs = kmalloc(BLOCK_SECTOR);
    if (!bs) return NULL;
    FatVol *v = NULL;
    if (!dev->read(dev, lba, 1, bs)) goto out;
    if (bs[510] != 0x55 || bs[511] != 0xAA || (bs[0] != 0xEB && bs[0] != 0xE9)) goto out;
    UINT32 bps = *(UINT16 *)(bs + 11), spc = bs[13], reserved = *(UINT16 *)(bs + 14), nfats = bs[16];
    UINT32 root_entries = *(UINT16 *)(bs + 17), total = *(UINT16 *)(bs + 19), fat16 = *(UINT16 *)(bs + 22);
    if (!total) total = *(UINT32 *)(bs + 32);
    UINT32 fat_sectors = fat16 ? fat16 : *(UINT32 *)(bs + 36);
    if (bps != BLOCK_SECTOR || !spc || (spc & (spc - 1)) || spc > 128 || !reserved || !nfats || !fat_sectors) goto out;
    UINT32 root_sectors = (root_entries * 32 + bps - 1) / bps;
    UINT32 data_start = reserved + nfats * fat_sectors + root_sectors;
    if (total <= data_start || lba + total > dev->sectors) goto out;
    UINT32 clusters = (total - data_start) / spc;
    if (clusters < 4085) goto out;                        /* FAT12: not supported */

    v = kzalloc(sizeof(FatVol));
    if (!v) goto out;
    v->cbuf = kmalloc(spc * bps);
    if (!v->cbuf) { kfree(v); v = NULL; goto out; }
    v->dev = dev;
    v->base = lba;
    v->type = clusters < 65525 ? 16 : 32;
    v->spc = spc;
    v->cluster_bytes = spc * bps;
    v->reserved = reserved;
    v->nfats = nfats;
    v->fat_sectors = fat_sectors;
    v->root_entries = root_entries;
    v->root_start = reserved + nfats * fat_sectors;
    v->root_sectors = root_sectors;
    v->data_start = data_start;
    v->clusters = clusters;
    v->total = total;
    v->free_count = -1;
    v->next_free = 2;
    if (v->type == 32) {
        v->root_cluster = *(UINT32 *)(bs + 44);
        v->fsinfo = *(UINT16 *)(bs + 48);
        if (v->fsinfo == 0 || v->fsinfo >= reserved) v->fsinfo = 0;
        memcpy(v->label, bs + 71, 11);
    } else {
        memcpy(v->label, bs + 43, 11);
    }
    /* the root directory's volume-label entry wins over the boot sector's copy */
    for (UINT32 idx = 0;; idx++) {
        CacheEnt *ce;
        UINT8 *e = dir_entry(v, FAT_ROOT, idx, false, &ce);
        if (!e || !e[0]) break;
        if (e[0] != 0xE5 && e[11] != 0x0F && (e[11] & 0x08)) { memcpy(v->label, e, 11); break; }
    }
    v->label[11] = '\0';
    trim_label(v->label);
    if (v->fsinfo) {
        CacheEnt *c = cache_get(v, v->fsinfo, true);
        if (c && *(UINT32 *)c->data == 0x41615252u) {
            UINT32 hint = *(UINT32 *)(c->data + 492);
            if (hint >= 2 && hint < clusters + 2) v->next_free = hint;
        }
    }
    v->clean_on_disk = v->clean_ok = (fat_get(v, 1) & clean_bit(v)) != 0;
    kprintf("[FAT] %s at LBA %llu: FAT%d \"%s\", %u MiB%s\n", dev->name, (unsigned long long)lba, v->type, v->label,
            (unsigned)(FatTotalBytes(v) >> 20), v->clean_ok ? "" : " (not closed cleanly)");
out:
    kfree(bs);
    return v;
}

void FatUnmount(FatVol *v)
{
    if (!v) return;
    FatSync(v);
    kfree(v->pending);
    kfree(v->cbuf);
    kfree(v);
}

FatVol *FatFormat(BlockDev *dev, UINT64 lba, UINT64 sectors, const char *label)
{
    if (sectors > 0xFFFFFFFFull) sectors = 0xFFFFFFFFull;
    UINT32 total = (UINT32)sectors, reserved = 32, nfats = 2, spc = 8;
    while (spc > 1 && total / spc < 70000) spc /= 2;
    if (total / spc < 66000) return NULL;                 /* too small for FAT32 (about 34 MiB) */
    UINT32 fs = 1;
    for (int i = 0; i < 8; i++) {
        UINT32 clusters = (total - reserved - nfats * fs) / spc;
        fs = ((clusters + 2) * 4 + BLOCK_SECTOR - 1) / BLOCK_SECTOR;
    }
    UINT32 data_start = reserved + nfats * fs;
    UINT32 clusters = (total - data_start) / spc;

    UINT8 *buf = kzalloc(64 * BLOCK_SECTOR);
    if (!buf) return NULL;
    bool ok = true;
    /* zero the reserved area, both FATs and the root directory's cluster */
    for (UINT32 s = 0; ok && s < data_start + spc; s += 64) {
        UINT32 n = data_start + spc - s < 64 ? data_start + spc - s : 64;
        ok = dev->write(dev, lba + s, n, buf);
    }
    UINT8 *bs = buf;
    RtcTime t;
    rtc_read(&t);
    UINT32 serial = (UINT32)t.second << 24 | (UINT32)t.minute << 16 | (UINT32)t.hour << 8 | t.day;
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "NOVAOS  ", 8);
    *(UINT16 *)(bs + 11) = BLOCK_SECTOR;
    bs[13] = (UINT8)spc;
    *(UINT16 *)(bs + 14) = (UINT16)reserved;
    bs[16] = (UINT8)nfats;
    bs[21] = 0xF8;
    *(UINT16 *)(bs + 24) = 63;
    *(UINT16 *)(bs + 26) = 255;
    *(UINT32 *)(bs + 28) = (UINT32)lba;
    *(UINT32 *)(bs + 32) = total;
    *(UINT32 *)(bs + 36) = fs;
    *(UINT32 *)(bs + 44) = 2;                             /* root cluster */
    *(UINT16 *)(bs + 48) = 1;                             /* FSInfo */
    *(UINT16 *)(bs + 50) = 6;                             /* backup boot sector */
    bs[64] = 0x80;
    bs[66] = 0x29;
    *(UINT32 *)(bs + 67) = serial;
    memset(bs + 71, ' ', 11);
    for (int i = 0; i < 11 && label[i]; i++) bs[71 + i] = (UINT8)up(label[i]);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    UINT8 *fsi = buf + BLOCK_SECTOR;
    *(UINT32 *)fsi = 0x41615252u;
    *(UINT32 *)(fsi + 484) = 0x61417272u;
    *(UINT32 *)(fsi + 488) = clusters - 1;
    *(UINT32 *)(fsi + 492) = 3;
    *(UINT32 *)(fsi + 508) = 0xAA550000u;
    ok = ok && dev->write(dev, lba, 2, buf) && dev->write(dev, lba + 6, 2, buf);
    /* FAT[0], FAT[1] and the root directory's end of chain, in both FATs */
    memset(buf, 0, 2 * BLOCK_SECTOR);
    *(UINT32 *)buf = 0x0FFFFFF8u;
    *(UINT32 *)(buf + 4) = 0x0FFFFFFFu;
    *(UINT32 *)(buf + 8) = 0x0FFFFFFFu;
    for (UINT32 k = 0; ok && k < nfats; k++) ok = dev->write(dev, lba + reserved + k * fs, 1, buf);
    /* the volume label entry */
    memset(buf, 0, BLOCK_SECTOR);
    memset(buf, ' ', 11);
    for (int i = 0; i < 11 && label[i]; i++) buf[i] = (UINT8)up(label[i]);
    buf[11] = 0x08;
    ok = ok && dev->write(dev, lba + data_start, 1, buf);
    if (dev->flush) ok = dev->flush(dev) && ok;
    kfree(buf);
    if (!ok) return NULL;
    kprintf("[FAT] formatted %s at LBA %llu: FAT32 \"%s\", %u clusters of %u bytes\n", dev->name,
            (unsigned long long)lba, label, clusters, spc * BLOCK_SECTOR);
    return FatMount(dev, lba);
}
