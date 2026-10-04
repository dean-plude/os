/* edgeupdtest.exe — the Windows APIs Microsoft Edge Update (the installer of
 * the WebView2 runtime that Roblox and other programs need) calls:
 * Task Scheduler 2.0 (register, read, disable, run and delete a task, as
 * Edge Update registers its update tasks), the Data Protection API,
 * shlwapi's UrlCombine/UrlEscapeA/UrlUnescapeA, the package-name parsers,
 * WTSEnumerateSessions, MakeAbsoluteSD, the Azure AD and MDM enrolment
 * checks, and ole32's CoRegisterPSClsid and CoGetCallContext.
 *
 * `edgeupdtest` runs them all; `edgeupdtest mark FILE` (what the test task
 * runs) writes FILE, so the test can see the task ran. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

/* ---- Task Scheduler 2.0, called through its vtables by slot number ---- */
static const GUID CLSID_TaskScheduler_ = { 0x0F87369F, 0xA4E5, 0x4CFC, { 0xBD, 0x3E, 0x73, 0xE6, 0x15, 0x45, 0x72, 0xDD } };
static const GUID IID_ITaskService_ = { 0x2FABA4C7, 0x4DA9, 0x4013, { 0x96, 0x97, 0x20, 0xCC, 0x3F, 0xD4, 0x0F, 0x85 } };
typedef struct { void **vtbl; } Com;
#define SLOT(o, i, T) ((T)((Com *)(o))->vtbl[i])
typedef HRESULT (STDMETHODCALLTYPE *Fn_bstr_out)(void *, BSTR, void **);
typedef HRESULT (STDMETHODCALLTYPE *Fn_get_bstr)(void *, BSTR *);
typedef HRESULT (STDMETHODCALLTYPE *Fn_get_long)(void *, LONG *);
typedef HRESULT (STDMETHODCALLTYPE *Fn_get_bool)(void *, VARIANT_BOOL *);
typedef HRESULT (STDMETHODCALLTYPE *Fn_put_bool)(void *, VARIANT_BOOL);
typedef HRESULT (STDMETHODCALLTYPE *Fn_long_out)(void *, LONG, void **);
typedef HRESULT (STDMETHODCALLTYPE *Fn_bstr_long)(void *, BSTR, LONG);
typedef HRESULT (STDMETHODCALLTYPE *Fn_connect)(void *, VARIANT, VARIANT, VARIANT, VARIANT);
typedef HRESULT (STDMETHODCALLTYPE *Fn_register)(void *, BSTR, BSTR, LONG, VARIANT, VARIANT, int, VARIANT, void **);
typedef HRESULT (STDMETHODCALLTYPE *Fn_run)(void *, VARIANT, void **);
typedef HRESULT (STDMETHODCALLTYPE *Fn_item)(void *, VARIANT, void **);
typedef ULONG (STDMETHODCALLTYPE *Fn_release)(void *);
static void release(void *o) { if (o) SLOT(o, 2, Fn_release)(o); }

enum { SVC_GETFOLDER = 7, SVC_CONNECT = 10, SVC_CONNECTED = 11, SVC_VERSION = 15 };
enum { F_GETTASK = 13, F_GETTASKS = 14, F_DELETETASK = 15, F_REGISTER = 16 };
enum { T_NAME = 7, T_PATH = 8, T_STATE = 9, T_ENABLED = 10, T_PUTENABLED = 11, T_RUN = 12, T_XML = 20 };
enum { C_COUNT = 7, C_ITEM = 8 };

#define TASK_NAME L"NovaOS edgeupdtest"

