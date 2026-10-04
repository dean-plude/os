/*
 * sddl.c — security descriptors as SDDL text, both ways:
 * "O:BAG:SYD:PAI(A;OICI;FA;;;BA)(A;;0x1200a9;;;WD)S:(ML;;NW;;;LW)".
 *
 * Owner and group are SIDs ("S-1-...") or two-letter aliases; a DACL or
 * SACL is its flags (P protected, AI auto-inherited, AR auto-inherit
 * required, NO_ACCESS_CONTROL a NULL DACL) and ACEs
 * "(type;flags;rights;object guid;inherited object guid;sid)".  Rights
 * are a number or two-letter names (GA, FA, KR, ...).  NovaOS keeps no
 * SACL on files or objects, so a SACL (audit entries and the mandatory
 * label) is parsed and then not kept by the kernel.
 */

#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

/* ACE types */
static const struct { char name[3]; BYTE type; } g_types[] = {
    { "A", 0x00 }, { "D", 0x01 }, { "AU", 0x02 }, { "AL", 0x03 }, { "OA", 0x05 }, { "OD", 0x06 },
    { "OU", 0x07 }, { "OL", 0x08 }, { "ML", 0x11 },
};

/* ACE flags */
static const struct { char name[3]; BYTE flag; } g_ace_flags[] = {
    { "OI", 0x01 }, { "CI", 0x02 }, { "NP", 0x04 }, { "IO", 0x08 }, { "ID", 0x10 }, { "SA", 0x40 }, { "FA", 0x80 },
};

/* Rights: whole masks first (written as one name), then single bits */
static const struct { char name[3]; DWORD mask; } g_rights[] = {
    { "GA", 0x10000000 }, { "GR", 0x80000000 }, { "GW", 0x40000000 }, { "GX", 0x20000000 },
    { "FA", 0x001F01FF }, { "FR", 0x00120089 }, { "FW", 0x00120116 }, { "FX", 0x001200A0 },
    { "KA", 0x000F003F }, { "KR", 0x00020019 }, { "KW", 0x00020006 }, { "KX", 0x00020019 },
    { "RC", 0x00020000 }, { "SD", 0x00010000 }, { "WD", 0x00040000 }, { "WO", 0x00080000 },
    { "CC", 0x00000001 }, { "DC", 0x00000002 }, { "LC", 0x00000004 }, { "SW", 0x00000008 },
    { "RP", 0x00000010 }, { "WP", 0x00000020 }, { "DT", 0x00000040 }, { "LO", 0x00000080 },
    { "CR", 0x00000100 },
};
#define WHOLE_RIGHTS 12                     /* g_rights[0..11] name masks; the rest name bits */

/* Mandatory label policies (ML ACEs use these names for their bits) */
static const struct { char name[3]; DWORD mask; } g_label_rights[] = { { "NW", 1 }, { "NR", 2 }, { "NX", 4 } };

#define TOO_LONG 4096

/* -----------------------------------------------------------------------
 * Text to descriptor
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *p;                          /* the text left */
    BYTE acl[2][TOO_LONG];                  /* DACL, SACL */
    BOOL has[2], null_acl[2];
    WORD control;
    PSID owner, group;
} Parse;

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static BOOL is_tag(const char *p) { return (*p == 'O' || *p == 'G' || *p == 'D' || *p == 'S') && p[1] == ':'; }

/* A SID up to @stop (or the next "X:" part): LocalAlloc'd */
static BOOL parse_sid(const char *s, size_t n, PSID *out)
{
    char t[200];
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (!n || n >= sizeof(t)) return FALSE;
    memcpy(t, s, n);
    t[n] = 0;
    return sid_from_string(t, out);
}

