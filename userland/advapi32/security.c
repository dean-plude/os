/*
 * security.c — advapi32's security API.
 *
 * NovaOS has one desktop user, but tokens and object security are real:
 * the kernel keeps tokens (the user, S-1-5-21-…-1001, member of Users,
 * with Administrators only for denying since nothing is elevated), a
 * thread can impersonate another token, restricted tokens can be made,
 * and named kernel objects keep the security descriptor they were created
 * with and check it when they are opened.  Files' descriptors are not
 * kept yet: they read as owned by the user with no DACL (full access).
 */

#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"

NTSYSAPI NTSTATUS NTAPI NtAccessCheck(PSECURITY_DESCRIPTOR sd, HANDLE token, ACCESS_MASK want, PGENERIC_MAPPING map,
                                      PPRIVILEGE_SET privs, PULONG privs_len, PACCESS_MASK granted, NTSTATUS *status);
NTSYSAPI NTSTATUS NTAPI NtOpenProcessToken(HANDLE p, ACCESS_MASK access, PHANDLE token);
NTSYSAPI NTSTATUS NTAPI NtOpenThreadToken(HANDLE t, ACCESS_MASK access, BOOLEAN self, PHANDLE token);
NTSYSAPI NTSTATUS NTAPI NtOpenThreadTokenEx(HANDLE t, ACCESS_MASK access, BOOLEAN self, ULONG attrs, PHANDLE token);
NTSYSAPI NTSTATUS NTAPI NtDuplicateToken(HANDLE t, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, BOOLEAN effective, TOKEN_TYPE type, PHANDLE out);
NTSYSAPI NTSTATUS NTAPI NtFilterToken(HANDLE t, ULONG flags, PTOKEN_GROUPS disable, PTOKEN_PRIVILEGES del, PTOKEN_GROUPS restrict_sids, PHANDLE out);
NTSYSAPI NTSTATUS NTAPI NtQueryInformationToken(HANDLE t, ULONG cls, PVOID buf, ULONG n, PULONG ret);
NTSYSAPI NTSTATUS NTAPI NtImpersonateAnonymousToken(HANDLE thread);
NTSYSAPI NTSTATUS NTAPI NtQuerySecurityObject(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, ULONG len, PULONG need);
NTSYSAPI NTSTATUS NTAPI NtSetSecurityObject(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd);
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
/* WELL_KNOWN_SID_TYPE: identifier authority and sub-authorities; dom marks
 * the ones relative to a domain SID (the machine's when none is given) */
