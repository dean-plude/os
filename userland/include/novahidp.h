/*
 * novahidp.h — HID preparsed data: a report descriptor turned into the
 * tables hid.dll's HidP_* calls read
 *
 * Windows hands programs a device's "preparsed data" (HidD_GetPreparsedData,
 * Raw Input's RIDI_PREPARSEDDATA) and they pass it back to HidP_GetCaps,
 * HidP_GetButtonCaps, HidP_GetUsages, HidP_GetUsageValue... without
 * looking inside.  NovaOS's is built here from the report descriptor
 * (HID 1.11 section 6.2.2), by user32 and hid.dll alike, so it is one
 * self-contained block a program may copy: a header ("HidP KDR", as
 * Windows' starts), the capabilities and the link collections.
 *
 * Each main item (Input, Output, Feature) of the first top-level
 * collection becomes capabilities the way Windows' parser makes them:
 * a variable item with one-bit fields is buttons, other variable items
 * are values, an array item is buttons whose fields hold indices; a usage
 * range gives one capability (IsRange) and a list of usages one per
 * usage, the last one taking the fields left over (a value array when
 * that is more than one).  Every usage of a report type gets a data index,
 * in descriptor order.  Link collection 0 is the top-level collection;
 * its children are linked newest first, as Windows links them.
 */
#pragma once
#include <windows.h>
#include <string.h>
#include <novapad.h>

#define NOVA_HIDP_MAX_CAPS  256
#define NOVA_HIDP_MAX_LINKS 64

typedef struct {
    BYTE  rtype;                      /* 0 Input, 1 Output, 2 Feature */
    BYTE  id;                         /* report ID (0: none) */
    BYTE  button, range, array, absolute, has_null, constant;
    WORD  page, link;
    WORD  umin, umax;                 /* usages (umin == umax when not a range) */
    WORD  dmin, dmax;                 /* data indices */
    WORD  bit;                        /* first field's bit, after the ID byte */
    WORD  size, count;                /* bits per field, fields */
    WORD  bitfield;                   /* the main item's data */
    WORD  aofs;                       /* array: index of umin in the item's usage list */
    LONG  lmin, lmax, pmin, pmax;
    DWORD units, unit_exp;
} NovaHidCap;

typedef struct {
    WORD  usage, page, parent, nchildren, next, first;
    BYTE  type;
    BYTE  reserved[3];
} NovaHidLink;

typedef struct {
    char  magic[8];                   /* "HidP KDR" */
    DWORD size;                       /* of the whole block */
    WORD  usage, page;                /* the top-level collection */
    WORD  rlen[3];                    /* report byte lengths, with the ID byte (0: none of that type) */
    WORD  ndata[3];                   /* data indices per report type */
    WORD  ncaps, nlinks;
    NovaHidCap caps[1];               /* [ncaps], then NovaHidLink[nlinks] */
} NovaHidP;

static inline NovaHidLink *nova_hidp_links(const NovaHidP *p) { return (NovaHidLink *)(void *)&p->caps[p->ncaps]; }

static inline DWORD nova_hidp_u(const BYTE *v, int n)
{
    DWORD r = 0;
    for (int i = 0; i < n; i++) r |= (DWORD)v[i] << (8 * i);
    return r;
}

static inline LONG nova_hidp_s(const BYTE *v, int n)
{
    DWORD r = nova_hidp_u(v, n);
    if (n == 1) return (signed char)r;
    if (n == 2) return (short)r;
    return (LONG)r;
}

/* Build the preparsed data of descriptor @d (@len bytes) into @out (@cap
 * bytes, may be 0): the size it takes, 0 if @d holds no top-level
 * collection */
