/*
 * fsec.c — file security on drive C: (see fsec.h)
 */

#include "fsec.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

/* (kernel/um/um_security.c: the check against the effective token) */
UINT32 um_access_check_sd(const UINT8 *sd, UINT32 len, UINT32 want, const UINT32 map[4], UINT32 *granted);

static UINT16 rd16(const UINT8 *p) { UINT16 v; memcpy(&v, p, 2); return v; }
static UINT32 rd32(const UINT8 *p) { UINT32 v; memcpy(&v, p, 4); return v; }
static void   wr16(UINT8 *p, UINT16 v) { memcpy(p, &v, 2); }
static void   wr32(UINT8 *p, UINT32 v) { memcpy(p, &v, 4); }

/* The user's SIDs (as ntdll's token) */
static const UINT8 g_user[] = {                     /* S-1-5-21-1000-2000-3000-1001 */
    1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0xE8, 3, 0, 0, 0xD0, 7, 0, 0, 0xB8, 0x0B, 0, 0, 0xE9, 3, 0, 0 };
static const UINT8 g_users[]       = { 1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x21, 2, 0, 0 };     /* S-1-5-32-545 */
static const UINT8 g_creator_owner[] = { 1, 1, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0 };                     /* S-1-3-0 */
static const UINT8 g_creator_group[] = { 1, 1, 0, 0, 0, 0, 0, 3, 1, 0, 0, 0 };                     /* S-1-3-1 */

/* The drive's root, when it has no descriptor of its own (C: on FAT or
 * in memory): the DACL Windows gives C:\, as a new NTFS volume's root gets
 * it (kernel/fs/ntfs.c): SYSTEM and Administrators full control, CREATOR
 * OWNER full control of what is made below (the user, here), Authenticated
 * Users change, Users read and execute, all inherited.  (Without it
 * nothing had a DACL, so a single ACE an installer added to a folder
 * became its whole DACL and locked the user out.) */
#define ROOT_ACE(flags, size, mask) 0, (flags), (size), 0, (mask) & 0xFF, ((mask) >> 8) & 0xFF, ((mask) >> 16) & 0xFF, (mask) >> 24
static const UINT8 g_root_dacl[] = {
    2, 0, 116, 0, 5, 0, 0, 0,
    ROOT_ACE(0x03, 20, 0x001F01FFu), 1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0,                   /* SYSTEM */
    ROOT_ACE(0x03, 24, 0x001F01FFu), 1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 2, 0, 0,  /* Administrators */
    ROOT_ACE(0x0B, 20, 0x10000000u), 1, 1, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0,                    /* CREATOR OWNER (inherit only) */
    ROOT_ACE(0x03, 20, 0x001301BFu), 1, 1, 0, 0, 0, 0, 0, 5, 11, 0, 0, 0,                   /* Authenticated Users */
    ROOT_ACE(0x03, 24, 0x001200A9u), 1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x21, 2, 0, 0,  /* Users */
};
_Static_assert(sizeof(g_root_dacl) == 116, "the root's DACL size");

#define GENERIC_READ    0x80000000u
#define GENERIC_WRITE   0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL     0x10000000u
#define FILE_ALL        0x001F01FFu

#define ACE_OI   0x01                       /* OBJECT_INHERIT_ACE */
#define ACE_CI   0x02                       /* CONTAINER_INHERIT_ACE */
#define ACE_NP   0x04                       /* NO_PROPAGATE_INHERIT_ACE */
#define ACE_IO   0x08                       /* INHERIT_ONLY_ACE */
#define ACE_INH  0x10                       /* INHERITED_ACE */

#define SE_DACL_PRESENT       0x0004u
#define SE_DACL_AUTO_INHERITED 0x0400u
#define SE_DACL_PROTECTED     0x1000u
#define DACL_CONTROL          (SE_DACL_AUTO_INHERITED | SE_DACL_PROTECTED)    /* kept with the DACL */
#define SE_SELF_RELATIVE      0x8000u

static UINT32 sid_len(const UINT8 *s) { return 8u + 4u * s[1]; }

static bool sid_eq(const UINT8 *a, const UINT8 *b)
{
    return a[1] == b[1] && !memcmp(a, b, sid_len(a));
}

/* ---------------------------------------------------------------------------
 * Descriptors
 * ------------------------------------------------------------------------- */
static bool valid_sid(const UINT8 *s, UINT32 room)
{
    return room >= 8 && s[0] == 1 && s[1] <= 15 && sid_len(s) <= room;
}

