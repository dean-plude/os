/*
 * aclapi.c — access lists from EXPLICIT_ACCESS entries: SetEntriesInAcl,
 * GetExplicitEntriesFromAcl, BuildSecurityDescriptor and the trustee
 * helpers.  The ACLs they build are the ones the kernel checks when files
 * and named objects are opened (see security.c).
 *
 * SetEntriesInAcl merges the entries into a copy of the old ACL the way
 * Windows does: GRANT_ACCESS adds rights to the trustee's allow entry,
 * SET_ACCESS replaces whatever the trustee had, DENY_ACCESS adds rights to
 * its deny entry and REVOKE_ACCESS removes its entries.  The result is in
 * canonical order: explicit denies, explicit allows, then the inherited
 * entries of the old ACL as they were.
 */

#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

#define ACE_ALLOWED   0
#define ACE_DENIED    1
#define ACE_INHERITED 0x10
#define ACE_INHERIT_FLAGS 0x0F             /* OBJECT_, CONTAINER_, NO_PROPAGATE_, INHERIT_ONLY_ */

/* OBJECTS_AND_SID / OBJECTS_AND_NAME_W (trustee forms 3 and 4) */
typedef struct { DWORD ObjectsPresent; GUID ObjectTypeGuid, InheritedObjectTypeGuid; SID *pSid; } OBJECTS_AND_SID_;
typedef struct { DWORD ObjectsPresent; SE_OBJECT_TYPE ObjectType; LPWSTR ObjectTypeName, InheritedObjectTypeName, ptstrName; } OBJECTS_AND_NAME_;

/* The SID of the calling thread's user (its token's, else the process's) */
static DWORD current_user(BYTE *out)
{
    HANDLE t;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t) && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t))
        return GetLastError();
    BYTE buf[128];
    DWORD n = 0;
    BOOL ok = GetTokenInformation(t, TokenUser, buf, sizeof(buf), &n);
    CloseHandle(t);
    if (!ok) return GetLastError();
    PSID sid = ((TOKEN_USER *)buf)->User.Sid;
    memcpy(out, sid, GetLengthSid(sid));
    return ERROR_SUCCESS;
}

/* The SID a trustee names, into @out (SECURITY_MAX_SID_SIZE bytes);
 * names are ANSI if @ansi.  Object types (forms 3 and 4) only matter for
 * directory-service objects, which NovaOS has none of: their SID or name
 * is used. */
static DWORD trustee_sid(const TRUSTEE_W *t, BOOL ansi, BYTE *out)
{
    if (!t) return ERROR_INVALID_PARAMETER;
    PSID sid = 0;
    const void *name = 0;
    switch (t->TrusteeForm) {
    case TRUSTEE_IS_SID: sid = (PSID)t->ptstrName; break;
    case TRUSTEE_IS_NAME: name = t->ptstrName; break;
    case TRUSTEE_IS_OBJECTS_AND_SID: if (t->ptstrName) sid = ((const OBJECTS_AND_SID_ *)t->ptstrName)->pSid; break;
    case TRUSTEE_IS_OBJECTS_AND_NAME: if (t->ptstrName) name = ((const OBJECTS_AND_NAME_ *)t->ptstrName)->ptstrName; break;
    default: return ERROR_INVALID_PARAMETER;
    }
    if (name) {
        WCHAR w[256];
        if (ansi) { if (!MultiByteToWideChar(CP_ACP, 0, name, -1, w, 256)) return ERROR_INVALID_PARAMETER; }
        else {
            int i = 0;
            for (const WCHAR *s = name; *s && i < 255; s++) w[i++] = *s;
            w[i] = 0;
        }
        static const WCHAR cu[] = L"CURRENT_USER";
        int same = 1;
        for (int i = 0; same && (i == 0 || cu[i - 1]); i++) same = w[i] == cu[i];
        if (same) return current_user(out);
        DWORD ns = SECURITY_MAX_SID_SIZE, nd = 64;
        WCHAR dom[64];
        SID_NAME_USE use;
        if (!LookupAccountNameW(0, w, out, &ns, dom, &nd, &use)) return GetLastError();
        return ERROR_SUCCESS;
    }
    if (!sid || !IsValidSid(sid)) return ERROR_INVALID_SID;
    memcpy(out, sid, GetLengthSid(sid));
    return ERROR_SUCCESS;
}