static void scheduler(const WCHAR *self, const WCHAR *mark)
{
    void *svc = 0, *root = 0, *task = 0, *again = 0, *list = 0, *item = 0;
    HRESULT hr = CoCreateInstance(&CLSID_TaskScheduler_, 0, CLSCTX_INPROC_SERVER, &IID_ITaskService_, &svc);
    CHECK("CoCreateInstance(TaskScheduler)", hr == S_OK && svc);
    if (!svc) return;
    VARIANT empty;
    VariantInit(&empty);
    BSTR rootname = SysAllocString(L"\\");
    hr = SLOT(svc, SVC_GETFOLDER, Fn_bstr_out)(svc, rootname, &root);
    CHECK("GetFolder before Connect fails", FAILED(hr) && !root);
    CHECK("Connect", SLOT(svc, SVC_CONNECT, Fn_connect)(svc, empty, empty, empty, empty) == S_OK);
    VARIANT_BOOL on = 0;
    CHECK("get_Connected", SLOT(svc, SVC_CONNECTED, Fn_get_bool)(svc, &on) == S_OK && on == VARIANT_TRUE);
    DWORD version = 0;
    CHECK("get_HighestVersion", SLOT(svc, SVC_VERSION, HRESULT (STDMETHODCALLTYPE *)(void *, DWORD *))(svc, &version) == S_OK &&
          version >= 0x10002);
    CHECK("GetFolder(\\)", SLOT(svc, SVC_GETFOLDER, Fn_bstr_out)(svc, rootname, &root) == S_OK && root);
    if (!root) { release(svc); return; }

    BSTR name = SysAllocString(TASK_NAME);
    SLOT(root, F_DELETETASK, Fn_bstr_long)(root, name, 0);         /* from an earlier run */
    WCHAR xml[2048];
    _snwprintf(xml, 2048,
             L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
             L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
             L"  <RegistrationInfo><Author>NovaOS</Author><Description>edgeupdtest &amp; friends</Description></RegistrationInfo>\r\n"
             L"  <Triggers><LogonTrigger><Enabled>true</Enabled></LogonTrigger></Triggers>\r\n"
             L"  <Settings><Enabled>true</Enabled><Hidden>false</Hidden></Settings>\r\n"
             L"  <Actions Context=\"Author\"><Exec><Command>%ls</Command><Arguments>mark \"%ls\"</Arguments></Exec></Actions>\r\n"
             L"</Task>\r\n", self, mark);
    BSTR bxml = SysAllocString(xml), bad = SysAllocString(L"<Task><Oops>");
    hr = SLOT(root, F_REGISTER, Fn_register)(root, name, bad, 6, empty, empty, 3, empty, &task);
    CHECK("RegisterTask refuses malformed XML", hr == (HRESULT)0x8004131A && !task);
    hr = SLOT(root, F_REGISTER, Fn_register)(root, name, bxml, 4 /* TASK_UPDATE */, empty, empty, 3, empty, &task);
    CHECK("TASK_UPDATE of a missing task fails", hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    hr = SLOT(root, F_REGISTER, Fn_register)(root, name, bxml, 6 /* TASK_CREATE_OR_UPDATE */, empty, empty, 3, empty, &task);
    CHECK("RegisterTask", hr == S_OK && task);
    hr = SLOT(root, F_REGISTER, Fn_register)(root, name, bxml, 2 /* TASK_CREATE */, empty, empty, 3, empty, &again);
    CHECK("TASK_CREATE of an existing task fails", hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) && !again);
    release(task);
    task = 0;

    CHECK("GetTask", SLOT(root, F_GETTASK, Fn_bstr_out)(root, name, &task) == S_OK && task);
    if (task) {
        BSTR s = 0;
        CHECK("get_Name", SLOT(task, T_NAME, Fn_get_bstr)(task, &s) == S_OK && s && !wcscmp(s, TASK_NAME));
        SysFreeString(s); s = 0;
        CHECK("get_Path", SLOT(task, T_PATH, Fn_get_bstr)(task, &s) == S_OK && s && !wcscmp(s, L"\\" TASK_NAME));
        SysFreeString(s); s = 0;
        CHECK("get_Xml", SLOT(task, T_XML, Fn_get_bstr)(task, &s) == S_OK && s && wcsstr(s, L"<Author>NovaOS</Author>"));
        SysFreeString(s);
        LONG state = 0;
        CHECK("get_State ready", SLOT(task, T_STATE, Fn_get_long)(task, &state) == S_OK && state == 3);
        CHECK("put_Enabled(false)", SLOT(task, T_PUTENABLED, Fn_put_bool)(task, VARIANT_FALSE) == S_OK);
        on = VARIANT_TRUE;
        CHECK("get_Enabled false", SLOT(task, T_ENABLED, Fn_get_bool)(task, &on) == S_OK && on == VARIANT_FALSE);
        CHECK("get_State disabled", SLOT(task, T_STATE, Fn_get_long)(task, &state) == S_OK && state == 1);
        CHECK("put_Enabled(true)", SLOT(task, T_PUTENABLED, Fn_put_bool)(task, VARIANT_TRUE) == S_OK);
        void *running = 0;
        DeleteFileW(mark);
        CHECK("Run", SLOT(task, T_RUN, Fn_run)(task, empty, &running) == S_OK && running);
        release(running);
        DWORD a = INVALID_FILE_ATTRIBUTES;
        for (int i = 0; i < 300 && a == INVALID_FILE_ATTRIBUTES; i++) { Sleep(100); a = GetFileAttributesW(mark); }
        CHECK("the task's action ran", a != INVALID_FILE_ATTRIBUTES);
        release(task);
    }

    CHECK("GetTasks", SLOT(root, F_GETTASKS, Fn_long_out)(root, 0, &list) == S_OK && list);
    if (list) {
        LONG n = 0;
        VARIANT byname;
        VariantInit(&byname);
        V_VT(&byname) = VT_BSTR;
        V_BSTR(&byname) = name;
        CHECK("get_Count", SLOT(list, C_COUNT, Fn_get_long)(list, &n) == S_OK && n >= 1);
        CHECK("get_Item by name", SLOT(list, C_ITEM, Fn_item)(list, byname, &item) == S_OK && item);
        release(item);
        release(list);
    }
    WCHAR file[MAX_PATH];
    GetWindowsDirectoryW(file, MAX_PATH);
    wcscat(file, L"\\System32\\Tasks\\" TASK_NAME);
    PVOID old;
    BOOL off = Wow64DisableWow64FsRedirection(&old);
    CHECK("the task is a file in System32\\Tasks", GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES);
    CHECK("DeleteTask", SLOT(root, F_DELETETASK, Fn_bstr_long)(root, name, 0) == S_OK);
    CHECK("the file is gone", GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES);
    if (off) Wow64RevertWow64FsRedirection(old);
    task = 0;
    CHECK("GetTask after DeleteTask fails",
          SLOT(root, F_GETTASK, Fn_bstr_out)(root, name, &task) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && !task);
    SysFreeString(name); SysFreeString(bxml); SysFreeString(bad); SysFreeString(rootname);
    release(root);
    release(svc);
}

