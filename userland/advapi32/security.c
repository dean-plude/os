/*
 * security.c — advapi32's security API.
 *
 * NovaOS has a single user: every program runs as that user.  Tokens name
 * the user (S-1-5-21-…-1001, member of Users and Administrators, not
 * elevated), access checks evaluate the DACL they are given against that
 * token (ntdll's NtAccessCheck), and impersonation changes nothing.  Files
 * and directories on drive C: keep security descriptors, which the kernel
 * checks when they are opened (NtQuerySecurityObject and
 * NtSetSecurityObject); other objects are owned by the user and have no
 * DACL (full access).
 */

#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"

NTSYSAPI NTSTATUS NTAPI NtAccessCheck(PSECURITY_DESCRIPTOR sd, HANDLE token, ACCESS_MASK want, PGENERIC_MAPPING map,
                                      PPRIVILEGE_SET privs, PULONG privs_len, PACCESS_MASK granted, NTSTATUS *status);
NTSYSAPI NTSTATUS NTAPI NtQuerySecurityObject(HANDLE h, ULONG info, PSECURITY_DESCRIPTOR sd, ULONG len, PULONG need);
NTSYSAPI NTSTATUS NTAPI NtSetSecurityObject(HANDLE h, ULONG info, PSECURITY_DESCRIPTOR sd);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

static const BYTE g_user_sid[] = {                      /* S-1-5-21-1000-2000-3000-1001 */
    1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0xE8, 3, 0, 0, 0xD0, 7, 0, 0, 0xB8, 0x0B, 0, 0, 0xE9, 3, 0, 0 };
static const BYTE g_users_sid[] = { 1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x21, 2, 0, 0 };     /* S-1-5-32-545 */
static const BYTE g_admins_sid[] = { 1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 2, 0, 0 };    /* S-1-5-32-544 */
static const BYTE g_everyone_sid[] = { 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 };                    /* S-1-1-0 */
static const BYTE g_auth_users_sid[] = { 1, 1, 0, 0, 0, 0, 0, 5, 11, 0, 0, 0 };                 /* S-1-5-11 */
static const BYTE g_interactive_sid[] = { 1, 1, 0, 0, 0, 0, 0, 5, 4, 0, 0, 0 };                 /* S-1-5-4 */
static const BYTE g_medium_il_sid[] = { 1, 1, 0, 0, 0, 0, 0, 16, 0, 0x20, 0, 0 };               /* S-1-16-8192 */
static const BYTE g_system_sid[] = { 1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0 };                     /* S-1-5-18 */

/* -----------------------------------------------------------------------
 * SIDs
 * ----------------------------------------------------------------------- */
WINADVAPI BOOL WINAPI IsValidSid(PSID sid)
{
    const SID *s = sid;
    return s && s->Revision == 1 && s->SubAuthorityCount <= 15;
}

WINADVAPI DWORD WINAPI GetLengthSid(PSID sid) { return IsValidSid(sid) ? 8 + 4u * ((SID *)sid)->SubAuthorityCount : 0; }

WINADVAPI BOOL WINAPI EqualSid(PSID a, PSID b)
{
    if (!IsValidSid(a) || !IsValidSid(b)) return FALSE;
    DWORD n = GetLengthSid(a);
    if (n != GetLengthSid(b)) { SetLastError(0); return FALSE; }
    const BYTE *x = a, *y = b;
    for (DWORD i = 0; i < n; i++) if (x[i] != y[i]) { SetLastError(0); return FALSE; }
    return TRUE;
}

WINADVAPI BOOL WINAPI EqualPrefixSid(PSID a, PSID b)
{
    const SID *x = a, *y = b;
    if (!IsValidSid(a) || !IsValidSid(b) || x->SubAuthorityCount != y->SubAuthorityCount || !x->SubAuthorityCount) return FALSE;
    return !memcmp_(a, b, 8 + 4u * (x->SubAuthorityCount - 1));
}

WINADVAPI BOOL WINAPI CopySid(DWORD n, PSID dst, PSID src)
{
    DWORD len = GetLengthSid(src);
    if (!len) { SetLastError(ERROR_INVALID_SID); return FALSE; }
    if (n < len) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(dst, src, len);
    return TRUE;
}

WINADVAPI BOOL WINAPI InitializeSid(PSID sid, PSID_IDENTIFIER_AUTHORITY a, BYTE n)
{
    SID *s = sid;
    if (n > 15) return FALSE;
    s->Revision = 1;
    s->SubAuthorityCount = n;
    s->IdentifierAuthority = *a;
    memset(s->SubAuthority, 0, 4u * n);
    return TRUE;
}

WINADVAPI BOOL WINAPI AllocateAndInitializeSid(PSID_IDENTIFIER_AUTHORITY a, BYTE n, DWORD s0, DWORD s1, DWORD s2, DWORD s3,
                                               DWORD s4, DWORD s5, DWORD s6, DWORD s7, PSID *out)
{
    if (n > 8) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    SID *s = LocalAlloc(LMEM_ZEROINIT, 8 + 4u * n);
    if (!s) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    InitializeSid(s, a, n);
    DWORD v[8] = { s0, s1, s2, s3, s4, s5, s6, s7 };
    for (int i = 0; i < n; i++) s->SubAuthority[i] = v[i];
    *out = s;
    return TRUE;
}

WINADVAPI PVOID WINAPI FreeSid(PSID sid) { LocalFree(sid); return 0; }
WINADVAPI PSID_IDENTIFIER_AUTHORITY WINAPI GetSidIdentifierAuthority(PSID sid) { return &((SID *)sid)->IdentifierAuthority; }
WINADVAPI PDWORD WINAPI GetSidSubAuthority(PSID sid, DWORD i) { return &((SID *)sid)->SubAuthority[i]; }
WINADVAPI PUCHAR WINAPI GetSidSubAuthorityCount(PSID sid) { return &((SID *)sid)->SubAuthorityCount; }
WINADVAPI DWORD WINAPI GetSidLengthRequired(UCHAR n) { return 8 + 4u * n; }

/* "S-1-5-21-..." */
static int sid_to_string(PSID sid, char *out, int cap)
{
    const SID *s = sid;
    if (!IsValidSid(sid) || cap < 200) return 0;
    ULONGLONG auth = 0;
    for (int i = 0; i < 6; i++) auth = auth << 8 | s->IdentifierAuthority.Value[i];
    int o = 0;
    out[o++] = 'S'; out[o++] = '-'; out[o++] = '1';
    ULONGLONG parts[16];
    int np = 0;
    parts[np++] = auth;
    for (int i = 0; i < s->SubAuthorityCount; i++) parts[np++] = s->SubAuthority[i];
    for (int i = 0; i < np; i++) {
        char t[24];
        int k = 0;
        ULONGLONG v = parts[i];
        do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        out[o++] = '-';
        while (k) out[o++] = t[--k];
    }
    out[o] = 0;
    return o;
}

WINADVAPI BOOL WINAPI ConvertSidToStringSidA(PSID sid, LPSTR *out)
{
    char s[200];
    int n = sid_to_string(sid, s, sizeof(s));
    if (!n) { SetLastError(ERROR_INVALID_SID); return FALSE; }
    *out = LocalAlloc(0, (SIZE_T)n + 1);
    if (!*out) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    memcpy(*out, s, (SIZE_T)n + 1);
    return TRUE;
}