static UINT nova_hidp_build(const BYTE *d, int len, void *out, UINT cap)
{
    typedef struct { WORD page; LONG lmin, lmax, pmin, pmax; DWORD units, uexp; WORD size, count; BYTE id; } Glob;
    static NovaHidCap caps[NOVA_HIDP_MAX_CAPS];
    static NovaHidLink links[NOVA_HIDP_MAX_LINKS];
    static WORD bits[3][256];
    static SRWLOCK lock = SRWLOCK_INIT;            /* (the tables above are shared) */
    AcquireSRWLockExclusive(&lock);
    memset(bits, 0, sizeof(bits));
    Glob g, stack[8];
    memset(&g, 0, sizeof(g));
    int sp = 0, depth = 0, ncaps = 0, nlinks = 0, cur = -1;
    DWORD usages[64], umin = 0, umax = 0;
    int nusage = 0;
    BOOL have_min = FALSE, have_max = FALSE, done = FALSE;
    WORD ndata[3] = { 0, 0, 0 };
    for (int i = 0; i < len && !done;) {
        BYTE b = d[i];
        if (b == 0xFE) { if (i + 1 >= len) break; i += 3 + d[i + 1]; continue; }   /* long item */
        int n = (b & 3) == 3 ? 4 : (b & 3);
        if (i + 1 + n > len) break;
        const BYTE *v = &d[i + 1];
        i += 1 + n;
        BYTE tag = b & 0xFC;
        BOOL clear_locals = FALSE;
        switch (tag) {
        case 0x80: case 0x90: case 0xB0: {                         /* Input, Output, Feature */
            int t = tag == 0x80 ? 0 : tag == 0x90 ? 1 : 2;
            DWORD flags = nova_hidp_u(v, n);
            WORD *pos = &bits[t][g.id];
            clear_locals = TRUE;
            if (cur < 0 || !g.size || !g.count) { *pos = (WORD)(*pos + g.size * g.count); break; }
            BOOL range = have_min && have_max && !nusage;
            if (!range && !nusage) { *pos = (WORD)(*pos + g.size * g.count); break; }   /* padding */
            BOOL array = !(flags & 2);
            BOOL button = array || g.size == 1;
            /* The usages, as caps: a range is one, a list one per usage */
            int parts = range ? 1 : nusage;
            int field = 0;
            for (int k = 0; k < parts && field < g.count && ncaps < NOVA_HIDP_MAX_CAPS; k++) {
                NovaHidCap *c = &caps[ncaps++];
                memset(c, 0, sizeof(*c));
                DWORD u0 = range ? umin : usages[k], u1 = range ? umax : usages[k];
                c->rtype = (BYTE)t;
                c->id = g.id;
                c->button = (BYTE)button;
                c->array = (BYTE)array;
                c->range = (BYTE)range;
                c->absolute = !(flags & 4);
                c->has_null = (flags & 0x40) != 0;
                c->constant = flags & 1;
                c->bitfield = (WORD)flags;
                c->page = (WORD)((u0 >> 16) ? (u0 >> 16) : g.page);
                c->link = (WORD)cur;
                c->umin = (WORD)u0;
                c->umax = (WORD)u1;
                if (c->umax < c->umin) c->umax = c->umin;
                c->size = g.size;
                c->lmin = g.lmin; c->lmax = g.lmax;
                c->pmin = g.pmin; c->pmax = g.pmax;
                c->units = g.units; c->unit_exp = g.uexp;
                if (array) {                                       /* every field holds an index into the usages */
                    c->bit = *pos;
                    c->count = g.count;
                    c->aofs = (WORD)(range ? 0 : k);
                } else {
                    int fields = range ? (int)(u1 - u0 + 1) : (k == parts - 1 ? g.count - field : 1);
                    if (range && k == parts - 1 && field + fields < g.count) fields = g.count - field;
                    if (field + fields > g.count) fields = g.count - field;
                    c->bit = (WORD)(*pos + field * g.size);
                    c->count = (WORD)fields;
                    field += fields;
                    if (range && c->umax - c->umin + 1 > fields) c->umax = (WORD)(c->umin + fields - 1);
                }
                WORD span = (WORD)(c->umax - c->umin);
                c->dmin = ndata[t];
                c->dmax = (WORD)(ndata[t] + (c->range ? span : 0));
                ndata[t] = (WORD)(c->dmax + 1);
            }
            *pos = (WORD)(*pos + g.size * g.count);
            break;
        }
        case 0xA0: {                                               /* Collection */
            if (depth == 0 && nlinks) { done = TRUE; break; }     /* (a second top-level collection) */
            if (nlinks < NOVA_HIDP_MAX_LINKS) {
                NovaHidLink *l = &links[nlinks];
                memset(l, 0, sizeof(*l));
                DWORD u = nusage ? usages[0] : have_min ? umin : 0;
                l->usage = (WORD)u;
                l->page = (WORD)((u >> 16) ? (u >> 16) : g.page);
                l->type = (BYTE)nova_hidp_u(v, n);
                if (cur >= 0) {
                    l->parent = (WORD)cur;
                    l->next = links[cur].nchildren ? links[cur].first : 0;
                    links[cur].first = (WORD)nlinks;
                    links[cur].nchildren++;
                }
                cur = nlinks++;
            }
            depth++;
            clear_locals = TRUE;
            break;
        }
        case 0xC0:                                                 /* End Collection */
            if (depth) depth--;
            if (cur >= 0) cur = depth ? links[cur].parent : cur;
            if (!depth && nlinks) done = TRUE;
            clear_locals = TRUE;
            break;
        case 0x04: g.page = (WORD)nova_hidp_u(v, n); break;
        case 0x14: g.lmin = nova_hidp_s(v, n); break;
        case 0x24: g.lmax = nova_hidp_s(v, n); if (g.lmin >= 0 && g.lmax < 0) g.lmax = (LONG)nova_hidp_u(v, n); break;
        case 0x34: g.pmin = nova_hidp_s(v, n); break;
        case 0x44: g.pmax = nova_hidp_s(v, n); if (g.pmin >= 0 && g.pmax < 0) g.pmax = (LONG)nova_hidp_u(v, n); break;
        case 0x54: g.uexp = nova_hidp_u(v, n); break;
        case 0x64: g.units = nova_hidp_u(v, n); break;
        case 0x74: g.size = (WORD)nova_hidp_u(v, n); break;
        case 0x84: g.id = (BYTE)nova_hidp_u(v, n); break;
        case 0x94: g.count = (WORD)nova_hidp_u(v, n); break;
        case 0xA4: if (sp < 8) stack[sp++] = g; break;
        case 0xB4: if (sp) g = stack[--sp]; break;
        case 0x08: if (nusage < 64) usages[nusage++] = n == 4 ? nova_hidp_u(v, n) : nova_hidp_u(v, n) | (DWORD)g.page << 16; break;
        case 0x18: umin = n == 4 ? nova_hidp_u(v, n) : nova_hidp_u(v, n) | (DWORD)g.page << 16; have_min = TRUE; break;
        case 0x28: umax = n == 4 ? nova_hidp_u(v, n) : nova_hidp_u(v, n) | (DWORD)g.page << 16; have_max = TRUE; break;
        default: break;
        }
        if (clear_locals) { nusage = 0; have_min = have_max = FALSE; }
    }
    UINT need = 0;
    if (nlinks) {
        need = (UINT)(__builtin_offsetof(NovaHidP, caps) + ncaps * sizeof(NovaHidCap) + nlinks * sizeof(NovaHidLink));
        if (out && cap >= need) {
            NovaHidP *p = out;
            memset(p, 0, need);
            memcpy(p->magic, "HidP KDR", 8);
            p->size = need;
            p->usage = links[0].usage;
            p->page = links[0].page;
            for (int t = 0; t < 3; t++) {
                int most = -1;
                for (int id = 0; id < 256; id++) if (bits[t][id] && bits[t][id] > most) most = bits[t][id];
                for (int k = 0; k < ncaps; k++) if (caps[k].rtype == t && most < 0) most = 0;
                p->rlen[t] = most < 0 ? 0 : (WORD)(1 + (most + 7) / 8);
                p->ndata[t] = ndata[t];
            }
            p->ncaps = (WORD)ncaps;
            p->nlinks = (WORD)nlinks;
            memcpy(p->caps, caps, ncaps * sizeof(NovaHidCap));
            memcpy(nova_hidp_links(p), links, nlinks * sizeof(NovaHidLink));
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return need;
}

/* @p if it is preparsed data, else NULL */
static inline const NovaHidP *nova_hidp_check(const void *p)
{
    const NovaHidP *h = p;
    if (!h || memcmp(h->magic, "HidP KDR", 8) || h->size < __builtin_offsetof(NovaHidP, caps)) return NULL;
    return h;
}

/* A controller's preparsed data into @buf (@cap bytes; may be 0): the
 * bytes it takes, 0 if the slot is empty */
static inline UINT nova_pad_preparsed(int slot, void *buf, UINT cap)
{
    BYTE d[NOVA_PAD_DESC_MAX];
    int n = nova_pad_descriptor(slot, d, NULL);
    return n > 0 ? nova_hidp_build(d, n, buf, cap) : 0;
}
