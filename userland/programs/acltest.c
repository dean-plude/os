/* acltest.exe — AccessCheck evaluates the DACL it is given, for our own
 * token and restricted ones; named objects check theirs when opened */
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* (not in our headers yet) */
#ifndef FILE_GENERIC_READ
#define FILE_READ_DATA      0x0001
#define FILE_WRITE_DATA     0x0002
#define FILE_GENERIC_READ   0x120089
#define FILE_GENERIC_WRITE  0x120116
#endif
#ifndef EVENT_MODIFY_STATE
#define EVENT_MODIFY_STATE  0x0002
#endif
#ifndef MAXIMUM_ALLOWED
#define MAXIMUM_ALLOWED     0x02000000
#endif
#ifndef INHERIT_ONLY_ACE
#define OBJECT_INHERIT_ACE  0x01
#define INHERIT_ONLY_ACE    0x08
#endif
#ifndef DELETE
#define DELETE              0x00010000
#endif
#ifndef READ_CONTROL
#define READ_CONTROL        0x00020000
#define WRITE_DAC           0x00040000
#endif
WINADVAPI BOOL WINAPI AddAccessAllowedAceEx(PACL acl, DWORD rev, DWORD flags, DWORD mask, PSID sid);

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s\n", what); } } while (0)

static GENERIC_MAPPING g_map = { 0x120089, 0x120116, 0x1200A0, 0x1F01FF };   /* files' */
static HANDLE g_token;

/* AccessCheck against a descriptor with @dacl (NULL: none), owned by @owner */
static BOOL check(PACL dacl, PSID owner, DWORD want, DWORD *granted)
{
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, dacl != NULL, dacl, FALSE);
    SetSecurityDescriptorOwner(&sd, owner, FALSE);
    SetSecurityDescriptorGroup(&sd, owner, FALSE);
    BYTE ps[64];
    DWORD psn = sizeof(ps);
    BOOL status = FALSE;
    *granted = 0xDEAD;
    if (!AccessCheck(&sd, g_token, want, &g_map, (PPRIVILEGE_SET)ps, &psn, granted, &status)) return FALSE;
    return status;
}

/* as check(), with @t as the token */
static BOOL check_as(HANDLE t, PACL dacl, PSID owner, DWORD want, DWORD *granted)
{
    HANDLE keep = g_token;
    g_token = t;
    BOOL ok = check(dacl, owner, want, granted);
    g_token = keep;
    return ok;
}

static HANDLE restrict_token(DWORD flags, PSID disable, PSID restrict_to)
{
    HANDLE pt = 0, rt = 0, it = 0;
    SID_AND_ATTRIBUTES d = { disable, 0 }, r = { restrict_to, 0 };
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &pt);
    if (!CreateRestrictedToken(pt, flags, disable ? 1 : 0, disable ? &d : 0, 0, 0, restrict_to ? 1 : 0,
                               restrict_to ? &r : 0, &rt)) {
        printf("CreateRestrictedToken: error %lu\n", GetLastError());
        rt = 0;
    }
    if (rt && !DuplicateToken(rt, SecurityImpersonation, &it)) it = 0;
    CloseHandle(rt);
    CloseHandle(pt);
    return it;
}

