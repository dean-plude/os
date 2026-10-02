/*
 * um_security.c — tokens, security descriptors and access checks
 *
 * A token (UO_TOKEN) is who a thread acts as: a user SID, groups (each
 * enabled, disabled or only used for denying), restricting SIDs and
 * privileges.  Every process has a primary token, its creator's (or the
 * desktop user's for programs the desktop starts); a thread may
 * impersonate another (NtSetInformationThread(ThreadImpersonationToken)).
 * NtFilterToken makes restricted tokens: groups turned deny-only,
 * privileges dropped, restricting SIDs added.
 *
 * Objects (events, mutants, semaphores, sections, timers, directories,
 * symbolic links) may carry a security descriptor: the one their creator
 * gave (OBJECT_ATTRIBUTES.SecurityDescriptor), or one set later with
 * NtSetSecurityObject.  Opening an existing object checks the access
 * asked for against its DACL with the thread's token (the impersonation
 * token if it has one), the same check NtAccessCheck does: an object
 * without a descriptor, or with a NULL DACL, is open to everyone.
 * Descriptors are kept self-relative in kernel memory, with the owner,
 * group and DACL (no SACL).  Files' descriptors are the file system's
 * (kernel/fs/fsec.c, through um_file_query/set_security in um_syscall.c);
 * it checks opens with um_access_check_sd.
 */
#include "um_internal.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../ke/spinlock.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

#define ST_SUCCESS                0x00000000u
#define ST_INVALID_INFO_CLASS     0xC0000003u
#define ST_INFO_LENGTH_MISMATCH   0xC0000004u
#define ST_ACCESS_VIOLATION       0xC0000005u
#define ST_INVALID_HANDLE         0xC0000008u
#define ST_INVALID_PARAMETER      0xC000000Du
#define ST_NO_MEMORY              0xC0000017u
#define ST_ACCESS_DENIED          0xC0000022u
#define ST_BUFFER_TOO_SMALL       0xC0000023u
#define ST_OBJECT_TYPE_MISMATCH   0xC0000024u
#define ST_NO_IMPERSONATION_TOKEN 0xC000005Cu
#define ST_INVALID_SID            0xC0000078u
#define ST_INVALID_SECURITY_DESCR 0xC0000079u
#define ST_NO_TOKEN               0xC000007Cu
#define ST_BAD_TOKEN_TYPE         0xC00000A8u
#define ST_TOO_MANY_HANDLES       0xC000011Fu

#define SID_MAX         68              /* revision, count, authority, 15 sub-authorities */
#define TOKEN_GROUPS_MAX 16
#define TOKEN_RESTRICT_MAX 8
#define SD_MAX          (64 * 1024)

/* SE_GROUP_* */
#define GRP_MANDATORY   0x01u
#define GRP_DEFAULT     0x02u
#define GRP_ENABLED     0x04u
#define GRP_DENY_ONLY   0x10u
#define GRP_ON          (GRP_MANDATORY | GRP_DEFAULT | GRP_ENABLED)

/* NtFilterToken flags */
#define DISABLE_MAX_PRIVILEGE 0x1u
#define SANDBOX_INERT         0x2u
#define WRITE_RESTRICTED      0x8u

#define SE_CHANGE_NOTIFY 23             /* the privilege every token keeps */

typedef struct { UINT8 sid[SID_MAX]; UINT32 attrs; } SidAttr;

typedef struct UmToken {
    SidAttr user;
    SidAttr groups[TOKEN_GROUPS_MAX];
    int     ngroups;
    SidAttr restricted[TOKEN_RESTRICT_MAX];
    int     nrestricted;
    UINT64  privs, privs_on;            /* bit n: privilege LUID n held / enabled */
    UINT32  type;                       /* 1 TokenPrimary, 2 TokenImpersonation */
    UINT32  level;                      /* SECURITY_IMPERSONATION_LEVEL (impersonation tokens) */
    UINT32  id, modified;               /* LUIDs */
    bool    write_restricted, sandbox_inert;
} UmToken;

static const UINT8 g_user_sid[] = {                     /* S-1-5-21-1000-2000-3000-1001 */
    1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0xE8, 3, 0, 0, 0xD0, 7, 0, 0, 0xB8, 0x0B, 0, 0, 0xE9, 3, 0, 0 };
static const UINT8 g_users_sid[] = { 1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x21, 2, 0, 0 };     /* S-1-5-32-545 */
static const UINT8 g_admins_sid[] = { 1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 2, 0, 0 };    /* S-1-5-32-544 */
static const UINT8 g_everyone_sid[] = { 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 };                    /* S-1-1-0 */
static const UINT8 g_auth_users_sid[] = { 1, 1, 0, 0, 0, 0, 0, 5, 11, 0, 0, 0 };                 /* S-1-5-11 */
static const UINT8 g_interactive_sid[] = { 1, 1, 0, 0, 0, 0, 0, 5, 4, 0, 0, 0 };                 /* S-1-5-4 */
static const UINT8 g_logon_sid[] = { 1, 3, 0, 0, 0, 0, 0, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0x2A, 0, 0, 0 };  /* S-1-5-5-0-42 */
static const UINT8 g_medium_il_sid[] = { 1, 1, 0, 0, 0, 0, 0, 16, 0, 0x20, 0, 0 };               /* S-1-16-8192 */
static const UINT8 g_system_sid[] = { 1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0 };                     /* S-1-5-18 */

/* Descriptors change under this lock (they are read under it too) */
static KSpinLock g_sd_lock = KSPINLOCK_INIT;

static UINT32 next_luid(void)
{
    static volatile UINT32 next = 0x20000;
    return __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
}

/* -----------------------------------------------------------------------
 * SIDs, ACLs, descriptors (self-relative, in kernel memory)
 * ----------------------------------------------------------------------- */
static UINT32 sid_len(const UINT8 *s) { return 8u + 4u * s[1]; }

static bool sid_ok(const UINT8 *s, UINT32 avail)
{
    return avail >= 8 && s[0] == 1 && s[1] <= 15 && sid_len(s) <= avail;
}

static bool sid_eq(const UINT8 *a, const UINT8 *b)
{
    return a[1] == b[1] && !memcmp(a, b, sid_len(a));
}

static UINT32 rd32(const UINT8 *p) { UINT32 v; memcpy(&v, p, 4); return v; }
static UINT16 rd16(const UINT8 *p) { return (UINT16)(p[0] | p[1] << 8); }

/* The parts of a self-relative descriptor (NULL: absent); false if malformed */
typedef struct { const UINT8 *owner, *group, *dacl; bool dacl_present; UINT16 control; } SdView;