/* An ACE being built: allow or deny ones are taken apart, others kept as
 * they are (raw) */
typedef struct {
    BYTE  type, flags;
    DWORD mask;
    BYTE  sid[SECURITY_MAX_SID_SIZE];
    const BYTE *raw;                        /* (other types: the ACE in the old ACL) */
} Ace;

static DWORD ace_size(const Ace *a) { return a->raw ? ((const ACE_HEADER *)a->raw)->AceSize : 8 + GetLengthSid((PSID)a->sid); }

/* Remove @sid's explicit allow and deny entries */
static void drop_trustee(Ace *list, DWORD *n, PSID sid)
{
    DWORD k = 0;
    for (DWORD i = 0; i < *n; i++)
        if (list[i].raw || !EqualSid((PSID)list[i].sid, sid)) list[k++] = list[i];
    *n = k;
}

/* Add @mask to @sid's entry of @type with @flags, or a new one */
static void merge(Ace *list, DWORD *n, BYTE type, BYTE flags, DWORD mask, PSID sid)
{
    if (!mask) return;
    for (DWORD i = 0; i < *n; i++) {
        Ace *a = &list[i];
        if (!a->raw && a->type == type && a->flags == flags && EqualSid((PSID)a->sid, sid)) { a->mask |= mask; return; }
    }
    Ace *a = &list[(*n)++];
    memset(a, 0, sizeof(*a));
    a->type = type;
    a->flags = flags;
    a->mask = mask;
    memcpy(a->sid, sid, GetLengthSid(sid));
}

static DWORD entries_in_acl(ULONG count, const EXPLICIT_ACCESS_W *entries, BOOL ansi, PACL old, PACL *out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = 0;
    if (count && !entries) return ERROR_INVALID_PARAMETER;
    if (!count && !old) return ERROR_SUCCESS;                   /* (no ACL at all, as on Windows) */
    if (old && !IsValidAcl(old)) return ERROR_INVALID_ACL;
    DWORD nold = old ? old->AceCount : 0, cap = nold + count + 1;
    Ace *expl = HeapAlloc(GetProcessHeap(), 0, 2 * cap * sizeof(Ace)), *inh = expl + cap;
    if (!expl) return ERROR_NOT_ENOUGH_MEMORY;
    DWORD ne = 0, ni = 0, e = ERROR_SUCCESS;
    BYTE rev = ACL_REVISION;

    /* the old ACL: explicit entries to merge into, inherited ones kept */
    const BYTE *p = old ? (const BYTE *)old + 8 : 0, *end = old ? (const BYTE *)old + old->AclSize : 0;
    for (DWORD i = 0; i < nold && p + 8 <= end; i++) {
        const ACE_HEADER *h = (const ACE_HEADER *)p;
        if (h->AceSize < 8 || p + h->AceSize > end) break;
        Ace *a = (h->AceFlags & ACE_INHERITED) ? &inh[ni++] : &expl[ne++];
        memset(a, 0, sizeof(*a));
        if (h->AceType <= ACE_DENIED && h->AceSize >= 16 && IsValidSid((PSID)(p + 8))) {
            a->type = h->AceType;
            a->flags = h->AceFlags;
            a->mask = ((const ACCESS_ALLOWED_ACE *)p)->Mask;
            memcpy(a->sid, p + 8, GetLengthSid((PSID)(p + 8)));
        } else {
            a->raw = p;
            if (old->AclRevision > rev) rev = old->AclRevision;
        }
        p += h->AceSize;
    }

    for (ULONG i = 0; i < count && !e; i++) {
        const EXPLICIT_ACCESS_W *ea = &entries[i];
        BYTE sid[SECURITY_MAX_SID_SIZE];
        if (ea->grfAccessMode == NOT_USED_ACCESS) continue;
        if (ea->grfAccessMode == SET_AUDIT_SUCCESS || ea->grfAccessMode == SET_AUDIT_FAILURE) continue;   /* (SACL entries) */
        if (ea->grfAccessMode > SET_AUDIT_FAILURE) { e = ERROR_INVALID_PARAMETER; break; }
        if ((e = trustee_sid(&ea->Trustee, ansi, sid))) break;
        BYTE flags = (BYTE)(ea->grfInheritance & ACE_INHERIT_FLAGS);
        switch (ea->grfAccessMode) {
        case GRANT_ACCESS: merge(expl, &ne, ACE_ALLOWED, flags, ea->grfAccessPermissions, sid); break;
        case SET_ACCESS:
            drop_trustee(expl, &ne, sid);
            merge(expl, &ne, ACE_ALLOWED, flags, ea->grfAccessPermissions, sid);
            break;
        case DENY_ACCESS: merge(expl, &ne, ACE_DENIED, flags, ea->grfAccessPermissions, sid); break;
        case REVOKE_ACCESS: drop_trustee(expl, &ne, sid); break;
        default: break;
        }
    }

    if (!e) {
        DWORD size = 8;
        for (DWORD i = 0; i < ne; i++) size += ace_size(&expl[i]);
        for (DWORD i = 0; i < ni; i++) size += ace_size(&inh[i]);
        PACL acl = size <= 0xFFFF ? LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, size) : 0;
        if (!acl) e = size > 0xFFFF ? ERROR_INVALID_PARAMETER : ERROR_NOT_ENOUGH_MEMORY;
        else {
            InitializeAcl(acl, size, rev);
            BYTE *o = (BYTE *)acl + 8;
            /* explicit denies, explicit allows, other explicit entries, inherited ones */
            for (int pass = 0; pass < 4; pass++) {
                Ace *list = pass < 3 ? expl : inh;
                DWORD n = pass < 3 ? ne : ni;
                for (DWORD i = 0; i < n; i++) {
                    const Ace *a = &list[i];
                    if (pass == 0 && (a->raw || a->type != ACE_DENIED)) continue;
                    if (pass == 1 && (a->raw || a->type != ACE_ALLOWED)) continue;
                    if (pass == 2 && !a->raw) continue;
                    DWORD len = ace_size(a);
                    if (a->raw) memcpy(o, a->raw, len);
                    else {
                        ACE_HEADER *h = (ACE_HEADER *)o;
                        h->AceType = a->type;
                        h->AceFlags = a->flags;
                        h->AceSize = (WORD)len;
                        memcpy(o + 4, &a->mask, 4);
                        memcpy(o + 8, a->sid, len - 8);
                    }
                    o += len;
                    acl->AceCount++;
                }
            }
            *out = acl;
        }
    }
    HeapFree(GetProcessHeap(), 0, expl);
    return e;
}