static const struct { BYTE type, auth, dom, n; DWORD sub[2]; } g_known[] = {
    {  0,  0, 0, 1, { 0 } },      {  1,  1, 0, 1, { 0 } },      {  2,  2, 0, 1, { 0 } },
    {  3,  3, 0, 1, { 0 } },      {  4,  3, 0, 1, { 1 } },      {  5,  3, 0, 1, { 2 } },
    {  6,  3, 0, 1, { 3 } },      {  7,  5, 0, 0, { 0 } },      {  8,  5, 0, 1, { 1 } },
    {  9,  5, 0, 1, { 2 } },      { 10,  5, 0, 1, { 3 } },      { 11,  5, 0, 1, { 4 } },
    { 12,  5, 0, 1, { 6 } },      { 13,  5, 0, 1, { 7 } },      { 14,  5, 0, 1, { 8 } },
    { 15,  5, 0, 1, { 9 } },      { 16,  5, 0, 1, { 10 } },     { 17,  5, 0, 1, { 11 } },
    { 18,  5, 0, 1, { 12 } },     { 19,  5, 0, 1, { 13 } },     { 20,  5, 0, 1, { 14 } },
    { 21,  5, 0, 1, { 5 } },      { 22,  5, 0, 1, { 18 } },     { 23,  5, 0, 1, { 19 } },
    { 24,  5, 0, 1, { 20 } },     { 25,  5, 0, 1, { 32 } },
    { 26,  5, 0, 2, { 32, 544 } }, { 27, 5, 0, 2, { 32, 545 } }, { 28, 5, 0, 2, { 32, 546 } },
    { 29,  5, 0, 2, { 32, 547 } }, { 30, 5, 0, 2, { 32, 548 } }, { 31, 5, 0, 2, { 32, 549 } },
    { 32,  5, 0, 2, { 32, 550 } }, { 33, 5, 0, 2, { 32, 551 } }, { 34, 5, 0, 2, { 32, 552 } },
    { 35,  5, 0, 2, { 32, 554 } }, { 36, 5, 0, 2, { 32, 555 } }, { 37, 5, 0, 2, { 32, 556 } },
    { 38,  5, 1, 1, { 500 } },    { 39,  5, 1, 1, { 501 } },    { 40,  5, 1, 1, { 502 } },
    { 41,  5, 1, 1, { 512 } },    { 42,  5, 1, 1, { 513 } },    { 43,  5, 1, 1, { 514 } },
    { 44,  5, 1, 1, { 515 } },    { 45,  5, 1, 1, { 516 } },    { 46,  5, 1, 1, { 517 } },
    { 47,  5, 1, 1, { 518 } },    { 48,  5, 1, 1, { 519 } },    { 49,  5, 1, 1, { 520 } },
    { 50,  5, 1, 1, { 553 } },    { 51,  5, 0, 2, { 64, 10 } }, { 52,  5, 0, 2, { 64, 21 } },
    { 53,  5, 0, 2, { 64, 14 } }, { 54,  5, 0, 1, { 15 } },     { 55,  5, 0, 1, { 1000 } },
    { 56,  5, 0, 2, { 32, 557 } }, { 57, 5, 0, 2, { 32, 558 } }, { 58, 5, 0, 2, { 32, 559 } },
    { 59,  5, 0, 2, { 32, 560 } }, { 60, 5, 0, 2, { 32, 561 } }, { 61, 5, 0, 2, { 32, 562 } },
    { 62,  5, 0, 2, { 32, 568 } }, { 63, 5, 0, 1, { 17 } },     { 64,  5, 0, 2, { 32, 569 } },
    { 65, 16, 0, 1, { 0 } },      { 66, 16, 0, 1, { 4096 } },   { 67, 16, 0, 1, { 8192 } },
    { 68, 16, 0, 1, { 12288 } },  { 69, 16, 0, 1, { 16384 } },  { 70,  5, 0, 1, { 33 } },
    { 71,  3, 0, 1, { 4 } },      { 72,  5, 1, 1, { 571 } },    { 73,  5, 1, 1, { 572 } },
    { 74,  5, 1, 1, { 498 } },    { 75,  5, 1, 1, { 521 } },    { 76,  5, 0, 2, { 32, 573 } },
    { 77,  5, 1, 1, { 498 } },    { 78,  5, 0, 2, { 32, 574 } }, { 79, 16, 0, 1, { 8448 } },
    { 80,  2, 0, 1, { 0 } },      { 81,  2, 0, 1, { 1 } },      { 82,  5, 0, 2, { 65, 1 } },
    { 83, 15, 0, 1, { 2 } },      { 84, 15, 0, 2, { 2, 1 } },   { 85, 15, 0, 2, { 3, 1 } },
    { 86, 15, 0, 2, { 3, 2 } },   { 87, 15, 0, 2, { 3, 3 } },   { 88, 15, 0, 2, { 3, 4 } },
    { 89, 15, 0, 2, { 3, 5 } },   { 90, 15, 0, 2, { 3, 6 } },   { 91, 15, 0, 2, { 3, 7 } },
    { 92, 15, 0, 2, { 3, 9 } },   { 93, 15, 0, 2, { 3, 8 } },   { 94, 15, 0, 2, { 3, 10 } },
};