static bool sd_view(const UINT8 *sd, UINT32 len, SdView *v)
{
    memset(v, 0, sizeof(*v));
    if (!sd || len < 20 || sd[0] != 1) return false;
    v->control = rd16(sd + 2);
    UINT32 off[3] = { rd32(sd + 4), rd32(sd + 8), rd32(sd + 16) };
    for (int i = 0; i < 2; i++) {
        if (!off[i]) continue;
        if (off[i] >= len || !sid_ok(sd + off[i], len - off[i])) return false;
        (i ? &v->group : &v->owner)[0] = sd + off[i];
    }
    v->dacl_present = (v->control & 0x0004) != 0;
    if (v->dacl_present && off[2]) {
        if (off[2] > len - 8 || rd16(sd + off[2] + 2) < 8 || rd16(sd + off[2] + 2) > len - off[2]) return false;
        v->dacl = sd + off[2];
    }
    return true;
}

/* A self-relative descriptor of these parts (kmalloc'd, with its length
 * in *len), or NULL if out of memory */
static UINT8 *sd_build(const UINT8 *owner, const UINT8 *group, bool dacl_present, const UINT8 *dacl,
                       UINT16 control, UINT32 *len)
{
    UINT32 ol = owner ? sid_len(owner) : 0, gl = group ? sid_len(group) : 0, dl = dacl ? rd16(dacl + 2) : 0;
    UINT32 n = 20 + ol + gl + dl;
    UINT8 *sd = kzalloc(n);
    if (!sd) return NULL;
    sd[0] = 1;
    control = (UINT16)((control & 0x1C00) | 0x8000 | (dacl_present ? 0x0004 : 0));   /* keep the inheritance bits */
    sd[2] = (UINT8)control; sd[3] = (UINT8)(control >> 8);
    UINT32 at = 20;
    if (ol) { memcpy(sd + at, owner, ol); memcpy(sd + 4, &at, 4); at += ol; }
    if (gl) { memcpy(sd + at, group, gl); memcpy(sd + 8, &at, 4); at += gl; }
    if (dl) { memcpy(sd + at, dacl, dl); memcpy(sd + 16, &at, 4); at += dl; }
    *len = n;
    return sd;
}

/* Stored descriptors: the length, then the descriptor */
typedef struct { UINT32 len; UINT8 b[]; } UmSd;

static UmSd *sd_store(const UINT8 *sd, UINT32 len)
{
    UmSd *s = kmalloc(sizeof(UmSd) + len);
    if (!s) return NULL;
    s->len = len;
    memcpy(s->b, sd, len);
    return s;
}

/* Read user memory into a fresh kernel buffer */
static void *user_copy(UINT64 p, UINT32 n)
{
    void *k = kmalloc(n ? n : 1);
    if (k && n && !NT_SUCCESS(CopyFromUser(k, (const void *)(uintptr_t)p, n))) { kfree(k); return NULL; }
    return k;
}

static bool user_sid(UINT64 p, UINT8 *out)
{
    if (!NT_SUCCESS(CopyFromUser(out, (const void *)(uintptr_t)p, 8))) return false;
    if (out[0] != 1 || out[1] > 15) return false;
    return !out[1] || NT_SUCCESS(CopyFromUser(out + 8, (const void *)(uintptr_t)(p + 8), 4u * out[1]));
}

static UINT8 *user_acl(UINT64 p)
{
    UINT8 h[8];
    if (!NT_SUCCESS(CopyFromUser(h, (const void *)(uintptr_t)p, 8))) return NULL;
    UINT16 size = rd16(h + 2);
    if (size < 8) return NULL;
    return user_copy(p, size);
}

/* A program's descriptor (self-relative or absolute, in the caller's
 * pointer size) as a stored kernel copy; *st says why not */
static UmSd *sd_capture(UINT64 p, UINT32 *st)
{
    UINT8 h[40];
    *st = ST_INVALID_SECURITY_DESCR;
    if (!NT_SUCCESS(CopyFromUser(h, (const void *)(uintptr_t)p, 4))) { *st = ST_ACCESS_VIOLATION; return NULL; }
    if (h[0] != 1) return NULL;
    UINT16 control = rd16(h + 2);
    UINT8 owner[SID_MAX], group[SID_MAX], *dacl = NULL;
    bool has_o = false, has_g = false, present = (control & 0x0004) != 0;
    if (control & 0x8000) {                                         /* self-relative: offsets */
        if (!NT_SUCCESS(CopyFromUser(h, (const void *)(uintptr_t)p, 20))) { *st = ST_ACCESS_VIOLATION; return NULL; }
        UINT32 oo = rd32(h + 4), go = rd32(h + 8), dof = rd32(h + 16);
        if (oo && !(has_o = user_sid(p + oo, owner))) return NULL;
        if (go && !(has_g = user_sid(p + go, group))) return NULL;
        if (present && dof && !(dacl = user_acl(p + dof))) return NULL;
    } else {                                                        /* absolute: pointers */
        bool wow = UmCurrent() && UmCurrent()->wow;
        if (!NT_SUCCESS(CopyFromUser(h, (const void *)(uintptr_t)p, wow ? 20 : 40))) { *st = ST_ACCESS_VIOLATION; return NULL; }
        UINT64 ptr[4];
        for (int i = 0; i < 4; i++) {
            if (wow) ptr[i] = rd32(h + 4 + 4 * i);
            else memcpy(&ptr[i], h + 8 + 8 * i, 8);
        }
        if (ptr[0] && !(has_o = user_sid(ptr[0], owner))) return NULL;
        if (ptr[1] && !(has_g = user_sid(ptr[1], group))) return NULL;
        if (present && ptr[3] && !(dacl = user_acl(ptr[3]))) return NULL;
    }
    UINT32 len;
    UINT8 *sd = sd_build(has_o ? owner : NULL, has_g ? group : NULL, present, dacl, control, &len);
    kfree(dacl);
    if (!sd) { *st = ST_NO_MEMORY; return NULL; }
    UmSd *s = sd_store(sd, len);
    kfree(sd);
    *st = s ? ST_SUCCESS : ST_NO_MEMORY;
    return s;
}

/* -----------------------------------------------------------------------
 * Tokens
 * ----------------------------------------------------------------------- */
static void token_destroy(UmObject *o) { kfree(o->ptr); }

static UmObject *token_new(const UmToken *from)
{
    UmObject *o = kzalloc(sizeof(*o));
    UmToken *t = kmalloc(sizeof(*t));
    if (!o || !t) { kfree(o); kfree(t); return NULL; }
    *t = *from;
    t->id = next_luid();
    t->modified = t->id;
    o->type = UO_TOKEN;
    o->refs = 1;
    o->ptr = t;
    o->destroy = token_destroy;
    o->free_unlocked = true;
    return o;
}