WINADVAPI DWORD WINAPI SetEntriesInAclW(ULONG n, PEXPLICIT_ACCESS_W entries, PACL old, PACL *out)
{
    return entries_in_acl(n, entries, FALSE, old, out);
}

WINADVAPI DWORD WINAPI SetEntriesInAclA(ULONG n, PEXPLICIT_ACCESS_A entries, PACL old, PACL *out)
{
    return entries_in_acl(n, (const EXPLICIT_ACCESS_W *)entries, TRUE, old, out);    /* (the same layout) */
}

/* The explicit allow and deny entries of @acl as EXPLICIT_ACCESS entries
 * naming SIDs, in one LocalAlloc'd block */
static DWORD explicit_entries(PACL acl, PULONG n, PEXPLICIT_ACCESS_W *out)
{
    if (!n || !out) return ERROR_INVALID_PARAMETER;
    *n = 0;
    *out = 0;
    if (!acl) return ERROR_SUCCESS;
    if (!IsValidAcl(acl)) return ERROR_INVALID_ACL;
    DWORD count = 0, sids = 0;
    const BYTE *p = (const BYTE *)acl + 8, *end = (const BYTE *)acl + acl->AclSize;
    for (DWORD i = 0; i < acl->AceCount && p + 8 <= end; i++) {
        const ACE_HEADER *h = (const ACE_HEADER *)p;
        if (h->AceSize < 8 || p + h->AceSize > end) break;
        if (h->AceType <= ACE_DENIED && !(h->AceFlags & ACE_INHERITED) && h->AceSize >= 16) { count++; sids += h->AceSize - 8; }
        p += h->AceSize;
    }
    if (!count) return ERROR_SUCCESS;
    EXPLICIT_ACCESS_W *ea = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, count * sizeof(*ea) + sids);
    if (!ea) return ERROR_NOT_ENOUGH_MEMORY;
    BYTE *tail = (BYTE *)(ea + count);
    DWORD k = 0;
    p = (const BYTE *)acl + 8;
    for (DWORD i = 0; i < acl->AceCount && k < count; i++) {
        const ACE_HEADER *h = (const ACE_HEADER *)p;
        if (h->AceType <= ACE_DENIED && !(h->AceFlags & ACE_INHERITED) && h->AceSize >= 16) {
            memcpy(tail, p + 8, h->AceSize - 8u);
            ea[k].grfAccessPermissions = ((const ACCESS_ALLOWED_ACE *)p)->Mask;
            ea[k].grfAccessMode = h->AceType == ACE_DENIED ? DENY_ACCESS : GRANT_ACCESS;
            ea[k].grfInheritance = h->AceFlags & ACE_INHERIT_FLAGS;
            BuildTrusteeWithSidW(&ea[k].Trustee, tail);
            tail += h->AceSize - 8u;
            k++;
        }
        p += h->AceSize;
    }
    *n = count;
    *out = ea;
    return ERROR_SUCCESS;
}