static void restricted(PSID me, PSID everyone, PSID users, PSID admins, PSID system)
{
    BYTE buf[512];
    PACL acl = (PACL)buf;
    DWORD g, n;
    HANDLE pt;
    BOOL member;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &pt);
    CHECK("AccessCheck needs an impersonation token", !AccessCheck(buf, pt, 1, &g_map, 0, 0, &g, &member) &&
          GetLastError() == 1309 /* ERROR_NO_IMPERSONATION_TOKEN */);
    CHECK("our token is not restricted", !IsTokenRestricted(pt));
    CHECK("CheckTokenMembership: Users", CheckTokenMembership(0, users, &member) && member);
    CHECK("CheckTokenMembership: not Administrators (not elevated)", CheckTokenMembership(0, admins, &member) && !member);
    CloseHandle(pt);

    /* restricted to Everyone: both passes must grant */
    HANDLE r = restrict_token(0, 0, everyone);
    CHECK("CreateRestrictedToken", r != 0);
    CHECK("IsTokenRestricted", IsTokenRestricted(r));
    struct { DWORD n; SID_AND_ATTRIBUTES g[2]; BYTE sids[64]; } rs;
    CHECK("TokenRestrictedSids: Everyone", GetTokenInformation(r, TokenRestrictedSids, &rs, sizeof(rs), &n) && rs.n == 1 &&
          EqualSid(rs.g[0].Sid, everyone));
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
    CHECK("restricted: our own ACE alone grants nothing", !check_as(r, acl, system, FILE_READ_DATA, &g));
    AddAccessAllowedAce(acl, ACL_REVISION, FILE_GENERIC_READ, everyone);
    CHECK("restricted: Everyone's read is granted", check_as(r, acl, system, FILE_GENERIC_READ, &g));
    CHECK("restricted: but not write", !check_as(r, acl, system, FILE_WRITE_DATA, &g));
    CHECK("restricted: MAXIMUM_ALLOWED is the overlap", check_as(r, acl, system, MAXIMUM_ALLOWED, &g) && g == FILE_GENERIC_READ);
    CHECK("restricted: a member of Users", CheckTokenMembership(r, users, &member) && !member);
    CHECK("restricted: still Everyone", CheckTokenMembership(r, everyone, &member) && member);
    CloseHandle(r);

    /* write-restricted: only write rights need the restricting SIDs */
    r = restrict_token(WRITE_RESTRICTED, 0, everyone);
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
    CHECK("write-restricted: read is ours", check_as(r, acl, system, FILE_READ_DATA, &g));
    CHECK("write-restricted: write is not", !check_as(r, acl, system, FILE_WRITE_DATA, &g));
    CloseHandle(r);

    /* Users deny-only: its grants stop, its denies stay */
    r = restrict_token(0, users, 0);
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_READ, users);
    CHECK("deny-only Users: its grant is gone", !check_as(r, acl, system, FILE_READ_DATA, &g));
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessDeniedAce(acl, ACL_REVISION, FILE_WRITE_DATA, users);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
    CHECK("deny-only Users: its deny holds", !check_as(r, acl, system, FILE_WRITE_DATA, &g));
    CHECK("deny-only Users: not a member", CheckTokenMembership(r, users, &member) && !member);

    /* impersonating it, and back */
    HANDLE t = 0;
    CHECK("SetThreadToken", SetThreadToken(0, r));
    CHECK("OpenThreadToken while impersonating", OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t));
    CHECK("CheckTokenMembership(NULL) uses the thread's token", CheckTokenMembership(0, users, &member) && !member);
    CloseHandle(t);
    CHECK("RevertToSelf", RevertToSelf());
    CHECK("no thread token after RevertToSelf", !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t) &&
          GetLastError() == 1008 /* ERROR_NO_TOKEN */);
    CHECK("ImpersonateSelf", ImpersonateSelf(SecurityImpersonation));
    CHECK("ImpersonateSelf: Users again", CheckTokenMembership(0, users, &member) && member);
    RevertToSelf();
    CloseHandle(r);
}

/* A named event only its owner may change: denied to a restricted token */
static void protected_event(PSID me, PSID everyone)
{
    BYTE buf[256];
    PACL acl = (PACL)buf;
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
    AddAccessAllowedAce(acl, ACL_REVISION, SYNCHRONIZE, everyone);
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
    SetSecurityDescriptorOwner(&sd, me, FALSE);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
    HANDLE ev = CreateEventA(&sa, TRUE, FALSE, "acltest-protected");
    CHECK("CreateEvent with a descriptor", ev != 0);
    HANDLE h = OpenEventA(EVENT_MODIFY_STATE, FALSE, "acltest-protected");
    CHECK("we may open it to signal", h != 0);
    if (h) CloseHandle(h);

    HANDLE r = restrict_token(0, 0, everyone);
    CHECK("impersonate a restricted token", SetThreadToken(0, r));
    h = OpenEventA(EVENT_MODIFY_STATE, FALSE, "acltest-protected");
    CHECK("restricted: OpenEvent(EVENT_MODIFY_STATE) is denied", !h && GetLastError() == ERROR_ACCESS_DENIED);
    if (h) CloseHandle(h);
    h = OpenEventA(SYNCHRONIZE, FALSE, "acltest-protected");
    CHECK("restricted: OpenEvent(SYNCHRONIZE) is allowed", h != 0);
    if (h) CloseHandle(h);
    h = CreateEventA(0, TRUE, FALSE, "acltest-protected");
    CHECK("restricted: CreateEvent of the existing name is denied", !h && GetLastError() == ERROR_ACCESS_DENIED);
    RevertToSelf();
    CloseHandle(r);

    /* the descriptor reads back, and a new DACL takes effect */
    DWORD need = 0;
    BYTE got[512];
    BOOL present = FALSE, def;
    PACL dacl = 0;
    CHECK("GetKernelObjectSecurity", GetKernelObjectSecurity(ev, DACL_SECURITY_INFORMATION, got, sizeof(got), &need) &&
          GetSecurityDescriptorDacl(got, &present, &dacl, &def) && present && dacl && dacl->AceCount == 2);
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, everyone);
    CHECK("SetKernelObjectSecurity", SetKernelObjectSecurity(ev, DACL_SECURITY_INFORMATION, &sd));
    r = restrict_token(0, 0, everyone);
    SetThreadToken(0, r);
    h = OpenEventA(EVENT_MODIFY_STATE, FALSE, "acltest-protected");
    CHECK("restricted: allowed once Everyone may", h != 0);
    if (h) CloseHandle(h);
    RevertToSelf();
    CloseHandle(r);
    CloseHandle(ev);
}