static void sid_set(SidAttr *s, const UINT8 *sid, UINT32 attrs)
{
    memcpy(s->sid, sid, sid_len(sid));
    s->attrs = attrs;
}

/* The desktop user's token: a standard user, member of Users, with
 * Administrators only for denying (not elevated) */
static UmObject *default_token(void)
{
    static UmObject *g;
    if (g) return g;
    UmToken t;
    memset(&t, 0, sizeof(t));
    sid_set(&t.user, g_user_sid, 0);
    const UINT8 *gs[] = { g_everyone_sid, g_users_sid, g_admins_sid, g_interactive_sid, g_auth_users_sid, g_logon_sid };
    const UINT32 ga[] = { GRP_ON, GRP_ON, GRP_DENY_ONLY, GRP_ON, GRP_ON, GRP_ON | 0xC0000000u /* LOGON_ID */ };
    for (int i = 0; i < 6; i++) sid_set(&t.groups[t.ngroups++], gs[i], ga[i]);
    t.privs = t.privs_on = 1ull << SE_CHANGE_NOTIFY;
    t.privs |= 1ull << 19 | 1ull << 25 | 1ull << 33 | 1ull << 34;  /* Shutdown, Undock, IncreaseWorkingSet, TimeZone (off) */
    t.type = 1;
    UmObject *o = token_new(&t);
    if (o && __atomic_compare_exchange_n(&g, &(UmObject *){ NULL }, o, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return o;
    if (o) um_ob_unref(o);
    return g;
}

UmObject *um_token_for_process(UmProcess *creator)
{
    UmObject *t = creator && creator->token ? creator->token : default_token();
    return t ? um_ob_ref(t) : NULL;
}

/* The token the calling thread acts as (referenced) */
static UmObject *effective_token(void)
{
    UmThread *me = UmCurrentThread();
    UmProcess *p = UmCurrent();
    UmObject *o = NULL;
    IrqState s = ob_lock();
    if (me && me->imp) o = um_ob_ref(me->imp);
    else if (p && p->token) o = um_ob_ref(p->token);
    ob_unlock(s);
    if (!o && (o = default_token())) um_ob_ref(o);
    return o;
}

void um_thread_drop_token(UmThread *t)
{
    IrqState s = ob_lock();
    UmObject *o = t->imp;
    t->imp = NULL;
    ob_unlock(s);
    if (o) um_ob_unref(o);
}

/* -----------------------------------------------------------------------
 * The access check
 * ----------------------------------------------------------------------- */
static const UINT32 g_maps[][4] = {
    [UO_EVENT]     = { 0x20001, 0x20002, 0x120000, 0x1F0003 },
    [UO_MUTANT]    = { 0x20001, 0x20000, 0x120000, 0x1F0001 },
    [UO_SEMAPHORE] = { 0x20001, 0x20002, 0x120000, 0x1F0003 },
    [UO_SECTION]   = { 0x20005, 0x20002, 0x20008, 0xF001F },
    [UO_TIMER]     = { 0x20001, 0x20002, 0x120000, 0x1F0003 },
    [UO_DIRECTORY] = { 0x20003, 0x2000C, 0x20003, 0xF000F },
    [UO_SYMLINK]   = { 0x20001, 0x20000, 0x20001, 0xF0001 },
    [UO_TOKEN]     = { 0x20008, 0x200E0, 0x20000, 0xF01FF },
};
static const UINT32 g_file_map[4] = { 0x120089, 0x120116, 0x1200A0, 0x1F01FF };

static UINT32 map_generic(UINT32 m, const UINT32 *map)
{
    if (m & 0x80000000u) m |= map[0];
    if (m & 0x40000000u) m |= map[1];
    if (m & 0x20000000u) m |= map[2];
    if (m & 0x10000000u) m |= map[3];
    return m & 0x0FFFFFFFu;
}

/* Does @t hold @sid?  Allow ACEs match the user and enabled groups; deny
 * ACEs also the deny-only ones.  The restricting pass matches only the
 * restricting SIDs. */
static bool holds(const UmToken *t, const UINT8 *sid, bool deny, bool restricting)
{
    if (restricting) {
        for (int i = 0; i < t->nrestricted; i++) if (sid_eq(t->restricted[i].sid, sid)) return true;
        return false;
    }
    if (sid_eq(t->user.sid, sid)) return deny || !(t->user.attrs & GRP_DENY_ONLY);
    for (int i = 0; i < t->ngroups; i++) {
        if (!sid_eq(t->groups[i].sid, sid)) continue;
        if (t->groups[i].attrs & GRP_DENY_ONLY) return deny;
        if (t->groups[i].attrs & GRP_ENABLED) return true;
    }
    return false;
}

/* The rights a DACL (present, not NULL) grants one pass of the check */
static UINT32 dacl_grants(const SdView *v, const UmToken *t, const UINT32 *map, bool restricting)
{
    UINT32 allowed = 0, denied = 0;
    if (v->owner && holds(t, v->owner, false, restricting)) allowed = 0x00060000u;   /* READ_CONTROL | WRITE_DAC */
    const UINT8 *acl = v->dacl, *p = acl + 8, *end = acl + rd16(acl + 2);
    UINT16 n = rd16(acl + 4);
    for (UINT16 i = 0; i < n; i++) {
        if (p + 8 > end) break;
        UINT8 type = p[0], flags = p[1];
        UINT16 size = rd16(p + 2);
        if (size < 16 || p + size > end) break;
        const UINT8 *ace = p;
        p += size;
        if (flags & 0x08) continue;                     /* INHERIT_ONLY_ACE: for children */
        if (type > 1) continue;                         /* only ACCESS_ALLOWED / ACCESS_DENIED */
        const UINT8 *sid = ace + 8;
        if (!sid_ok(sid, size - 8u) || !holds(t, sid, type == 1, restricting)) continue;
        UINT32 m = map_generic(rd32(ace + 4), map);
        if (type == 0) allowed |= m & ~denied;
        else denied |= m & ~allowed;
    }
    return allowed;
}

/* @want (generic rights mapped by @map; MAXIMUM_ALLOWED asks for all
 * that is granted) against descriptor @sd for token @t: ST_SUCCESS with
 * *granted, or ST_ACCESS_DENIED.  A restricted token must pass twice, as
 * itself and as its restricting SIDs (for write rights only if it is
 * write-restricted). */
static UINT32 check(const UINT8 *sd, UINT32 len, const UmToken *t, UINT32 want, const UINT32 *map, UINT32 *granted)
{
    SdView v;
    *granted = 0;
    if (!sd_view(sd, len, &v)) return ST_INVALID_SECURITY_DESCR;
    bool max = (want & 0x02000000u) != 0;
    UINT32 m = map_generic(want & ~0x03000000u, map);   /* (ACCESS_SYSTEM_SECURITY: not checked) */
    if (!v.dacl_present || !v.dacl) {                   /* no DACL: whatever is asked, all of GenericAll for MAXIMUM_ALLOWED */
        *granted = max ? map[3] | m : m;
        return ST_SUCCESS;
    }
    UINT32 allowed = dacl_grants(&v, t, map, false);
    if (t->nrestricted) {
        UINT32 r = dacl_grants(&v, t, map, true);
        if (t->write_restricted) r |= ~(map[1] & ~0x00120000u);   /* only its write rights are restricted */
        allowed &= r;
    }
    if (m & ~allowed) return ST_ACCESS_DENIED;
    *granted = max ? allowed | m : m;
    return max && !*granted ? ST_ACCESS_DENIED : ST_SUCCESS;
}

/* Open-time checks against the descriptor in *@slot (NULL: none, open to
 * all), read under g_sd_lock since NtSetSecurityObject may replace it */
static UINT32 check_open(void *const *slot, UINT32 want, const UINT32 *map)
{
    if (!*slot) return ST_SUCCESS;
    UmObject *to = effective_token();
    if (!to) return ST_SUCCESS;
    UINT32 granted, st = ST_SUCCESS;
    IrqState s = spin_lock_irqsave(&g_sd_lock);
    const UmSd *sd = *slot;
    if (sd) st = check(sd->b, sd->len, to->ptr, want, map, &granted);
    spin_unlock_irqrestore(&g_sd_lock, s);
    um_ob_unref(to);
    return st == ST_INVALID_SECURITY_DESCR ? ST_ACCESS_DENIED : st;
}

UINT32 um_check_object(UmObject *o, UINT32 want)
{
    if (o->type >= (int)(sizeof(g_maps) / sizeof(g_maps[0])) || !g_maps[o->type][3]) return ST_SUCCESS;
    return check_open(&o->sd, want, g_maps[o->type]);
}

/* For the file system: @want against a self-relative descriptor, as the
 * calling thread */
UINT32 um_access_check_sd(const UINT8 *sd, UINT32 len, UINT32 want, const UINT32 map[4], UINT32 *granted)
{
    *granted = 0;
    if (!sd) { *granted = map_generic(want & ~0x03000000u, map); return ST_SUCCESS; }
    UmObject *to = effective_token();
    if (!to) return ST_SUCCESS;
    UINT32 st = check(sd, len, to->ptr, want, map, granted);
    um_ob_unref(to);
    return st == ST_INVALID_SECURITY_DESCR ? ST_ACCESS_DENIED : st;
}

/* OBJECT_ATTRIBUTES.SecurityDescriptor of @oa (user pointer, may be 0) for
 * a new object: *out is NULL when there is none */
UINT32 um_oa_security(UINT64 oa, void **out)
{
    *out = NULL;
    if (!oa) return ST_SUCCESS;
    UINT64 sd;                                          /* (WoW's ntdll passes the 64-bit layout) */
    if (!NT_SUCCESS(CopyFromUser(&sd, (const void *)(uintptr_t)(oa + 32), 8))) return ST_ACCESS_VIOLATION;
    if (!sd) return ST_SUCCESS;
    UINT32 st;
    *out = sd_capture(sd, &st);
    return st;
}

void um_sd_free(void *sd) { kfree(sd); }

/* -----------------------------------------------------------------------
 * Writing structures back in the caller's layout (x64, or x86 for WoW)
 * ----------------------------------------------------------------------- */
typedef struct {
    UINT8 *b;                   /* kernel staging buffer */
    UINT32 n, cap;              /* bytes needed so far / staged */
    UINT64 user;                /* where it goes */
    int    ps;                  /* pointer size */
} Out;

static void out_put(Out *o, UINT32 at, const void *v, UINT32 n)
{
    if (at + n <= o->cap) memcpy(o->b + at, v, n);
    if (at + n > o->n) o->n = at + n;
}

static void out_ptr(Out *o, UINT32 at, UINT32 target)
{
    UINT64 v = o->user + target;
    out_put(o, at, &v, (UINT32)o->ps);
}

/* SID_AND_ATTRIBUTES[n] (after a count, TOKEN_GROUPS, if @counted) and the SIDs */
static void out_groups(Out *o, const SidAttr *g, int n, bool counted)
{
    UINT32 head = counted ? (UINT32)o->ps : 0, ent = (UINT32)o->ps * 2;   /* { PSID; ULONG; } padded */
    if (counted) { UINT32 c = (UINT32)n; out_put(o, 0, &c, 4); }
    UINT32 tail = head + ent * (UINT32)n;
    for (int i = 0; i < n; i++) {
        UINT32 l = sid_len(g[i].sid);
        out_ptr(o, head + ent * (UINT32)i, tail);
        out_put(o, head + ent * (UINT32)i + (UINT32)o->ps, &g[i].attrs, 4);
        if (o->ps == 8) { UINT32 z = 0; out_put(o, head + ent * (UINT32)i + 12, &z, 4); }
        out_put(o, tail, g[i].sid, l);
        tail += l;
    }
    if (!n) o->n = o->n > head ? o->n : head;
}

/* { PSID or PACL } then what it points to */
static void out_ref(Out *o, const UINT8 *data, UINT32 len)
{
    if (!data) { UINT64 z = 0; out_put(o, 0, &z, (UINT32)o->ps); return; }
    out_ptr(o, 0, (UINT32)o->ps);
    out_put(o, (UINT32)o->ps, data, len);
}

/* -----------------------------------------------------------------------
 * System services
 * ----------------------------------------------------------------------- */
static bool put_u32(UINT64 p, UINT32 v) { return !p || NT_SUCCESS(CopyToUser((void *)(uintptr_t)p, &v, 4)); }

static bool put_handle(UINT64 p, UINT64 h)
{
    UmProcess *me = UmCurrent();
    if (me && me->wow) { UINT32 v = (UINT32)h; return NT_SUCCESS(CopyToUser((void *)(uintptr_t)p, &v, 4)); }
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)p, &h, 8));
}

