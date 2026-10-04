/* acltest.exe — AccessCheck evaluates the DACL it is given, for our own
 * token and restricted ones; named objects check theirs when opened, and
 * files on drive C: keep their own ACLs, which opening, deleting and
 * renaming obey (for restricted tokens too).
 *
 * SetEntriesInAcl, SDDL and SetNamedSecurityInfo build the ACLs installers
 * secure folders with (as WiX Burn does its Package Cache): checked as
 * the elevated token (the linked one, impersonated) and as ourselves.
 *
 * Drive C:'s root has the DACL Windows gives C:\, inherited
 * by what has none of its own, so adding one entry to a folder (as
 * Chromium's setup grants ALL APPLICATION PACKAGES its install folder)
 * keeps the user's own entries.
 *
 * It leaves C:\AclTest\kept.txt behind with a DACL that denies writing;
 * run again after a restart, it checks that the file still has it (drive
 * C: on NTFS keeps it; on FAT it is gone, and the run says so). */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
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
#ifndef INHERITED_ACE
#define INHERITED_ACE       0x10
#endif
#ifndef ACCESS_DENIED_ACE_TYPE
#define ACCESS_ALLOWED_ACE_TYPE 0
#define ACCESS_DENIED_ACE_TYPE  1
#endif
#define FSEC_DELETE_CHILD   0x40
#define FILE_ALL_ACCESS_    0x1F01FF
#define CONTAINER_INHERIT_ACE_ 0x02
WINADVAPI BOOL WINAPI AddAccessAllowedAceEx(PACL acl, DWORD rev, DWORD flags, DWORD mask, PSID sid);
WINADVAPI BOOL WINAPI GetFileSecurityA(LPCSTR name, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, DWORD n, LPDWORD need);
WINADVAPI BOOL WINAPI SetFileSecurityA(LPCSTR name, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd);
WINADVAPI BOOL WINAPI GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd, LPBOOL present, PACL *acl, LPBOOL def);
WINADVAPI BOOL WINAPI GetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID *o, LPBOOL def);
WINADVAPI BOOL WINAPI GetAce(PACL acl, DWORD i, LPVOID *ace);

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

/* ---- files on drive C: ---- */

#define DIR_PATH  "C:\\AclTest"
#define FILE_PATH "C:\\AclTest\\secret.txt"
#define KEPT_PATH "C:\\AclTest\\kept.txt"

static BOOL write_file(const char *path, const char *text)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD n;
    BOOL ok = WriteFile(h, text, (DWORD)strlen(text), &n, NULL);
    CloseHandle(h);
    return ok;
}

static BOOL read_is(const char *path, const char *text)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    char buf[64];
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &n, NULL);
    CloseHandle(h);
    buf[n] = 0;
    return ok && !strcmp(buf, text);
}

static BOOL open_denied(const char *path, DWORD access)
{
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return FALSE; }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

/* Give @path a DACL of @acl (owner and group left as they are) */
static BOOL set_dacl(const char *path, PACL acl)
{
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
    return SetFileSecurityA(path, DACL_SECURITY_INFORMATION, &sd);
}

/* @path's DACL: how many ACEs, and the first one's type, flags and mask */
static int dacl_of(const char *path, BYTE *type, BYTE *flags, DWORD *mask)
{
    static BYTE sd[1024];
    DWORD need;
    BOOL present, def;
    PACL acl;
    if (!GetFileSecurityA(path, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, sd, sizeof(sd), &need)) return -1;
    if (!GetSecurityDescriptorDacl(sd, &present, &acl, &def) || !present || !acl) return -2;
    ACE_HEADER *a;
    if (acl->AceCount && GetAce(acl, 0, (LPVOID *)&a)) {
        *type = a->AceType;
        *flags = a->AceFlags;
        *mask = ((ACCESS_ALLOWED_ACE *)a)->Mask;
    }
    return acl->AceCount;
}