WINADVAPI BOOL WINAPI ConvertSidToStringSidW(PSID sid, LPWSTR *out)
{
    char s[200];
    int n = sid_to_string(sid, s, sizeof(s));
    if (!n) { SetLastError(ERROR_INVALID_SID); return FALSE; }
    *out = LocalAlloc(0, 2 * ((SIZE_T)n + 1));
    if (!*out) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (int i = 0; i <= n; i++) (*out)[i] = (WCHAR)s[i];
    return TRUE;
}

static BOOL string_to_sid(const char *s, PSID *out)
{
    /* well-known abbreviations */
    static const struct { const char *abbr; const BYTE *sid; } ab[] = {
        { "WD", g_everyone_sid }, { "BA", g_admins_sid }, { "BU", g_users_sid }, { "AU", g_auth_users_sid },
        { "IU", g_interactive_sid }, { "SY", g_system_sid }, { "ME", 0 },
    };
    const BYTE *known = 0;
    for (unsigned i = 0; i < sizeof(ab) / sizeof(ab[0]); i++)
        if (s[0] == ab[i].abbr[0] && s[1] == ab[i].abbr[1] && !s[2]) { known = ab[i].sid ? ab[i].sid : g_user_sid; break; }
    if (known) {
        DWORD n = GetLengthSid((PSID)known);
        *out = LocalAlloc(0, n);
        if (!*out) return FALSE;
        memcpy(*out, known, n);
        return TRUE;
    }
    if ((s[0] | 0x20) != 's' || s[1] != '-' || s[2] != '1' || s[3] != '-') { SetLastError(ERROR_INVALID_SID); return FALSE; }
    ULONGLONG v[17];
    int n = 0;
    for (const char *p = s + 4; *p && n < 17; ) {
        ULONGLONG x = 0;
        if (*p < '0' || *p > '9') { SetLastError(ERROR_INVALID_SID); return FALSE; }
        while (*p >= '0' && *p <= '9') x = x * 10 + (ULONGLONG)(*p++ - '0');
        v[n++] = x;
        if (*p == '-') p++;
        else if (*p) { SetLastError(ERROR_INVALID_SID); return FALSE; }
    }
    if (n < 1 || n > 16) { SetLastError(ERROR_INVALID_SID); return FALSE; }
    SID *sid = LocalAlloc(LMEM_ZEROINIT, 8 + 4u * (DWORD)(n - 1));
    if (!sid) return FALSE;
    sid->Revision = 1;
    sid->SubAuthorityCount = (BYTE)(n - 1);
    for (int i = 0; i < 6; i++) sid->IdentifierAuthority.Value[i] = (BYTE)(v[0] >> (8 * (5 - i)));
    for (int i = 1; i < n; i++) sid->SubAuthority[i - 1] = (DWORD)v[i];
    *out = sid;
    return TRUE;
}

WINADVAPI BOOL WINAPI ConvertStringSidToSidA(LPCSTR s, PSID *out) { return string_to_sid(s, out); }

WINADVAPI BOOL WINAPI ConvertStringSidToSidW(LPCWSTR s, PSID *out)
{
    char a[200];
    int i = 0;
    for (; s[i] && i < 199; i++) a[i] = (char)s[i];
    a[i] = 0;
    return string_to_sid(a, out);
}