/* A handle to @o (referenced by the caller, who keeps its reference) */
static UINT32 give_handle(UmObject *o, UINT64 out, UINT32 attrs)
{
    UmProcess *p = UmCurrent();
    UINT64 h = um_handle_new_object(p, o);
    if (!h) return ST_TOO_MANY_HANDLES;
    if (attrs & 2) um_handle_set_inherit(p, h, true);        /* OBJ_INHERIT */
    if (!put_handle(out, h)) { um_close_handle(h); return ST_ACCESS_VIOLATION; }
    return ST_SUCCESS;
}

/* A token handle (or the pseudo-handles -4 process, -5 thread, -6
 * effective token), referenced */
static UmObject *token_of(UINT64 h)
{
    UmProcess *p = UmCurrent();
    if (h == UINT64_C(0xFFFFFFFFFFFFFFFC) || h == UINT64_C(0xFFFFFFFFFFFFFFFA) || h == UINT64_C(0xFFFFFFFFFFFFFFFB)) {
        if (h == UINT64_C(0xFFFFFFFFFFFFFFFC)) return um_token_for_process(p);
        UmThread *me = UmCurrentThread();
        UmObject *o = NULL;
        IrqState s = ob_lock();
        if (me && me->imp) o = um_ob_ref(me->imp);
        ob_unlock(s);
        if (o || h == UINT64_C(0xFFFFFFFFFFFFFFFB)) return o;
        return um_token_for_process(p);
    }
    return um_handle_object(p, h, UO_TOKEN);
}