/* A DACL that denies writing (and deleting) to everyone, and lets @me do the rest */
static void read_only_acl(PACL acl, DWORD size, PSID me, PSID everyone)
{
    InitializeAcl(acl, size, ACL_REVISION);
    AddAccessDeniedAce(acl, ACL_REVISION, FILE_WRITE_DATA | 0x4 /* FILE_APPEND_DATA */ | DELETE, everyone);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
}

static void file_tests(PSID me, PSID everyone)
{
    BYTE buf[512];
    PACL acl = (PACL)buf;
    BYTE t = 0, f = 0;
    DWORD m = 0;

    /* From the last run (after a restart): kept.txt still refuses writing */
    if (GetFileAttributesA(KEPT_PATH) != INVALID_FILE_ATTRIBUTES) {
        int n = dacl_of(KEPT_PATH, &t, &f, &m);
        if (n == 2 && t == ACCESS_DENIED_ACE_TYPE) {
            printf("acltest: C:\\AclTest\\kept.txt kept its DACL\n");
            CHECK("kept.txt: the deny ACE is still first", m == (FILE_WRITE_DATA | 0x4 | DELETE));
            CHECK("kept.txt: still can't be written", open_denied(KEPT_PATH, GENERIC_WRITE));
            CHECK("kept.txt: still can be read", read_is(KEPT_PATH, "kept"));
        } else {
            printf("acltest: C:\\AclTest\\kept.txt lost its DACL (drive C: on FAT keeps no ACLs)\n");
        }
    }

    /* The folder: everything for the user except deleting what is in it;
     * its files get full control (inherited) */
    CreateDirectoryA(DIR_PATH, NULL);
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, 0x1F01FF & ~FSEC_DELETE_CHILD, me);
    AddAccessAllowedAceEx(acl, ACL_REVISION, OBJECT_INHERIT_ACE | INHERIT_ONLY_ACE, GENERIC_ALL, me);
    CHECK("C:\\AclTest: set its DACL", set_dacl(DIR_PATH, acl));

    DeleteFileA(FILE_PATH);
    CHECK("secret.txt: created", write_file(FILE_PATH, "secret"));
    int n = dacl_of(FILE_PATH, &t, &f, &m);
    CHECK("secret.txt: inherits one ACE from the folder", n == 1 && t == ACCESS_ALLOWED_ACE_TYPE &&
          (f & INHERITED_ACE) && (m == GENERIC_ALL || m == 0x1F01FF));

    read_only_acl(acl, sizeof(buf), me, everyone);
    CHECK("secret.txt: set a DACL that denies writing", set_dacl(FILE_PATH, acl));
    n = dacl_of(FILE_PATH, &t, &f, &m);
    CHECK("GetFileSecurity returns it", n == 2 && t == ACCESS_DENIED_ACE_TYPE && m == (FILE_WRITE_DATA | 0x4 | DELETE));
    CHECK("opening for writing is denied", open_denied(FILE_PATH, GENERIC_WRITE));
    CHECK("appending is denied", open_denied(FILE_PATH, 0x4));
    CHECK("reading still works", read_is(FILE_PATH, "secret"));
    CHECK("deleting is denied", !DeleteFileA(FILE_PATH) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK("renaming is denied", !MoveFileA(FILE_PATH, DIR_PATH "\\renamed.txt") && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK("MAXIMUM_ALLOWED leaves writing out", open_denied(FILE_PATH, GENERIC_WRITE | MAXIMUM_ALLOWED));

    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_READ, everyone);
    CHECK("the owner may still change the DACL", set_dacl(FILE_PATH, acl));
    CHECK("... and now only reading is allowed", open_denied(FILE_PATH, FILE_WRITE_DATA) && read_is(FILE_PATH, "secret"));

    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
    CHECK("full control again", set_dacl(FILE_PATH, acl) && write_file(FILE_PATH, "changed") && read_is(FILE_PATH, "changed"));
    HANDLE r = restrict_token(0, 0, everyone);           /* (only our own ACE: Everyone has none) */
    CHECK("restricted: opening it for reading is denied", r && SetThreadToken(0, r) && open_denied(FILE_PATH, GENERIC_READ));
    RevertToSelf();
    CloseHandle(r);
    CHECK("and it can be deleted", DeleteFileA(FILE_PATH));

    /* A file made with a descriptor of its own (CreateFile's SECURITY_ATTRIBUTES) */
    read_only_acl(acl, sizeof(buf), me, everyone);
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
    if (GetFileAttributesA(KEPT_PATH) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileA(KEPT_PATH, GENERIC_WRITE, 0, &sa, CREATE_NEW, 0, NULL);
        DWORD w;
        CHECK("kept.txt: created with a DACL that denies writing", h != INVALID_HANDLE_VALUE && WriteFile(h, "kept", 4, &w, NULL));
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        CHECK("kept.txt: its DACL is the one given", dacl_of(KEPT_PATH, &t, &f, &m) == 2 && t == ACCESS_DENIED_ACE_TYPE);
        CHECK("kept.txt: can't be opened for writing", open_denied(KEPT_PATH, GENERIC_WRITE));
        printf("acltest: left C:\\AclTest\\kept.txt; run acltest again after a restart to check it kept its DACL\n");
    }
}