WINADVAPI BOOL WINAPI CreateWellKnownSid(int type, PSID domain, PSID out, DWORD *n)
{
    static const BYTE machine[] = { 1, 4, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0xE8, 3, 0, 0, 0xD0, 7, 0, 0, 0xB8, 0x0B, 0, 0 };
    int k = -1;
    for (int i = 0; i < (int)(sizeof(g_known) / sizeof(g_known[0])); i++) if (g_known[i].type == type) k = i;
    if (k < 0 || !n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    BYTE sid[SECURITY_MAX_SID_SIZE];
    memset(sid, 0, sizeof(sid));
    sid[0] = 1;
    if (g_known[k].dom) {                               /* the domain's SID, then the RID */
        const BYTE *d = domain ? (const BYTE *)domain : machine;
        if (!IsValidSid((PSID)d) || d[1] > 14) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        memcpy(sid, d, GetLengthSid((PSID)d));
    } else {
        sid[7] = g_known[k].auth;
    }
    for (int i = 0; i < g_known[k].n; i++) memcpy(sid + 8 + 4 * sid[1]++, &g_known[k].sub[i], 4);
    DWORD len = 8 + 4u * sid[1];
    if (!out || *n < len) { *n = len; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(out, sid, len);
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
 * Tokens: kernel objects (NtOpenProcessToken and friends).  A process runs
 * with the token of the program that started it; a thread can impersonate
 * another (an impersonation token) and access checks use that.
 * ----------------------------------------------------------------------- */
static BOOL nt_ok(NTSTATUS s)
{
    if (NT_SUCCESS(s)) return TRUE;
    SetLastError(RtlNtStatusToDosError(s));
    return FALSE;
}

WINADVAPI BOOL WINAPI OpenProcessToken(HANDLE p, DWORD access, PHANDLE token)
{
    return nt_ok(NtOpenProcessToken(p, access, token));
}

WINADVAPI BOOL WINAPI OpenThreadToken(HANDLE t, DWORD access, BOOL self, PHANDLE token)
{
    return nt_ok(NtOpenThreadToken(t, access, (BOOLEAN)self, token));   /* ERROR_NO_TOKEN: not impersonating */
}

WINADVAPI BOOL WINAPI OpenThreadTokenEx(HANDLE t, DWORD access, BOOL self, DWORD attr, PHANDLE token)
{
    return nt_ok(NtOpenThreadTokenEx(t, access, (BOOLEAN)self, attr, token));
}

WINADVAPI BOOL WINAPI DuplicateTokenEx(HANDLE t, DWORD access, LPSECURITY_ATTRIBUTES sa, SECURITY_IMPERSONATION_LEVEL l, int type, PHANDLE out)
{
    SECURITY_QUALITY_OF_SERVICE qos = { sizeof(qos), l, 0, FALSE };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.Attributes = sa && sa->bInheritHandle ? OBJ_INHERIT : 0;
    oa.SecurityDescriptor = sa ? sa->lpSecurityDescriptor : 0;
    oa.SecurityQualityOfService = &qos;
    return nt_ok(NtDuplicateToken(t, access, &oa, FALSE, (TOKEN_TYPE)type, out));
}

WINADVAPI BOOL WINAPI DuplicateToken(HANDLE t, SECURITY_IMPERSONATION_LEVEL l, PHANDLE out)
{
    return DuplicateTokenEx(t, TOKEN_IMPERSONATE | TOKEN_QUERY, 0, l, TokenImpersonation, out);
}

WINADVAPI BOOL WINAPI GetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS c, LPVOID buf, DWORD n, PDWORD ret)
{
    ULONG got = 0;
    NTSTATUS s = NtQueryInformationToken(token, (ULONG)c, buf, n, &got);
    if (ret) *ret = got;
    if (s == (NTSTATUS)0xC0000023L) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }   /* BUFFER_TOO_SMALL */
    return nt_ok(s);
}

WINADVAPI BOOL WINAPI SetTokenInformation(HANDLE t, TOKEN_INFORMATION_CLASS c, LPVOID buf, DWORD n) { (void)t; (void)c; (void)buf; (void)n; return TRUE; }

/* The thread acts as @token (an impersonation token: a primary one is
 * copied into one first), or as itself again (0) */
static BOOL impersonate(HANDLE thread, HANDLE token)
{
    HANDLE imp = token;
    DWORD type = TokenImpersonation, n;
    if (token && GetTokenInformation(token, TokenType, &type, sizeof(type), &n) && type == TokenPrimary &&
        !DuplicateTokenEx(token, TOKEN_IMPERSONATE | TOKEN_QUERY, 0, SecurityImpersonation, TokenImpersonation, &imp))
        return FALSE;
    BOOL ok = nt_ok(NtSetInformationThread(thread, 5 /* ThreadImpersonationToken */, &imp, sizeof(imp)));
    if (imp != token) CloseHandle(imp);
    return ok;
}

WINADVAPI BOOL WINAPI ImpersonateSelf(SECURITY_IMPERSONATION_LEVEL level)
{
    HANDLE p, t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &p)) return FALSE;
    BOOL ok = DuplicateTokenEx(p, TOKEN_IMPERSONATE | TOKEN_QUERY, 0, level, TokenImpersonation, &t);
    CloseHandle(p);
    if (!ok) return FALSE;
    ok = impersonate(GetCurrentThread(), t);
    CloseHandle(t);
    return ok;
}
WINADVAPI BOOL WINAPI RevertToSelf(void) { return impersonate(GetCurrentThread(), 0); }
WINADVAPI BOOL WINAPI ImpersonateLoggedOnUser(HANDLE t) { return impersonate(GetCurrentThread(), t); }
WINADVAPI BOOL WINAPI ImpersonateAnonymousToken(HANDLE t) { return nt_ok(NtImpersonateAnonymousToken(t)); }
WINADVAPI BOOL WINAPI SetThreadToken(PHANDLE t, HANDLE token) { return impersonate(t ? *t : GetCurrentThread(), token); }