/* NtOpenProcessToken(HANDLE Process, ACCESS_MASK, PHANDLE Token) and
 * NtOpenProcessTokenEx(HANDLE, ACCESS_MASK, ULONG HandleAttributes, PHANDLE) */
static UINT32 open_process_token(UINT64 ph, UINT64 out, UINT32 attrs)
{
    UmObject *pob;
    UmProcess *p = um_proc_of(UmCurrent(), ph, &pob);
    if (!p) return ST_INVALID_HANDLE;
    UmObject *t = um_token_for_process(p);
    if (pob) um_ob_unref(pob);
    if (!t) return ST_NO_MEMORY;
    UINT32 st = give_handle(t, out, attrs);
    um_ob_unref(t);
    return st;
}

static UINT64 sys_open_process_token(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a4;
    return open_process_token(a1, a3, 0);
}

static UINT64 sys_open_process_token_ex(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2;
    return open_process_token(a1, a4, (UINT32)a3);
}

/* NtOpenThreadToken(HANDLE Thread, ACCESS_MASK, BOOLEAN OpenAsSelf, PHANDLE)
 * and NtOpenThreadTokenEx(..., OpenAsSelf, ULONG HandleAttributes, PHANDLE) */
static UINT32 open_thread_token(UINT64 th, UINT64 out, UINT32 attrs)
{
    UmObject *to = um_handle_object(UmCurrent(), th, UO_THREAD);
    if (!to) return ST_INVALID_HANDLE;
    UmThread *t = (UmThread *)to;
    UmObject *tok = NULL;
    IrqState s = ob_lock();
    if (t->imp) tok = um_ob_ref(t->imp);
    ob_unlock(s);
    um_ob_unref(to);
    if (!tok) return ST_NO_TOKEN;
    UINT32 st = give_handle(tok, out, attrs);
    um_ob_unref(tok);
    return st;
}

static UINT64 sys_open_thread_token(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3;
    return open_thread_token(a1, a4, 0);
}

static UINT64 sys_open_thread_token_ex(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3;
    return open_thread_token(a1, um_stack_arg(5), (UINT32)a4);
}

/* NtDuplicateToken(HANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN
 * EffectiveOnly, TOKEN_TYPE, PHANDLE): the impersonation level comes from
 * the attributes' SECURITY_QUALITY_OF_SERVICE */
static UINT64 sys_duplicate_token(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a4;
    UINT32 type = (UINT32)um_stack_arg(5);
    UINT64 out = um_stack_arg(6);
    if (type != 1 && type != 2) return ST_INVALID_PARAMETER;
    UINT32 level = 0, attrs = 0;
    if (a3) {
        UINT64 oa[6];
        if (!NT_SUCCESS(CopyFromUser(oa, (const void *)(uintptr_t)a3, sizeof(oa)))) return ST_ACCESS_VIOLATION;
        attrs = (UINT32)oa[3];
        UINT32 qos[2];
        if (oa[5] && !NT_SUCCESS(CopyFromUser(qos, (const void *)(uintptr_t)oa[5], 8))) return ST_ACCESS_VIOLATION;
        if (oa[5]) level = qos[1];
    }
    if (level > 3) return ST_INVALID_PARAMETER;
    UmObject *src = token_of(a1);
    if (!src) return ST_INVALID_HANDLE;
    UmToken t = *(UmToken *)src->ptr;
    um_ob_unref(src);
    t.type = type;
    t.level = type == 2 ? level : 0;
    UmObject *o = token_new(&t);
    if (!o) return ST_NO_MEMORY;
    UINT32 st = give_handle(o, out, attrs);
    um_ob_unref(o);
    return st;
}

/* TOKEN_GROUPS from the caller: each SID found in @t's user or groups
 * becomes deny-only (@restrict false), or is added as a restricting SID */
static UINT32 apply_groups(UmToken *t, UINT64 p, bool restricting)
{
    if (!p) return ST_SUCCESS;
    int ps = UmCurrent() && UmCurrent()->wow ? 4 : 8;
    UINT32 n;
    if (!NT_SUCCESS(CopyFromUser(&n, (const void *)(uintptr_t)p, 4))) return ST_ACCESS_VIOLATION;
    if (n > 64) return ST_INVALID_PARAMETER;
    for (UINT32 i = 0; i < n; i++) {
        UINT64 e = p + (UINT64)ps + (UINT64)i * 2 * (UINT64)ps, sp = 0;
        if (!NT_SUCCESS(CopyFromUser(&sp, (const void *)(uintptr_t)e, (size_t)ps))) return ST_ACCESS_VIOLATION;
        UINT8 sid[SID_MAX];
        if (!sp || !user_sid(sp, sid)) return ST_INVALID_SID;
        if (restricting) {
            if (t->nrestricted >= TOKEN_RESTRICT_MAX) return ST_INVALID_PARAMETER;
            sid_set(&t->restricted[t->nrestricted++], sid, GRP_ON);
            continue;
        }
        if (sid_eq(t->user.sid, sid)) t->user.attrs |= GRP_DENY_ONLY;
        for (int k = 0; k < t->ngroups; k++)
            if (sid_eq(t->groups[k].sid, sid)) t->groups[k].attrs = (t->groups[k].attrs & ~(GRP_ENABLED | GRP_DEFAULT)) | GRP_DENY_ONLY;
    }
    return ST_SUCCESS;
}

