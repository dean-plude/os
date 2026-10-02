/* acltest.exe — AccessCheck evaluates the DACL it is given, and files on
 * drive C: keep their own ACLs, which opening, deleting and renaming obey.
 *
 * It leaves C:\AclTest\kept.txt behind with a DACL that denies writing;
 * run again after a restart, it checks that the file still has it (drive
 * C: on NTFS keeps it; on FAT it is gone, and the run says so). */
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
#ifndef INHERITED_ACE
#define INHERITED_ACE       0x10
#endif
#ifndef ACCESS_DENIED_ACE_TYPE
#define ACCESS_ALLOWED_ACE_TYPE 0
#define ACCESS_DENIED_ACE_TYPE  1
#endif
#define FSEC_DELETE_CHILD   0x40
#ifndef READ_CONTROL
#define READ_CONTROL        0x00020000
#define WRITE_DAC           0x00040000
#endif
WINADVAPI BOOL WINAPI SetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID o, BOOL def);
WINADVAPI BOOL WINAPI SetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID g, BOOL def);
WINADVAPI BOOL WINAPI DuplicateToken(HANDLE t, SECURITY_IMPERSONATION_LEVEL l, PHANDLE out);
WINADVAPI BOOL WINAPI AddAccessDeniedAce(PACL acl, DWORD rev, DWORD mask, PSID sid);
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

    file_tests(me, everyone);

    printf("acltest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