static BOOL parse_guid(const char *s, size_t n, GUID *g)
{
    if (n != 36) return FALSE;
    BYTE v[16];
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return FALSE; continue; }
        int c = upper(s[i]), d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return FALSE;
        if (k & 1) v[k / 2] = (BYTE)(v[k / 2] | d); else v[k / 2] = (BYTE)(d << 4);
        k++;
    }
    g->Data1 = (DWORD)v[0] << 24 | (DWORD)v[1] << 16 | (DWORD)v[2] << 8 | v[3];
    g->Data2 = (WORD)(v[4] << 8 | v[5]);
    g->Data3 = (WORD)(v[6] << 8 | v[7]);
    memcpy(g->Data4, v + 8, 8);
    return TRUE;
}

static BOOL parse_rights(const char *s, size_t n, BOOL label, DWORD *mask)
{
    *mask = 0;
    if (n > 2 && s[0] == '0' && (s[1] | 0x20) == 'x') {
        for (size_t i = 2; i < n; i++) {
            int c = upper(s[i]), d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return FALSE;
            *mask = *mask << 4 | (DWORD)d;
        }
        return TRUE;
    }
    if (n && s[0] >= '0' && s[0] <= '9') {
        for (size_t i = 0; i < n; i++) {
            if (s[i] < '0' || s[i] > '9') return FALSE;
            *mask = *mask * 10 + (DWORD)(s[i] - '0');
        }
        return TRUE;
    }
    if (n & 1) return FALSE;
    for (size_t i = 0; i < n; i += 2) {
        BOOL found = FALSE;
        for (unsigned k = 0; !found && label && k < sizeof(g_label_rights) / sizeof(g_label_rights[0]); k++)
            if (upper(s[i]) == g_label_rights[k].name[0] && upper(s[i + 1]) == g_label_rights[k].name[1]) {
                *mask |= g_label_rights[k].mask;
                found = TRUE;
            }
        for (unsigned k = 0; !found && k < sizeof(g_rights) / sizeof(g_rights[0]); k++)
            if (upper(s[i]) == g_rights[k].name[0] && upper(s[i + 1]) == g_rights[k].name[1]) {
                *mask |= g_rights[k].mask;
                found = TRUE;
            }
        if (!found) return FALSE;
    }
    return TRUE;
}