/* NtFilterToken(HANDLE Existing, ULONG Flags, PTOKEN_GROUPS SidsToDisable,
 *               PTOKEN_PRIVILEGES PrivilegesToDelete, PTOKEN_GROUPS
 *               RestrictedSids, PHANDLE NewToken): CreateRestrictedToken */
static UINT64 sys_filter_token(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT64 restricted = um_stack_arg(5), out = um_stack_arg(6);
    UmObject *src = token_of(a1);
    if (!src) return ST_INVALID_HANDLE;
    UmToken t = *(UmToken *)src->ptr;
    um_ob_unref(src);
    UINT32 st = apply_groups(&t, a3, false);
    if (!st) st = apply_groups(&t, restricted, true);
    if (st) return st;
    if (a4) {                                               /* TOKEN_PRIVILEGES: { count, { LUID, attrs }[] } */
        UINT32 n;
        if (!NT_SUCCESS(CopyFromUser(&n, (const void *)(uintptr_t)a4, 4))) return ST_ACCESS_VIOLATION;
        if (n > 64) return ST_INVALID_PARAMETER;
        for (UINT32 i = 0; i < n; i++) {
            UINT32 luid[2];
            if (!NT_SUCCESS(CopyFromUser(luid, (const void *)(uintptr_t)(a4 + 4 + 12 * i), 8))) return ST_ACCESS_VIOLATION;
            if (!luid[1] && luid[0] < 64) { t.privs &= ~(1ull << luid[0]); t.privs_on &= ~(1ull << luid[0]); }
        }
    }
    if (a2 & DISABLE_MAX_PRIVILEGE) { t.privs &= 1ull << SE_CHANGE_NOTIFY; t.privs_on &= 1ull << SE_CHANGE_NOTIFY; }
    if (a2 & SANDBOX_INERT) t.sandbox_inert = true;
    if (a2 & WRITE_RESTRICTED) t.write_restricted = true;
    UmObject *o = token_new(&t);
    if (!o) return ST_NO_MEMORY;
    st = give_handle(o, out, 0);
    um_ob_unref(o);
    return st;
}

/* NtQueryInformationToken(HANDLE, TOKEN_INFORMATION_CLASS, PVOID, ULONG, PULONG) */
static UINT64 sys_query_token(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT64 ret_ptr = um_stack_arg(5);
    UmObject *to = token_of(a1);
    if (!to) return ST_INVALID_HANDLE;
    UmToken t = *(UmToken *)to->ptr;
    um_ob_unref(to);
    UINT32 cap = (UINT32)a4 < 4096 ? (UINT32)a4 : 4096;
    Out o = { kzalloc(4096), 0, cap, a3, UmCurrent() && UmCurrent()->wow ? 4 : 8 };
    if (!o.b) return ST_NO_MEMORY;
    UINT32 v, st = ST_SUCCESS;
    switch ((UINT32)a2) {
    case 1:  out_groups(&o, &t.user, 1, false); break;                          /* TokenUser */
    case 2: {                                                                   /* TokenGroups */
        out_groups(&o, t.groups, t.ngroups, true);
        break;
    }
    case 3: {                                                                   /* TokenPrivileges */
        UINT32 n = 0;
        for (int i = 0; i < 64; i++) {
            if (!(t.privs & (1ull << i))) continue;
            UINT32 e[3] = { (UINT32)i, 0, (t.privs_on & (1ull << i)) ? 3u : 0u };   /* ENABLED_BY_DEFAULT | ENABLED */
            out_put(&o, 4 + 12 * n++, e, 12);
        }
        out_put(&o, 0, &n, 4);
        break;
    }
    case 4:  out_ref(&o, t.user.sid, sid_len(t.user.sid)); break;               /* TokenOwner */
    case 5:  out_ref(&o, g_users_sid, sizeof(g_users_sid)); break;              /* TokenPrimaryGroup */
    case 6: {                                                                   /* TokenDefaultDacl: user and SYSTEM, full access */
        UINT8 acl[8 + 2 * (8 + SID_MAX)];
        UINT32 ul = sid_len(t.user.sid), at = 8;
        memset(acl, 0, sizeof(acl));
        acl[0] = 2;
        const UINT8 *sids[2] = { t.user.sid, g_system_sid };
        UINT32 lens[2] = { ul, sizeof(g_system_sid) };
        for (int i = 0; i < 2; i++) {
            UINT16 size = (UINT16)(8 + lens[i]);
            UINT32 mask = 0x10000000u;                                          /* GENERIC_ALL */
            acl[at + 2] = (UINT8)size; acl[at + 3] = (UINT8)(size >> 8);
            memcpy(acl + at + 4, &mask, 4);
            memcpy(acl + at + 8, sids[i], lens[i]);
            at += size;
        }
        acl[2] = (UINT8)at; acl[3] = (UINT8)(at >> 8); acl[4] = 2;
        out_ref(&o, acl, at);
        break;
    }
    case 7: {                                                                   /* TokenSource */
        UINT8 src[16] = { 'U', 's', 'e', 'r', '3', '2', ' ', 0, 0xE9, 3 };
        out_put(&o, 0, src, 16);
        break;
    }
    case 8:  v = t.type; out_put(&o, 0, &v, 4); break;                          /* TokenType */
    case 9:                                                                     /* TokenImpersonationLevel */
        if (t.type != 2) { st = ST_INVALID_INFO_CLASS; break; }
        v = t.level; out_put(&o, 0, &v, 4); break;
    case 10: {                                                                  /* TokenStatistics */
        UINT8 s[56];
        memset(s, 0, sizeof(s));
        memcpy(s, &t.id, 4);
        UINT32 auth = 0x3E7 + 1, n = (UINT32)t.ngroups, np = 0;
        for (int i = 0; i < 64; i++) if (t.privs & (1ull << i)) np++;
        memcpy(s + 8, &auth, 4);
        memcpy(s + 24, &t.type, 4);
        memcpy(s + 28, &t.level, 4);
        memcpy(s + 40, &n, 4);
        memcpy(s + 44, &np, 4);
        memcpy(s + 48, &t.modified, 4);
        out_put(&o, 0, s, 56);
        break;
    }
    case 11: out_groups(&o, t.restricted, t.nrestricted, true); break;          /* TokenRestrictedSids */
    case 12: v = 1; out_put(&o, 0, &v, 4); break;                               /* TokenSessionId */
    case 15: v = t.sandbox_inert; out_put(&o, 0, &v, 4); break;                 /* TokenSandBoxInert */
    case 18: v = 1; out_put(&o, 0, &v, 4); break;                               /* TokenElevationType: default */
    case 19: st = ST_NO_TOKEN; break;                                           /* TokenLinkedToken: not a split token */
    case 20: v = 0; out_put(&o, 0, &v, 4); break;                               /* TokenElevation */
    case 21: v = t.nrestricted ? 1 : 0; out_put(&o, 0, &v, 4); break;           /* TokenHasRestrictions */
    case 23: case 24: case 26: case 29:                                         /* virtualization, UIAccess, AppContainer */
        v = 0; out_put(&o, 0, &v, 4); break;
    case 25: {                                                                  /* TokenIntegrityLevel */
        SidAttr il;
        sid_set(&il, g_medium_il_sid, 0x20);                                    /* SE_GROUP_INTEGRITY */
        out_groups(&o, &il, 1, false);
        break;
    }
    case 27: v = 1; out_put(&o, 0, &v, 4); break;                               /* TokenMandatoryPolicy: NO_WRITE_UP */
    case 28: {                                                                  /* TokenLogonSid */
        SidAttr ls;
        sid_set(&ls, g_logon_sid, GRP_ON | 0xC0000000u);
        out_groups(&o, &ls, 1, true);
        break;
    }
    default:
        st = ST_INVALID_INFO_CLASS;
    }
    if (st) { kfree(o.b); return st; }
    UINT32 need = o.n;
    if (!put_u32(ret_ptr, need)) { kfree(o.b); return ST_ACCESS_VIOLATION; }
    if ((UINT32)a4 < need) { kfree(o.b); return ST_BUFFER_TOO_SMALL; }
    bool ok = NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, o.b, need));
    kfree(o.b);
    return ok ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* NtSetInformationThread(ThreadImpersonationToken): impersonate the
 * token in the buffer (an impersonation token), or stop (0) */
