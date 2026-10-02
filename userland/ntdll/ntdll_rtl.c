/*
 * ntdll_rtl.c — the rest of the native API that needs no kernel of its own
 *
 * Tokens, SIDs, ACLs and security descriptors; counted strings; the
 * current directory; registry queries; system information; and the
 * native calls NovaOS answers without a kernel object (transactions,
 * jobs, quotas, file locks...).  Programs that talk to ntdll directly,
 * such as the Cygwin/MSYS2 runtime, use these.
 *
 * Security follows advapi32's model (security.c): one user, who has full
 * access to everything.  A token is a handle to an event, so it can be
 * closed, duplicated and inherited like any handle; what a token says is
 * always that user (S-1-5-21-1000-2000-3000-1001, in Users and
 * Administrators, not elevated).
 */
#define NOVA_BUILD_NTDLL
#include <winternl.h>

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs);
PVOID NTAPI RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T n);
BOOLEAN NTAPI RtlFreeHeap(PVOID heap, ULONG flags, PVOID p);
PVOID NTAPI RtlGetProcessHeap(void);
VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING s, PCWSTR w);

#define ST_SUCCESS               ((NTSTATUS)0x00000000)
#define ST_BUFFER_TOO_SMALL      ((NTSTATUS)0xC0000023)
#define ST_INVALID_PARAMETER     ((NTSTATUS)0xC000000D)
#define ST_ACCESS_DENIED         ((NTSTATUS)0xC0000022)
#define ST_INVALID_INFO_CLASS    ((NTSTATUS)0xC0000003)
#define ST_INFO_LENGTH_MISMATCH  ((NTSTATUS)0xC0000004)
#define ST_NO_TOKEN              ((NTSTATUS)0xC000007C)
#define ST_NOT_SUPPORTED         ((NTSTATUS)0xC00000BB)
#define ST_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010)
#define ST_NO_MEMORY             ((NTSTATUS)0xC0000017)
#define ST_INVALID_SID           ((NTSTATUS)0xC0000078)
#define ST_INVALID_ACL           ((NTSTATUS)0xC0000077)
#define ST_INVALID_SECURITY_DESCR ((NTSTATUS)0xC0000079)
#define ST_ALLOTTED_SPACE_EXCEEDED ((NTSTATUS)0xC0000099)
#define ST_NO_MORE_ENTRIES       ((NTSTATUS)0x8000001A)
#define ST_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define ST_NOT_IMPLEMENTED       ((NTSTATUS)0xC0000002)
#define ST_BUFFER_OVERFLOW       ((NTSTATUS)0x80000005)

typedef struct { BYTE Revision, Sbz1; WORD Control; DWORD Owner, Group, Sacl, Dacl; } SD_RELATIVE;
#define SE_OWNER_DEFAULTED 0x0001
#define SE_GROUP_DEFAULTED 0x0002
#define SE_DACL_DEFAULTED  0x0008
#define SE_SACL_PRESENT    0x0010

typedef BOOLEAN *PBOOLEAN;
typedef NTSTATUS *PNTSTATUS;

static PVOID heap(void) { return RtlGetProcessHeap(); }

/* -----------------------------------------------------------------------
 * The one user's SIDs (as advapi32's security.c)
 * ----------------------------------------------------------------------- */
static const BYTE g_user_sid[] = {                      /* S-1-5-21-1000-2000-3000-1001 */
    1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0xE8, 3, 0, 0, 0xD0, 7, 0, 0, 0xB8, 0x0B, 0, 0, 0xE9, 3, 0, 0 };
static const BYTE g_users_sid[] = { 1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x21, 2, 0, 0 };     /* S-1-5-32-545 */
static const BYTE g_admins_sid[] = { 1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 2, 0, 0 };    /* S-1-5-32-544 */
static const BYTE g_everyone_sid[] = { 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 };                    /* S-1-1-0 */
static const BYTE g_auth_users_sid[] = { 1, 1, 0, 0, 0, 0, 0, 5, 11, 0, 0, 0 };                 /* S-1-5-11 */
static const BYTE g_interactive_sid[] = { 1, 1, 0, 0, 0, 0, 0, 5, 4, 0, 0, 0 };                 /* S-1-5-4 */
static const BYTE g_logon_sid[] = { 1, 3, 0, 0, 0, 0, 0, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0x2A, 0, 0, 0 };  /* S-1-5-5-0-42 */
static const BYTE g_medium_il_sid[] = { 1, 1, 0, 0, 0, 0, 0, 16, 0, 0x20, 0, 0 };               /* S-1-16-8192 */

/* -----------------------------------------------------------------------
 * SIDs
 * ----------------------------------------------------------------------- */
NTSYSAPI BOOLEAN NTAPI RtlValidSid(PSID sid)
{
    const SID *s = sid;
    return s && s->Revision == 1 && s->SubAuthorityCount <= 15;
}
NTSYSAPI ULONG NTAPI RtlLengthSid(PSID sid) { return RtlValidSid(sid) ? 8 + 4u * ((SID *)sid)->SubAuthorityCount : 0; }
NTSYSAPI ULONG NTAPI RtlLengthRequiredSid(ULONG n) { return 8 + 4 * n; }

NTSYSAPI NTSTATUS NTAPI RtlCopySid(ULONG len, PSID dst, PSID src)
{
    ULONG n = RtlLengthSid(src);
    if (!n) return ST_INVALID_SID;
    if (len < n) return ST_BUFFER_TOO_SMALL;
    memcpy(dst, src, n);
    return ST_SUCCESS;
}

NTSYSAPI BOOLEAN NTAPI RtlEqualSid(PSID a, PSID b)
{
    ULONG n = RtlLengthSid(a);
    return n && n == RtlLengthSid(b) && !memcmp(a, b, n);
}

NTSYSAPI BOOLEAN NTAPI RtlEqualPrefixSid(PSID a, PSID b)
{
    const SID *x = a, *y = b;
    if (!RtlValidSid(a) || !RtlValidSid(b) || x->SubAuthorityCount != y->SubAuthorityCount) return FALSE;
    if (!x->SubAuthorityCount) return !memcmp(a, b, 8);
    return !memcmp(a, b, 8 + 4u * (x->SubAuthorityCount - 1u));
}