/* Whether @token (an impersonation token; NULL: the thread's, else the
 * process's) has @sid enabled: what an ACE naming @sid would grant it */
WINADVAPI BOOL WINAPI CheckTokenMembership(HANDLE token, PSID sid, PBOOL is_member)
{
    HANDLE t = token, p;
    if (!sid || !is_member || !IsValidSid(sid)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *is_member = FALSE;
    if (!t && !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t)) {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &p)) return FALSE;
        BOOL ok = DuplicateToken(p, SecurityIdentification, &t);
        CloseHandle(p);
        if (!ok) return FALSE;
    }
    BYTE acl_buf[sizeof(ACL) + 8 + SECURITY_MAX_SID_SIZE];
    PACL acl = (PACL)acl_buf;
    SECURITY_DESCRIPTOR sd;
    InitializeAcl(acl, sizeof(acl_buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, 1, sid);
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
    SetSecurityDescriptorOwner(&sd, (PSID)g_system_sid, FALSE);
    SetSecurityDescriptorGroup(&sd, (PSID)g_system_sid, FALSE);
    GENERIC_MAPPING map = { 1, 1, 1, 1 };
    struct { PRIVILEGE_SET s; LUID_AND_ATTRIBUTES more[3]; } ps;
    DWORD psn = sizeof(ps), granted = 0;
    BOOL status = FALSE;
    BOOL ok = AccessCheck(&sd, t, 1, &map, &ps.s, &psn, &granted, &status);
    if (t != token) CloseHandle(t);
    if (!ok) return FALSE;
    *is_member = status && granted == 1;
    return TRUE;
}

/* A copy of @t with SIDs made deny-only, privileges removed and SIDs to
 * restrict it (an access needs both its groups and the restricting SIDs) */