UINT32 um_set_thread_token(UmThread *t, UINT64 buf, UINT32 len)
{
    UINT64 h = 0;
    if (len == 4) {
        INT32 v;
        if (!NT_SUCCESS(CopyFromUser(&v, (const void *)(uintptr_t)buf, 4))) return ST_ACCESS_VIOLATION;
        h = (UINT64)(INT64)v;
    } else if (len == 8) {
        if (!NT_SUCCESS(CopyFromUser(&h, (const void *)(uintptr_t)buf, 8))) return ST_ACCESS_VIOLATION;
    } else return ST_INFO_LENGTH_MISMATCH;
    UmObject *tok = NULL;
    if (h) {
        tok = um_handle_object(UmCurrent(), h, 0);
        if (!tok) return ST_INVALID_HANDLE;
        if (tok->type != UO_TOKEN) { um_ob_unref(tok); return ST_OBJECT_TYPE_MISMATCH; }
        if (((UmToken *)tok->ptr)->type != 2) { um_ob_unref(tok); return ST_BAD_TOKEN_TYPE; }
    }
    IrqState s = ob_lock();
    UmObject *old = t->imp;
    t->imp = tok;
    ob_unlock(s);
    if (old) um_ob_unref(old);
    return ST_SUCCESS;
}

/* NtImpersonateAnonymousToken(HANDLE Thread): the thread acts as ANONYMOUS
 * LOGON (S-1-5-7), member of Everyone and Network only */
static UINT64 sys_impersonate_anonymous(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    static const UINT8 anonymous[] = { 1, 1, 0, 0, 0, 0, 0, 5, 7, 0, 0, 0 };
    static const UINT8 network[] = { 1, 1, 0, 0, 0, 0, 0, 5, 2, 0, 0, 0 };
    UmObject *th = um_handle_object(UmCurrent(), a1, UO_THREAD);
    if (!th) return ST_INVALID_HANDLE;
    UmToken t;
    memset(&t, 0, sizeof(t));
    sid_set(&t.user, anonymous, 0);
    sid_set(&t.groups[t.ngroups++], g_everyone_sid, GRP_ON);
    sid_set(&t.groups[t.ngroups++], network, GRP_ON);
    t.type = 2;
    t.level = 2;                                                    /* SecurityImpersonation */
    UmObject *tok = token_new(&t);
    if (!tok) { um_ob_unref(th); return ST_NO_MEMORY; }
    IrqState s = ob_lock();
    UmObject *old = ((UmThread *)th)->imp;
    ((UmThread *)th)->imp = tok;
    ob_unlock(s);
    if (old) um_ob_unref(old);
    um_ob_unref(th);
    return ST_SUCCESS;
}

/* NtAccessCheck(PSECURITY_DESCRIPTOR, HANDLE ClientToken, ACCESS_MASK,
 *               PGENERIC_MAPPING, PPRIVILEGE_SET, PULONG PrivilegeSetLength,
 *               PACCESS_MASK GrantedAccess, PNTSTATUS AccessStatus) */
static UINT64 sys_access_check(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT64 privs = um_stack_arg(5), privs_len = um_stack_arg(6), granted_p = um_stack_arg(7), status_p = um_stack_arg(8);
    if (!a1 || !a4 || !granted_p || !status_p) return ST_INVALID_PARAMETER;
    UINT32 map[4], plen = 0;
    if (!NT_SUCCESS(CopyFromUser(map, (const void *)(uintptr_t)a4, 16))) return ST_ACCESS_VIOLATION;
    if (privs_len) {
        if (!NT_SUCCESS(CopyFromUser(&plen, (const void *)(uintptr_t)privs_len, 4))) return ST_ACCESS_VIOLATION;
        if (plen < 20) { put_u32(privs_len, 20); return ST_BUFFER_TOO_SMALL; }   /* sizeof(PRIVILEGE_SET) */
    }
    UmObject *to = um_handle_object(UmCurrent(), a2, 0);
    if (!to) return ST_INVALID_HANDLE;
    if (to->type != UO_TOKEN) { um_ob_unref(to); return ST_OBJECT_TYPE_MISMATCH; }
    if (((UmToken *)to->ptr)->type != 2) { um_ob_unref(to); return ST_NO_IMPERSONATION_TOKEN; }
    UINT32 st;
    UmSd *sd = sd_capture(a1, &st);
    if (!sd) { um_ob_unref(to); return st; }
    UINT32 granted, result = check(sd->b, sd->len, to->ptr, (UINT32)a3, map, &granted);
    kfree(sd);
    um_ob_unref(to);
    if (result == ST_INVALID_SECURITY_DESCR) return result;
    UINT32 zero[2] = { 0, 0 };                                      /* PRIVILEGE_SET: no privileges used */
    if (privs && privs_len && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)privs, zero, 8))) return ST_ACCESS_VIOLATION;
    if (!put_u32(granted_p, granted) || !put_u32(status_p, result)) return ST_ACCESS_VIOLATION;
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * NtQuerySecurityObject / NtSetSecurityObject
 * ----------------------------------------------------------------------- */