static bool valid_acl(const UINT8 *a, UINT32 room)
{
    if (room < 8 || (a[0] != 2 && a[0] != 4)) return false;
    UINT32 size = rd16(a + 2), count = rd16(a + 4), o = 8;
    if (size < 8 || size > room) return false;
    for (UINT32 i = 0; i < count; i++) {
        if (o + 8 > size) return false;
        UINT32 alen = rd16(a + o + 2);
        if (alen < 8 || o + alen > size) return false;
        if (a[o] <= 1 && (alen < 16 || !valid_sid(a + o + 8, alen - 8))) return false;
        o += alen;
    }
    return true;
}

bool FsecValid(const void *sd, UINT32 len, UINT32 *used)
{
    const UINT8 *p = sd;
    if (!p || len < 20 || p[0] != 1 || !(rd16(p + 2) & SE_SELF_RELATIVE)) return false;
    UINT32 end = 20, offs[4] = { rd32(p + 4), rd32(p + 8), rd32(p + 12), rd32(p + 16) };
    for (int i = 0; i < 4; i++) {
        UINT32 o = offs[i];
        if (!o) continue;
        if (o < 20 || o >= len) return false;
        if (i == 2 && !(rd16(p + 2) & 0x10)) continue;       /* (no SACL present: ignore the offset) */
        if (i == 3 && !(rd16(p + 2) & SE_DACL_PRESENT)) continue;
        bool ok = i < 2 ? valid_sid(p + o, len - o) : valid_acl(p + o, len - o);
        if (!ok) return false;
        UINT32 e = o + (i < 2 ? sid_len(p + o) : rd16(p + o + 2));
        if (e > end) end = e;
    }
    if (used) *used = end;
    return true;
}

/* A descriptor taken apart */
typedef struct {
    const UINT8 *owner, *group, *dacl;      /* (dacl NULL with dacl_present: everyone may do anything) */
    bool         dacl_present;
    UINT16       control;
} View;

static void parse(const UINT8 *sd, View *v)
{
    memset(v, 0, sizeof(*v));
    v->control = rd16(sd + 2);
    if (rd32(sd + 4)) v->owner = sd + rd32(sd + 4);
    if (rd32(sd + 8)) v->group = sd + rd32(sd + 8);
    v->dacl_present = v->control & SE_DACL_PRESENT;
    if (v->dacl_present && rd32(sd + 16)) v->dacl = sd + rd32(sd + 16);
}

/* A self-relative descriptor of @owner, @group and @dacl into @out (if it
 * fits @cap); its length */
static UINT32 build(UINT8 *out, UINT32 cap, const UINT8 *owner, const UINT8 *group, bool dacl_present,
                    const UINT8 *dacl, UINT16 extra)
{
    UINT32 len = 20 + (owner ? sid_len(owner) : 0) + (group ? sid_len(group) : 0) + (dacl ? rd16(dacl + 2) : 0);
    if (!out || len > cap) return len;
    memset(out, 0, 20);
    out[0] = 1;
    wr16(out + 2, (UINT16)(SE_SELF_RELATIVE | (dacl_present ? SE_DACL_PRESENT : 0) | extra));
    UINT32 o = 20;
    if (owner) { wr32(out + 4, o); memcpy(out + o, owner, sid_len(owner)); o += sid_len(owner); }
    if (group) { wr32(out + 8, o); memcpy(out + o, group, sid_len(group)); o += sid_len(group); }
    if (dacl)  { wr32(out + 16, o); memcpy(out + o, dacl, rd16(dacl + 2)); }
    return len;
}

/* The DACL @n inherits from @from_dacl (@depth levels above it) into @out
 * (cap @cap); false if it does not fit.  NULL *dacl: @from_dacl is NULL. */