WINADVAPI BOOL WINAPI CreateRestrictedToken(HANDLE t, DWORD flags, DWORD ndisable, PSID_AND_ATTRIBUTES disable, DWORD ndelete,
                                            PLUID_AND_ATTRIBUTES del, DWORD nrestrict, PSID_AND_ATTRIBUTES restricted, PHANDLE out)
{
    if (!out || ndisable > 64 || ndelete > 64 || nrestrict > 64) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD head = __builtin_offsetof(TOKEN_GROUPS, Groups);
    HANDLE heap = GetProcessHeap();
    TOKEN_GROUPS *d = 0, *r = 0;
    TOKEN_PRIVILEGES *p = 0;
    if (ndisable) d = HeapAlloc(heap, 0, head + ndisable * sizeof(SID_AND_ATTRIBUTES));
    if (nrestrict) r = HeapAlloc(heap, 0, head + nrestrict * sizeof(SID_AND_ATTRIBUTES));
    if (ndelete) p = HeapAlloc(heap, 0, __builtin_offsetof(TOKEN_PRIVILEGES, Privileges) + ndelete * sizeof(LUID_AND_ATTRIBUTES));
    BOOL ok = (!ndisable || d) && (!nrestrict || r) && (!ndelete || p);
    if (!ok) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    else {
        if (d) { d->GroupCount = ndisable; memcpy(d->Groups, disable, ndisable * sizeof(SID_AND_ATTRIBUTES)); }
        if (r) { r->GroupCount = nrestrict; memcpy(r->Groups, restricted, nrestrict * sizeof(SID_AND_ATTRIBUTES)); }
        if (p) { p->PrivilegeCount = ndelete; memcpy(p->Privileges, del, ndelete * sizeof(LUID_AND_ATTRIBUTES)); }
        ok = nt_ok(NtFilterToken(t, flags, d, p, r, out));
    }
    HeapFree(heap, 0, d);
    HeapFree(heap, 0, r);
    HeapFree(heap, 0, p);
    return ok;
}