/* One "(...)" ACE onto ACL @which; the text after it */
static BOOL parse_ace(Parse *ps, int which)
{
    const char *f[7];
    size_t len[7];
    int nf = 0;
    const char *p = ps->p + 1;               /* past "(" */
    f[0] = p;
    for (;; p++) {
        if (!*p) return FALSE;
        if (*p == ';' || *p == ')') {
            if (nf >= 7) return FALSE;
            len[nf] = (size_t)(p - f[nf]);
            nf++;
            if (*p == ')') break;
            f[nf] = p + 1;
        }
    }
    ps->p = p + 1;
    if (nf < 6) return FALSE;

    BYTE type = 0xFF;
    for (unsigned k = 0; k < sizeof(g_types) / sizeof(g_types[0]); k++) {
        size_t l = g_types[k].name[1] ? 2 : 1;
        if (len[0] == l && upper(f[0][0]) == g_types[k].name[0] && (l == 1 || upper(f[0][1]) == g_types[k].name[1]))
            type = g_types[k].type;
    }
    if (type == 0xFF) return FALSE;
    BOOL object = type >= 5 && type <= 8;
    if ((which == 0) != (type <= 1 || type == 5 || type == 6)) return FALSE;    /* access ACEs in a DACL, the rest in a SACL */

    BYTE flags = 0;
    if (len[1] & 1) return FALSE;
    for (size_t i = 0; i < len[1]; i += 2) {
        BOOL found = FALSE;
        for (unsigned k = 0; !found && k < sizeof(g_ace_flags) / sizeof(g_ace_flags[0]); k++)
            if (upper(f[1][i]) == g_ace_flags[k].name[0] && upper(f[1][i + 1]) == g_ace_flags[k].name[1]) {
                flags |= g_ace_flags[k].flag;
                found = TRUE;
            }
        if (!found) return FALSE;
    }

    DWORD mask;
    if (!parse_rights(f[2], len[2], type == 0x11, &mask)) return FALSE;

    GUID og, ig;
    DWORD oflags = 0;
    if (len[3]) { if (!object || !parse_guid(f[3], len[3], &og)) return FALSE; oflags |= 1; }
    if (len[4]) { if (!object || !parse_guid(f[4], len[4], &ig)) return FALSE; oflags |= 2; }

    PSID sid;
    if (!parse_sid(f[5], len[5], &sid)) return FALSE;

    PACL acl = (PACL)ps->acl[which];
    if (!ps->has[which]) { InitializeAcl(acl, TOO_LONG, ACL_REVISION); ps->has[which] = TRUE; }
    DWORD used = 8;
    const BYTE *q = (const BYTE *)acl + 8;
    for (WORD i = 0; i < acl->AceCount; i++) { used += ((const ACE_HEADER *)q)->AceSize; q += ((const ACE_HEADER *)q)->AceSize; }
    DWORD sl = GetLengthSid(sid);
    DWORD size = 8 + (object ? 4 + ((oflags & 1) ? 16 : 0) + ((oflags & 2) ? 16 : 0) : 0) + sl;
    if (used + size > TOO_LONG) { LocalFree(sid); return FALSE; }
    BYTE *o = (BYTE *)acl + used;
    ACE_HEADER *h = (ACE_HEADER *)o;
    h->AceType = type;
    h->AceFlags = flags;
    h->AceSize = (WORD)size;
    memcpy(o + 4, &mask, 4);
    DWORD at = 8;
    if (object) {
        memcpy(o + at, &oflags, 4); at += 4;
        if (oflags & 1) { memcpy(o + at, &og, 16); at += 16; }
        if (oflags & 2) { memcpy(o + at, &ig, 16); at += 16; }
        acl->AclRevision = 4;                               /* ACL_REVISION_DS */
    }
    memcpy(o + at, sid, sl);
    LocalFree(sid);
    acl->AceCount++;
    return TRUE;
}

/* "D:" or "S:" (@which 0 or 1): flags, then ACEs */
static BOOL parse_acl(Parse *ps, int which)
{
    static const struct { const char *name; WORD bit[2]; } fl[] = {
        { "NO_ACCESS_CONTROL", { 0, 0 } }, { "AR", { 0x0100, 0x0200 } }, { "AI", { 0x0400, 0x0800 } }, { "P", { 0x1000, 0x2000 } },
    };
    ps->has[which] = TRUE;
    InitializeAcl((PACL)ps->acl[which], TOO_LONG, ACL_REVISION);
    for (;;) {
        const char *p = ps->p;
        if (*p == '(') { if (!parse_ace(ps, which)) return FALSE; continue; }
        if (!*p || is_tag(p)) return TRUE;
        BOOL found = FALSE;
        for (unsigned k = 0; !found && k < sizeof(fl) / sizeof(fl[0]); k++) {
            size_t l = strlen_(fl[k].name), i = 0;
            while (i < l && upper(p[i]) == fl[k].name[i]) i++;
            if (i < l) continue;
            if (!k) ps->null_acl[which] = TRUE;
            ps->control |= fl[k].bit[which];
            ps->p += l;
            found = TRUE;
        }
        if (!found) return FALSE;
    }
}