/* -----------------------------------------------------------------------
 * aclapi and SDDL
 * ----------------------------------------------------------------------- */
#define CACHE_PATH "C:\\AclTest\\Cache"
#define SUB_PATH   CACHE_PATH "\\{0b5169e3}"
#define CACHED     SUB_PATH "\\setup.exe"

/* @acl's ACE @i: its type, flags, mask and SID */
static BOOL ace_is(PACL acl, DWORD i, BYTE type, BYTE flags, DWORD mask, PSID sid)
{
    ACCESS_ALLOWED_ACE *a;
    if (!acl || i >= acl->AceCount || !GetAce(acl, i, (LPVOID *)&a)) return FALSE;
    return a->Header.AceType == type && a->Header.AceFlags == flags && a->Mask == mask && EqualSid((PSID)&a->SidStart, sid);
}

static void ea_sid(EXPLICIT_ACCESS_W *ea, DWORD perms, ACCESS_MODE mode, DWORD inherit, PSID sid)
{
    memset(ea, 0, sizeof(*ea));
    ea->grfAccessPermissions = perms;
    ea->grfAccessMode = mode;
    ea->grfInheritance = inherit;
    BuildTrusteeWithSidW(&ea->Trustee, sid);
}

/* The linked (elevated) half of our token, as an impersonation token */
static HANDLE elevated_token(void)
{
    HANDLE pt = 0, linked = 0, it = 0;
    DWORD n = 0;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &pt);
    if (pt && GetTokenInformation(pt, TokenLinkedToken, &linked, sizeof(linked), &n) && linked)
        if (!DuplicateToken(linked, SecurityImpersonation, &it)) it = 0;
    if (linked) CloseHandle(linked);
    if (pt) CloseHandle(pt);
    return it;
}

/* The ACEs a folder holds: Burn's Package Cache root (SecurePath) */
static PACL burn_root_acl(PSID admins, PSID system, PSID everyone, PSID users)
{
    EXPLICIT_ACCESS_W ea[4];
    ea_sid(&ea[0], FILE_ALL_ACCESS_, SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT, admins);
    ea_sid(&ea[1], FILE_ALL_ACCESS_, SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT, system);
    ea_sid(&ea[2], GENERIC_READ | GENERIC_EXECUTE, SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT, everyone);
    ea_sid(&ea[3], GENERIC_READ | GENERIC_EXECUTE, SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT, users);
    PACL acl = 0;
    return SetEntriesInAclW(4, ea, NULL, &acl) == ERROR_SUCCESS ? acl : NULL;
}

/* Remove the cache folders (as the elevated token, which they let in) */
static void remove_cache(HANDLE elev)
{
    if (elev) SetThreadToken(0, elev);
    DeleteFileA(CACHED);
    RemoveDirectoryA(SUB_PATH);
    RemoveDirectoryA(CACHE_PATH);
    if (elev) RevertToSelf();
}