/* well-known SID types (WELL_KNOWN_SID_TYPE) */
WINADVAPI BOOL WINAPI CreateWellKnownSid(int type, PSID domain, PSID out, DWORD *n)
{
    (void)domain;
    const BYTE *s = type == 1 ? g_everyone_sid : type == 17 ? g_auth_users_sid : type == 22 ? g_system_sid :
                    type == 26 ? g_admins_sid : type == 27 ? g_users_sid : type == 9 ? g_interactive_sid :
                    type == 66 ? g_medium_il_sid : 0;
    if (!s) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD len = GetLengthSid((PSID)s);
    if (!out || *n < len) { *n = len; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(out, s, len);
    *n = len;
    return TRUE;
}

WINADVAPI BOOL WINAPI IsWellKnownSid(PSID sid, int type)
{
    BYTE buf[SECURITY_MAX_SID_SIZE];
    DWORD n = sizeof(buf);
    return CreateWellKnownSid(type, 0, buf, &n) && EqualSid(sid, buf);
}

static const char *account_of(PSID sid, const char **domain, SID_NAME_USE *use)
{
    *domain = "BUILTIN";
    *use = SidTypeAlias;
    if (EqualSid(sid, (PSID)g_user_sid)) { *domain = "NOVAOS"; *use = SidTypeUser; return user_name(); }
    static const BYTE domain_sid[] = { 1, 4, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0xE8, 3, 0, 0, 0xD0, 7, 0, 0, 0xB8, 0x0B, 0, 0 };
    if (EqualSid(sid, (PSID)domain_sid)) { *domain = "NOVAOS"; *use = SidTypeDomain; return "NOVAOS"; }
    if (EqualSid(sid, (PSID)g_admins_sid)) return "Administrators";
    if (EqualSid(sid, (PSID)g_users_sid)) return "Users";
    *domain = "";
    *use = SidTypeWellKnownGroup;
    if (EqualSid(sid, (PSID)g_everyone_sid)) return "Everyone";
    *domain = "NT AUTHORITY";
    if (EqualSid(sid, (PSID)g_auth_users_sid)) return "Authenticated Users";
    if (EqualSid(sid, (PSID)g_interactive_sid)) return "INTERACTIVE";
    if (EqualSid(sid, (PSID)g_system_sid)) return "SYSTEM";
    return 0;
}

WINADVAPI BOOL WINAPI LookupAccountSidA(LPCSTR sys, PSID sid, LPSTR name, LPDWORD nn, LPSTR dom, LPDWORD nd, PSID_NAME_USE use)
{
    (void)sys;
    const char *d;
    SID_NAME_USE u;
    const char *n = account_of(sid, &d, &u);
    if (!n) { SetLastError(1332 /* ERROR_NONE_MAPPED */); return FALSE; }
    DWORD ln = (DWORD)strlen_(n), ld = (DWORD)strlen_(d);
    if (*nn <= ln || *nd <= ld || !name || !dom) { *nn = ln + 1; *nd = ld + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(name, n, ln + 1);
    memcpy(dom, d, ld + 1);
    *nn = ln; *nd = ld;
    if (use) *use = u;
    return TRUE;
}

WINADVAPI BOOL WINAPI LookupAccountSidW(LPCWSTR sys, PSID sid, LPWSTR name, LPDWORD nn, LPWSTR dom, LPDWORD nd, PSID_NAME_USE use)
{
    (void)sys;
    const char *d;
    SID_NAME_USE u;
    const char *n = account_of(sid, &d, &u);
    if (!n) { SetLastError(1332); return FALSE; }
    DWORD ln = (DWORD)strlen_(n), ld = (DWORD)strlen_(d);
    if (*nn <= ln || *nd <= ld || !name || !dom) { *nn = ln + 1; *nd = ld + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (DWORD i = 0; i <= ln; i++) name[i] = (WCHAR)(BYTE)n[i];
    for (DWORD i = 0; i <= ld; i++) dom[i] = (WCHAR)(BYTE)d[i];
    *nn = ln; *nd = ld;
    if (use) *use = u;
    return TRUE;
}

WINADVAPI BOOL WINAPI LookupAccountNameW(LPCWSTR sys, LPCWSTR name, PSID sid, LPDWORD ns, LPWSTR dom, LPDWORD nd, PSID_NAME_USE use)
{
    (void)sys; (void)name;
    DWORD len = GetLengthSid((PSID)g_user_sid);
    if (!sid || *ns < len || !dom || *nd < 7) { *ns = len; *nd = 7; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(sid, g_user_sid, len);
    const char *d = "NOVAOS";
    for (int i = 0; i < 7; i++) dom[i] = (WCHAR)d[i];
    *ns = len; *nd = 6;
    if (use) *use = SidTypeUser;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Tokens: an event handle stands in (so CloseHandle works); what it
 * describes is always the one user
 * ----------------------------------------------------------------------- */
static HANDLE new_token(void) { return CreateEventW(0, TRUE, TRUE, 0); }

WINADVAPI BOOL WINAPI OpenProcessToken(HANDLE p, DWORD access, PHANDLE token)
{
    (void)p; (void)access;
    *token = new_token();
    return *token != 0;
}

WINADVAPI BOOL WINAPI OpenThreadToken(HANDLE t, DWORD access, BOOL self, PHANDLE token)
{
    (void)t; (void)access; (void)self;
    *token = 0;
    SetLastError(1008 /* ERROR_NO_TOKEN: the thread is not impersonating */);
    return FALSE;
}

WINADVAPI BOOL WINAPI OpenThreadTokenEx(HANDLE t, DWORD access, BOOL self, DWORD attr, PHANDLE token)
{
    (void)attr;
    return OpenThreadToken(t, access, self, token);
}

WINADVAPI BOOL WINAPI DuplicateToken(HANDLE t, SECURITY_IMPERSONATION_LEVEL l, PHANDLE out)
{
    (void)t; (void)l;
    *out = new_token();
    return *out != 0;
}

WINADVAPI BOOL WINAPI DuplicateTokenEx(HANDLE t, DWORD access, LPSECURITY_ATTRIBUTES sa, SECURITY_IMPERSONATION_LEVEL l, int type, PHANDLE out)
{
    (void)access; (void)sa; (void)type;
    return DuplicateToken(t, l, out);
}

static BOOL put_info(const void *data, DWORD n, LPVOID buf, DWORD cap, PDWORD ret)
{
    if (ret) *ret = n;
    if (!buf || cap < n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(buf, data, n);
    return TRUE;
}

/* a SID_AND_ATTRIBUTES-style header followed by the SID(s) it points to */
static BOOL put_groups(const BYTE *const *sids, int n, LPVOID buf, DWORD cap, PDWORD ret, BOOL with_count)
{
    DWORD head = (with_count ? 8 : 0) + 16u * (DWORD)n, need = head;
    for (int i = 0; i < n; i++) need += GetLengthSid((PSID)sids[i]);
    if (ret) *ret = need;
    if (!buf || cap < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    BYTE *b = buf, *tail = b + head;
    SID_AND_ATTRIBUTES *sa = (SID_AND_ATTRIBUTES *)(b + (with_count ? 8 : 0));
    if (with_count) *(DWORD *)b = (DWORD)n;
    for (int i = 0; i < n; i++) {
        DWORD l = GetLengthSid((PSID)sids[i]);
        memcpy(tail, sids[i], l);
        sa[i].Sid = tail;
        sa[i].Attributes = with_count ? 7 /* MANDATORY | ENABLED_BY_DEFAULT | ENABLED */ : 0;
        tail += l;
    }
    return TRUE;
}

WINADVAPI BOOL WINAPI GetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS c, LPVOID buf, DWORD n, PDWORD ret)
{
    (void)token;
    DWORD v;
    switch (c) {
    case TokenUser: case TokenOwner: case TokenPrimaryGroup: {
        const BYTE *s[1] = { g_user_sid };
        if (c == TokenPrimaryGroup) s[0] = g_users_sid;
        if (c == TokenUser) return put_groups(s, 1, buf, n, ret, FALSE);
        /* TOKEN_OWNER / TOKEN_PRIMARY_GROUP: just a PSID, then the SID */
        DWORD l = GetLengthSid((PSID)s[0]);
        if (ret) *ret = 8 + l;
        if (!buf || n < 8 + l) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        memcpy((BYTE *)buf + 8, s[0], l);
        *(PSID *)buf = (BYTE *)buf + 8;
        return TRUE;
    }
    case TokenGroups: case TokenLogonSid: {
        const BYTE *g[] = { g_everyone_sid, g_users_sid, g_admins_sid, g_interactive_sid, g_auth_users_sid };
        return put_groups(g, c == TokenLogonSid ? 1 : 5, buf, n, ret, TRUE);
    }
    case TokenIntegrityLevel: {
        const BYTE *g[] = { g_medium_il_sid };
        if (!put_groups(g, 1, buf, n, ret, FALSE)) return FALSE;
        ((SID_AND_ATTRIBUTES *)buf)->Attributes = 0x20;       /* SE_GROUP_INTEGRITY */
        return TRUE;
    }
    case TokenPrivileges: {
        struct { DWORD n; LUID_AND_ATTRIBUTES p[1]; } tp = { 1, { { { 23, 0 }, 3 } } };   /* SeChangeNotifyPrivilege */
        return put_info(&tp, sizeof(tp), buf, n, ret);
    }
    case TokenElevation:     v = 0; return put_info(&v, 4, buf, n, ret);
    case TokenElevationType: v = 1; return put_info(&v, 4, buf, n, ret);   /* TokenElevationTypeDefault */
    case TokenType:          v = 1; return put_info(&v, 4, buf, n, ret);   /* TokenPrimary */
    case TokenSessionId:     v = 1; return put_info(&v, 4, buf, n, ret);
    case TokenIsAppContainer: case TokenHasRestrictions: case TokenUIAccess: case TokenVirtualizationAllowed:
    case TokenVirtualizationEnabled: case TokenSandBoxInert:
        v = 0; return put_info(&v, 4, buf, n, ret);
    case TokenImpersonationLevel: v = SecurityImpersonation; return put_info(&v, 4, buf, n, ret);
    case TokenMandatoryPolicy: v = 1; return put_info(&v, 4, buf, n, ret);  /* NO_WRITE_UP */
    case TokenStatistics: {
        BYTE st[56];
        memset(st, 0, sizeof(st));
        *(DWORD *)st = 0x1000 + GetCurrentProcessId();                      /* TokenId */
        *(DWORD *)(st + 8) = 0x3E7 + 1;                                     /* AuthenticationId */
        *(DWORD *)(st + 24) = 1;                                            /* TokenType: primary */
        return put_info(st, sizeof(st), buf, n, ret);
    }
    case TokenDefaultDacl: {
        PVOID z = 0;                                                        /* no default DACL */
        return put_info(&z, sizeof(z), buf, n, ret);
    }
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

WINADVAPI BOOL WINAPI SetTokenInformation(HANDLE t, TOKEN_INFORMATION_CLASS c, LPVOID buf, DWORD n) { (void)t; (void)c; (void)buf; (void)n; return TRUE; }

WINADVAPI BOOL WINAPI CheckTokenMembership(HANDLE token, PSID sid, PBOOL is_member)
{
    (void)token;
    *is_member = EqualSid(sid, (PSID)g_user_sid) || EqualSid(sid, (PSID)g_users_sid) || EqualSid(sid, (PSID)g_admins_sid) ||
                 EqualSid(sid, (PSID)g_everyone_sid) || EqualSid(sid, (PSID)g_auth_users_sid) || EqualSid(sid, (PSID)g_interactive_sid);
    return TRUE;
}

static const char *const g_privs[] = {
    0, 0, "SeCreateTokenPrivilege", "SeAssignPrimaryTokenPrivilege", "SeLockMemoryPrivilege", "SeIncreaseQuotaPrivilege",
    "SeMachineAccountPrivilege", "SeTcbPrivilege", "SeSecurityPrivilege", "SeTakeOwnershipPrivilege", "SeLoadDriverPrivilege",
    "SeSystemProfilePrivilege", "SeSystemtimePrivilege", "SeProfileSingleProcessPrivilege", "SeIncreaseBasePriorityPrivilege",
    "SeCreatePagefilePrivilege", "SeCreatePermanentPrivilege", "SeBackupPrivilege", "SeRestorePrivilege", "SeShutdownPrivilege",
    "SeDebugPrivilege", "SeAuditPrivilege", "SeSystemEnvironmentPrivilege", "SeChangeNotifyPrivilege", "SeRemoteShutdownPrivilege",
    "SeUndockPrivilege", "SeSyncAgentPrivilege", "SeEnableDelegationPrivilege", "SeManageVolumePrivilege",
    "SeImpersonatePrivilege", "SeCreateGlobalPrivilege", "SeTrustedCredManAccessPrivilege", "SeRelabelPrivilege",
    "SeIncreaseWorkingSetPrivilege", "SeTimeZonePrivilege", "SeCreateSymbolicLinkPrivilege",
};

static int ieq_(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if ((*a | 0x20) != (*b | 0x20)) return 0;
    return *a == *b;
}

WINADVAPI BOOL WINAPI LookupPrivilegeValueA(LPCSTR sys, LPCSTR name, PLUID luid)
{
    (void)sys;
    for (unsigned i = 2; i < sizeof(g_privs) / sizeof(g_privs[0]); i++)
        if (ieq_(g_privs[i], name)) { luid->LowPart = i; luid->HighPart = 0; return TRUE; }
    SetLastError(1313 /* ERROR_NO_SUCH_PRIVILEGE */);
    return FALSE;
}

WINADVAPI BOOL WINAPI LookupPrivilegeValueW(LPCWSTR sys, LPCWSTR name, PLUID luid)
{
    (void)sys;
    char a[64];
    int i = 0;
    for (; name[i] && i < 63; i++) a[i] = (char)name[i];
    a[i] = 0;
    return LookupPrivilegeValueA(0, a, luid);
}

WINADVAPI BOOL WINAPI LookupPrivilegeNameW(LPCWSTR sys, PLUID luid, LPWSTR name, LPDWORD n)
{
    (void)sys;
    if (luid->HighPart || luid->LowPart < 2 || luid->LowPart >= sizeof(g_privs) / sizeof(g_privs[0])) {
        SetLastError(1313); return FALSE;
    }
    const char *s = g_privs[luid->LowPart];
    DWORD l = (DWORD)strlen_(s);
    if (!name || *n <= l) { *n = l + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (DWORD i = 0; i <= l; i++) name[i] = (WCHAR)s[i];
    *n = l;
    return TRUE;
}

WINADVAPI BOOL WINAPI AdjustTokenPrivileges(HANDLE token, BOOL disable_all, PTOKEN_PRIVILEGES p, DWORD n, PTOKEN_PRIVILEGES prev, PDWORD ret)
{
    (void)token; (void)disable_all;
    if (prev && n >= 4) { prev->PrivilegeCount = 0; if (ret) *ret = 4; }
    (void)p;
    SetLastError(ERROR_SUCCESS);            /* everything is allowed anyway */
    return TRUE;
}

WINADVAPI BOOL WINAPI PrivilegeCheck(HANDLE token, PPRIVILEGE_SET ps, LPBOOL result) { (void)token; (void)ps; *result = TRUE; return TRUE; }
WINADVAPI BOOL WINAPI ImpersonateSelf(SECURITY_IMPERSONATION_LEVEL level) { (void)level; return TRUE; }
WINADVAPI BOOL WINAPI RevertToSelf(void) { return TRUE; }
WINADVAPI BOOL WINAPI ImpersonateLoggedOnUser(HANDLE t) { (void)t; return TRUE; }
WINADVAPI BOOL WINAPI ImpersonateAnonymousToken(HANDLE t) { (void)t; return TRUE; }
WINADVAPI BOOL WINAPI SetThreadToken(PHANDLE t, HANDLE token) { (void)t; (void)token; return TRUE; }

WINADVAPI BOOL WINAPI LogonUserW(LPCWSTR user, LPCWSTR domain, LPCWSTR pass, DWORD type, DWORD prov, PHANDLE token)
{
    (void)user; (void)domain; (void)pass; (void)type; (void)prov;
    *token = 0;
    SetLastError(1326 /* ERROR_LOGON_FAILURE */);
    return FALSE;
}

WINADVAPI BOOL WINAPI CreateProcessAsUserW(HANDLE token, LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                           BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si,
                                           LPPROCESS_INFORMATION pi)
{
    (void)token;
    return CreateProcessW(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}

WINADVAPI BOOL WINAPI CreateProcessWithTokenW(HANDLE token, DWORD logon, LPCWSTR app, LPWSTR cmd, DWORD flags, LPVOID env,
                                              LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    (void)token; (void)logon;
    return CreateProcessW(app, cmd, 0, 0, FALSE, flags, env, dir, si, pi);
}

/* -----------------------------------------------------------------------
 * User name
 * ----------------------------------------------------------------------- */
const char *user_name(void)
{
    static char name[64];
    if (!name[0]) {
        DWORD n = GetEnvironmentVariableA("USERNAME", name, sizeof(name));
        if (!n || n >= sizeof(name)) memcpy(name, "User", 5);
    }
    return name;
}

WINADVAPI BOOL WINAPI GetUserNameA(LPSTR buf, LPDWORD n)
{
    const char *u = user_name();
    DWORD l = (DWORD)strlen_(u);
    if (!buf || *n <= l) { *n = l + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(buf, u, l + 1);
    *n = l + 1;
    return TRUE;
}

WINADVAPI BOOL WINAPI GetUserNameW(LPWSTR buf, LPDWORD n)
{
    const char *u = user_name();
    int l = MultiByteToWideChar(CP_UTF8, 0, u, -1, 0, 0);
    if (!buf || *n < (DWORD)l) { *n = (DWORD)l; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    MultiByteToWideChar(CP_UTF8, 0, u, -1, buf, l);
    *n = (DWORD)l;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Security descriptors, ACLs, access checks
 * ----------------------------------------------------------------------- */
WINADVAPI BOOL WINAPI InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR sd, DWORD rev)
{
    if (rev != SECURITY_DESCRIPTOR_REVISION) { SetLastError(1305 /* ERROR_UNKNOWN_REVISION */); return FALSE; }
    memset(sd, 0, sizeof(SECURITY_DESCRIPTOR));
    ((SECURITY_DESCRIPTOR *)sd)->Revision = 1;
    return TRUE;
}

WINADVAPI BOOL WINAPI IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR sd) { return sd && ((SECURITY_DESCRIPTOR *)sd)->Revision == 1; }

/* self-relative descriptors keep offsets where absolute ones keep pointers */
static void *sd_part(PSECURITY_DESCRIPTOR sd, int which)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (s->Control & SE_SELF_RELATIVE) {
        DWORD off = ((DWORD *)((BYTE *)sd + 4))[which];
        return off ? (BYTE *)sd + off : 0;
    }
    return which == 0 ? s->Owner : which == 1 ? s->Group : which == 2 ? (void *)s->Sacl : (void *)s->Dacl;
}

WINADVAPI BOOL WINAPI SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd, BOOL present, PACL acl, BOOL defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (s->Control & SE_SELF_RELATIVE) { SetLastError(1336 /* ERROR_INVALID_SECURITY_DESCR */); return FALSE; }
    s->Control = (WORD)((s->Control & ~0x000C) | (present ? SE_DACL_PRESENT : 0) | (defaulted ? 0x0008 : 0));
    s->Dacl = present ? acl : 0;
    return TRUE;
}

WINADVAPI BOOL WINAPI GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd, LPBOOL present, PACL *acl, LPBOOL defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    *present = (s->Control & SE_DACL_PRESENT) != 0;
    *acl = *present ? sd_part(sd, 3) : 0;
    if (defaulted) *defaulted = (s->Control & 0x0008) != 0;
    return TRUE;
}

WINADVAPI BOOL WINAPI SetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID o, BOOL def)
{
    SECURITY_DESCRIPTOR *s = sd;
    s->Owner = o;
    s->Control = (WORD)((s->Control & ~1) | (def ? 1 : 0));
    return TRUE;
}

WINADVAPI BOOL WINAPI SetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID g, BOOL def)
{
    SECURITY_DESCRIPTOR *s = sd;
    s->Group = g;
    s->Control = (WORD)((s->Control & ~2) | (def ? 2 : 0));
    return TRUE;
}

WINADVAPI BOOL WINAPI GetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID *o, LPBOOL def)
{
    *o = sd_part(sd, 0);
    if (def) *def = (((SECURITY_DESCRIPTOR *)sd)->Control & 1) != 0;
    return TRUE;
}

WINADVAPI BOOL WINAPI GetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID *g, LPBOOL def)
{
    *g = sd_part(sd, 1);
    if (def) *def = (((SECURITY_DESCRIPTOR *)sd)->Control & 2) != 0;
    return TRUE;
}

WINADVAPI BOOL WINAPI SetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR sd, BOOL present, PACL acl, BOOL def)
{
    (void)def;
    SECURITY_DESCRIPTOR *s = sd;
    s->Sacl = present ? acl : 0;
    return TRUE;
}

WINADVAPI BOOL WINAPI GetSecurityDescriptorControl(PSECURITY_DESCRIPTOR sd, PSECURITY_DESCRIPTOR_CONTROL c, LPDWORD rev)
{
    *c = ((SECURITY_DESCRIPTOR *)sd)->Control;
    *rev = ((SECURITY_DESCRIPTOR *)sd)->Revision;
    return TRUE;
}

static DWORD acl_len(PACL a) { return a ? a->AclSize : 0; }

WINADVAPI DWORD WINAPI GetSecurityDescriptorLength(PSECURITY_DESCRIPTOR sd)
{
    DWORD n = 20;
    PSID o = sd_part(sd, 0), g = sd_part(sd, 1);
    n += (o ? GetLengthSid(o) : 0) + (g ? GetLengthSid(g) : 0) + acl_len(sd_part(sd, 2)) + acl_len(sd_part(sd, 3));
    return n;
}

/* A self-relative copy of @sd */
static BOOL make_self_relative(PSECURITY_DESCRIPTOR sd, BYTE *out, DWORD *n)
{
    DWORD need = GetSecurityDescriptorLength(sd);
    if (!out || *n < need) { *n = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset(out, 0, 20);
    out[0] = 1;
    WORD control = (WORD)(((SECURITY_DESCRIPTOR *)sd)->Control | SE_SELF_RELATIVE);
    memcpy(out + 2, &control, 2);
    DWORD off = 20;
    for (int i = 0; i < 4; i++) {
        void *p = sd_part(sd, i);
        if (!p) continue;
        DWORD l = i < 2 ? GetLengthSid(p) : acl_len(p);
        memcpy(out + off, p, l);
        memcpy(out + 4 + 4 * i, &off, 4);
        off += l;
    }
    *n = need;
    return TRUE;
}

WINADVAPI BOOL WINAPI MakeSelfRelativeSD(PSECURITY_DESCRIPTOR abs, PSECURITY_DESCRIPTOR rel, LPDWORD n)
{
    return make_self_relative(abs, rel, n);
}

WINADVAPI BOOL WINAPI InitializeAcl(PACL acl, DWORD n, DWORD rev)
{
    if (n < sizeof(ACL)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset(acl, 0, sizeof(ACL));
    acl->AclRevision = (BYTE)rev;
    acl->AclSize = (WORD)n;
    return TRUE;
}

WINADVAPI BOOL WINAPI IsValidAcl(PACL acl) { return acl && acl->AclRevision >= 2 && acl->AclRevision <= 4; }

static DWORD acl_used(PACL acl)
{
    DWORD o = sizeof(ACL);
    for (WORD i = 0; i < acl->AceCount; i++) o += ((ACE_HEADER *)((BYTE *)acl + o))->AceSize;
    return o;
}

static BOOL add_ace(PACL acl, BYTE type, DWORD flags, DWORD mask, PSID sid)
{
    DWORD sl = GetLengthSid(sid), size = 8 + sl, at = acl_used(acl);
    if (at + size > acl->AclSize) { SetLastError(1344 /* ERROR_ALLOTTED_SPACE_EXCEEDED */); return FALSE; }
    ACCESS_ALLOWED_ACE *a = (ACCESS_ALLOWED_ACE *)((BYTE *)acl + at);
    a->Header.AceType = type;
    a->Header.AceFlags = (BYTE)flags;
    a->Header.AceSize = (WORD)size;
    a->Mask = mask;
    memcpy(&a->SidStart, sid, sl);
    acl->AceCount++;
    return TRUE;
}

WINADVAPI BOOL WINAPI AddAccessAllowedAce(PACL acl, DWORD rev, DWORD mask, PSID sid) { (void)rev; return add_ace(acl, 0, 0, mask, sid); }
WINADVAPI BOOL WINAPI AddAccessAllowedAceEx(PACL acl, DWORD rev, DWORD f, DWORD mask, PSID sid) { (void)rev; return add_ace(acl, 0, f, mask, sid); }
WINADVAPI BOOL WINAPI AddAccessDeniedAce(PACL acl, DWORD rev, DWORD mask, PSID sid) { (void)rev; return add_ace(acl, 1, 0, mask, sid); }
WINADVAPI BOOL WINAPI AddAccessDeniedAceEx(PACL acl, DWORD rev, DWORD f, DWORD mask, PSID sid) { (void)rev; return add_ace(acl, 1, f, mask, sid); }
WINADVAPI BOOL WINAPI AddMandatoryAce(PACL acl, DWORD rev, DWORD f, DWORD policy, PSID sid) { (void)rev; return add_ace(acl, 0x11, f, policy, sid); }

WINADVAPI BOOL WINAPI GetAce(PACL acl, DWORD i, LPVOID *ace)
{
    if (i >= acl->AceCount) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD o = sizeof(ACL);
    for (DWORD k = 0; k < i; k++) o += ((ACE_HEADER *)((BYTE *)acl + o))->AceSize;
    *ace = (BYTE *)acl + o;
    return TRUE;
}

WINADVAPI BOOL WINAPI GetAclInformation(PACL acl, LPVOID info, DWORD n, int cls)
{
    if (cls == 1) {                                         /* AclRevisionInformation */
        if (n < 4) return FALSE;
        *(DWORD *)info = acl->AclRevision;
    } else {                                                /* AclSizeInformation */
        if (n < 12) return FALSE;
        DWORD used = acl_used(acl);
        ((DWORD *)info)[0] = acl->AceCount;
        ((DWORD *)info)[1] = used;
        ((DWORD *)info)[2] = acl->AclSize - used;
    }
    return TRUE;
}

WINADVAPI VOID WINAPI MapGenericMask(PDWORD mask, PGENERIC_MAPPING m)
{
    if (*mask & GENERIC_READ) *mask |= m->GenericRead;
    if (*mask & GENERIC_WRITE) *mask |= m->GenericWrite;
    if (*mask & GENERIC_EXECUTE) *mask |= m->GenericExecute;
    if (*mask & GENERIC_ALL) *mask |= m->GenericAll;
    *mask &= ~(DWORD)(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

WINADVAPI BOOL WINAPI AccessCheck(PSECURITY_DESCRIPTOR sd, HANDLE token, DWORD want, PGENERIC_MAPPING m, PPRIVILEGE_SET ps,
                                  LPDWORD psn, LPDWORD granted, LPBOOL status)
{
    NTSTATUS result;
    NTSTATUS s = NtAccessCheck(sd, token, want, m, ps, psn, granted, &result);   /* evaluates the DACL */
    if (!NT_SUCCESS(s)) { SetLastError(RtlNtStatusToDosError(s)); return FALSE; }
    *status = NT_SUCCESS(result);
    if (!*status) SetLastError(ERROR_ACCESS_DENIED);
    return TRUE;
}

/* The descriptor every object has: owned by the user, no DACL (full access) */
static PSECURITY_DESCRIPTOR default_sd(void)
{
    SECURITY_DESCRIPTOR abs;
    InitializeSecurityDescriptor(&abs, 1);
    abs.Owner = (PSID)g_user_sid;
    abs.Group = (PSID)g_users_sid;
    DWORD n = 0;
    make_self_relative(&abs, 0, &n);
    BYTE *sd = LocalAlloc(LMEM_ZEROINIT, n);
    if (sd) make_self_relative(&abs, sd, &n);
    return sd;
}

/* A file or directory, opened to read or change its descriptor */
static HANDLE open_named(LPCWSTR name, SECURITY_INFORMATION si, BOOL write)
{
    DWORD access = !write ? 0x00020000 /* READ_CONTROL */
                 : ((si & DACL_SECURITY_INFORMATION) ? 0x00040000 /* WRITE_DAC */ : 0) |
                   ((si & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION)) ? 0x00080000 /* WRITE_OWNER */ : 0);
    return CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
                       0x02000000 /* FILE_FLAG_BACKUP_SEMANTICS: directories too */, 0);
}

/* The descriptor of the object @h (LocalAlloc'd) */
static DWORD query_sd(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR *out)
{
    ULONG need = 0;
    NTSTATUS s = NtQuerySecurityObject(h, si & 7, 0, 0, &need);
    if (s != (NTSTATUS)0xC0000023 /* STATUS_BUFFER_TOO_SMALL */ && !NT_SUCCESS(s)) return RtlNtStatusToDosError(s);
    BYTE *sd = LocalAlloc(LMEM_ZEROINIT, need ? need : 20);
    if (!sd) return ERROR_NOT_ENOUGH_MEMORY;
    s = NtQuerySecurityObject(h, si & 7, sd, need, &need);
    if (!NT_SUCCESS(s)) { LocalFree(sd); return RtlNtStatusToDosError(s); }
    *out = sd;
    return ERROR_SUCCESS;
}

static DWORD security_info(HANDLE h, SECURITY_INFORMATION si, PSID *owner, PSID *group, PACL *dacl, PACL *sacl,
                           PSECURITY_DESCRIPTOR *sd)
{
    PSECURITY_DESCRIPTOR d;
    DWORD e = query_sd(h, si, &d);
    if (e) return e;
    if (owner) *owner = sd_part(d, 0);
    if (group) *group = sd_part(d, 1);
    if (dacl) *dacl = sd_part(d, 3);
    if (sacl) *sacl = 0;
    if (sd) *sd = d;
    else LocalFree(d);                      /* only the pointers were wanted: MSDN requires @sd with them */
    return ERROR_SUCCESS;
}

static DWORD named_info(LPCWSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID *owner, PSID *group,
                        PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd)
{
    if (t != SE_FILE_OBJECT) return security_info(0, si, owner, group, dacl, sacl, sd);   /* (the default one) */
    HANDLE h = open_named(name, si, FALSE);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD e = security_info(h, si, owner, group, dacl, sacl, sd);
    CloseHandle(h);
    return e;
}

WINADVAPI DWORD WINAPI GetNamedSecurityInfoW(LPCWSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID *owner, PSID *group,
                                             PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd)
{
    return named_info(name, t, si, owner, group, dacl, sacl, sd);
}

WINADVAPI DWORD WINAPI GetNamedSecurityInfoA(LPCSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID *owner, PSID *group,
                                             PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd)
{
    WCHAR w[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH)) return ERROR_INVALID_NAME;
    return named_info(w, t, si, owner, group, dacl, sacl, sd);
}

WINADVAPI DWORD WINAPI GetSecurityInfo(HANDLE h, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID *owner, PSID *group,
                                       PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd)
{
    (void)t;
    return security_info(h, si, owner, group, dacl, sacl, sd);
}

/* Give @h the parts @si names (a SACL is not kept) */
static DWORD set_parts(HANDLE h, SECURITY_INFORMATION si, PSID o, PSID g, PACL d)
{
    SECURITY_DESCRIPTOR abs;
    InitializeSecurityDescriptor(&abs, SECURITY_DESCRIPTOR_REVISION);
    if (si & OWNER_SECURITY_INFORMATION) abs.Owner = o;
    if (si & GROUP_SECURITY_INFORMATION) abs.Group = g;
    if (si & DACL_SECURITY_INFORMATION) SetSecurityDescriptorDacl(&abs, TRUE, d, FALSE);
    NTSTATUS s = NtSetSecurityObject(h, si & 7, &abs);
    return NT_SUCCESS(s) ? ERROR_SUCCESS : RtlNtStatusToDosError(s);
}

WINADVAPI DWORD WINAPI SetNamedSecurityInfoW(LPWSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID o, PSID g, PACL d, PACL s)
{
    (void)s;
    if (t != SE_FILE_OBJECT) return ERROR_SUCCESS;          /* (nowhere to keep one) */
    if (!(si & 7)) return ERROR_SUCCESS;
    HANDLE h = open_named(name, si, TRUE);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD e = set_parts(h, si, o, g, d);
    CloseHandle(h);
    return e;
}

WINADVAPI DWORD WINAPI SetSecurityInfo(HANDLE h, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID o, PSID g, PACL d, PACL s)
{
    (void)t; (void)s;
    return (si & 7) ? set_parts(h, si, o, g, d) : ERROR_SUCCESS;
}

WINADVAPI BOOL WINAPI GetFileSecurityW(LPCWSTR name, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, DWORD n, LPDWORD need)
{
    HANDLE h = open_named(name, si, FALSE);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ULONG len = 0;
    NTSTATUS s = NtQuerySecurityObject(h, si & 7, sd, n, &len);
    CloseHandle(h);
    *need = len;
    if (!NT_SUCCESS(s)) { SetLastError(RtlNtStatusToDosError(s)); return FALSE; }
    return TRUE;
}

WINADVAPI BOOL WINAPI SetFileSecurityW(LPCWSTR name, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd)
{
    if (!(si & 7)) return GetFileAttributesW(name) != INVALID_FILE_ATTRIBUTES;
    HANDLE h = open_named(name, si, TRUE);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    NTSTATUS s = NtSetSecurityObject(h, si & 7, sd);
    CloseHandle(h);
    if (!NT_SUCCESS(s)) { SetLastError(RtlNtStatusToDosError(s)); return FALSE; }
    return TRUE;
}

WINADVAPI BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorW(LPCWSTR s, DWORD rev, PSECURITY_DESCRIPTOR *sd, PULONG n)
{
    (void)s; (void)rev;                     /* the SDDL's rules would all allow access here */
    *sd = default_sd();
    if (!*sd) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (n) *n = GetSecurityDescriptorLength(*sd);
    return TRUE;
}

WINADVAPI BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorA(LPCSTR s, DWORD rev, PSECURITY_DESCRIPTOR *sd, PULONG n)
{
    (void)s;
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(0, rev, sd, n);
}

WINADVAPI BOOL WINAPI ConvertSecurityDescriptorToStringSecurityDescriptorW(PSECURITY_DESCRIPTOR sd, DWORD rev, SECURITY_INFORMATION si,
                                                                          LPWSTR *out, PULONG n)
{
    (void)sd; (void)rev; (void)si;
    static const char text[] = "O:S-1-5-21-1000-2000-3000-1001G:BU";
    *out = LocalAlloc(0, sizeof(text) * 2);
    if (!*out) return FALSE;
    for (unsigned i = 0; i < sizeof(text); i++) (*out)[i] = (WCHAR)text[i];
    if (n) *n = sizeof(text) - 1;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Credentials: none stored
 * ----------------------------------------------------------------------- */
WINADVAPI BOOL WINAPI CredReadW(LPCWSTR target, DWORD type, DWORD flags, PVOID *cred)
{
    (void)target; (void)type; (void)flags;
    *cred = 0;
    SetLastError(1168 /* ERROR_NOT_FOUND */);
    return FALSE;
}

WINADVAPI BOOL WINAPI CredWriteW(PVOID cred, DWORD flags)
{
    (void)cred; (void)flags;
    SetLastError(1312 /* ERROR_NO_SUCH_LOGON_SESSION: no credential store */);
    return FALSE;
}

WINADVAPI BOOL WINAPI CredDeleteW(LPCWSTR target, DWORD type, DWORD flags) { (void)target; (void)type; (void)flags; SetLastError(1168); return FALSE; }
WINADVAPI BOOL WINAPI CredEnumerateW(LPCWSTR filter, DWORD flags, DWORD *n, PVOID **creds) { (void)filter; (void)flags; *n = 0; *creds = 0; SetLastError(1168); return FALSE; }
WINADVAPI VOID WINAPI CredFree(PVOID p) { LocalFree(p); }

/* -----------------------------------------------------------------------
 * LSA policy: a standalone machine (workgroup WORKGROUP) whose account
 * domain is NOVAOS (S-1-5-21-1000-2000-3000), where the user's account
 * lives.  Account rights and private data cannot be changed.
 * ----------------------------------------------------------------------- */
#define STATUS_ACCESS_DENIED_ ((NTSTATUS)0xC0000022L)
#define STATUS_INVALID_HANDLE_ ((NTSTATUS)0xC0000008L)
#define STATUS_INVALID_PARAMETER_ ((NTSTATUS)0xC000000DL)
#define STATUS_OBJECT_NAME_NOT_FOUND_ ((NTSTATUS)0xC0000034L)
#define STATUS_NONE_MAPPED_ ((NTSTATUS)0xC0000073L)
typedef PVOID LSA_HANDLE, *PLSA_HANDLE;
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } LSA_US;
static const BYTE g_domain_sid[] = { 1, 4, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0xE8, 3, 0, 0, 0xD0, 7, 0, 0, 0xB8, 0x0B, 0, 0 };
static int g_lsa_policy;                                /* what a policy handle points at */

WINADVAPI NTSTATUS WINAPI LsaOpenPolicy(PVOID system, PVOID attrs, ACCESS_MASK access, PLSA_HANDLE h)
{
    (void)system; (void)attrs;
    if (!h) return STATUS_INVALID_PARAMETER_;
    /* reading is allowed; changing the policy is not */
    if (access & (0x00000020 | 0x00000040 | 0x00000080 | 0x00000100 | 0x00000200 | 0x00000400 | 0x00000800 | 0x00001000)) {
        *h = 0;
        return STATUS_ACCESS_DENIED_;
    }
    *h = &g_lsa_policy;
    return 0;
}
WINADVAPI NTSTATUS WINAPI LsaClose(LSA_HANDLE h) { return h ? 0 : STATUS_INVALID_HANDLE_; }
WINADVAPI NTSTATUS WINAPI LsaFreeMemory(PVOID p) { if (p) HeapFree(GetProcessHeap(), 0, p); return 0; }
WINADVAPI NTSTATUS WINAPI LsaAddAccountRights(LSA_HANDLE h, PSID sid, PVOID rights, ULONG n) { (void)h; (void)sid; (void)rights; (void)n; return STATUS_ACCESS_DENIED_; }
WINADVAPI NTSTATUS WINAPI LsaRemoveAccountRights(LSA_HANDLE h, PSID sid, BOOLEAN all, PVOID rights, ULONG n) { (void)h; (void)sid; (void)all; (void)rights; (void)n; return STATUS_ACCESS_DENIED_; }
WINADVAPI NTSTATUS WINAPI LsaEnumerateAccountRights(LSA_HANDLE h, PSID sid, PVOID *rights, PULONG n) { (void)h; (void)sid; if (rights) *rights = 0; if (n) *n = 0; return STATUS_OBJECT_NAME_NOT_FOUND_; }

/* Put @s as an LSA_UNICODE_STRING at @us, its characters at *@tail */
static void lsa_str(LSA_US *us, const char *s, BYTE **tail)
{
    WCHAR *w = (WCHAR *)*tail;
    int n = s ? (int)strlen_(s) : 0;
    for (int i = 0; i < n; i++) w[i] = (WCHAR)(BYTE)s[i];
    w[n] = 0;
    us->Buffer = w;
    us->Length = (USHORT)(2 * n);
    us->MaximumLength = (USHORT)(2 * n + 2);
    *tail += 2 * n + 2;
}

WINADVAPI NTSTATUS WINAPI LsaQueryInformationPolicy(LSA_HANDLE h, int cls, PVOID *buf)
{
    if (!h || !buf) return STATUS_INVALID_HANDLE_;
    *buf = 0;
    BYTE *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 512);
    if (!b) return (NTSTATUS)0xC0000017L;
    BYTE *tail = b + 96;
    switch (cls) {
    case 3:                                             /* PolicyPrimaryDomainInformation: { Name, Sid } */
        lsa_str((LSA_US *)b, "WORKGROUP", &tail);
        *(PSID *)(b + sizeof(LSA_US)) = 0;              /* no domain: not a member of one */
        break;
    case 5: {                                           /* PolicyAccountDomainInformation: { DomainName, DomainSid } */
        lsa_str((LSA_US *)b, "NOVAOS", &tail);
        memcpy(tail, g_domain_sid, sizeof(g_domain_sid));
        *(PSID *)(b + sizeof(LSA_US)) = tail;
        break;
    }
    case 12: {                                          /* PolicyDnsDomainInformation */
        /* { Name, DnsDomainName, DnsForestName, GUID DomainGuid, PSID Sid } */
        lsa_str((LSA_US *)b, "WORKGROUP", &tail);
        lsa_str((LSA_US *)(b + sizeof(LSA_US)), "", &tail);
        lsa_str((LSA_US *)(b + 2 * sizeof(LSA_US)), "", &tail);
        *(PSID *)(b + 3 * sizeof(LSA_US) + 16) = 0;
        break;
    }
    default:
        HeapFree(GetProcessHeap(), 0, b);
        return STATUS_INVALID_PARAMETER_;
    }
    *buf = b;
    return 0;
}

/* LsaLookupSids: LSA_REFERENCED_DOMAIN_LIST and LSA_TRANSLATED_NAME for each SID */
WINADVAPI NTSTATUS WINAPI LsaLookupSids(LSA_HANDLE h, ULONG n, PSID *sids, PVOID *domains, PVOID *names)
{
    (void)h;
    typedef struct { LSA_US Name; PSID Sid; } TRUST;
    typedef struct { ULONG Entries; TRUST *Domains; } DOMLIST;
    typedef struct { SID_NAME_USE Use; LSA_US Name; LONG DomainIndex; } NAME;
    DOMLIST *dl = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DOMLIST) + n * (sizeof(TRUST) + 160));
    NAME *nm = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * (sizeof(NAME) + 160) + 8);
    if (!dl || !nm) { HeapFree(GetProcessHeap(), 0, dl); HeapFree(GetProcessHeap(), 0, nm); return (NTSTATUS)0xC0000017L; }
    dl->Domains = (TRUST *)(dl + 1);
    BYTE *dtail = (BYTE *)(dl->Domains + n), *ntail = (BYTE *)(nm + n);
    ULONG mapped = 0;
    for (ULONG i = 0; i < n; i++) {
        const char *d;
        SID_NAME_USE u;
        const char *name = account_of(sids[i], &d, &u);
        nm[i].DomainIndex = -1;
        if (!name) { nm[i].Use = 8; /* SidTypeUnknown */ continue; }
        mapped++;
        nm[i].Use = u;
        lsa_str(&nm[i].Name, name, &ntail);
        ULONG k = 0;
        for (; k < dl->Entries; k++) {
            const WCHAR *w = dl->Domains[k].Name.Buffer;
            int j = 0;
            while (d[j] && w[j] == (WCHAR)(BYTE)d[j]) j++;
            if (!d[j] && !w[j]) break;
        }
        if (k == dl->Entries) {
            lsa_str(&dl->Domains[k].Name, d, &dtail);
            dl->Domains[k].Sid = 0;
            dl->Entries++;
        }
        nm[i].DomainIndex = (LONG)k;
    }
    *domains = dl;
    *names = nm;
    return !mapped ? STATUS_NONE_MAPPED_ : mapped < n ? (NTSTATUS)0x00000107L /* SOME_NOT_MAPPED */ : 0;
}