/* ---- the Data Protection API ---- */
typedef struct { DWORD cbData; BYTE *pbData; } Blob;
typedef BOOL (WINAPI *PROTECT)(Blob *, LPCWSTR, Blob *, PVOID, PVOID, DWORD, Blob *);
typedef BOOL (WINAPI *UNPROTECT)(Blob *, LPWSTR *, Blob *, PVOID, PVOID, DWORD, Blob *);

static void dpapi(void)
{
    HMODULE c = LoadLibraryA("crypt32.dll");
    PROTECT protect = (PROTECT)GetProcAddress(c, "CryptProtectData");
    UNPROTECT unprotect = (UNPROTECT)GetProcAddress(c, "CryptUnprotectData");
    CHECK("crypt32 has the DPAPI", protect && unprotect);
    if (!protect || !unprotect) return;
    static const char secret[] = "edge update session token";
    Blob in = { sizeof secret, (BYTE *)secret }, ent = { 4, (BYTE *)"salt" }, bad = { 4, (BYTE *)"SALT" }, out = { 0 }, back = { 0 };
    for (DWORD flags = 0; flags <= 4; flags += 4) {                 /* the user's key, then CRYPTPROTECT_LOCAL_MACHINE */
        CHECK("CryptProtectData", protect(&in, L"desc", &ent, 0, 0, flags, &out) && out.cbData > sizeof secret);
        CHECK("the blob hides the data", out.pbData && !strstr((char *)out.pbData + 40, "token"));
        LPWSTR desc = 0;
        CHECK("CryptUnprotectData", unprotect(&out, &desc, &ent, 0, 0, 0, &back) && back.cbData == sizeof secret &&
              !memcmp(back.pbData, secret, sizeof secret) && desc && !wcscmp(desc, L"desc"));
        LocalFree(desc);
        LocalFree(back.pbData);
        back.pbData = 0;
        CHECK("the wrong entropy fails", !unprotect(&out, 0, &bad, 0, 0, 0, &back) && GetLastError() == 0x80090005);
        out.pbData[out.cbData / 2] ^= 1;
        CHECK("a changed blob fails", !unprotect(&out, 0, &ent, 0, 0, 0, &back));
        LocalFree(out.pbData);
    }
}