static BOOL parse_sddl(Parse *ps)
{
    while (*ps->p) {
        while (*ps->p == ' ') ps->p++;
        if (!*ps->p) break;
        if (!is_tag(ps->p)) return FALSE;
        char tag = ps->p[0];
        ps->p += 2;
        if (tag == 'O' || tag == 'G') {
            const char *e = ps->p;
            while (*e && !is_tag(e)) e++;
            PSID *slot = tag == 'O' ? &ps->owner : &ps->group;
            if (*slot || !parse_sid(ps->p, (size_t)(e - ps->p), slot)) return FALSE;
            ps->p = e;
        } else if (!parse_acl(ps, tag == 'S')) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL text_to_sd(const char *text, DWORD rev, PSECURITY_DESCRIPTOR *out, PULONG n)
{
    if (!text || !out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (rev != SDDL_REVISION_1) { SetLastError(ERROR_UNKNOWN_REVISION); return FALSE; }
    Parse *ps = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Parse));
    if (!ps) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    ps->p = text;
    BOOL ok = parse_sddl(ps);
    if (ok) {
        SECURITY_DESCRIPTOR abs;
        InitializeSecurityDescriptor(&abs, SECURITY_DESCRIPTOR_REVISION);
        if (ps->owner) SetSecurityDescriptorOwner(&abs, ps->owner, FALSE);
        if (ps->group) SetSecurityDescriptorGroup(&abs, ps->group, FALSE);
        /* (an ACL is trimmed to its ACEs) */
        for (int w = 0; w < 2; w++) {
            if (!ps->has[w]) continue;
            PACL acl = (PACL)ps->acl[w];
            DWORD used = 8;
            const BYTE *q = ps->acl[w] + 8;
            for (WORD i = 0; i < acl->AceCount; i++) { used += ((const ACE_HEADER *)q)->AceSize; q += ((const ACE_HEADER *)q)->AceSize; }
            acl->AclSize = (WORD)used;
            PACL a = ps->null_acl[w] && !acl->AceCount ? 0 : acl;
            if (w) SetSecurityDescriptorSacl(&abs, TRUE, a, FALSE);
            else SetSecurityDescriptorDacl(&abs, TRUE, a, FALSE);
        }
        abs.Control |= ps->control;
        DWORD len = 0;
        MakeSelfRelativeSD(&abs, 0, &len);
        PSECURITY_DESCRIPTOR sd = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, len);
        ok = sd && MakeSelfRelativeSD(&abs, sd, &len);
        if (ok) { *out = sd; if (n) *n = len; }
        else { if (sd) LocalFree(sd); SetLastError(ERROR_NOT_ENOUGH_MEMORY); }
    } else {
        SetLastError(ERROR_INVALID_PARAMETER);
    }
    if (ps->owner) LocalFree(ps->owner);
    if (ps->group) LocalFree(ps->group);
    HeapFree(GetProcessHeap(), 0, ps);
    return ok;
}

WINADVAPI BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorA(LPCSTR s, DWORD rev, PSECURITY_DESCRIPTOR *sd, PULONG n)
{
    return text_to_sd(s, rev, sd, n);
}

WINADVAPI BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorW(LPCWSTR s, DWORD rev, PSECURITY_DESCRIPTOR *sd, PULONG n)
{
    if (!s) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, 0, 0, 0, 0);
    char *a = len > 0 ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)len) : 0;
    if (!a) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    WideCharToMultiByte(CP_UTF8, 0, s, -1, a, len, 0, 0);
    BOOL ok = text_to_sd(a, rev, sd, n);
    HeapFree(GetProcessHeap(), 0, a);
    return ok;
}

/* -----------------------------------------------------------------------
 * Descriptor to text
 * ----------------------------------------------------------------------- */
typedef struct { char *b; DWORD n, cap; } Text;

static void put(Text *t, const char *s)
{
    while (*s) {
        if (t->n + 1 < t->cap) t->b[t->n] = *s;
        t->n++;
        s++;
    }
}

static void put_hex(Text *t, DWORD v)
{
    char h[11] = "0x";
    int k = 2;
    BOOL started = FALSE;
    for (int sh = 28; sh >= 0; sh -= 4) {
        int d = (int)(v >> sh & 15);
        if (d || started || !sh) { h[k++] = "0123456789abcdef"[d]; started = TRUE; }
    }
    h[k] = 0;
    put(t, h);
}

static void put_sid(Text *t, PSID sid)
{
    const char *a = sid_alias(sid);
    char s[200];
    if (a) { put(t, a); return; }
    if (sid_string(sid, s, sizeof(s))) put(t, s);
}

