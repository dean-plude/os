/* acltest.exe — AccessCheck evaluates the DACL it is given */
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
WINADVAPI BOOL WINAPI SetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID o, BOOL def);
WINADVAPI BOOL WINAPI SetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID g, BOOL def);
WINADVAPI BOOL WINAPI DuplicateToken(HANDLE t, SECURITY_IMPERSONATION_LEVEL l, PHANDLE out);
WINADVAPI BOOL WINAPI AddAccessDeniedAce(PACL acl, DWORD rev, DWORD mask, PSID sid);
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

    printf("acltest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