/* Private data (stored passwords): there is none, and none can be stored */
WINADVAPI NTSTATUS WINAPI LsaRetrievePrivateData(LSA_HANDLE h, PVOID key, PVOID *data) { (void)h; (void)key; if (data) *data = 0; return STATUS_OBJECT_NAME_NOT_FOUND_; }
WINADVAPI NTSTATUS WINAPI LsaStorePrivateData(LSA_HANDLE h, PVOID key, PVOID data) { (void)h; (void)key; (void)data; return STATUS_ACCESS_DENIED_; }

/* EFS: file encryption is not available */
WINADVAPI BOOL WINAPI EncryptFileW(LPCWSTR name) { (void)name; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
WINADVAPI BOOL WINAPI DecryptFileW(LPCWSTR name, DWORD r) { (void)name; (void)r; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
WINADVAPI BOOL WINAPI EncryptFileA(LPCSTR name) { (void)name; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
WINADVAPI BOOL WINAPI DecryptFileA(LPCSTR name, DWORD r) { (void)name; (void)r; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
WINADVAPI ULONG WINAPI LsaNtStatusToWinError(NTSTATUS s) { return RtlNtStatusToDosError(s); }

/* AddAce: ACEs already built (@list, @n bytes) go at the end of the ACL */
WINADVAPI BOOL WINAPI AddAce(PACL acl, DWORD rev, DWORD start, LPVOID list, DWORD n)
{
    (void)rev; (void)start;
    DWORD at = acl_used(acl), count = 0;
    if (at + n > acl->AclSize) { SetLastError(1344 /* ERROR_ALLOTTED_SPACE_EXCEEDED */); return FALSE; }
    for (DWORD o = 0; o + sizeof(ACE_HEADER) <= n; count++) {
        WORD sz = ((ACE_HEADER *)((BYTE *)list + o))->AceSize;
        if (sz < sizeof(ACE_HEADER)) break;
        o += sz;
    }
    memcpy((BYTE *)acl + at, list, n);
    acl->AceCount = (WORD)(acl->AceCount + count);
    return TRUE;
}

static BOOL a_to_w(LPCSTR a, WCHAR *w, int cap)
{
    if (!a) return FALSE;
    return MultiByteToWideChar(CP_ACP, 0, a, -1, w, cap) > 0;
}
WINADVAPI BOOL WINAPI GetFileSecurityA(LPCSTR name, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, DWORD n, LPDWORD need)
{
    WCHAR w[MAX_PATH];
    if (!a_to_w(name, w, MAX_PATH)) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    return GetFileSecurityW(w, si, sd, n, need);
}
WINADVAPI BOOL WINAPI SetFileSecurityA(LPCSTR name, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd)
{
    WCHAR w[MAX_PATH];
    if (!a_to_w(name, w, MAX_PATH)) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    return SetFileSecurityW(w, si, sd);
}
WINADVAPI BOOL WINAPI LookupAccountNameA(LPCSTR sys, LPCSTR name, PSID sid, LPDWORD ns, LPSTR dom, LPDWORD nd, PSID_NAME_USE use)
{
    (void)sys; (void)name;
    DWORD len = GetLengthSid((PSID)g_user_sid);
    if (!sid || *ns < len || !dom || *nd < 7) { *ns = len; *nd = 7; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(sid, g_user_sid, len);
    memcpy(dom, "NOVAOS", 7);
    *ns = len; *nd = 6;
    if (use) *use = SidTypeUser;
    return TRUE;
}
WINADVAPI BOOL WINAPI SetKernelObjectSecurity(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd)
{
    (void)si; (void)sd;
    return h != 0;
}