static void put_rights(Text *t, DWORD mask, BOOL label)
{
    if (label) {
        for (unsigned k = 0; k < sizeof(g_label_rights) / sizeof(g_label_rights[0]); k++)
            if (mask & g_label_rights[k].mask) put(t, g_label_rights[k].name);
        if (mask & ~7u) put_hex(t, mask);
        return;
    }
    for (unsigned k = 0; k < WHOLE_RIGHTS; k++) {
        if (mask == g_rights[k].mask && k != 11) { put(t, g_rights[k].name); return; }    /* (KX is KR) */
    }
    DWORD covered = 0;
    for (unsigned k = 0; k < sizeof(g_rights) / sizeof(g_rights[0]); k++)
        if (k < 4 || k >= WHOLE_RIGHTS) covered |= g_rights[k].mask & mask;
    if (covered != mask) { put_hex(t, mask); return; }
    for (unsigned k = 0; k < sizeof(g_rights) / sizeof(g_rights[0]); k++)
        if ((k < 4 || k >= WHOLE_RIGHTS) && (mask & g_rights[k].mask)) put(t, g_rights[k].name);
}

static void put_guid(Text *t, const GUID *g)
{
    char s[37];
    const BYTE *d4 = g->Data4;
    static const char hx[] = "0123456789abcdef";
    int k = 0;
    for (int sh = 28; sh >= 0; sh -= 4) s[k++] = hx[g->Data1 >> sh & 15];
    s[k++] = '-';
    for (int sh = 12; sh >= 0; sh -= 4) s[k++] = hx[g->Data2 >> sh & 15];
    s[k++] = '-';
    for (int sh = 12; sh >= 0; sh -= 4) s[k++] = hx[g->Data3 >> sh & 15];
    s[k++] = '-';
    for (int i = 0; i < 8; i++) {
        if (i == 2) s[k++] = '-';
        s[k++] = hx[d4[i] >> 4];
        s[k++] = hx[d4[i] & 15];
    }
    s[k] = 0;
    put(t, s);
}

/* An ACL's flags and ACEs; @labels_only: only mandatory label ACEs, @no_labels: all but them */
static void put_acl(Text *t, PACL acl, WORD control, int which, BOOL labels_only, BOOL no_labels)
{
    if (control & (which ? 0x2000 : 0x1000)) put(t, "P");
    if (control & (which ? 0x0200 : 0x0100)) put(t, "AR");
    if (control & (which ? 0x0800 : 0x0400)) put(t, "AI");
    if (!acl) { put(t, "NO_ACCESS_CONTROL"); return; }
    const BYTE *p = (const BYTE *)acl + 8, *end = (const BYTE *)acl + acl->AclSize;
    for (WORD i = 0; i < acl->AceCount && p + 8 <= end; i++) {
        const ACE_HEADER *h = (const ACE_HEADER *)p;
        if (h->AceSize < 8 || p + h->AceSize > end) break;
        BOOL label = h->AceType == 0x11;
        if ((labels_only && !label) || (no_labels && label)) { p += h->AceSize; continue; }
        const char *tn = 0;
        for (unsigned k = 0; k < sizeof(g_types) / sizeof(g_types[0]); k++) if (g_types[k].type == h->AceType) tn = g_types[k].name;
        if (!tn) { p += h->AceSize; continue; }
        put(t, "(");
        put(t, tn);
        put(t, ";");
        for (unsigned k = 0; k < sizeof(g_ace_flags) / sizeof(g_ace_flags[0]); k++)
            if (h->AceFlags & g_ace_flags[k].flag) put(t, g_ace_flags[k].name);
        put(t, ";");
        DWORD mask;
        memcpy(&mask, p + 4, 4);
        put_rights(t, mask, label);
        put(t, ";");
        DWORD at = 8;
        if (h->AceType >= 5 && h->AceType <= 8) {
            DWORD of;
            GUID g;
            memcpy(&of, p + at, 4); at += 4;
            if (of & 1) { memcpy(&g, p + at, 16); at += 16; put_guid(t, &g); }
            put(t, ";");
            if (of & 2) { memcpy(&g, p + at, 16); at += 16; put_guid(t, &g); }
            put(t, ";");
        } else {
            put(t, ";;");
        }
        if (at < h->AceSize && IsValidSid((PSID)(p + at))) put_sid(t, (PSID)(p + at));
        put(t, ")");
        p += h->AceSize;
    }
}