/* ---- shlwapi URLs ---- */
typedef HRESULT (WINAPI *URLCOMBINE)(LPCWSTR, LPCWSTR, LPWSTR, DWORD *, DWORD);
typedef HRESULT (WINAPI *URLESCAPEA)(LPCSTR, LPSTR, DWORD *, DWORD);
typedef HRESULT (WINAPI *URLUNESCAPEA)(LPSTR, LPSTR, DWORD *, DWORD);

static void urls(void)
{
    HMODULE s = LoadLibraryA("shlwapi.dll");
    URLCOMBINE combine = (URLCOMBINE)GetProcAddress(s, "UrlCombineW");
    URLESCAPEA escape = (URLESCAPEA)GetProcAddress(s, "UrlEscapeA");
    URLUNESCAPEA unescape = (URLUNESCAPEA)GetProcAddress(s, "UrlUnescapeA");
    CHECK("shlwapi has UrlCombineW, UrlEscapeA, UrlUnescapeA", combine && escape && unescape);
    if (!combine || !escape || !unescape) return;
    static const struct { const WCHAR *rel, *want; } cases[] = {
        { L"g", L"http://a/b/c/g" }, { L"../g", L"http://a/b/g" }, { L"/g", L"http://a/g" }, { L"//g", L"http://g" },
        { L"?y", L"http://a/b/c/d;p?y" }, { L"#s", L"http://a/b/c/d;p?q#s" }, { L"../../../g", L"http://a/g" },
        { L"g/./h/../i", L"http://a/b/c/g/i" }, { L"https://x/y", L"https://x/y" } };
    for (int i = 0; i < 9; i++) {
        WCHAR out[256];
        DWORD n = 256;
        BOOL ok = combine(L"http://a/b/c/d;p?q", cases[i].rel, out, &n, 0) == S_OK && !wcscmp(out, cases[i].want);
        if (!ok) printf("  UrlCombineW(%ls) = %ls\n", cases[i].rel, out);
        CHECK("UrlCombineW (RFC 3986 examples)", ok);
    }
    char buf[64];
    DWORD n = sizeof buf;
    CHECK("UrlEscapeA", escape("http://h/a b<c>", buf, &n, 0) == S_OK && !strcmp(buf, "http://h/a%20b%3Cc%3E") && n == 21);
    n = sizeof buf;
    char in[] = "a%20b%2fc%E9";
    CHECK("UrlUnescapeA", unescape(in, buf, &n, 0) == S_OK && !strcmp(buf, "a b/c\xE9") && n == 6);
    CHECK("UrlUnescapeA in place", unescape(in, 0, 0, 0x00100000) == S_OK && !strcmp(in, "a b/c\xE9"));
}

/* ---- package names (no package is installed) ---- */
typedef LONG (WINAPI *FAMILYFROMFULL)(PCWSTR, UINT32 *, PWSTR);
typedef LONG (WINAPI *IDFROMFULL)(PCWSTR, UINT32, UINT32 *, BYTE *);
typedef LONG (WINAPI *BYFAMILY)(PCWSTR, UINT32 *, PWSTR *, UINT32 *, WCHAR *);
typedef struct { UINT32 reserved, processorArchitecture; UINT64 version; PWSTR name, publisher, resourceId, publisherId; } PackageId;