/* As C:\AclTest\Work's owner: give ourselves full control, then remove it */
static BOOL let_in_and_remove(PSID me)
{
    BYTE buf[128];
    PACL acl = (PACL)buf;
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
    return SetNamedSecurityInfoW((LPWSTR)L"" DIR_PATH "\\Work", SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, 0, 0, acl, 0) == 0 &&
           RemoveDirectoryA(DIR_PATH "\\Work");
}

static void aclapi_tests(PSID me, PSID everyone, PSID users, PSID admins, PSID system)
{
    BYTE buf[512];
    PACL old = (PACL)buf, acl = 0;
    EXPLICIT_ACCESS_W ea[4];
    WCHAR name[64];

    /* SetEntriesInAcl: Burn's four entries */
    acl = burn_root_acl(admins, system, everyone, users);
    CHECK("SetEntriesInAclW: four entries", acl && acl->AceCount == 4);
    CHECK("SetEntriesInAclW: Administrators full control, inherited by all",
          ace_is(acl, 0, ACCESS_ALLOWED_ACE_TYPE, 3, FILE_ALL_ACCESS_, admins));
    CHECK("SetEntriesInAclW: Users read and execute",
          ace_is(acl, 3, ACCESS_ALLOWED_ACE_TYPE, 3, GENERIC_READ | GENERIC_EXECUTE, users));
    DWORD g;
    CHECK("SetEntriesInAclW's ACL: we may read", check(acl, system, FILE_GENERIC_READ, &g));
    CHECK("SetEntriesInAclW's ACL: not write (not elevated)", !check(acl, system, FILE_WRITE_DATA, &g));
    ULONG n = 0;
    EXPLICIT_ACCESS_W *got = 0;
    CHECK("GetExplicitEntriesFromAclW", GetExplicitEntriesFromAclW(acl, &n, &got) == ERROR_SUCCESS && n == 4 && got &&
          got[1].grfAccessMode == GRANT_ACCESS && got[1].grfInheritance == 3 &&
          got[1].Trustee.TrusteeForm == TRUSTEE_IS_SID && EqualSid((PSID)got[1].Trustee.ptstrName, system));
    LocalFree(got);
    LocalFree(acl);

    /* merging into an old ACL: GRANT adds to the allow entry, DENY goes
     * first, inherited entries stay last, SET replaces, REVOKE removes */
    InitializeAcl(old, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAceEx(old, ACL_REVISION, INHERITED_ACE, GENERIC_READ, users);
    AddAccessAllowedAce(old, ACL_REVISION, FILE_READ_DATA, me);
    ea_sid(&ea[0], FILE_WRITE_DATA, GRANT_ACCESS, NO_INHERITANCE, me);
    ea_sid(&ea[1], DELETE, DENY_ACCESS, NO_INHERITANCE, everyone);
    CHECK("SetEntriesInAclW(GRANT, DENY) into an old ACL", SetEntriesInAclW(2, ea, old, &acl) == ERROR_SUCCESS && acl &&
          acl->AceCount == 3);
    CHECK("... the deny first", ace_is(acl, 0, ACCESS_DENIED_ACE_TYPE, 0, DELETE, everyone));
    CHECK("... GRANT added to our entry", ace_is(acl, 1, ACCESS_ALLOWED_ACE_TYPE, 0, FILE_READ_DATA | FILE_WRITE_DATA, me));
    CHECK("... the inherited entry last", ace_is(acl, 2, ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE, GENERIC_READ, users));
    PACL acl2 = 0;
    ea_sid(&ea[0], GENERIC_READ, SET_ACCESS, SUB_OBJECTS_ONLY_INHERIT, me);
    CHECK("SetEntriesInAclW(SET) replaces our entry", SetEntriesInAclW(1, ea, acl, &acl2) == ERROR_SUCCESS && acl2 &&
          acl2->AceCount == 3 && ace_is(acl2, 1, ACCESS_ALLOWED_ACE_TYPE, OBJECT_INHERIT_ACE, GENERIC_READ, me));
    LocalFree(acl2);
    ea_sid(&ea[0], 0, REVOKE_ACCESS, NO_INHERITANCE, me);
    CHECK("SetEntriesInAclW(REVOKE) removes it", SetEntriesInAclW(1, ea, acl, &acl2) == ERROR_SUCCESS && acl2 &&
          acl2->AceCount == 2 && ace_is(acl2, 1, ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE, GENERIC_READ, users));
    LocalFree(acl2);
    LocalFree(acl);
    acl = (PACL)1;
    CHECK("SetEntriesInAclW(no entries, no ACL): no ACL", SetEntriesInAclW(0, NULL, NULL, &acl) == ERROR_SUCCESS && !acl);

    /* trustees by name */
    wcscpy(name, L"BUILTIN\\Administrators");
    BuildExplicitAccessWithNameW(&ea[0], name, GENERIC_ALL, SET_ACCESS, NO_INHERITANCE);
    BuildExplicitAccessWithNameW(&ea[1], (LPWSTR)L"SYSTEM", GENERIC_ALL, SET_ACCESS, NO_INHERITANCE);
    BuildExplicitAccessWithNameW(&ea[2], (LPWSTR)L"Everyone", GENERIC_READ, SET_ACCESS, NO_INHERITANCE);
    BuildExplicitAccessWithNameW(&ea[3], (LPWSTR)L"CURRENT_USER", GENERIC_ALL, SET_ACCESS, NO_INHERITANCE);
    CHECK("SetEntriesInAclW: trustees by name", SetEntriesInAclW(4, ea, NULL, &acl) == ERROR_SUCCESS && acl &&
          ace_is(acl, 0, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, admins) &&
          ace_is(acl, 1, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, system) &&
          ace_is(acl, 2, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_READ, everyone) &&
          ace_is(acl, 3, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, me));
    LocalFree(acl);
    BuildExplicitAccessWithNameW(&ea[0], (LPWSTR)L"Nobody Here", GENERIC_ALL, SET_ACCESS, NO_INHERITANCE);
    CHECK("SetEntriesInAclW: an unknown name is not mapped", SetEntriesInAclW(1, ea, NULL, &acl) == ERROR_NONE_MAPPED);

    BYTE sid[SECURITY_MAX_SID_SIZE];
    WCHAR dom[32];
    DWORD ns = sizeof(sid), nd = 32;
    SID_NAME_USE use;
    CHECK("LookupAccountNameW(Users)", LookupAccountNameW(0, L"Users", sid, &ns, dom, &nd, &use) && EqualSid(sid, users) &&
          use == SidTypeAlias && !wcscmp(dom, L"BUILTIN"));
    char user[64];
    DWORD un = sizeof(user);
    GetUserNameA(user, &un);
    ns = sizeof(sid);
    char an[16];
    DWORD dn = sizeof(an);
    CHECK("LookupAccountNameA(our user name)", LookupAccountNameA(0, user, sid, &ns, an, &dn, &use) && EqualSid(sid, me) &&
          use == SidTypeUser);
    ns = sizeof(sid); nd = 32;
    CHECK("LookupAccountNameW(unknown) fails", !LookupAccountNameW(0, L"Nobody Here", sid, &ns, dom, &nd, &use) &&
          GetLastError() == ERROR_NONE_MAPPED);

    /* SDDL both ways */
    static const WCHAR text[] = L"O:BAG:SYD:PAI(D;;WD;;;WD)(A;;FA;;;BA)(A;OICIIO;GA;;;BA)(A;;0x1200a9;;;BU)";
    PSECURITY_DESCRIPTOR sd = 0;
    ULONG len = 0;
    CHECK("ConvertStringSecurityDescriptorToSecurityDescriptorW",
          ConvertStringSecurityDescriptorToSecurityDescriptorW(text, SDDL_REVISION_1, &sd, &len) && sd &&
          len == GetSecurityDescriptorLength(sd));
    BOOL present = FALSE, def;
    PACL dacl = 0;
    PSID owner = 0;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD rev;
    if (sd) {
        GetSecurityDescriptorDacl(sd, &present, &dacl, &def);
        GetSecurityDescriptorOwner(sd, &owner, &def);
        GetSecurityDescriptorControl(sd, &control, &rev);
    }
    CHECK("SDDL: owner Administrators", owner && EqualSid(owner, admins));
    CHECK("SDDL: protected, auto-inherited", (control & (SE_DACL_PROTECTED | SE_DACL_AUTO_INHERITED)) ==
          (SE_DACL_PROTECTED | SE_DACL_AUTO_INHERITED));
    CHECK("SDDL: four ACEs as written", present && dacl && dacl->AceCount == 4 &&
          ace_is(dacl, 0, ACCESS_DENIED_ACE_TYPE, 0, WRITE_DAC, everyone) &&
          ace_is(dacl, 1, ACCESS_ALLOWED_ACE_TYPE, 0, FILE_ALL_ACCESS_, admins) &&
          ace_is(dacl, 2, ACCESS_ALLOWED_ACE_TYPE, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE_ | INHERIT_ONLY_ACE, GENERIC_ALL, admins) &&
          ace_is(dacl, 3, ACCESS_ALLOWED_ACE_TYPE, 0, 0x1200a9, users));
    CHECK("SDDL's DACL: Users may read", dacl && check(dacl, system, FILE_GENERIC_READ, &g));
    CHECK("SDDL's DACL: but not write", dacl && !check(dacl, system, FILE_WRITE_DATA, &g));
    LPWSTR back = 0;
    CHECK("ConvertSecurityDescriptorToStringSecurityDescriptorW gives it back",
          sd && ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION |
          GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &back, &len) && back && !wcscmp(back, text));
    if (back && wcscmp(back, text)) wprintf(L"  got %ls\n", back);
    LocalFree(back);
    LocalFree(sd);
    sd = 0;
    CHECK("SDDL: D:NO_ACCESS_CONTROL is a NULL DACL",
          ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:NO_ACCESS_CONTROL", SDDL_REVISION_1, &sd, NULL) &&
          GetSecurityDescriptorDacl(sd, &present, &dacl, &def) && present && !dacl);
    LocalFree(sd);
    sd = 0;
    CHECK("SDDL: a bad ACE is refused",
          !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;FA;;;XX)", SDDL_REVISION_1, &sd, NULL));
    CHECK("SDDL: revision 2 is refused",
          !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:", 2, &sd, NULL) && GetLastError() == ERROR_UNKNOWN_REVISION);

    /* Burn's Package Cache: the root secured with SetEntriesInAcl, a
     * folder per bundle made by the elevated engine, then reset to inherit
     * (an empty DACL with UNPROTECTED_DACL_SECURITY_INFORMATION) */
    HANDLE elev = elevated_token();
    CHECK("TokenLinkedToken: the elevated token", elev != 0);
    remove_cache(elev);
    CHECK("Cache: created", CreateDirectoryA(CACHE_PATH, NULL));
    acl = burn_root_acl(admins, system, everyone, users);
    DWORD e = acl ? SetNamedSecurityInfoW((LPWSTR)L"" CACHE_PATH, SE_FILE_OBJECT,
                                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, 0, 0, acl, 0) : 1;
    CHECK("Cache: SetNamedSecurityInfoW(protected DACL) by its owner", e == 0);
    if (e) printf("  error %lu\n", e);
    LocalFree(acl);
    CHECK("Cache: not elevated, a folder in it is denied",
          !CreateDirectoryA(SUB_PATH, NULL) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK("Cache: elevated, the folder is made", elev && SetThreadToken(0, elev) && CreateDirectoryA(SUB_PATH, NULL));
    InitializeAcl(old, sizeof(buf), ACL_REVISION);
    CHECK("Cache: reset to inherit (empty unprotected DACL)",
          SetNamedSecurityInfoW((LPWSTR)L"" SUB_PATH, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, 0, 0, old, 0) == 0);
    dacl = 0;
    sd = 0;
    CHECK("Cache: the folder inherited the root's four entries",
          GetNamedSecurityInfoW(L"" SUB_PATH, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, 0, 0, &dacl, 0, &sd) == 0 &&
          dacl && dacl->AceCount == 4 &&
          ace_is(dacl, 0, ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE | 3, FILE_ALL_ACCESS_, admins));
    LocalFree(sd);
    CHECK("Cache: elevated, a file is copied in", write_file(CACHED, "bundle"));
    RevertToSelf();
    CHECK("Cache: not elevated, it can be read", read_is(CACHED, "bundle"));
    CHECK("Cache: but not written", open_denied(CACHED, GENERIC_WRITE));
    CHECK("Cache: elevated, protected empty DACL", SetThreadToken(0, elev) &&
          SetNamedSecurityInfoW((LPWSTR)L"" SUB_PATH, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, 0, 0, old, 0) == 0);
    CHECK("Cache: ... then nobody may add to it", !write_file(SUB_PATH "\\other.txt", "x") && GetLastError() == ERROR_ACCESS_DENIED);
    InitializeAcl(old, sizeof(buf), ACL_REVISION);
    AddAccessAllowedAceEx(old, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE_, GENERIC_ALL, admins);
    SetNamedSecurityInfoW((LPWSTR)L"" SUB_PATH, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                          0, 0, old, 0);
    RevertToSelf();
    remove_cache(elev);
    CHECK("Cache: removed", GetFileAttributesA(CACHE_PATH) == INVALID_FILE_ATTRIBUTES);
    if (elev) CloseHandle(elev);

    /* a folder made with Burn's working-folder SDDL in SECURITY_ATTRIBUTES */
    sd = 0;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:PAI(A;;FA;;;BA)(A;OICIIO;GA;;;BA)(A;;FA;;;SY)(A;OICIIO;GA;;;SY)",
                                                         SDDL_REVISION_1, &sd, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    if (GetFileAttributesA(DIR_PATH "\\Work") != INVALID_FILE_ATTRIBUTES) let_in_and_remove(me);
    CHECK("CreateDirectory with an SDDL descriptor", sd && CreateDirectoryA(DIR_PATH "\\Work", &sa));
    CHECK("... not elevated, nothing may go in it", !write_file(DIR_PATH "\\Work\\x.txt", "x") && GetLastError() == ERROR_ACCESS_DENIED);
    LocalFree(sd);
    CHECK("... and its owner may still let itself in and remove it", let_in_and_remove(me));
}

/* ---- the root's DACL, inherited ---- */

#define APP_PATH "C:\\AclTestApp"

/* Has @acl an allow ACE with exactly @flags, @mask for @sid? */
static BOOL has_ace(PACL acl, BYTE flags, DWORD mask, PSID sid)
{
    for (DWORD i = 0; acl && i < acl->AceCount; i++)
        if (ace_is(acl, i, ACCESS_ALLOWED_ACE_TYPE, flags, mask, sid)) return TRUE;
    return FALSE;
}

static void root_tests(PSID me, PSID users, PSID admins, PSID system)
{
    PACL dacl = 0;
    PSECURITY_DESCRIPTOR sd = 0;
    SID_IDENTIFIER_AUTHORITY nt = { { 0, 0, 0, 0, 0, 5 } }, creator = { { 0, 0, 0, 0, 0, 3 } };
    PSID auth_users = 0, creator_owner = 0;
    AllocateAndInitializeSid(&nt, 1, 11, 0, 0, 0, 0, 0, 0, 0, &auth_users);
    AllocateAndInitializeSid(&creator, 1, 0, 0, 0, 0, 0, 0, 0, 0, &creator_owner);
    CHECK("C:\\: Windows' DACL for C:\\ (SYSTEM, Administrators, CREATOR OWNER, Authenticated Users, Users)",
          GetNamedSecurityInfoW(L"C:\\", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, 0, 0, &dacl, 0, &sd) == 0 && dacl &&
          dacl->AceCount == 5 && ace_is(dacl, 0, ACCESS_ALLOWED_ACE_TYPE, 3, FILE_ALL_ACCESS_, system) &&
          ace_is(dacl, 1, ACCESS_ALLOWED_ACE_TYPE, 3, FILE_ALL_ACCESS_, admins) &&
          ace_is(dacl, 2, ACCESS_ALLOWED_ACE_TYPE, 3 | INHERIT_ONLY_ACE, GENERIC_ALL, creator_owner) &&
          ace_is(dacl, 3, ACCESS_ALLOWED_ACE_TYPE, 3, 0x1301BF, auth_users) &&
          ace_is(dacl, 4, ACCESS_ALLOWED_ACE_TYPE, 3, 0x1200A9, users));
    LocalFree(sd);
    dacl = 0;
    sd = 0;
    CHECK("C:\\Windows inherits it (CREATOR OWNER as the user)",
          GetNamedSecurityInfoW(L"C:\\Windows", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, 0, 0, &dacl, 0, &sd) == 0 &&
          has_ace(dacl, INHERITED_ACE | 3, GENERIC_ALL, me) && has_ace(dacl, INHERITED_ACE | 3, 0x1200A9, users));
    LocalFree(sd);
    FreeSid(auth_users);
    FreeSid(creator_owner);

    /* Chromium's setup (base::win's GrantAccessToPath): the folder's DACL
     * read, ALL APPLICATION PACKAGES granted read and execute with
     * SetEntriesInAcl, set back with SetNamedSecurityInfo */
    RemoveDirectoryA(APP_PATH "\\SetupMetrics");
    RemoveDirectoryA(APP_PATH);
    CHECK("AclTestApp: created", CreateDirectoryA(APP_PATH, NULL));
    dacl = 0;
    sd = 0;
    CHECK("AclTestApp: its DACL is inherited from C:\\",
          GetNamedSecurityInfoW(L"" APP_PATH, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, 0, 0, &dacl, 0, &sd) == 0 && dacl &&
          dacl->AceCount == 5 && has_ace(dacl, INHERITED_ACE | 3, GENERIC_ALL, me));
    SID_IDENTIFIER_AUTHORITY app = { { 0, 0, 0, 0, 0, 15 } };
    PSID packages = 0;
    AllocateAndInitializeSid(&app, 2, 2, 1, 0, 0, 0, 0, 0, 0, &packages);          /* S-1-15-2-1 */
    EXPLICIT_ACCESS_W ea;
    ea_sid(&ea, GENERIC_READ | GENERIC_EXECUTE, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT, packages);
    PACL added = 0;
    CHECK("AclTestApp: SetEntriesInAclW(ALL APPLICATION PACKAGES) keeps the inherited entries",
          dacl && SetEntriesInAclW(1, &ea, dacl, &added) == ERROR_SUCCESS && added && added->AceCount == 6);
    LocalFree(sd);
    CHECK("AclTestApp: SetNamedSecurityInfoW", added &&
          SetNamedSecurityInfoW((LPWSTR)L"" APP_PATH, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, 0, 0, added, 0) == 0);
    LocalFree(added);
    dacl = 0;
    sd = 0;
    CHECK("AclTestApp: the new entry and the inherited ones",
          GetNamedSecurityInfoW(L"" APP_PATH, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, 0, 0, &dacl, 0, &sd) == 0 && dacl &&
          dacl->AceCount == 6 && has_ace(dacl, 3, GENERIC_READ | GENERIC_EXECUTE, packages) &&
          has_ace(dacl, INHERITED_ACE | 3, GENERIC_ALL, me));
    LocalFree(sd);
    CHECK("AclTestApp: a folder can still be made in it", CreateDirectoryA(APP_PATH "\\SetupMetrics", NULL));
    dacl = 0;
    sd = 0;
    CHECK("AclTestApp\\SetupMetrics inherits the new entry",
          GetNamedSecurityInfoW(L"" APP_PATH "\\SetupMetrics", SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, 0, 0, &dacl, 0, &sd) == 0 &&
          has_ace(dacl, INHERITED_ACE | 3, GENERIC_READ | GENERIC_EXECUTE, packages) &&
          has_ace(dacl, INHERITED_ACE | 3, GENERIC_ALL, me));
    LocalFree(sd);
    CHECK("AclTestApp: removed", RemoveDirectoryA(APP_PATH "\\SetupMetrics") && RemoveDirectoryA(APP_PATH));
    FreeSid(packages);
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
    file_tests(me, everyone);
    aclapi_tests(me, everyone, users, admins, system);
    root_tests(me, users, admins, system);

    printf("acltest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