WINADVAPI DWORD WINAPI GetExplicitEntriesFromAclW(PACL acl, PULONG n, PEXPLICIT_ACCESS_W *entries)
{
    return explicit_entries(acl, n, entries);
}

WINADVAPI DWORD WINAPI GetExplicitEntriesFromAclA(PACL acl, PULONG n, PEXPLICIT_ACCESS_A *entries)
{
    return explicit_entries(acl, n, (PEXPLICIT_ACCESS_W *)entries);    /* (trustees are SIDs: no names to convert) */
}

/* -----------------------------------------------------------------------
 * Trustees
 * ----------------------------------------------------------------------- */
WINADVAPI void WINAPI BuildTrusteeWithSidW(PTRUSTEE_W t, PSID sid)
{
    if (!t) return;
    t->pMultipleTrustee = 0;
    t->MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
    t->TrusteeForm = TRUSTEE_IS_SID;
    t->TrusteeType = TRUSTEE_IS_UNKNOWN;
    t->ptstrName = (LPWSTR)sid;
}
WINADVAPI void WINAPI BuildTrusteeWithSidA(PTRUSTEE_A t, PSID sid) { BuildTrusteeWithSidW((PTRUSTEE_W)t, sid); }

WINADVAPI void WINAPI BuildTrusteeWithNameW(PTRUSTEE_W t, LPWSTR name)
{
    if (!t) return;
    t->pMultipleTrustee = 0;
    t->MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
    t->TrusteeForm = TRUSTEE_IS_NAME;
    t->TrusteeType = TRUSTEE_IS_UNKNOWN;
    t->ptstrName = name;
}
WINADVAPI void WINAPI BuildTrusteeWithNameA(PTRUSTEE_A t, LPSTR name) { BuildTrusteeWithNameW((PTRUSTEE_W)t, (LPWSTR)name); }

WINADVAPI void WINAPI BuildExplicitAccessWithNameW(PEXPLICIT_ACCESS_W ea, LPWSTR name, DWORD perms, ACCESS_MODE mode, DWORD inherit)
{
    if (!ea) return;
    ea->grfAccessPermissions = perms;
    ea->grfAccessMode = mode;
    ea->grfInheritance = inherit;
    BuildTrusteeWithNameW(&ea->Trustee, name);
}
WINADVAPI void WINAPI BuildExplicitAccessWithNameA(PEXPLICIT_ACCESS_A ea, LPSTR name, DWORD perms, ACCESS_MODE mode, DWORD inherit)
{
    BuildExplicitAccessWithNameW((PEXPLICIT_ACCESS_W)ea, (LPWSTR)name, perms, mode, inherit);
}

WINADVAPI TRUSTEE_FORM WINAPI GetTrusteeFormW(PTRUSTEE_W t) { return t ? t->TrusteeForm : TRUSTEE_BAD_FORM; }
WINADVAPI TRUSTEE_FORM WINAPI GetTrusteeFormA(PTRUSTEE_A t) { return t ? t->TrusteeForm : TRUSTEE_BAD_FORM; }
WINADVAPI TRUSTEE_TYPE WINAPI GetTrusteeTypeW(PTRUSTEE_W t) { return t ? t->TrusteeType : TRUSTEE_IS_UNKNOWN; }
WINADVAPI TRUSTEE_TYPE WINAPI GetTrusteeTypeA(PTRUSTEE_A t) { return t ? t->TrusteeType : TRUSTEE_IS_UNKNOWN; }
WINADVAPI LPWSTR WINAPI GetTrusteeNameW(PTRUSTEE_W t) { return t ? t->ptstrName : 0; }
WINADVAPI LPSTR WINAPI GetTrusteeNameA(PTRUSTEE_A t) { return t ? t->ptstrName : 0; }