WINADVAPI BOOL WINAPI IsTokenRestricted(HANDLE t)
{
    DWORD v = 0, n;
    if (!GetTokenInformation(t, TokenHasRestrictions, &v, sizeof(v), &n)) return FALSE;
    SetLastError(ERROR_SUCCESS);
    return v != 0;
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

static DWORD security_info(SECURITY_INFORMATION si, PSID *owner, PSID *group, PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd)
{
    (void)si;
    PSECURITY_DESCRIPTOR d = default_sd();
    if (!d) return ERROR_NOT_ENOUGH_MEMORY;
    if (owner) *owner = sd_part(d, 0);
    if (group) *group = sd_part(d, 1);
    if (dacl) *dacl = 0;
    if (sacl) *sacl = 0;
    if (sd) *sd = d;
    else LocalFree(d);                      /* only the pointers were wanted: MSDN requires @sd with them */
    return ERROR_SUCCESS;
}

WINADVAPI DWORD WINAPI GetNamedSecurityInfoW(LPCWSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID *owner, PSID *group,
                                             PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd)
{
    if (t == SE_FILE_OBJECT && GetFileAttributesW(name) == INVALID_FILE_ATTRIBUTES) return GetLastError();
    return security_info(si, owner, group, dacl, sacl, sd);
}

WINADVAPI DWORD WINAPI GetNamedSecurityInfoA(LPCSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID *owner, PSID *group,
                                             PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd)
{
    if (t == SE_FILE_OBJECT && GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES) return GetLastError();
    return security_info(si, owner, group, dacl, sacl, sd);
}

/* A kernel object's descriptor (self-relative, LocalAlloc'd) */
static DWORD object_sd(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR *out)
{
    ULONG need = 0;
    NTSTATUS st = NtQuerySecurityObject(h, si, 0, 0, &need);
    for (int tries = 0; tries < 4 && st == (NTSTATUS)0xC0000023L; tries++) {   /* BUFFER_TOO_SMALL */
        BYTE *b = LocalAlloc(LMEM_ZEROINIT, need);
        if (!b) return ERROR_NOT_ENOUGH_MEMORY;
        st = NtQuerySecurityObject(h, si, b, need, &need);
        if (NT_SUCCESS(st)) { *out = b; return ERROR_SUCCESS; }
        LocalFree(b);
    }
    return NT_SUCCESS(st) ? 1338 /* ERROR_INVALID_SECURITY_DESCR */ : RtlNtStatusToDosError(st);
}

WINADVAPI DWORD WINAPI GetSecurityInfo(HANDLE h, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID *owner, PSID *group,
                                       PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd)
{
    /* NovaOS's one window station and desktop are user32 pseudo handles,
     * not kernel objects: they carry the default descriptor */
    if (t == SE_WINDOW_OBJECT) return security_info(si, owner, group, dacl, sacl, sd);
    PSECURITY_DESCRIPTOR d;
    DWORD e = object_sd(h, si, &d);
    if (e) return e;
    if (owner) *owner = sd_part(d, 0);
    if (group) *group = sd_part(d, 1);
    if (dacl) *dacl = sd_part(d, 3);
    if (sacl) *sacl = 0;
    if (sd) *sd = d;
    else LocalFree(d);                      /* only the pointers were wanted: MSDN requires @sd with them */
    return ERROR_SUCCESS;
}

WINADVAPI BOOL WINAPI GetKernelObjectSecurity(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, DWORD n, LPDWORD need)
{
    ULONG got = 0;
    NTSTATUS st = NtQuerySecurityObject(h, si, sd, n, &got);
    if (need) *need = got;
    if (st == (NTSTATUS)0xC0000023L) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    if (!NT_SUCCESS(st)) { SetLastError(RtlNtStatusToDosError(st)); return FALSE; }
    return TRUE;
}

WINADVAPI DWORD WINAPI SetNamedSecurityInfoW(LPWSTR name, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID o, PSID g, PACL d, PACL s)
{
    (void)t; (void)si; (void)o; (void)g; (void)d; (void)s;
    return GetFileAttributesW(name) == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
}

WINADVAPI DWORD WINAPI SetSecurityInfo(HANDLE h, SE_OBJECT_TYPE t, SECURITY_INFORMATION si, PSID o, PSID g, PACL d, PACL s)
{
    (void)s;                                /* (SACLs are not kept) */
    if (t == SE_WINDOW_OBJECT) return ERROR_SUCCESS;    /* (see GetSecurityInfo) */
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    if (si & OWNER_SECURITY_INFORMATION) sd.Owner = o;
    if (si & GROUP_SECURITY_INFORMATION) sd.Group = g;
    if (si & DACL_SECURITY_INFORMATION) SetSecurityDescriptorDacl(&sd, TRUE, d, FALSE);
    NTSTATUS st = NtSetSecurityObject(h, si & 7, &sd);
    return NT_SUCCESS(st) ? ERROR_SUCCESS : RtlNtStatusToDosError(st);
}

WINADVAPI BOOL WINAPI GetFileSecurityW(LPCWSTR name, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, DWORD n, LPDWORD need)
{
    (void)si;
    if (GetFileAttributesW(name) == INVALID_FILE_ATTRIBUTES) return FALSE;
    PSECURITY_DESCRIPTOR d = default_sd();
    if (!d) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    DWORD len = GetSecurityDescriptorLength(d);
    *need = len;
    BOOL ok = sd && n >= len;
    if (ok) memcpy(sd, d, len);
    else SetLastError(ERROR_INSUFFICIENT_BUFFER);
    LocalFree(d);
    return ok;
}

WINADVAPI BOOL WINAPI SetFileSecurityW(LPCWSTR name, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd)
{
    (void)si; (void)sd;
    return GetFileAttributesW(name) != INVALID_FILE_ATTRIBUTES;
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
    NTSTATUS st = NtSetSecurityObject(h, si, sd);
    if (!NT_SUCCESS(st)) { SetLastError(RtlNtStatusToDosError(st)); return FALSE; }
    return TRUE;
}

WINADVAPI BOOL WINAPI GetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR sd, LPBOOL present, PACL *acl, LPBOOL defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    *present = (s->Control & 0x0010 /* SE_SACL_PRESENT */) != 0;
    *acl = *present ? sd_part(sd, 2) : 0;
    if (defaulted) *defaulted = (s->Control & 0x0020) != 0;
    return TRUE;
}