static void packages(void)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");
    FAMILYFROMFULL family = (FAMILYFROMFULL)GetProcAddress(k, "PackageFamilyNameFromFullName");
    IDFROMFULL id = (IDFROMFULL)GetProcAddress(k, "PackageIdFromFullName");
    BYFAMILY byfamily = (BYFAMILY)GetProcAddress(k, "GetPackagesByPackageFamily");
    CHECK("kernel32 has the package-name functions", family && id && byfamily);
    if (!family || !id || !byfamily) return;
    static const WCHAR full[] = L"Microsoft.WebView2Runtime_154.0.4258.53_x64__8wekyb3d8bbwe";
    WCHAR out[128];
    UINT32 n = 2;
    CHECK("PackageFamilyNameFromFullName asks for room", family(full, &n, out) == ERROR_INSUFFICIENT_BUFFER && n == 40);
    n = 128;
    CHECK("PackageFamilyNameFromFullName", family(full, &n, out) == ERROR_SUCCESS && !wcscmp(out, L"Microsoft.WebView2Runtime_8wekyb3d8bbwe"));
    CHECK("PackageFamilyNameFromFullName refuses a bad name", family(L"no-underscores", &n, out) == ERROR_INVALID_PARAMETER);
    BYTE buf[512];
    UINT32 len = sizeof buf;
    PackageId *p = (PackageId *)buf;
    CHECK("PackageIdFromFullName", id(full, 0, &len, buf) == ERROR_SUCCESS && p->processorArchitecture == 9 &&
          p->version == ((UINT64)154 << 48 | (UINT64)0 << 32 | (UINT64)4258 << 16 | 53) &&
          !wcscmp(p->name, L"Microsoft.WebView2Runtime") && !wcscmp(p->publisherId, L"8wekyb3d8bbwe") && !p->resourceId[0]);
    UINT32 count = 9, size = 9;
    CHECK("GetPackagesByPackageFamily: none installed",
          byfamily(L"Microsoft.WebView2Runtime_8wekyb3d8bbwe", &count, 0, &size, 0) == ERROR_SUCCESS && !count && !size);
}

/* ---- the rest: sessions, security descriptors, enrolment, COM ---- */
typedef struct { DWORD SessionId; LPWSTR pWinStationName; int State; } SessionInfoW;
typedef BOOL (WINAPI *ENUMSESSIONS)(HANDLE, DWORD, DWORD, SessionInfoW **, DWORD *);
typedef void (WINAPI *WTSFREE)(PVOID);
typedef HRESULT (WINAPI *ISREGISTERED)(BOOL *, DWORD, LPWSTR);
typedef HRESULT (WINAPI *AADJOIN)(LPCWSTR, PVOID *);