static BOOL sd_to_text(PSECURITY_DESCRIPTOR sd, SECURITY_INFORMATION si, Text *t)
{
    PSID o = 0, g = 0;
    PACL dacl = 0, sacl = 0;
    BOOL dp = FALSE, sp = FALSE, def;
    SECURITY_DESCRIPTOR_CONTROL c = 0;
    DWORD rev;
    GetSecurityDescriptorControl(sd, &c, &rev);
    GetSecurityDescriptorOwner(sd, &o, &def);
    GetSecurityDescriptorGroup(sd, &g, &def);
    GetSecurityDescriptorDacl(sd, &dp, &dacl, &def);
    GetSecurityDescriptorSacl(sd, &sp, &sacl, &def);
    if ((si & OWNER_SECURITY_INFORMATION) && o) { put(t, "O:"); put_sid(t, o); }
    if ((si & GROUP_SECURITY_INFORMATION) && g) { put(t, "G:"); put_sid(t, g); }
    if ((si & DACL_SECURITY_INFORMATION) && dp) { put(t, "D:"); put_acl(t, dacl, c, 0, FALSE, FALSE); }
    if ((si & (SACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION)) && sp) {
        put(t, "S:");
        put_acl(t, sacl, c, 1, !(si & SACL_SECURITY_INFORMATION), FALSE);
    }
    return TRUE;
}

/* The text (LocalAlloc'd, as UTF-16 if @wide) and its length in characters */
static BOOL sd_string(PSECURITY_DESCRIPTOR sd, DWORD rev, SECURITY_INFORMATION si, void **out, PULONG n, BOOL wide)
{
    if (!sd || !out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (rev != SDDL_REVISION_1) { SetLastError(ERROR_UNKNOWN_REVISION); return FALSE; }
    if (!IsValidSecurityDescriptor(sd)) { SetLastError(ERROR_INVALID_SECURITY_DESCR); return FALSE; }
    Text t = { 0, 0, 0 };
    sd_to_text(sd, si, &t);                 /* (measures) */
    t.cap = t.n + 1;
    t.b = HeapAlloc(GetProcessHeap(), 0, t.cap);
    if (!t.b) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    t.n = 0;
    sd_to_text(sd, si, &t);
    t.b[t.n] = 0;
    void *r = LocalAlloc(LMEM_FIXED, (SIZE_T)(t.n + 1) * (wide ? 2 : 1));
    if (r) {
        for (DWORD i = 0; i <= t.n; i++) {
            if (wide) ((WCHAR *)r)[i] = (WCHAR)(BYTE)t.b[i];
            else ((char *)r)[i] = t.b[i];
        }
        *out = r;
        if (n) *n = t.n + 1;
    }
    HeapFree(GetProcessHeap(), 0, t.b);
    if (!r) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    return TRUE;
}

WINADVAPI BOOL WINAPI ConvertSecurityDescriptorToStringSecurityDescriptorW(PSECURITY_DESCRIPTOR sd, DWORD rev, SECURITY_INFORMATION si,
                                                                          LPWSTR *out, PULONG n)
{
    return sd_string(sd, rev, si, (void **)out, n, TRUE);
}

WINADVAPI BOOL WINAPI ConvertSecurityDescriptorToStringSecurityDescriptorA(PSECURITY_DESCRIPTOR sd, DWORD rev, SECURITY_INFORMATION si,
                                                                          LPSTR *out, PULONG n)
{
    return sd_string(sd, rev, si, (void **)out, n, FALSE);
}