static bool inherit(const RamNode *n, const UINT8 *from_dacl, int depth, UINT8 *out, UINT32 cap, const UINT8 **dacl)
{
    *dacl = NULL;
    if (!from_dacl) return true;
    UINT32 o = 8, count = 0, src = 8, size = rd16(from_dacl + 2), n_aces = rd16(from_dacl + 4);
    for (UINT32 i = 0; i < n_aces; i++) {
        const UINT8 *a = from_dacl + src;
        UINT32 alen = rd16(a + 2);
        src += alen;
        if (src > size) break;
        UINT8 fl = a[1];
        if (a[0] > 1) continue;                                  /* only allow and deny ACEs */
        if ((fl & ACE_NP) && depth > 1) continue;
        if (!(fl & (ACE_CI | ACE_OI))) continue;
        if (!n->dir && !(fl & ACE_OI)) continue;
        /* a directory keeps an ACE meant for files (OBJECT_INHERIT only)
         * as inherit-only, for the files in it */
        bool for_files = n->dir && !(fl & ACE_CI);
        if (for_files && (fl & ACE_NP)) continue;
        const UINT8 *sid = a + 8;
        if (sid_eq(sid, g_creator_owner)) sid = g_user;
        else if (sid_eq(sid, g_creator_group)) sid = g_users;
        UINT32 nlen = 8 + sid_len(sid);
        if (o + nlen > cap) return false;
        out[o] = a[0];
        out[o + 1] = (UINT8)(ACE_INH | (n->dir && !(fl & ACE_NP) ? fl & (ACE_OI | ACE_CI) : 0) | (for_files ? ACE_IO : 0));
        wr16(out + o + 2, (UINT16)nlen);
        wr32(out + o + 4, rd32(a + 4));
        memcpy(out + o + 8, sid, sid_len(sid));
        o += nlen;
        count++;
    }
    out[0] = 2; out[1] = 0;
    wr16(out + 2, (UINT16)o);
    wr16(out + 4, (UINT16)count);
    wr16(out + 6, 0);
    *dacl = out;
    return true;
}

#define DACL_MAX 4096

/* @n's descriptor as it applies: its own, or the inherited one (whose DACL
 * goes in @buf).  False if out of room. */
static bool view_of(RamNode *n, View *v, UINT8 *buf)
{
    if (n->sd) { parse(n->sd, v); return true; }
    memset(v, 0, sizeof(*v));
    v->owner = g_user;
    v->group = g_users;
    v->dacl_present = true;
    if (!n->parent) { v->dacl = g_root_dacl; return true; }     /* the root's own */
    v->control = SE_DACL_AUTO_INHERITED;
    int depth = 1;
    RamNode *a = n->parent;
    for (; a->parent && !a->sd; a = a->parent) depth++;
    if (!a->sd) return inherit(n, g_root_dacl, depth, buf, DACL_MAX, &v->dacl);
    View f;
    parse(a->sd, &f);
    return inherit(n, f.dacl_present ? f.dacl : NULL, depth, buf, DACL_MAX, &v->dacl);
}

/* ---------------------------------------------------------------------------
 * Checking and changing
 * ------------------------------------------------------------------------- */
bool FsecAccess(RamNode *n, UINT32 want, UINT32 *granted)
{
    static const UINT32 map[4] = { 0x00120089u, 0x00120116u, 0x001200A0u, FILE_ALL };
    UINT32 g = 0, len = FsecQuery(n, 7, NULL, 0);
    UINT8 *sd = len ? kmalloc(len) : NULL;
    bool ok = sd && FsecQuery(n, 7, sd, len) == len &&
              um_access_check_sd(sd, len, want, map, &g) == 0;  /* as the calling thread's token */
    kfree(sd);
    if (granted) *granted = ok ? g : 0;
    return ok;
}

UINT32 FsecQuery(RamNode *n, UINT32 info, void *out, UINT32 cap)
{
    UINT8 *buf = kmalloc(DACL_MAX);
    View v;
    if (!buf || !view_of(n, &v, buf)) { kfree(buf); return 0; }
    UINT32 len = build(out, cap, (info & 1) ? v.owner : NULL, (info & 2) ? v.group : NULL,
                       (info & 4) && v.dacl_present, (info & 4) ? v.dacl : NULL,
                       (UINT16)((info & 4) ? v.control & DACL_CONTROL : 0));
    kfree(buf);
    return len;
}

bool FsecSet(RamNode *n, UINT32 info, const void *sd, UINT32 len)
{
    UINT32 used;
    if (!FsecValid(sd, len, &used)) return false;
    View now, in;
    parse(sd, &in);
    UINT8 *buf = kmalloc(DACL_MAX);
    if (!buf || !view_of(n, &now, buf)) { kfree(buf); return false; }
    const UINT8 *owner = (info & 1) && in.owner ? in.owner : now.owner;
    const UINT8 *group = (info & 2) && in.group ? in.group : now.group;
    bool present = (info & 4) ? in.dacl_present : now.dacl_present;
    const UINT8 *dacl = (info & 4) ? in.dacl : now.dacl;
    UINT16 control = (UINT16)(((info & 4) ? in.control : now.control) & DACL_CONTROL);
    UINT32 need = build(NULL, 0, owner, group, present, dacl, control);
    UINT8 *nsd = kmalloc(need);
    if (!nsd) { kfree(buf); return false; }
    build(nsd, need, owner, group, present, dacl, control);
    kfree(buf);
    kfree(n->sd);
    n->sd = nsd;
    n->sdlen = need;
    RamfsMarkChanged(n);
    return true;
}