/* Where a handle's descriptor lives (H_OBJECT or a file).  *@valid says
 * whether the handle exists at all: one that does but keeps no descriptor
 * (a console, a process, a file system without them) gives NULL, and reads
 * as the default descriptor */
static void **sd_slot(UINT64 h, UmObject **ob, const UINT32 **map, bool *valid)
{
    UmProcess *p = UmCurrent();
    *ob = NULL;
    *map = g_file_map;
    int kind = um_handle_kind(p, h);
    *valid = kind > 0;
    UmObject *o = um_handle_object(p, h, 0);
    if (!o) return NULL;
    *valid = true;
    if (o->type == UO_THREAD || o->type == UO_PROCESS) { um_ob_unref(o); return NULL; }   /* (not kept yet) */
    *ob = o;
    if (o->type < (int)(sizeof(g_maps) / sizeof(g_maps[0])) && g_maps[o->type][3]) *map = g_maps[o->type];
    return &o->sd;
}

/* Files and directories: their descriptors (inherited ones too) are the
 * file system's, kept by kernel/fs/fsec.c */
static bool is_file(UINT64 h)
{
    int kind = um_handle_kind(UmCurrent(), h);
    return kind == H_FILE || kind == H_DIR;
}

/* NtQuerySecurityObject(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR,
 * ULONG Length, PULONG LengthNeeded): an object without a descriptor
 * reports the user as owner, Users as group and a NULL DACL */
static UINT64 sys_query_security(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    if (is_file(a1)) return um_file_query_security(a1, a2, a3, a4);
    UINT64 need_p = um_stack_arg(5);
    UmObject *ob;
    const UINT32 *map;
    DesktopLock();                                                  /* files' nodes stay put */
    bool valid;
    void **slot = sd_slot(a1, &ob, &map, &valid);
    if (!slot && !valid) { DesktopUnlock(); return ST_INVALID_HANDLE; }
    UmSd *have = NULL;
    bool had = false;
    if (slot) {
        IrqState s = spin_lock_irqsave(&g_sd_lock);
        have = *slot ? sd_store(((UmSd *)*slot)->b, ((UmSd *)*slot)->len) : NULL;
        had = *slot != NULL;
        spin_unlock_irqrestore(&g_sd_lock, s);
    }
    DesktopUnlock();
    if (ob) um_ob_unref(ob);
    if (had && !have) return ST_NO_MEMORY;
    SdView v;
    if (have) sd_view(have->b, have->len, &v);
    else { memset(&v, 0, sizeof(v)); v.owner = g_user_sid; v.group = g_users_sid; v.dacl_present = true; }
    UINT32 info = (UINT32)a2, len;
    UINT8 *out = sd_build((info & 1) ? v.owner : NULL, (info & 2) ? v.group : NULL,
                          (info & 4) && v.dacl_present, (info & 4) ? v.dacl : NULL, v.control, &len);
    kfree(have);
    if (!out) return ST_NO_MEMORY;
    UINT32 st = ST_SUCCESS;
    if (!put_u32(need_p, len)) st = ST_ACCESS_VIOLATION;
    else if ((UINT32)a4 < len) st = ST_BUFFER_TOO_SMALL;
    else if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, out, len))) st = ST_ACCESS_VIOLATION;
    kfree(out);
    return st;
}

/* NtSetSecurityObject(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR):
 * the parts @info names replace the object's.  Changing the DACL takes
 * WRITE_DAC, the owner WRITE_OWNER, under the descriptor it has now. */
static UINT64 sys_set_security(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    if (is_file(a1)) return um_file_set_security(a1, a2, a3, a4);
    (void)a4;
    UINT32 info = (UINT32)a2 & 7, st;
    if (!a3) return ST_INVALID_PARAMETER;
    UmSd *in = sd_capture(a3, &st);
    if (!in) return st;
    SdView nv;
    sd_view(in->b, in->len, &nv);
    UmObject *ob;
    const UINT32 *map;
    DesktopLock();
    bool valid;
    void **slot = sd_slot(a1, &ob, &map, &valid);
    if (!slot) { DesktopUnlock(); kfree(in); return valid ? ST_SUCCESS : ST_INVALID_HANDLE; }   /* kept nowhere: accepted */
    UINT32 want = ((info & 4) ? 0x00040000u : 0) | ((info & 3) ? 0x00080000u : 0);
    st = check_open(slot, want, map);
    UmSd *old = *slot, *fresh = NULL;                               /* (only we change it, under DesktopLock) */
    if (!st) {
        SdView ov;
        if (old) sd_view(old->b, old->len, &ov);
        else { memset(&ov, 0, sizeof(ov)); ov.owner = g_user_sid; ov.group = g_users_sid; ov.dacl_present = true; }
        UINT32 len;
        UINT8 *b = sd_build((info & 1) ? nv.owner : ov.owner, (info & 2) ? nv.group : ov.group,
                            (info & 4) ? nv.dacl_present : ov.dacl_present, (info & 4) ? nv.dacl : ov.dacl,
                            (info & 4) ? nv.control : ov.control, &len);
        fresh = b ? sd_store(b, len) : NULL;
        kfree(b);
        if (!fresh) st = ST_NO_MEMORY;
    }
    if (!st) {
        IrqState s = spin_lock_irqsave(&g_sd_lock);
        *slot = fresh;
        spin_unlock_irqrestore(&g_sd_lock, s);
        kfree(old);
    }
    DesktopUnlock();
    if (ob) um_ob_unref(ob);
    kfree(in);
    return st;
}

void um_security_syscalls_init(void)
{
    um_install(SYSCALL_NtAccessCheck,            sys_access_check);
    um_install(SYSCALL_NtOpenProcessToken,       sys_open_process_token);
    um_install(SYSCALL_NtOpenProcessTokenEx,     sys_open_process_token_ex);
    um_install(SYSCALL_NtOpenThreadToken,        sys_open_thread_token);
    um_install(SYSCALL_NtOpenThreadTokenEx,      sys_open_thread_token_ex);
    um_install(SYSCALL_NtDuplicateToken,         sys_duplicate_token);
    um_install(SYSCALL_NtFilterToken,            sys_filter_token);
    um_install(SYSCALL_NtQueryInformationToken,  sys_query_token);
    um_install(SYSCALL_NtQuerySecurityObject,    sys_query_security);
    um_install(SYSCALL_NtSetSecurityObject,      sys_set_security);
    um_install(SYSCALL_NtImpersonateAnonymousToken, sys_impersonate_anonymous);
}