NTSYSAPI NTSTATUS NTAPI RtlInitializeSid(PSID sid, PSID_IDENTIFIER_AUTHORITY auth, UCHAR n)
{
    SID *s = sid;
    if (n > 15) return ST_INVALID_PARAMETER;
    s->Revision = 1;
    s->SubAuthorityCount = n;
    s->IdentifierAuthority = *auth;
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlAllocateAndInitializeSid(PSID_IDENTIFIER_AUTHORITY auth, UCHAR n, ULONG s0, ULONG s1, ULONG s2,
                                                    ULONG s3, ULONG s4, ULONG s5, ULONG s6, ULONG s7, PSID *out)
{
    if (n > 8) return ST_INVALID_SID;
    SID *s = RtlAllocateHeap(heap(), 8 /* HEAP_ZERO_MEMORY */, 8 + 4u * n);
    if (!s) return ST_NO_MEMORY;
    RtlInitializeSid(s, auth, n);
    ULONG v[8] = { s0, s1, s2, s3, s4, s5, s6, s7 };
    for (int i = 0; i < n; i++) s->SubAuthority[i] = v[i];
    *out = s;
    return ST_SUCCESS;
}
NTSYSAPI PVOID NTAPI RtlFreeSid(PSID sid) { RtlFreeHeap(heap(), 0, sid); return 0; }

NTSYSAPI PSID_IDENTIFIER_AUTHORITY NTAPI RtlIdentifierAuthoritySid(PSID sid) { return &((SID *)sid)->IdentifierAuthority; }
NTSYSAPI PULONG NTAPI RtlSubAuthoritySid(PSID sid, ULONG i) { return (PULONG)&((SID *)sid)->SubAuthority[i]; }
NTSYSAPI PUCHAR NTAPI RtlSubAuthorityCountSid(PSID sid) { return &((SID *)sid)->SubAuthorityCount; }

/* "S-1-5-21-..." as a UNICODE_STRING (allocated if @alloc) */
NTSYSAPI NTSTATUS NTAPI RtlConvertSidToUnicodeString(PUNICODE_STRING out, PSID sid, BOOLEAN alloc)
{
    const SID *s = sid;
    if (!RtlValidSid(sid)) return ST_INVALID_SID;
    WCHAR buf[200];
    int o = 0;
    ULONGLONG auth = 0;
    for (int i = 0; i < 6; i++) auth = auth << 8 | s->IdentifierAuthority.Value[i];
    buf[o++] = 'S'; buf[o++] = '-'; buf[o++] = '1';
    for (int i = -1; i < s->SubAuthorityCount; i++) {
        ULONGLONG v = i < 0 ? auth : s->SubAuthority[i];
        WCHAR t[24];
        int k = 0;
        do { t[k++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
        buf[o++] = '-';
        while (k) buf[o++] = t[--k];
    }
    buf[o] = 0;
    USHORT bytes = (USHORT)(2 * o);
    if (alloc) {
        out->Buffer = RtlAllocateHeap(heap(), 0, bytes + 2u);
        if (!out->Buffer) return ST_NO_MEMORY;
        out->MaximumLength = (USHORT)(bytes + 2);
    } else if (out->MaximumLength < bytes) return ST_BUFFER_OVERFLOW;
    memcpy(out->Buffer, buf, bytes);
    if (out->MaximumLength > bytes) out->Buffer[o] = 0;
    out->Length = bytes;
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * ACLs
 * ----------------------------------------------------------------------- */
NTSYSAPI NTSTATUS NTAPI RtlCreateAcl(PACL acl, ULONG len, ULONG rev)
{
    if (len < sizeof(ACL) || len > 0xFFFF || rev < 2 || rev > 4) return ST_INVALID_PARAMETER;
    memset(acl, 0, sizeof(ACL));
    acl->AclRevision = (BYTE)rev;
    acl->AclSize = (WORD)(len & ~3u);
    return ST_SUCCESS;
}

NTSYSAPI BOOLEAN NTAPI RtlValidAcl(PACL acl) { return acl && acl->AclRevision >= 2 && acl->AclRevision <= 4 && acl->AclSize >= sizeof(ACL); }

/* The free space after the last ACE (NULL if the ACL is corrupt) */
static BYTE *acl_end(PACL acl)
{
    BYTE *p = (BYTE *)(acl + 1), *end = (BYTE *)acl + acl->AclSize;
    for (int i = 0; i < acl->AceCount; i++) {
        ACE_HEADER *h = (ACE_HEADER *)p;
        if (p + sizeof(ACE_HEADER) > end || h->AceSize < sizeof(ACE_HEADER) || p + h->AceSize > end) return 0;
        p += h->AceSize;
    }
    return p;
}

NTSYSAPI BOOLEAN NTAPI RtlFirstFreeAce(PACL acl, PVOID *ace)
{
    *ace = 0;
    if (!RtlValidAcl(acl)) return FALSE;
    BYTE *p = acl_end(acl);
    if (!p) return FALSE;
    if (p < (BYTE *)acl + acl->AclSize) *ace = p;
    return TRUE;
}

NTSYSAPI NTSTATUS NTAPI RtlGetAce(PACL acl, ULONG index, PVOID *ace)
{
    if (!RtlValidAcl(acl) || index >= acl->AceCount) return ST_INVALID_PARAMETER;
    BYTE *p = (BYTE *)(acl + 1);
    for (ULONG i = 0; i < index; i++) p += ((ACE_HEADER *)p)->AceSize;
    *ace = p;
    return ST_SUCCESS;
}

static NTSTATUS add_ace(PACL acl, ULONG rev, BYTE type, ULONG flags, ACCESS_MASK mask, PSID sid)
{
    if (!RtlValidAcl(acl) || !RtlValidSid(sid)) return !RtlValidSid(sid) ? ST_INVALID_SID : ST_INVALID_ACL;
    BYTE *p = acl_end(acl);
    if (!p) return ST_INVALID_ACL;
    ULONG slen = RtlLengthSid(sid), size = 8 + slen;
    if (p + size > (BYTE *)acl + acl->AclSize) return ST_ALLOTTED_SPACE_EXCEEDED;
    ACCESS_ALLOWED_ACE *a = (ACCESS_ALLOWED_ACE *)p;
    a->Header.AceType = type;
    a->Header.AceFlags = (BYTE)flags;
    a->Header.AceSize = (WORD)size;
    a->Mask = mask;
    memcpy(&a->SidStart, sid, slen);
    acl->AceCount++;
    if (rev > acl->AclRevision) acl->AclRevision = (BYTE)rev;
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlAddAccessAllowedAce(PACL acl, ULONG rev, ACCESS_MASK mask, PSID sid) { return add_ace(acl, rev, 0, 0, mask, sid); }
NTSYSAPI NTSTATUS NTAPI RtlAddAccessAllowedAceEx(PACL acl, ULONG rev, ULONG flags, ACCESS_MASK mask, PSID sid) { return add_ace(acl, rev, 0, flags, mask, sid); }
NTSYSAPI NTSTATUS NTAPI RtlAddAccessDeniedAce(PACL acl, ULONG rev, ACCESS_MASK mask, PSID sid) { return add_ace(acl, rev, 1, 0, mask, sid); }
NTSYSAPI NTSTATUS NTAPI RtlAddAccessDeniedAceEx(PACL acl, ULONG rev, ULONG flags, ACCESS_MASK mask, PSID sid) { return add_ace(acl, rev, 1, flags, mask, sid); }

NTSYSAPI NTSTATUS NTAPI RtlDeleteAce(PACL acl, ULONG index)
{
    PVOID ace;
    NTSTATUS s = RtlGetAce(acl, index, &ace);
    if (s) return s;
    BYTE *p = ace, *end = acl_end(acl);
    WORD n = ((ACE_HEADER *)p)->AceSize;
    memmove(p, p + n, (SIZE_T)(end - p - n));
    acl->AceCount--;
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Security descriptors: absolute (pointers) or self-relative (offsets)
 * ----------------------------------------------------------------------- */
NTSYSAPI NTSTATUS NTAPI RtlCreateSecurityDescriptor(PSECURITY_DESCRIPTOR sd, ULONG rev)
{
    if (rev != SECURITY_DESCRIPTOR_REVISION) return ST_INVALID_PARAMETER;
    memset(sd, 0, sizeof(SECURITY_DESCRIPTOR));
    ((SECURITY_DESCRIPTOR *)sd)->Revision = 1;
    return ST_SUCCESS;
}

NTSYSAPI BOOLEAN NTAPI RtlValidSecurityDescriptor(PSECURITY_DESCRIPTOR sd) { return sd && ((SECURITY_DESCRIPTOR *)sd)->Revision == 1; }

static BOOL self_relative(PSECURITY_DESCRIPTOR sd) { return ((SECURITY_DESCRIPTOR *)sd)->Control & SE_SELF_RELATIVE; }

/* A member (0 owner, 1 group, 2 SACL, 3 DACL) of either form */
static PVOID sd_part(PSECURITY_DESCRIPTOR sd, int which)
{
    if (self_relative(sd)) {
        SD_RELATIVE *r = sd;
        DWORD off = which == 0 ? r->Owner : which == 1 ? r->Group : which == 2 ? r->Sacl : r->Dacl;
        return off ? (BYTE *)sd + off : 0;
    }
    SECURITY_DESCRIPTOR *a = sd;
    return which == 0 ? a->Owner : which == 1 ? a->Group : which == 2 ? (PVOID)a->Sacl : (PVOID)a->Dacl;
}

NTSYSAPI NTSTATUS NTAPI RtlGetControlSecurityDescriptor(PSECURITY_DESCRIPTOR sd, PSECURITY_DESCRIPTOR_CONTROL control, PULONG rev)
{
    if (!RtlValidSecurityDescriptor(sd)) return ST_INVALID_SECURITY_DESCR;
    *control = ((SECURITY_DESCRIPTOR *)sd)->Control;
    *rev = ((SECURITY_DESCRIPTOR *)sd)->Revision;
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlSetControlSecurityDescriptor(PSECURITY_DESCRIPTOR sd, SECURITY_DESCRIPTOR_CONTROL mask,
                                                        SECURITY_DESCRIPTOR_CONTROL bits)
{
    SECURITY_DESCRIPTOR *s = sd;
    s->Control = (WORD)((s->Control & ~mask) | (bits & mask));
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlGetDaclSecurityDescriptor(PSECURITY_DESCRIPTOR sd, PBOOLEAN present, PACL *dacl, PBOOLEAN defaulted)
{
    if (!RtlValidSecurityDescriptor(sd)) return ST_INVALID_SECURITY_DESCR;
    WORD c = ((SECURITY_DESCRIPTOR *)sd)->Control;
    *present = (c & SE_DACL_PRESENT) != 0;
    if (*present) {
        *dacl = sd_part(sd, 3);
        if (defaulted) *defaulted = (c & SE_DACL_DEFAULTED) != 0;
    }
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlSetDaclSecurityDescriptor(PSECURITY_DESCRIPTOR sd, BOOLEAN present, PACL dacl, BOOLEAN defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (self_relative(sd)) return ST_INVALID_SECURITY_DESCR;
    s->Control &= (WORD)~(SE_DACL_PRESENT | SE_DACL_DEFAULTED);
    s->Dacl = 0;
    if (present) {
        s->Control |= SE_DACL_PRESENT | (defaulted ? SE_DACL_DEFAULTED : 0);
        s->Dacl = dacl;
    }
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlGetSaclSecurityDescriptor(PSECURITY_DESCRIPTOR sd, PBOOLEAN present, PACL *sacl, PBOOLEAN defaulted)
{
    WORD c = ((SECURITY_DESCRIPTOR *)sd)->Control;
    *present = (c & SE_SACL_PRESENT) != 0;
    if (*present) { *sacl = sd_part(sd, 2); if (defaulted) *defaulted = FALSE; }
    return ST_SUCCESS;
}

static NTSTATUS get_sid_part(PSECURITY_DESCRIPTOR sd, int which, PSID *sid, PBOOLEAN defaulted)
{
    if (!RtlValidSecurityDescriptor(sd)) return ST_INVALID_SECURITY_DESCR;
    *sid = sd_part(sd, which);
    if (defaulted) *defaulted = (((SECURITY_DESCRIPTOR *)sd)->Control & (which ? SE_GROUP_DEFAULTED : SE_OWNER_DEFAULTED)) != 0;
    return ST_SUCCESS;
}
NTSYSAPI NTSTATUS NTAPI RtlGetOwnerSecurityDescriptor(PSECURITY_DESCRIPTOR sd, PSID *sid, PBOOLEAN d) { return get_sid_part(sd, 0, sid, d); }
NTSYSAPI NTSTATUS NTAPI RtlGetGroupSecurityDescriptor(PSECURITY_DESCRIPTOR sd, PSID *sid, PBOOLEAN d) { return get_sid_part(sd, 1, sid, d); }

static NTSTATUS set_sid_part(PSECURITY_DESCRIPTOR sd, int which, PSID sid, BOOLEAN defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (self_relative(sd)) return ST_INVALID_SECURITY_DESCR;
    WORD bit = which ? SE_GROUP_DEFAULTED : SE_OWNER_DEFAULTED;
    if (which) s->Group = sid; else s->Owner = sid;
    s->Control = (WORD)((s->Control & ~bit) | (defaulted ? bit : 0));
    return ST_SUCCESS;
}
NTSYSAPI NTSTATUS NTAPI RtlSetOwnerSecurityDescriptor(PSECURITY_DESCRIPTOR sd, PSID sid, BOOLEAN d) { return set_sid_part(sd, 0, sid, d); }
NTSYSAPI NTSTATUS NTAPI RtlSetGroupSecurityDescriptor(PSECURITY_DESCRIPTOR sd, PSID sid, BOOLEAN d) { return set_sid_part(sd, 1, sid, d); }

NTSYSAPI ULONG NTAPI RtlLengthSecurityDescriptor(PSECURITY_DESCRIPTOR sd)
{
    ULONG n = sizeof(SD_RELATIVE);
    for (int i = 0; i < 4; i++) {
        PVOID p = sd_part(sd, i);
        if (!p) continue;
        n += i < 2 ? RtlLengthSid(p) : ((ACL *)p)->AclSize;
        n = (n + 3) & ~3u;
    }
    return n;
}

NTSYSAPI NTSTATUS NTAPI RtlMakeSelfRelativeSD(PSECURITY_DESCRIPTOR abs, PSECURITY_DESCRIPTOR rel, PULONG len)
{
    ULONG need = RtlLengthSecurityDescriptor(abs);
    if (*len < need) { *len = need; return ST_BUFFER_TOO_SMALL; }
    SD_RELATIVE *r = rel;
    memset(r, 0, sizeof(*r));
    r->Revision = 1;
    r->Control = (WORD)(((SECURITY_DESCRIPTOR *)abs)->Control | SE_SELF_RELATIVE);
    ULONG off = sizeof(SD_RELATIVE);
    DWORD *slots[4] = { &r->Owner, &r->Group, &r->Sacl, &r->Dacl };
    for (int i = 0; i < 4; i++) {
        PVOID p = sd_part(abs, i);
        if (!p) continue;
        ULONG n = i < 2 ? RtlLengthSid(p) : ((ACL *)p)->AclSize;
        memcpy((BYTE *)rel + off, p, n);
        *slots[i] = off;
        off = (off + n + 3) & ~3u;
    }
    *len = need;
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlAbsoluteToSelfRelativeSD(PSECURITY_DESCRIPTOR abs, PSECURITY_DESCRIPTOR rel, PULONG len)
{
    if (self_relative(abs)) return ST_INVALID_SECURITY_DESCR;
    return RtlMakeSelfRelativeSD(abs, rel, len);
}

/* (NtQuerySecurityObject and NtSetSecurityObject are system calls: files on
 * drive C: keep their own descriptors) */

/* -----------------------------------------------------------------------
 * Tokens
 * ----------------------------------------------------------------------- */
static NTSTATUS new_token(PHANDLE out)
{
    return NtCreateEvent(out, 0x1F0003, 0, NotificationEvent, TRUE);
}

NTSYSAPI NTSTATUS NTAPI NtOpenProcessToken(HANDLE p, ACCESS_MASK access, PHANDLE token) { (void)p; (void)access; return new_token(token); }
NTSYSAPI NTSTATUS NTAPI NtOpenProcessTokenEx(HANDLE p, ACCESS_MASK access, ULONG attrs, PHANDLE token) { (void)attrs; return NtOpenProcessToken(p, access, token); }
/* a thread has no token of its own: nothing impersonates */
NTSYSAPI NTSTATUS NTAPI NtOpenThreadToken(HANDLE t, ACCESS_MASK access, BOOLEAN self, PHANDLE token) { (void)t; (void)access; (void)self; *token = 0; return ST_NO_TOKEN; }
NTSYSAPI NTSTATUS NTAPI NtOpenThreadTokenEx(HANDLE t, ACCESS_MASK access, BOOLEAN self, ULONG attrs, PHANDLE token) { (void)attrs; return NtOpenThreadToken(t, access, self, token); }
NTSYSAPI NTSTATUS NTAPI NtDuplicateToken(HANDLE t, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, BOOLEAN effective, ULONG type, PHANDLE out)
{
    (void)t; (void)access; (void)oa; (void)effective; (void)type;
    return new_token(out);
}

static NTSTATUS put_info(const void *data, ULONG n, PVOID buf, ULONG cap, PULONG ret)
{
    if (ret) *ret = n;
    if (!buf || cap < n) return ST_BUFFER_TOO_SMALL;
    memcpy(buf, data, n);
    return ST_SUCCESS;
}

/* SID_AND_ATTRIBUTES entries (after a count if @with_count), then the SIDs */
static NTSTATUS put_groups(const BYTE *const *sids, const ULONG *attrs, int n, PVOID buf, ULONG cap, PULONG ret, BOOL with_count)
{
    ULONG head = (with_count ? sizeof(ULONG_PTR) : 0) + (ULONG)sizeof(SID_AND_ATTRIBUTES) * (ULONG)n, need = head;
    for (int i = 0; i < n; i++) need += RtlLengthSid((PSID)sids[i]);
    if (ret) *ret = need;
    if (!buf || cap < need) return ST_BUFFER_TOO_SMALL;
    BYTE *b = buf, *tail = b + head;
    SID_AND_ATTRIBUTES *sa = (SID_AND_ATTRIBUTES *)(b + (with_count ? sizeof(ULONG_PTR) : 0));
    if (with_count) *(ULONG *)b = (ULONG)n;
    for (int i = 0; i < n; i++) {
        ULONG l = RtlLengthSid((PSID)sids[i]);
        memcpy(tail, sids[i], l);
        sa[i].Sid = tail;
        sa[i].Attributes = attrs[i];
        tail += l;
    }
    return ST_SUCCESS;
}

/* PSID then the SID (TOKEN_OWNER, TOKEN_PRIMARY_GROUP) */
static NTSTATUS put_sid_ptr(const BYTE *sid, PVOID buf, ULONG cap, PULONG ret)
{
    ULONG l = RtlLengthSid((PSID)sid), need = (ULONG)sizeof(PVOID) + l;
    if (ret) *ret = need;
    if (!buf || cap < need) return ST_BUFFER_TOO_SMALL;
    memcpy((BYTE *)buf + sizeof(PVOID), sid, l);
    *(PSID *)buf = (BYTE *)buf + sizeof(PVOID);
    return ST_SUCCESS;
}

#define GROUP_ON   7u               /* MANDATORY | ENABLED_BY_DEFAULT | ENABLED */

NTSYSAPI NTSTATUS NTAPI NtQueryInformationToken(HANDLE token, ULONG cls, PVOID buf, ULONG n, PULONG ret)
{
    (void)token;
    ULONG v;
    switch (cls) {
    case TokenUser: {
        const BYTE *s[1] = { g_user_sid };
        ULONG a[1] = { 0 };
        return put_groups(s, a, 1, buf, n, ret, FALSE);
    }
    case TokenOwner:        return put_sid_ptr(g_user_sid, buf, n, ret);
    case TokenPrimaryGroup: return put_sid_ptr(g_users_sid, buf, n, ret);
    case TokenGroups: {
        const BYTE *g[] = { g_everyone_sid, g_users_sid, g_admins_sid, g_interactive_sid, g_auth_users_sid, g_logon_sid };
        ULONG a[] = { GROUP_ON, GROUP_ON, 0x10 /* USE_FOR_DENY_ONLY: not elevated */, GROUP_ON, GROUP_ON, GROUP_ON | 0xC0000000u /* LOGON_ID */ };
        return put_groups(g, a, 6, buf, n, ret, TRUE);
    }
    case TokenLogonSid: {
        const BYTE *g[] = { g_logon_sid };
        ULONG a[] = { GROUP_ON | 0xC0000000u };
        return put_groups(g, a, 1, buf, n, ret, TRUE);
    }
    case TokenIntegrityLevel: {
        const BYTE *g[] = { g_medium_il_sid };
        ULONG a[] = { 0x20 /* SE_GROUP_INTEGRITY */ };
        return put_groups(g, a, 1, buf, n, ret, FALSE);
    }
    case TokenPrivileges: {
        struct { ULONG n; LUID_AND_ATTRIBUTES p[1]; } tp = { 1, { { { 23, 0 }, 3 } } };   /* SeChangeNotifyPrivilege */
        return put_info(&tp, sizeof(tp), buf, n, ret);
    }
    case TokenDefaultDacl: {
        /* TOKEN_DEFAULT_DACL { PACL } then an ACL: the user and SYSTEM, full access */
        static const BYTE system_sid[] = { 1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0 };
        ULONG acl_len = sizeof(ACL) + 8 + sizeof(g_user_sid) + 8 + sizeof(system_sid);
        ULONG need = (ULONG)sizeof(PVOID) + acl_len;
        if (ret) *ret = need;
        if (!buf || n < need) return ST_BUFFER_TOO_SMALL;
        PACL acl = (PACL)((BYTE *)buf + sizeof(PVOID));
        RtlCreateAcl(acl, acl_len, ACL_REVISION);
        add_ace(acl, ACL_REVISION, 0, 0, 0x10000000 /* GENERIC_ALL */, (PSID)g_user_sid);
        add_ace(acl, ACL_REVISION, 0, 0, 0x10000000, (PSID)system_sid);
        *(PACL *)buf = acl;
        return ST_SUCCESS;
    }
    case TokenSource: {
        struct { CHAR name[8]; LUID id; } src = { { 'U', 's', 'e', 'r', '3', '2', ' ', 0 }, { 0x3E9, 0 } };
        return put_info(&src, sizeof(src), buf, n, ret);
    }
    case TokenStatistics: {
        BYTE st[56];
        memset(st, 0, sizeof(st));
        *(ULONG *)st = 0x1000;                                          /* TokenId */
        *(ULONG *)(st + 8) = 0x3E7 + 1;                                 /* AuthenticationId */
        *(ULONG *)(st + 32) = 1;                                        /* TokenType: primary */
        *(ULONG *)(st + 40) = 6;                                        /* GroupCount */
        *(ULONG *)(st + 44) = 1;                                        /* PrivilegeCount */
        return put_info(st, sizeof(st), buf, n, ret);
    }
    case TokenLinkedToken: return ST_NO_TOKEN;                          /* not a split (UAC) token */
    case TokenElevation:     v = 0; return put_info(&v, 4, buf, n, ret);
    case TokenElevationType: v = 1; return put_info(&v, 4, buf, n, ret);   /* TokenElevationTypeDefault */
    case TokenType:          v = 1; return put_info(&v, 4, buf, n, ret);   /* TokenPrimary */
    case TokenSessionId:     v = 1; return put_info(&v, 4, buf, n, ret);
    case TokenImpersonationLevel: v = SecurityImpersonation; return put_info(&v, 4, buf, n, ret);
    case TokenMandatoryPolicy: v = 1; return put_info(&v, 4, buf, n, ret);  /* NO_WRITE_UP */
    case TokenIsAppContainer: case TokenHasRestrictions: case TokenUIAccess: case TokenVirtualizationAllowed:
    case TokenVirtualizationEnabled: case TokenSandBoxInert:
        v = 0; return put_info(&v, 4, buf, n, ret);
    default:
        return ST_INVALID_INFO_CLASS;
    }
}

NTSYSAPI NTSTATUS NTAPI NtSetInformationToken(HANDLE t, ULONG cls, PVOID buf, ULONG n) { (void)t; (void)cls; (void)buf; (void)n; return ST_SUCCESS; }

/* Every privilege asked for is granted (nothing is checked) */
NTSYSAPI NTSTATUS NTAPI NtAdjustPrivilegesToken(HANDLE t, BOOLEAN disable_all, PTOKEN_PRIVILEGES want, ULONG len,
                                                PTOKEN_PRIVILEGES prev, PULONG ret)
{
    (void)t; (void)disable_all; (void)want;
    ULONG need = sizeof(ULONG);
    if (ret) *ret = need;
    if (prev) {
        if (len < need) return ST_BUFFER_TOO_SMALL;
        prev->PrivilegeCount = 0;
    }
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI NtPrivilegeCheck(HANDLE t, PPRIVILEGE_SET set, PBOOLEAN result)
{
    (void)t;
    for (ULONG i = 0; set && i < set->PrivilegeCount; i++) set->Privilege[i].Attributes |= 0x80000000u;   /* USED_FOR_ACCESS */
    *result = TRUE;
    return ST_SUCCESS;
}

/* Whether the token holds @sid: its user and enabled groups, and for deny
 * ACEs also the groups only used for denying (Administrators: not elevated) */
static BOOL token_holds(PSID sid, BOOL deny)
{
    const BYTE *on[] = { g_user_sid, g_everyone_sid, g_users_sid, g_interactive_sid, g_auth_users_sid, g_logon_sid };
    for (unsigned i = 0; i < sizeof(on) / sizeof(on[0]); i++)
        if (RtlEqualSid(sid, (PSID)on[i])) return TRUE;
    return deny && RtlEqualSid(sid, (PSID)g_admins_sid);
}

static ACCESS_MASK map_generic(ACCESS_MASK m, PGENERIC_MAPPING map)
{
    if (!map) return m & 0x10000000u ? 0x001FFFFFu | (m & 0x0FFFFFFFu) : m;   /* GENERIC_ALL: every right */
    if (m & 0x80000000u) m |= map->GenericRead;
    if (m & 0x40000000u) m |= map->GenericWrite;
    if (m & 0x20000000u) m |= map->GenericExecute;
    if (m & 0x10000000u) m |= map->GenericAll;
    return m & 0x0FFFFFFFu;
}

/* The DACL decides, as on Windows: no DACL grants everything, an empty one
 * nothing; the ACEs are taken in order, and each whose SID the token holds
 * grants its rights unless an earlier one denied them, or denies its
 * rights unless an earlier one granted them.  The owner always may read
 * and change the DACL.  MAXIMUM_ALLOWED asks for whatever is granted. */
NTSYSAPI NTSTATUS NTAPI NtAccessCheck(PSECURITY_DESCRIPTOR sd, HANDLE token, ACCESS_MASK want, PGENERIC_MAPPING map,
                                      PPRIVILEGE_SET privs, PULONG privs_len, PACCESS_MASK granted, PNTSTATUS status)
{
    (void)token;
    if (!sd || !granted || !status) return ST_INVALID_PARAMETER;
    if (privs && privs_len && *privs_len >= sizeof(PRIVILEGE_SET)) privs->PrivilegeCount = 0;
    BOOL max = (want & 0x02000000u) != 0;
    ACCESS_MASK m = map_generic(want & ~0x03000000u, map);              /* (ACCESS_SYSTEM_SECURITY: not checked) */
    ACCESS_MASK all = map ? map->GenericAll : 0x001FFFFFu;

    BOOLEAN present = FALSE, def;
    PACL dacl = 0;
    RtlGetDaclSecurityDescriptor(sd, &present, &dacl, &def);
    if (!present || !dacl) {
        *granted = max ? all | m : m;
        *status = ST_SUCCESS;
        return ST_SUCCESS;
    }
    ACCESS_MASK allowed = 0, denied = 0;
    PSID owner = 0;
    RtlGetOwnerSecurityDescriptor(sd, &owner, &def);
    if (owner && token_holds(owner, FALSE)) allowed = 0x00060000u;      /* READ_CONTROL | WRITE_DAC */
    const BYTE *p = (const BYTE *)(dacl + 1), *end = (const BYTE *)dacl + dacl->AclSize;
    for (USHORT i = 0; i < dacl->AceCount; i++) {
        const ACE_HEADER *h = (const ACE_HEADER *)p;
        if (p + sizeof(ACE_HEADER) > end || h->AceSize < 16 || p + h->AceSize > end) break;
        p += h->AceSize;
        if (h->AceFlags & 0x08) continue;                               /* INHERIT_ONLY_ACE: for children */
        if (h->AceType > 1) continue;                                   /* not ACCESS_ALLOWED / ACCESS_DENIED */
        const ACCESS_ALLOWED_ACE *ace = (const ACCESS_ALLOWED_ACE *)h;
        PSID sid = (PSID)&ace->SidStart;
        if (8u + 4u * ((const BYTE *)sid)[1] > h->AceSize - 8u || !token_holds(sid, h->AceType == 1)) continue;
        ACCESS_MASK am = map_generic(ace->Mask, map);
        if (h->AceType == 0) allowed |= am & ~denied;
        else denied |= am & ~allowed;
    }
    if (m & ~allowed) {
        *granted = 0;
        *status = ST_ACCESS_DENIED;
    } else {
        *granted = max ? allowed | m : m;
        *status = max && !*granted ? ST_ACCESS_DENIED : ST_SUCCESS;
    }
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI NtAllocateLocallyUniqueId(PLUID luid)
{
    static volatile LONG next = 0x10000;
    luid->LowPart = (DWORD)__atomic_add_fetch(&next, 1, __ATOMIC_SEQ_CST);
    luid->HighPart = 0;
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Counted strings
 * ----------------------------------------------------------------------- */
static WCHAR up(WCHAR c)
{
    if (c >= 'a' && c <= 'z') return (WCHAR)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (WCHAR)(c - 32);   /* Latin-1 */
    return c;
}
static WCHAR down(WCHAR c)
{
    if (c >= 'A' && c <= 'Z') return (WCHAR)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (WCHAR)(c + 32);
    return c;
}

NTSYSAPI WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR c) { return up(c); }
NTSYSAPI WCHAR NTAPI RtlDowncaseUnicodeChar(WCHAR c) { return down(c); }

NTSYSAPI NTSTATUS NTAPI RtlAppendUnicodeStringToString(PUNICODE_STRING dst, const UNICODE_STRING *src)
{
    if (!src->Length) return ST_SUCCESS;
    if (dst->Length + src->Length > dst->MaximumLength) return ST_BUFFER_TOO_SMALL;
    memmove((BYTE *)dst->Buffer + dst->Length, src->Buffer, src->Length);
    dst->Length = (USHORT)(dst->Length + src->Length);
    if (dst->Length + 2 <= dst->MaximumLength) dst->Buffer[dst->Length / 2] = 0;
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlAppendUnicodeToString(PUNICODE_STRING dst, PCWSTR src)
{
    if (!src) return ST_SUCCESS;
    UNICODE_STRING s;
    RtlInitUnicodeString(&s, src);
    return RtlAppendUnicodeStringToString(dst, &s);
}

NTSYSAPI LONG NTAPI RtlCompareUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b, BOOLEAN ci)
{
    USHORT na = a->Length / 2, nb = b->Length / 2, n = na < nb ? na : nb;
    for (USHORT i = 0; i < n; i++) {
        WCHAR x = a->Buffer[i], y = b->Buffer[i];
        if (ci) { x = up(x); y = up(y); }
        if (x != y) return (LONG)x - (LONG)y;
    }
    return (LONG)na - (LONG)nb;
}

NTSYSAPI BOOLEAN NTAPI RtlEqualUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b, BOOLEAN ci)
{
    return a->Length == b->Length && !RtlCompareUnicodeString(a, b, ci);
}

NTSYSAPI BOOLEAN NTAPI RtlPrefixUnicodeString(const UNICODE_STRING *prefix, const UNICODE_STRING *s, BOOLEAN ci)
{
    if (prefix->Length > s->Length) return FALSE;
    for (USHORT i = 0; i < prefix->Length / 2; i++) {
        WCHAR x = prefix->Buffer[i], y = s->Buffer[i];
        if (ci) { x = up(x); y = up(y); }
        if (x != y) return FALSE;
    }
    return TRUE;
}

NTSYSAPI VOID NTAPI RtlCopyUnicodeString(PUNICODE_STRING dst, const UNICODE_STRING *src)
{
    if (!src) { dst->Length = 0; return; }
    USHORT n = src->Length < dst->MaximumLength ? src->Length : dst->MaximumLength;
    memmove(dst->Buffer, src->Buffer, n);
    dst->Length = n;
    if (n + 2 <= dst->MaximumLength) dst->Buffer[n / 2] = 0;
}

static NTSTATUS case_convert(PUNICODE_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc, BOOL upper)
{
    if (alloc) {
        dst->Buffer = RtlAllocateHeap(heap(), 0, src->Length + 2u);
        if (!dst->Buffer) return ST_NO_MEMORY;
        dst->MaximumLength = (USHORT)(src->Length + 2);
    } else if (dst->MaximumLength < src->Length) return ST_BUFFER_OVERFLOW;
    for (USHORT i = 0; i < src->Length / 2; i++) dst->Buffer[i] = upper ? up(src->Buffer[i]) : down(src->Buffer[i]);
    dst->Length = src->Length;
    return ST_SUCCESS;
}
NTSYSAPI NTSTATUS NTAPI RtlUpcaseUnicodeString(PUNICODE_STRING d, const UNICODE_STRING *s, BOOLEAN a) { return case_convert(d, s, a, TRUE); }
NTSYSAPI NTSTATUS NTAPI RtlDowncaseUnicodeString(PUNICODE_STRING d, const UNICODE_STRING *s, BOOLEAN a) { return case_convert(d, s, a, FALSE); }

NTSYSAPI VOID NTAPI RtlFreeUnicodeString(PUNICODE_STRING s)
{
    if (s->Buffer) RtlFreeHeap(heap(), 0, s->Buffer);
    s->Buffer = 0;
    s->Length = s->MaximumLength = 0;
}

typedef struct { USHORT Length, MaximumLength; CHAR *Buffer; } NT_ANSI_STRING;

NTSYSAPI VOID NTAPI RtlInitAnsiString(NT_ANSI_STRING *s, const CHAR *src)
{
    SIZE_T n = 0;
    if (src) while (src[n]) n++;
    if (n > 0xFFFE) n = 0xFFFE;
    s->Buffer = (CHAR *)src;
    s->Length = (USHORT)n;
    s->MaximumLength = src ? (USHORT)(n + 1) : 0;
}

NTSYSAPI VOID NTAPI RtlFreeAnsiString(NT_ANSI_STRING *s)
{
    if (s->Buffer) RtlFreeHeap(heap(), 0, s->Buffer);
    s->Buffer = 0;
    s->Length = s->MaximumLength = 0;
}

/* The "ANSI" code page is UTF-8 on NovaOS */
static ULONG utf8_of(const WCHAR *w, ULONG n, CHAR *out, ULONG cap)
{
    ULONG o = 0;
    for (ULONG i = 0; i < n; i++) {
        ULONG c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00u);
        }
        CHAR t[4];
        int k = 0;
        if (c < 0x80) t[k++] = (CHAR)c;
        else if (c < 0x800) { t[k++] = (CHAR)(0xC0 | c >> 6); t[k++] = (CHAR)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { t[k++] = (CHAR)(0xE0 | c >> 12); t[k++] = (CHAR)(0x80 | ((c >> 6) & 0x3F)); t[k++] = (CHAR)(0x80 | (c & 0x3F)); }
        else { t[k++] = (CHAR)(0xF0 | c >> 18); t[k++] = (CHAR)(0x80 | ((c >> 12) & 0x3F)); t[k++] = (CHAR)(0x80 | ((c >> 6) & 0x3F)); t[k++] = (CHAR)(0x80 | (c & 0x3F)); }
        for (int j = 0; j < k; j++) { if (out && o < cap) out[o] = t[j]; o++; }
    }
    return o;
}

NTSYSAPI NTSTATUS NTAPI RtlUnicodeStringToAnsiString(NT_ANSI_STRING *dst, const UNICODE_STRING *src, BOOLEAN alloc)
{
    ULONG n = utf8_of(src->Buffer, src->Length / 2u, 0, 0);
    if (n > 0xFFFE) return ST_INVALID_PARAMETER;
    if (alloc) {
        dst->Buffer = RtlAllocateHeap(heap(), 0, n + 1);
        if (!dst->Buffer) return ST_NO_MEMORY;
        dst->MaximumLength = (USHORT)(n + 1);
    } else if (dst->MaximumLength <= n) {
        ULONG k = dst->MaximumLength ? dst->MaximumLength - 1u : 0;
        utf8_of(src->Buffer, src->Length / 2u, dst->Buffer, k);
        dst->Length = (USHORT)k;
        if (dst->MaximumLength) dst->Buffer[k] = 0;
        return ST_BUFFER_OVERFLOW;
    }
    utf8_of(src->Buffer, src->Length / 2u, dst->Buffer, n);
    dst->Buffer[n] = 0;
    dst->Length = (USHORT)n;
    return ST_SUCCESS;
}

NTSYSAPI ULONG NTAPI RtlUnicodeStringToAnsiSize(const UNICODE_STRING *s) { return utf8_of(s->Buffer, s->Length / 2u, 0, 0) + 1; }

/* -----------------------------------------------------------------------
 * The current directory, kept as Windows keeps it: a FAST_CWD structure
 * on the process heap that RtlpCurDirRef points to, whose path the
 * process parameters' CurrentDirectory.DosPath also points at, guarded by
 * the PEB's FastPebLock.  A program may put a FAST_CWD of its own in
 * place (Cygwin does, finding RtlpCurDirRef through
 * RtlGetCurrentDirectory_U's code, which is laid out below the way
 * Windows' is); kernel32 reads the path from the process parameters.
 * ----------------------------------------------------------------------- */
typedef struct {
    LONG ReferenceCount;
    HANDLE DirectoryHandle;
    ULONG OldDismountCount;
    UNICODE_STRING Path;
    LONG FSCharacteristics;
    WCHAR Buffer[2] __attribute__((aligned(8)));
} FAST_CWD;

FAST_CWD *RtlpCurDirRef;
RTL_CRITICAL_SECTION RtlpFastPebLock;

static void cwd_release(FAST_CWD *f)
{
    if (f && !__atomic_sub_fetch(&f->ReferenceCount, 1, __ATOMIC_SEQ_CST)) {
        if (f->DirectoryHandle) NtClose(f->DirectoryHandle);
        RtlFreeHeap(heap(), 0, f);
    }
}

/* The current FAST_CWD, referenced (the helper whose code Cygwin reads) */
FAST_CWD *RtlpReferenceCurrentDirectory(void);
#ifdef __x86_64__
__asm__(".text\n"
        ".globl RtlpReferenceCurrentDirectory\n"
        "RtlpReferenceCurrentDirectory:\n\t"
        "pushq %rbx\n\t"
        "subq $32, %rsp\n\t"
        "leaq RtlpFastPebLock(%rip), %rcx\n\t"     /* 48 8d 0d: the FastPebLock */
        "call RtlEnterCriticalSection\n\t"          /* e8 */
        "movq RtlpCurDirRef(%rip), %rbx\n\t"        /* 48 8b 1d: the FAST_CWD pointer */
        "testq %rbx, %rbx\n\t"                      /* 48 85 db */
        "jz 1f\n\t"
        "lock incl (%rbx)\n"
        "1:\n\t"
        "leaq RtlpFastPebLock(%rip), %rcx\n\t"
        "call RtlLeaveCriticalSection\n\t"
        "movq %rbx, %rax\n\t"
        "addq $32, %rsp\n\t"
        "popq %rbx\n\t"
        "ret\n");
#else
FAST_CWD *RtlpReferenceCurrentDirectory(void)
{
    RtlEnterCriticalSection(&RtlpFastPebLock);
    FAST_CWD *f = RtlpCurDirRef;
    if (f) __atomic_add_fetch(&f->ReferenceCount, 1, __ATOMIC_SEQ_CST);
    RtlLeaveCriticalSection(&RtlpFastPebLock);
    return f;
}
#endif

ULONG NTAPI nova_get_cwd(ULONG len, PWSTR buf, FAST_CWD *f);

/* RtlGetCurrentDirectory_U(ULONG BufferLength (bytes), PWSTR Buffer): the
 * length in bytes (without the NUL), or the size needed */
#ifdef __x86_64__
__asm__(".text\n"
        ".globl RtlGetCurrentDirectory_U\n"
        "RtlGetCurrentDirectory_U:\n\t"
        "pushq %rsi\n\t"
        "pushq %rdi\n\t"
        "subq $40, %rsp\n\t"
        "movl %ecx, %esi\n\t"
        "movq %rdx, %rdi\n\t"
        "call RtlpReferenceCurrentDirectory\n\t"   /* the first call: to the helper */
        "movl %esi, %ecx\n\t"
        "movq %rdi, %rdx\n\t"
        "movq %rax, %r8\n\t"
        "call nova_get_cwd\n\t"
        "addq $40, %rsp\n\t"
        "popq %rdi\n\t"
        "popq %rsi\n\t"
        "ret\n"
        ".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:RtlGetCurrentDirectory_U\"\n\t"
        ".text\n");
#else
NTSYSAPI ULONG NTAPI RtlGetCurrentDirectory_U(ULONG len, PWSTR buf) { return nova_get_cwd(len, buf, RtlpReferenceCurrentDirectory()); }
#endif

ULONG NTAPI nova_get_cwd(ULONG len, PWSTR buf, FAST_CWD *f)
{
    ULONG n = 0;
    if (f) {
        n = f->Path.Length;
        if (n > 6 && f->Path.Buffer[n / 2 - 1] == '\\') n -= 2;    /* "C:\dir\" -> "C:\dir" (not "C:\") */
        if (len >= n + 2) {
            memcpy(buf, f->Path.Buffer, n);
            buf[n / 2] = 0;
        } else n += 2;
        cwd_release(f);
    }
    return n;
}

/* Make @path (a full "C:\dir\" DOS path, @n characters) the current directory */
static NTSTATUS set_cwd(const WCHAR *path, ULONG n, HANDLE dir)
{
    FAST_CWD *f = RtlAllocateHeap(heap(), 0, sizeof(FAST_CWD) + 2 * n + 4);
    if (!f) return ST_NO_MEMORY;
    f->ReferenceCount = 1;
    f->DirectoryHandle = dir;
    f->OldDismountCount = 0;
    f->FSCharacteristics = 0;
    memcpy(f->Buffer, path, 2 * n);
    f->Buffer[n] = 0;
    f->Path.Buffer = f->Buffer;
    f->Path.Length = (USHORT)(2 * n);
    f->Path.MaximumLength = (USHORT)(2 * n + 2);
    PPEB peb = RtlGetCurrentPeb();
    RtlEnterCriticalSection(&RtlpFastPebLock);
    FAST_CWD *old = RtlpCurDirRef;
    RtlpCurDirRef = f;
    peb->ProcessParameters->CurrentDirectory.DosPath = f->Path;
    peb->ProcessParameters->CurrentDirectory.Handle = dir;
    RtlLeaveCriticalSection(&RtlpFastPebLock);
    cwd_release(old);
    return ST_SUCCESS;
}

/* RtlSetCurrentDirectory_U: a directory that exists, absolute ("C:\x",
 * "\\?\C:\x", "\??\C:\x") or relative to the current one */
NTSYSAPI NTSTATUS NTAPI RtlSetCurrentDirectory_U(PUNICODE_STRING name)
{
    WCHAR full[MAX_PATH + 8];
    ULONG n = name->Length / 2, k = 0;
    const WCHAR *s = name->Buffer;
    if (n >= 4 && s[0] == '\\' && (s[1] == '\\' || s[1] == '?') && s[2] == '?' && s[3] == '\\') { s += 4; n -= 4; }
    if (n >= 2 && s[1] == ':') {
        if (n > MAX_PATH) return ST_INVALID_PARAMETER;
        for (; k < n; k++) full[k] = s[k] == '/' ? '\\' : s[k];
    } else {
        FAST_CWD *f = RtlpReferenceCurrentDirectory();
        ULONG base = f ? f->Path.Length / 2u : 0;
        if (base + n + 2 > MAX_PATH) { cwd_release(f); return ST_INVALID_PARAMETER; }
        if (f) memcpy(full, f->Path.Buffer, 2 * base);
        cwd_release(f);
        k = base;
        if (n && (s[0] == '\\' || s[0] == '/')) k = 2;                  /* "\x": the drive's root */
        else if (k && full[k - 1] != '\\') full[k++] = '\\';
        for (ULONG i = 0; i < n; i++) full[k++] = s[i] == '/' ? '\\' : s[i];
    }
    /* "." and ".." */
    WCHAR out[MAX_PATH + 8];
    ULONG o = 0;
    for (ULONG i = 0; i < k; ) {
        ULONG j = i;
        while (j < k && full[j] != '\\') j++;
        ULONG len = j - i;
        if (len == 1 && full[i] == '.') { }
        else if (len == 2 && full[i] == '.' && full[i + 1] == '.') {
            if (o > 3) { o--; while (o > 3 && out[o - 1] != '\\') o--; }
        } else if (len) {
            memcpy(out + o, full + i, 2 * len);
            o += len;
            out[o++] = '\\';
        } else if (!o) out[o++] = '\\';
        i = j + 1;
    }
    if (o < 3) return ST_INVALID_PARAMETER;
    if (out[o - 1] != '\\') out[o++] = '\\';
    /* open it (it must be a directory) */
    WCHAR nt[MAX_PATH + 16] = { '\\', '?', '?', '\\' };
    memcpy(nt + 4, out, 2 * o);
    UNICODE_STRING us = { (USHORT)(2 * (o + 4)), (USHORT)(2 * (o + 4)), nt };
    if (o > 3) us.Length -= 2;                                        /* no trailing '\' except the root */
    OBJECT_ATTRIBUTES oa = { sizeof(oa), 0, &us, OBJ_CASE_INSENSITIVE, 0, 0 };
    IO_STATUS_BLOCK io;
    HANDLE h;
    NTSTATUS st = NtOpenFile(&h, SYNCHRONIZE | 0x20 /* FILE_TRAVERSE */, &oa, &io, 7,
                             FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(st)) return st;
    st = set_cwd(out, o, h);
    if (!NT_SUCCESS(st)) NtClose(h);
    return st;
}

/* At process start: the lock, and the directory the process was started in */
void RtlNovaInitProcess(void)
{
    PPEB peb = RtlGetCurrentPeb();
    RtlInitializeCriticalSection(&RtlpFastPebLock);
    peb->FastPebLock = &RtlpFastPebLock;
    UNICODE_STRING *d = &peb->ProcessParameters->CurrentDirectory.DosPath;
    WCHAR buf[MAX_PATH + 2];
    ULONG n = d->Length / 2u;
    if (n > MAX_PATH - 1) n = MAX_PATH - 1;
    memcpy(buf, d->Buffer, 2 * n);
    if (n < 2) { buf[0] = 'C'; buf[1] = ':'; n = 2; }
    if (buf[n - 1] != '\\') buf[n++] = '\\';
    set_cwd(buf, n, 0);
}

/* -----------------------------------------------------------------------
 * Registry queries
 * ----------------------------------------------------------------------- */
typedef NTSTATUS (NTAPI *RTL_QUERY_REGISTRY_ROUTINE)(PCWSTR name, ULONG type, PVOID data, ULONG len, PVOID ctx, PVOID entry_ctx);
typedef struct {
    RTL_QUERY_REGISTRY_ROUTINE QueryRoutine;
    ULONG Flags;
    PWSTR Name;
    PVOID EntryContext;
    ULONG DefaultType;
    PVOID DefaultData;
    ULONG DefaultLength;
} RTL_QUERY_REGISTRY_TABLE;
#define RTL_QUERY_REGISTRY_SUBKEY   0x00000001
#define RTL_QUERY_REGISTRY_TOPKEY   0x00000002
#define RTL_QUERY_REGISTRY_REQUIRED 0x00000004
#define RTL_QUERY_REGISTRY_NOVALUE  0x00000008
#define RTL_QUERY_REGISTRY_DIRECT   0x00000020
#define RTL_REGISTRY_HANDLE         0x40000000
#define RTL_REGISTRY_OPTIONAL       0x80000000

/* Open @path relative to one of the RTL_REGISTRY_* roots */
static NTSTATUS open_rel_key(ULONG rel, PCWSTR path, PHANDLE key)
{
    if (rel & RTL_REGISTRY_HANDLE) { *key = (HANDLE)path; return ST_SUCCESS; }
    static const WCHAR *const roots[] = {
        L"", L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\",
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\",
        L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion\\",
        L"\\Registry\\Machine\\Hardware\\DeviceMap\\", L"\\Registry\\User\\.Default\\" };
    ULONG r = rel & 0xFFFF;
    if (r > 5) return ST_INVALID_PARAMETER;
    WCHAR full[512];
    ULONG n = 0;
    for (const WCHAR *c = roots[r]; *c && n < 500; c++) full[n++] = *c;
    for (const WCHAR *c = path; c && *c && n < 510; c++) full[n++] = *c;
    full[n] = 0;
    UNICODE_STRING us = { (USHORT)(2 * n), (USHORT)(2 * n + 2), full };
    OBJECT_ATTRIBUTES oa = { sizeof(oa), 0, &us, OBJ_CASE_INSENSITIVE, 0, 0 };
    return NtOpenKey(key, 0x20019 /* KEY_READ */, &oa);
}

NTSYSAPI NTSTATUS NTAPI RtlCheckRegistryKey(ULONG rel, PWSTR path)
{
    HANDLE k;
    NTSTATUS s = open_rel_key(rel, path, &k);
    if (NT_SUCCESS(s) && !(rel & RTL_REGISTRY_HANDLE)) NtClose(k);
    return s;
}

/* Deliver one value: to the routine, or (DIRECT) into EntryContext */
static NTSTATUS deliver(RTL_QUERY_REGISTRY_TABLE *t, ULONG type, PVOID data, ULONG len, PVOID ctx)
{
    if (!(t->Flags & RTL_QUERY_REGISTRY_DIRECT))
        return t->QueryRoutine ? t->QueryRoutine(t->Name, type, data, len, ctx, t->EntryContext) : ST_SUCCESS;
    if (type == 1 || type == 2 || type == 7) {                       /* REG_SZ, EXPAND_SZ, MULTI_SZ */
        UNICODE_STRING *u = t->EntryContext;
        ULONG n = len >= 2 && !((WCHAR *)data)[len / 2 - 1] ? len - 2 : len;
        if (!u->Buffer) {
            u->Buffer = RtlAllocateHeap(heap(), 0, n + 2);
            if (!u->Buffer) return ST_NO_MEMORY;
            u->MaximumLength = (USHORT)(n + 2);
        }
        if (n + 2 > u->MaximumLength) return ST_BUFFER_TOO_SMALL;
        memcpy(u->Buffer, data, n);
        u->Buffer[n / 2] = 0;
        u->Length = (USHORT)n;
    } else if (len <= sizeof(ULONG)) {
        memcpy(t->EntryContext, data, len);
    } else {                                                        /* binary: LONG size then data */
        LONG cap = *(LONG *)t->EntryContext;
        if (cap < 0 && (ULONG)-cap < len) return ST_BUFFER_TOO_SMALL;
        if (cap < 0) memcpy(t->EntryContext, data, len);
        else { *(ULONG *)t->EntryContext = len; memcpy((BYTE *)t->EntryContext + 4, data, len); }
    }
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlQueryRegistryValues(ULONG rel, PCWSTR path, RTL_QUERY_REGISTRY_TABLE *table, PVOID ctx, PVOID env)
{
    (void)env;
    HANDLE top;
    NTSTATUS s = open_rel_key(rel, path, &top);
    if (!NT_SUCCESS(s)) return s;
    HANDLE key = top;
    BYTE *info = RtlAllocateHeap(heap(), 0, 4096);
    if (!info) { s = ST_NO_MEMORY; goto done; }
    for (RTL_QUERY_REGISTRY_TABLE *t = table; t->QueryRoutine || t->Name; t++) {
        if (t->Flags & (RTL_QUERY_REGISTRY_SUBKEY | RTL_QUERY_REGISTRY_TOPKEY)) {
            if (key != top) NtClose(key);
            key = top;
            if ((t->Flags & RTL_QUERY_REGISTRY_SUBKEY) && t->Name) {
                UNICODE_STRING us;
                RtlInitUnicodeString(&us, t->Name);
                OBJECT_ATTRIBUTES oa = { sizeof(oa), top, &us, OBJ_CASE_INSENSITIVE, 0, 0 };
                s = NtOpenKey(&key, 0x20019, &oa);
                if (!NT_SUCCESS(s)) { key = top; goto done; }
            }
            if (!t->Name || (t->Flags & RTL_QUERY_REGISTRY_SUBKEY)) continue;
        }
        if (t->Flags & RTL_QUERY_REGISTRY_NOVALUE) {
            if (t->QueryRoutine) t->QueryRoutine(t->Name, 0, 0, 0, ctx, t->EntryContext);
            continue;
        }
        if (!t->Name) {                                             /* every value of the key */
            for (ULONG i = 0; ; i++) {
                ULONG got;
                if (!NT_SUCCESS(NtEnumerateValueKey(key, i, 1 /* Full */, info, 4096, &got))) break;
                ULONG *v = (ULONG *)info;                           /* TitleIndex, Type, DataOffset, DataLength, NameLength, Name */
                WCHAR nm[256];
                ULONG nl = v[4] / 2 < 255 ? v[4] / 2 : 255;
                memcpy(nm, v + 5, 2 * nl);
                nm[nl] = 0;
                RTL_QUERY_REGISTRY_TABLE one = *t;
                one.Name = nm;
                s = deliver(&one, v[1], info + v[2], v[3], ctx);
                if (!NT_SUCCESS(s)) goto done;
            }
            continue;
        }
        UNICODE_STRING vn;
        RtlInitUnicodeString(&vn, t->Name);
        ULONG got;
        NTSTATUS q = NtQueryValueKey(key, &vn, 2 /* Partial */, info, 4096, &got);
        if (NT_SUCCESS(q)) {
            ULONG *v = (ULONG *)info;                               /* TitleIndex, Type, DataLength, Data */
            s = deliver(t, v[1], v + 3, v[2], ctx);
        } else if (t->Flags & RTL_QUERY_REGISTRY_REQUIRED) {
            s = ST_OBJECT_NAME_NOT_FOUND;
        } else if (t->DefaultType) {
            ULONG dl = t->DefaultLength;
            if (!dl && t->DefaultData && (t->DefaultType == 1 || t->DefaultType == 2)) {
                const WCHAR *w = t->DefaultData;
                while (w[dl / 2]) dl += 2;
                dl += 2;
            }
            s = deliver(t, t->DefaultType, t->DefaultData, dl, ctx);
        }
        if (!NT_SUCCESS(s)) goto done;
    }
    s = ST_SUCCESS;
done:
    if (info) RtlFreeHeap(heap(), 0, info);
    if (key != top) NtClose(key);
    if (!(rel & RTL_REGISTRY_HANDLE)) NtClose(top);
    return s;
}

/* -----------------------------------------------------------------------
 * System information
 * ----------------------------------------------------------------------- */
static ULONG cpu_count(void)
{
    ULONG n = *(volatile ULONG *)(ULONG_PTR)0x7FFE03C0;             /* KUSER_SHARED_DATA.ActiveProcessorCount */
    return n ? n : 1;
}

NTSYSAPI NTSTATUS NTAPI NtQuerySystemInformation(ULONG cls, PVOID buf, ULONG len, PULONG ret)
{
    switch (cls) {
    case 0: {                                                       /* SystemBasicInformation */
        struct { ULONG Reserved, TimerResolution, PageSize, NumberOfPhysicalPages, LowestPhysicalPageNumber,
                 HighestPhysicalPageNumber, AllocationGranularity; ULONG_PTR MinimumUserModeAddress,
                 MaximumUserModeAddress, ActiveProcessorsAffinityMask; CHAR NumberOfProcessors; } b;
        memset(&b, 0, sizeof(b));
        ULONG n = cpu_count();
        b.TimerResolution = 156250;
        b.PageSize = 4096;
        b.NumberOfPhysicalPages = 2u << 18;                         /* 2 GiB */
        b.LowestPhysicalPageNumber = 1;
        b.HighestPhysicalPageNumber = b.NumberOfPhysicalPages;
        b.AllocationGranularity = 65536;
        b.MinimumUserModeAddress = 0x10000;
#ifdef _WIN64
        b.MaximumUserModeAddress = 0x7FFFFFFEFFFFULL;
#else
        b.MaximumUserModeAddress = 0x7FFEFFFF;
#endif
        b.ActiveProcessorsAffinityMask = n >= 8 * sizeof(ULONG_PTR) ? ~(ULONG_PTR)0 : ((ULONG_PTR)1 << n) - 1;
        b.NumberOfProcessors = (CHAR)n;
        return put_info(&b, sizeof(b), buf, len, ret) == ST_BUFFER_TOO_SMALL ? ST_INFO_LENGTH_MISMATCH : ST_SUCCESS;
    }
    case 2: {                                                       /* SystemPerformanceInformation */
        BYTE p[344];
        memset(p, 0, sizeof(p));
        *(ULONG *)(p + 0x3C) = 1u << 18;                            /* AvailablePages */
        *(ULONG *)(p + 0x40) = 1u << 17;                            /* CommittedPages */
        *(ULONG *)(p + 0x44) = 1u << 19;                            /* CommitLimit */
        if (ret) *ret = sizeof(p);
        if (len < sizeof(p)) return ST_INFO_LENGTH_MISMATCH;
        memcpy(buf, p, sizeof(p));
        return ST_SUCCESS;
    }
    case 3: {                                                       /* SystemTimeOfDayInformation */
        struct { LARGE_INTEGER BootTime, CurrentTime, TimeZoneBias; ULONG TimeZoneId, Reserved; ULONGLONG BootTimeBias, SleepTimeBias; } t;
        memset(&t, 0, sizeof(t));
        NtQuerySystemTime(&t.CurrentTime);
        LARGE_INTEGER c, f;
        NtQueryPerformanceCounter(&c, &f);
        ULONGLONG up = 0;                                           /* 100 ns units since boot */
        if (f.QuadPart > 0) up = (ULONGLONG)c.QuadPart / (ULONGLONG)f.QuadPart * 10000000ULL +
                                 (ULONGLONG)c.QuadPart % (ULONGLONG)f.QuadPart * 10000000ULL / (ULONGLONG)f.QuadPart;
        t.BootTime.QuadPart = t.CurrentTime.QuadPart - (LONGLONG)up;
        ULONG n = len < sizeof(t) ? len : sizeof(t);
        memcpy(buf, &t, n);
        if (ret) *ret = n;
        return ST_SUCCESS;
    }
    case 5: {                                                       /* SystemProcessInformation */
        NOVA_PROCESS_ENTRY list[64];
        ULONG n = 0;
        NtNovaProcessList(list, 64, &n);
        if (n > 64) n = 64;
        /* SYSTEM_PROCESS_INFORMATION (0x100 bytes on x64, 0xB8 on x86), then its name */
        const ULONG rec = sizeof(PVOID) == 8 ? 0x100 : 0xB8;
        ULONG need = 0;
        for (ULONG i = 0; i < n; i++) if (!list[i].Exited) {
            ULONG nl = 0;
            while (list[i].Name[nl]) nl++;
            need += (rec + 2 * nl + 2 + 7) & ~7u;
        }
        if (!need) need = rec;
        if (ret) *ret = need;
        if (len < need) return ST_INFO_LENGTH_MISMATCH;
        memset(buf, 0, need);
        BYTE *p = buf, *last = 0;
        for (ULONG i = 0; i < n; i++) {
            if (list[i].Exited) continue;
            ULONG nl = 0;
            while (list[i].Name[nl]) nl++;
            ULONG size = (rec + 2 * nl + 2 + 7) & ~7u;
            *(ULONG *)(p + 4) = list[i].Threads;                    /* NumberOfThreads */
            UNICODE_STRING *img = (UNICODE_STRING *)(p + 0x38);     /* ImageName (x64); x86: 0x38 too */
            WCHAR *w = (WCHAR *)(p + rec);
            for (ULONG k = 0; k < nl; k++) w[k] = (BYTE)list[i].Name[k];
            img->Length = (USHORT)(2 * nl);
            img->MaximumLength = (USHORT)(2 * nl + 2);
            img->Buffer = w;
            if (sizeof(PVOID) == 8) {
                *(ULONG_PTR *)(p + 0x50) = list[i].Pid;              /* UniqueProcessId */
                *(ULONG_PTR *)(p + 0x68) = (ULONG_PTR)list[i].MemoryKb * 1024;  /* PeakVirtualSize... */
                *(ULONG_PTR *)(p + 0x90) = (ULONG_PTR)list[i].MemoryKb * 1024;  /* WorkingSetSize */
            } else {
                *(ULONG_PTR *)(p + 0x44) = list[i].Pid;
                *(ULONG_PTR *)(p + 0x6C) = (ULONG_PTR)list[i].MemoryKb * 1024;
            }
            if (last) *(ULONG *)last = (ULONG)(p - last);           /* NextEntryOffset */
            last = p;
            p += size;
        }
        return ST_SUCCESS;
    }
    case 8: {                                                       /* SystemProcessorPerformanceInformation */
        ULONG n = cpu_count(), need = 48 * n;
        if (ret) *ret = need;
        if (len < need) return ST_INFO_LENGTH_MISMATCH;
        memset(buf, 0, need);
        return ST_SUCCESS;
    }
    default:
        if (ret) *ret = 0;
        return ST_INVALID_INFO_CLASS;
    }
}

NTSYSAPI NTSTATUS NTAPI NtQueryTimerResolution(PULONG max, PULONG min, PULONG cur)
{
    *max = 156250; *min = 5000; *cur = 156250;
    return ST_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI NtSetTimerResolution(ULONG want, BOOLEAN set, PULONG cur) { (void)want; (void)set; *cur = 156250; return ST_SUCCESS; }

/* -----------------------------------------------------------------------
 * What NovaOS answers without a kernel object
 * ----------------------------------------------------------------------- */
/* Transactions (TxF): none */
NTSYSAPI HANDLE NTAPI RtlGetCurrentTransaction(void) { return 0; }
NTSYSAPI BOOLEAN NTAPI RtlSetCurrentTransaction(HANDLE t) { return t == 0; }
NTSYSAPI NTSTATUS NTAPI NtCreateTransaction(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES oa, LPGUID uow, HANDLE tm,
                                            ULONG opt, ULONG iso, ULONG isof, PLARGE_INTEGER timeout, PUNICODE_STRING d)
{
    (void)a; (void)oa; (void)uow; (void)tm; (void)opt; (void)iso; (void)isof; (void)timeout; (void)d;
    *h = 0;
    return ST_NOT_SUPPORTED;
}
NTSYSAPI NTSTATUS NTAPI NtCommitTransaction(HANDLE h, BOOLEAN wait) { (void)h; (void)wait; return ST_NOT_SUPPORTED; }
NTSYSAPI NTSTATUS NTAPI NtRollbackTransaction(HANDLE h, BOOLEAN wait) { (void)h; (void)wait; return ST_NOT_SUPPORTED; }

/* Job objects: none */
NTSYSAPI NTSTATUS NTAPI NtCreateJobObject(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES oa) { (void)a; (void)oa; *h = 0; return ST_NOT_SUPPORTED; }
NTSYSAPI NTSTATUS NTAPI NtOpenJobObject(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES oa) { (void)a; (void)oa; *h = 0; return ST_OBJECT_NAME_NOT_FOUND; }
NTSYSAPI NTSTATUS NTAPI NtAssignProcessToJobObject(HANDLE j, HANDLE p) { (void)j; (void)p; return ST_NOT_SUPPORTED; }
NTSYSAPI NTSTATUS NTAPI NtQueryInformationJobObject(HANDLE j, ULONG c, PVOID b, ULONG n, PULONG r) { (void)j; (void)c; (void)b; (void)n; if (r) *r = 0; return ST_NOT_SUPPORTED; }
NTSYSAPI NTSTATUS NTAPI NtSetInformationJobObject(HANDLE j, ULONG c, PVOID b, ULONG n) { (void)j; (void)c; (void)b; (void)n; return ST_NOT_SUPPORTED; }
NTSYSAPI NTSTATUS NTAPI NtIsProcessInJob(HANDLE p, HANDLE j) { (void)p; (void)j; return 0x00000124; /* STATUS_PROCESS_NOT_IN_JOB */ }

/* Files: every write reaches the disk anyway; byte-range locks always
 * succeed (no one else holds one); no quotas; no volume labels to set */
NTSYSAPI NTSTATUS NTAPI NtFlushBuffersFile(HANDLE h, PIO_STATUS_BLOCK io) { (void)h; io->Status = ST_SUCCESS; io->Information = 0; return ST_SUCCESS; }
NTSYSAPI NTSTATUS NTAPI NtLockFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io, PLARGE_INTEGER off,
                                   PLARGE_INTEGER len, ULONG key, BOOLEAN fail_now, BOOLEAN exclusive)
{
    (void)h; (void)apc; (void)ctx; (void)off; (void)len; (void)key; (void)fail_now; (void)exclusive;
    io->Status = ST_SUCCESS;
    io->Information = 0;
    if (ev) NtSetEvent(ev, 0);
    return ST_SUCCESS;
}
NTSYSAPI NTSTATUS NTAPI NtUnlockFile(HANDLE h, PIO_STATUS_BLOCK io, PLARGE_INTEGER off, PLARGE_INTEGER len, ULONG key)
{
    (void)h; (void)off; (void)len; (void)key;
    io->Status = ST_SUCCESS;
    io->Information = 0;
    return ST_SUCCESS;
}
NTSYSAPI NTSTATUS NTAPI NtQueryQuotaInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID b, ULONG n, BOOLEAN single, PVOID sids,
                                                    ULONG sl, PSID start, BOOLEAN restart)
{
    (void)h; (void)io; (void)b; (void)n; (void)single; (void)sids; (void)sl; (void)start; (void)restart;
    return ST_INVALID_DEVICE_REQUEST;
}
NTSYSAPI NTSTATUS NTAPI NtSetQuotaInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID b, ULONG n)
{ (void)h; (void)io; (void)b; (void)n; return ST_INVALID_DEVICE_REQUEST; }
NTSYSAPI NTSTATUS NTAPI NtSetVolumeInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID b, ULONG n, ULONG c)
{ (void)h; (void)io; (void)b; (void)n; (void)c; return ST_INVALID_DEVICE_REQUEST; }

/* Memory is never paged out, so locking it in is a no-op */
NTSYSAPI NTSTATUS NTAPI NtLockVirtualMemory(HANDLE p, PVOID *base, PSIZE_T size, ULONG type) { (void)p; (void)base; (void)size; (void)type; return ST_SUCCESS; }
NTSYSAPI NTSTATUS NTAPI NtUnlockVirtualMemory(HANDLE p, PVOID *base, PSIZE_T size, ULONG type) { (void)p; (void)base; (void)size; (void)type; return ST_SUCCESS; }

/* Process debug information (module and heap lists for a debugger): none */
NTSYSAPI PVOID NTAPI RtlCreateQueryDebugBuffer(ULONG size, BOOLEAN event) { (void)size; (void)event; return 0; }
NTSYSAPI NTSTATUS NTAPI RtlDestroyQueryDebugBuffer(PVOID b) { (void)b; return ST_SUCCESS; }
NTSYSAPI NTSTATUS NTAPI RtlQueryProcessDebugInformation(HANDLE pid, ULONG flags, PVOID b) { (void)pid; (void)flags; (void)b; return ST_NOT_IMPLEMENTED; }

/* Cloud-file placeholders: NovaOS has none to hide or show */
NTSYSAPI CHAR NTAPI RtlSetProcessPlaceholderCompatibilityMode(CHAR mode) { (void)mode; return 2; /* PHCM_EXPOSE_PLACEHOLDERS */ }
NTSYSAPI CHAR NTAPI RtlQueryProcessPlaceholderCompatibilityMode(void) { return 2; }

/* -----------------------------------------------------------------------
 * User APCs: kernel32 keeps them (QueueUserAPC, completion routines) and
 * hands ntdll the routine that runs the calling thread's; NtTestAlert
 * runs them, as the loader does once the DLLs are initialized
 * ----------------------------------------------------------------------- */
static BOOL (*g_apc_runner)(void);
NTSYSAPI VOID NTAPI RtlNovaSetApcRunner(BOOL (*fn)(void)) { g_apc_runner = fn; }
NTSYSAPI NTSTATUS NTAPI NtTestAlert(void)
{
    if (g_apc_runner && g_apc_runner()) return 0x000000C0;       /* STATUS_USER_APC */
    return ST_SUCCESS;
}

/* Whether the process is shutting down (DLL_PROCESS_DETACH at exit) */
NTSYSAPI BOOLEAN NTAPI RtlDllShutdownInProgress(void) { return FALSE; }

/* Device I/O controls: no driver here answers them (pipes and the file
 * system use NtFsControlFile) */
NTSYSAPI NTSTATUS NTAPI NtDeviceIoControlFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io, ULONG code,
                                              PVOID in, ULONG in_len, PVOID out, ULONG out_len)
{
    (void)h; (void)ev; (void)apc; (void)ctx; (void)code; (void)in; (void)in_len; (void)out; (void)out_len;
    if (io) { io->Status = (NTSTATUS)0xC0000010; io->Information = 0; }
    return (NTSTATUS)0xC0000010;                         /* STATUS_INVALID_DEVICE_REQUEST */
}

/* The return addresses of the calling stack: @skip frames above this one,
 * at most @count; @hash (optional) gets their sum */
NTSYSAPI USHORT NTAPI RtlCaptureStackBackTrace(ULONG skip, ULONG count, PVOID *frames, PULONG hash)
{
    USHORT n = 0;
    ULONG sum = 0;
#ifdef _WIN64
    CONTEXT c;
    RtlCaptureContext(&c);
    for (ULONG i = 0; n < count && i < skip + count + 1 && c.Rip; i++) {
        DWORD64 base;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, 0);
        if (f) {
            PVOID hd; DWORD64 est;
            RtlVirtualUnwind(0, base, c.Rip, f, &c, &hd, &est, 0);
        } else {
            if (!c.Rsp) break;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
        if (!c.Rip) break;
        if (i >= skip) { frames[n++] = (PVOID)c.Rip; sum += (ULONG)c.Rip; }
    }
#else
    ULONG_PTR *fp = __builtin_frame_address(0);
    for (ULONG i = 0; n < count && fp && i < skip + count + 1; i++) {
        ULONG_PTR ret = fp[1];
        if (!ret) break;
        if (i >= skip) { frames[n++] = (PVOID)ret; sum += (ULONG)ret; }
        ULONG_PTR *next = (ULONG_PTR *)fp[0];
        if (next <= fp) break;
        fp = next;
    }
#endif
    if (hash) *hash = sum;
    return n;
}

/* The last NTSTATUS a failing call recorded (TEB LastStatusValue) */
NTSYSAPI NTSTATUS NTAPI RtlGetLastNtStatus(void)
{
#ifdef _WIN64
    return *(NTSTATUS *)(NtCurrentTebBytes() + 0x1250);
#else
    return *(NTSTATUS *)(NtCurrentTebBytes() + 0xBF4);
#endif
}