/* TRUSTEE_W naming a SID */
typedef struct _TRUSTEE_W_ {
    struct _TRUSTEE_W_ *pMultipleTrustee;
    int MultipleTrusteeOperation, TrusteeForm, TrusteeType;
    LPWSTR ptstrName;
} TRUSTEE_W_;
WINADVAPI VOID WINAPI BuildTrusteeWithSidW(TRUSTEE_W_ *t, PSID sid)
{
    if (!t) return;
    t->pMultipleTrustee = 0;
    t->MultipleTrusteeOperation = 0;                /* NO_MULTIPLE_TRUSTEE */
    t->TrusteeForm = 0;                             /* TRUSTEE_IS_SID */
    t->TrusteeType = 0;                             /* TRUSTEE_IS_UNKNOWN */
    t->ptstrName = (LPWSTR)sid;
}
WINADVAPI VOID WINAPI BuildTrusteeWithSidA(TRUSTEE_W_ *t, PSID sid) { BuildTrusteeWithSidW(t, sid); }

/* Private object security: the new object's descriptor is a self-relative
 * copy of the creator's (no inheritance from the parent: NovaOS's own
 * objects carry no ACLs to inherit) */
WINADVAPI BOOL WINAPI CreatePrivateObjectSecurityEx(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR creator,
                                                    PSECURITY_DESCRIPTOR *out, GUID *type, BOOL container,
                                                    ULONG flags, HANDLE token, PGENERIC_MAPPING map)
{
    (void)parent; (void)type; (void)container; (void)flags; (void)token; (void)map;
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    SECURITY_DESCRIPTOR empty;
    if (!creator) {
        InitializeSecurityDescriptor(&empty, 1);
        creator = &empty;
    }
    DWORD n = 0;
    if (((SECURITY_DESCRIPTOR *)creator)->Control & SE_SELF_RELATIVE) n = GetSecurityDescriptorLength(creator);
    else MakeSelfRelativeSD(creator, 0, &n);
    PSECURITY_DESCRIPTOR sd = n ? HeapAlloc(GetProcessHeap(), 0, n) : 0;
    if (!sd) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (((SECURITY_DESCRIPTOR *)creator)->Control & SE_SELF_RELATIVE) memcpy(sd, creator, n);
    else if (!MakeSelfRelativeSD(creator, sd, &n)) { HeapFree(GetProcessHeap(), 0, sd); return FALSE; }
    *out = sd;
    return TRUE;
}
WINADVAPI BOOL WINAPI CreatePrivateObjectSecurity(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR creator,
                                                  PSECURITY_DESCRIPTOR *out, BOOL container, HANDLE token, PGENERIC_MAPPING map)
{
    return CreatePrivateObjectSecurityEx(parent, creator, out, 0, container, 0, token, map);
}
WINADVAPI BOOL WINAPI DestroyPrivateObjectSecurity(PSECURITY_DESCRIPTOR *sd)
{
    if (sd && *sd) { HeapFree(GetProcessHeap(), 0, *sd); *sd = 0; }
    return TRUE;
}

/* Credentials, ANSI forms: none stored either */
WINADVAPI BOOL WINAPI CredReadA(LPCSTR target, DWORD type, DWORD flags, PVOID *cred) { (void)target; return CredReadW(0, type, flags, cred); }
WINADVAPI BOOL WINAPI CredWriteA(PVOID cred, DWORD flags) { return CredWriteW(cred, flags); }
WINADVAPI BOOL WINAPI CredDeleteA(LPCSTR target, DWORD type, DWORD flags) { (void)target; return CredDeleteW(0, type, flags); }