static void others(void)
{
    HMODULE w = LoadLibraryA("wtsapi32.dll");
    ENUMSESSIONS enumerate = (ENUMSESSIONS)GetProcAddress(w, "WTSEnumerateSessionsW");
    WTSFREE wfree = (WTSFREE)GetProcAddress(w, "WTSFreeMemory");
    SessionInfoW *s = 0;
    DWORD n = 0;
    CHECK("WTSEnumerateSessionsW", enumerate && enumerate(0, 0, 1, &s, &n) && n == 2 && s[1].SessionId == 1 && s[1].State == 0 &&
          !wcscmp(s[1].pWinStationName, L"Console"));
    if (s && wfree) wfree(s);

    /* MakeAbsoluteSD undoes MakeSelfRelativeSD */
    SECURITY_DESCRIPTOR sd, abs;
    BYTE rel[256], acl_buf[64], dacl[64], sacl[64], owner[64], group[64];
    SID_IDENTIFIER_AUTHORITY world = { SECURITY_WORLD_SID_AUTHORITY };
    PSID everyone;
    AllocateAndInitializeSid(&world, 1, 0, 0, 0, 0, 0, 0, 0, 0, &everyone);
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    InitializeAcl((PACL)acl_buf, sizeof acl_buf, ACL_REVISION);
    AddAccessAllowedAce((PACL)acl_buf, ACL_REVISION, GENERIC_READ, everyone);
    SetSecurityDescriptorDacl(&sd, TRUE, (PACL)acl_buf, FALSE);
    SetSecurityDescriptorOwner(&sd, everyone, FALSE);
    DWORD rn = sizeof rel, an = 1, dn = 1, sn = 1, on = 1, gn = 1;
    MakeSelfRelativeSD(&sd, rel, &rn);
    CHECK("MakeAbsoluteSD asks for room", !MakeAbsoluteSD(rel, &abs, &an, (PACL)dacl, &dn, (PACL)sacl, &sn, owner, &on, group, &gn) &&
          GetLastError() == ERROR_INSUFFICIENT_BUFFER && dn == ((PACL)acl_buf)->AclSize && on == GetLengthSid(everyone) && !sn && !gn);
    an = sizeof abs; dn = sn = on = gn = 64;
    PSID got = 0;
    BOOL present = FALSE, dflt;
    PACL gotacl = 0;
    CHECK("MakeAbsoluteSD", MakeAbsoluteSD(rel, &abs, &an, (PACL)dacl, &dn, (PACL)sacl, &sn, owner, &on, group, &gn) &&
          GetSecurityDescriptorOwner(&abs, &got, &dflt) && got == (PSID)owner && EqualSid(got, everyone) &&
          GetSecurityDescriptorDacl(&abs, &present, &gotacl, &dflt) && present && gotacl == (PACL)dacl && gotacl->AceCount == 1);
    FreeSid(everyone);

    HMODULE m = LoadLibraryA("MDMRegistration.dll");
    ISREGISTERED registered = (ISREGISTERED)GetProcAddress(m, "IsDeviceRegisteredWithManagement");
    BOOL yes = TRUE;
    CHECK("IsDeviceRegisteredWithManagement: not enrolled", registered && registered(&yes, 0, 0) == S_OK && !yes);
    HMODULE na = LoadLibraryA("netapi32.dll");
    AADJOIN aad = (AADJOIN)GetProcAddress(na, "NetGetAadJoinInformation");
    PVOID info = (PVOID)1;
    CHECK("NetGetAadJoinInformation: not joined", aad && aad(0, &info) == S_OK && !info);

    static const GUID iid = { 0x12345678, 1, 2, { 3, 4, 5, 6, 7, 8, 9, 10 } };
    CHECK("CoRegisterPSClsid", CoRegisterPSClsid(&iid, &iid) == S_OK);
    void *ctx = (void *)1;
    CHECK("CoGetCallContext outside a call", CoGetCallContext(&IID_IUnknown, &ctx) == (HRESULT)0x80010117 && !ctx);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "mark")) {                     /* what the test task runs */
        FILE *f = fopen(argv[2], "w");
        if (f) { fputs("ran\n", f); fclose(f); }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "utf16")) {                    /* a UTF-16 log, as Edge Update writes one */
        FILE *f = fopen(argv[2], "wb");
        if (!f) return 1;
        fputc(0xFF, f);
        fputc(0xFE, f);
        for (int i = 0; i < 1000; i++) {
            WCHAR line[64];
            int n = _snwprintf(line, 64, L"line %d caf\x00e9 \xD83D\xDE00 ok\r\n", i);
            fwrite(line, sizeof(WCHAR), n, f);
        }
        fwrite(L"end of the UTF-16 file\r\n", sizeof(WCHAR), 24, f);
        fclose(f);
        return 0;
    }
    WCHAR self[MAX_PATH], mark[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    GetTempPathW(MAX_PATH, mark);
    wcscat(mark, sizeof(void *) == 8 ? L"edgeupdtest-64.txt" : L"edgeupdtest-32.txt");
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    scheduler(self, mark);
    dpapi();
    urls();
    packages();
    others();
    DeleteFileW(mark);
    CoUninitialize();
    printf("edgeupdtest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