/* -----------------------------------------------------------------------
 * BuildSecurityDescriptor: @old's parts with the trustees and entries
 * given, as a self-relative descriptor (LocalFree'd by the caller).  A
 * SACL is not kept (NovaOS keeps none).
 * ----------------------------------------------------------------------- */
static DWORD build_sd(const TRUSTEE_W *owner, const TRUSTEE_W *group, ULONG n, const EXPLICIT_ACCESS_W *access, BOOL ansi,
                      PSECURITY_DESCRIPTOR old, PULONG size, PSECURITY_DESCRIPTOR *out)
{
    if (!out || !size) return ERROR_INVALID_PARAMETER;
    *out = 0;
    BYTE osid[SECURITY_MAX_SID_SIZE], gsid[SECURITY_MAX_SID_SIZE];
    PSID o = 0, g = 0;
    PACL olddacl = 0, dacl = 0;
    BOOL present = FALSE, def;
    if (old) {
        if (!IsValidSecurityDescriptor(old)) return ERROR_INVALID_SECURITY_DESCR;
        GetSecurityDescriptorOwner(old, &o, &def);
        GetSecurityDescriptorGroup(old, &g, &def);
        GetSecurityDescriptorDacl(old, &present, &olddacl, &def);
    }
    DWORD e;
    if (owner) { if ((e = trustee_sid(owner, ansi, osid))) return e; o = osid; }
    if (group) { if ((e = trustee_sid(group, ansi, gsid))) return e; g = gsid; }
    if (n || olddacl) {
        if ((e = entries_in_acl(n, access, ansi, olddacl, &dacl))) return e;
        present = TRUE;
    }
    SECURITY_DESCRIPTOR abs;
    InitializeSecurityDescriptor(&abs, SECURITY_DESCRIPTOR_REVISION);
    if (o) SetSecurityDescriptorOwner(&abs, o, FALSE);
    if (g) SetSecurityDescriptorGroup(&abs, g, FALSE);
    if (present) SetSecurityDescriptorDacl(&abs, TRUE, dacl, FALSE);
    DWORD len = 0;
    MakeSelfRelativeSD(&abs, 0, &len);
    PSECURITY_DESCRIPTOR sd = len ? LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, len) : 0;
    e = !sd ? ERROR_NOT_ENOUGH_MEMORY : MakeSelfRelativeSD(&abs, sd, &len) ? ERROR_SUCCESS : GetLastError();
    if (dacl) LocalFree(dacl);
    if (e) { if (sd) LocalFree(sd); return e; }
    *size = len;
    *out = sd;
    return ERROR_SUCCESS;
}

WINADVAPI DWORD WINAPI BuildSecurityDescriptorW(PTRUSTEE_W owner, PTRUSTEE_W group, ULONG n, PEXPLICIT_ACCESS_W access,
                                                ULONG naudit, PEXPLICIT_ACCESS_W audit, PSECURITY_DESCRIPTOR old, PULONG size,
                                                PSECURITY_DESCRIPTOR *out)
{
    (void)naudit; (void)audit;
    return build_sd(owner, group, n, access, FALSE, old, size, out);
}

WINADVAPI DWORD WINAPI BuildSecurityDescriptorA(PTRUSTEE_A owner, PTRUSTEE_A group, ULONG n, PEXPLICIT_ACCESS_A access,
                                                ULONG naudit, PEXPLICIT_ACCESS_A audit, PSECURITY_DESCRIPTOR old, PULONG size,
                                                PSECURITY_DESCRIPTOR *out)
{
    (void)naudit; (void)audit;
    return build_sd((const TRUSTEE_W *)owner, (const TRUSTEE_W *)group, n, (const EXPLICIT_ACCESS_W *)access, TRUE,
                    old, size, out);
}