int main(void)
{
    SID_IDENTIFIER_AUTHORITY world = { { 0, 0, 0, 0, 0, 1 } }, nt = { { 0, 0, 0, 0, 0, 5 } };
    PSID everyone, users, admins, system, me;
    AllocateAndInitializeSid(&world, 1, 0, 0, 0, 0, 0, 0, 0, 0, &everyone);
    AllocateAndInitializeSid(&nt, 2, 32, 545, 0, 0, 0, 0, 0, 0, &users);
    AllocateAndInitializeSid(&nt, 2, 32, 544, 0, 0, 0, 0, 0, 0, &admins);
    AllocateAndInitializeSid(&nt, 1, 18, 0, 0, 0, 0, 0, 0, 0, &system);
    HANDLE pt;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &pt);
    DuplicateToken(pt, SecurityImpersonation, &g_token);
    BYTE ub[128];
    DWORD n;
    GetTokenInformation(pt, TokenUser, ub, sizeof(ub), &n);
    me = ((TOKEN_USER *)ub)->User.Sid;

    BYTE buf[512];
    PACL acl = (PACL)buf;
    DWORD g;

    CHECK("no DACL: full access", check(NULL, system, FILE_GENERIC_READ | FILE_GENERIC_WRITE, &g) &&
          g == (FILE_GENERIC_READ | FILE_GENERIC_WRITE));
    CHECK("no DACL: MAXIMUM_ALLOWED is everything", check(NULL, system, MAXIMUM_ALLOWED, &g) && g == 0x1F01FF);

    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    CHECK("empty DACL: nothing", !check(acl, system, FILE_READ_DATA, &g) && GetLastError() == ERROR_ACCESS_DENIED && g == 0);

    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_READ, users);
    CHECK("Users may read", check(acl, system, FILE_GENERIC_READ, &g) && g == FILE_GENERIC_READ);
    CHECK("but not write", !check(acl, system, FILE_WRITE_DATA, &g));
    CHECK("MAXIMUM_ALLOWED: read", check(acl, system, MAXIMUM_ALLOWED, &g) && g == FILE_GENERIC_READ);

    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessDeniedAce(acl, ACL_REVISION, FILE_WRITE_DATA, everyone);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
    CHECK("a deny first wins", !check(acl, system, FILE_WRITE_DATA, &g));
    CHECK("other rights still granted", check(acl, system, FILE_READ_DATA | DELETE, &g));
    CHECK("MAXIMUM_ALLOWED leaves out the denied right", check(acl, system, MAXIMUM_ALLOWED, &g) &&
          g == (0x1F01FF & ~FILE_WRITE_DATA));

    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
    AddAccessDeniedAce(acl, ACL_REVISION, FILE_WRITE_DATA, everyone);
    CHECK("a deny after the grant loses", check(acl, system, FILE_WRITE_DATA, &g));

    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, admins);
    CHECK("not elevated: Administrators grants nothing", !check(acl, system, FILE_READ_DATA, &g));
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessDeniedAce(acl, ACL_REVISION, FILE_READ_DATA, admins);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, everyone);
    CHECK("but its denies apply", !check(acl, system, FILE_READ_DATA, &g));

    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, system);
    CHECK("a SID we don't hold grants nothing", !check(acl, system, FILE_READ_DATA, &g));
    CHECK("the owner may read and change the DACL", check(acl, me, READ_CONTROL | WRITE_DAC, &g));

    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAceEx(acl, ACL_REVISION, INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE, GENERIC_ALL, everyone);
    CHECK("inherit-only ACEs are for children", !check(acl, system, FILE_READ_DATA, &g));

    restricted(me, everyone, users, admins, system);
    protected_event(me, everyone);

    printf("acltest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
