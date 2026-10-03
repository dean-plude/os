/* msitest.exe — Windows Installer self-test: rollback, transforms, patches
 * and services that start at boot, with the packages tools/msitest/mkpkg.py
 * writes to C:\Tests\Msi at build time
 *
 *   msitest transform     MsiDatabaseApplyTransform; TRANSFORMS= on install
 *                         (and one made for another product is refused)
 *   msitest patch         msiexec /p, then /uninstall of the patch
 *   msitest rollback      a package that fails half-way leaves nothing behind
 *   msitest service       installs an automatic service (started on install)
 *   msitest service-boot  after a restart: services.exe started it; uninstall
 *   msitest script        JScript and VBScript custom actions (types 5, 6, 21,
 *                         22, 37, 38, 53, 54) set properties, read them back
 *                         and write files; a failing script fails the install
 *
 * and, as the packages' own program:
 *   msitest mark FILE     writes FILE (the rollback custom action)
 *   msitest service       run by the service manager (NOVA_SERVICE set):
 *                         the NovaMsiTestSvc service, appending to svc.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winsvc.h>
#include <stdbool.h>

typedef unsigned long MSIHANDLE;
__declspec(dllimport) UINT WINAPI MsiOpenDatabaseW(LPCWSTR path, LPCWSTR persist, MSIHANDLE *out);
__declspec(dllimport) UINT WINAPI MsiDatabaseApplyTransformW(MSIHANDLE db, LPCWSTR path, int errors);
__declspec(dllimport) UINT WINAPI MsiDatabaseOpenViewW(MSIHANDLE db, LPCWSTR sql, MSIHANDLE *out);
__declspec(dllimport) UINT WINAPI MsiViewExecute(MSIHANDLE v, MSIHANDLE rec);
__declspec(dllimport) UINT WINAPI MsiViewFetch(MSIHANDLE v, MSIHANDLE *out);
__declspec(dllimport) UINT WINAPI MsiRecordGetStringA(MSIHANDLE h, UINT i, LPSTR buf, DWORD *pcch);
__declspec(dllimport) UINT WINAPI MsiCloseHandle(MSIHANDLE h);
__declspec(dllimport) UINT WINAPI MsiInstallProductW(LPCWSTR package, LPCWSTR cmdline);
__declspec(dllimport) int WINAPI MsiSetInternalUI(int level, HWND *phwnd);

#define PKG       "C:\\Tests\\Msi\\"
#define OUT       "C:\\Tests\\MsiOut\\"
#define BASE      "{6E1D0C3A-5A1B-4C2D-8E3F-00000000B001}"
#define RB        "{6E1D0C3A-5A1B-4C2D-8E3F-00000000F001}"
#define SVC       "{6E1D0C3A-5A1B-4C2D-8E3F-000000005001}"
#define SCRIPT    "{6E1D0C3A-5A1B-4C2D-8E3F-00000000C001}"
#define SCRIPT_FAIL "{6E1D0C3A-5A1B-4C2D-8E3F-00000000C002}"
#define SVC_NAME  "NovaMsiTestSvc"
#define UNINSTALL "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
#define MSIEXEC   "C:\\Windows\\System32\\msiexec.exe"

static int pass, fail;
#define CHECK(cond) do { if (cond) pass++; else { fail++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); if (_a == _b) pass++; \
    else { fail++; printf("FAIL line %d: %s is %ld, not %ld\n", __LINE__, #a, _a, _b); } } while (0)

/* msiexec with these arguments; its exit code */
static DWORD msiexec(const char *args)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"" MSIEXEC "\" %s", args);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    printf("  msiexec %s\n", args);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0xFFFFFFFF;
    WaitForSingleObject(pi.hProcess, 300000);
    DWORD code = 0xFFFFFFFE;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static UINT install(const WCHAR *pkg, const WCHAR *props)
{
    MsiSetInternalUI(2 /* INSTALLUILEVEL_NONE */, NULL);
    return MsiInstallProductW(pkg, props);
}

/* the text of a small file ("" if there is none) */
static const char *slurp(const char *path)
{
    static char buf[4096];
    buf[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

/* does the file (a log, of any size) contain @text */
static bool file_has(const char *path, const char *text)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    bool found = false;
    if (buf) {
        buf[fread(buf, 1, (size_t)n, f)] = 0;
        found = strstr(buf, text) != NULL;
        free(buf);
    }
    fclose(f);
    return found;
}

static bool exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/* a REG_SZ under HKLM ("" if it is not there) */
static const char *reg(const char *key, const char *name)
{
    static char buf[512];
    DWORD size = sizeof(buf) - 1, type;
    HKEY k;
    buf[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k)) return buf;
    if (RegQueryValueExA(k, name, NULL, &type, (BYTE *)buf, &size) || type != REG_SZ) buf[0] = 0;
    else buf[size] = 0;
    RegCloseKey(k);
    return buf;
}

static bool key_exists(const char *key)
{
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k)) return false;
    RegCloseKey(k);
    return true;
}

static int done(const char *what)
{
    printf("msitest %s: %d passed, %d failed\n", what, pass, fail);
    return fail ? 1 : 0;
}

/* ------------------------------------------------------------------ */

static int t_transform(void)
{
    /* the database API: the transform's Registry row appears */
    MSIHANDLE db = 0, view = 0, rec = 0;
    CHECK_EQ(MsiOpenDatabaseW(L"" PKG "base.msi", (LPCWSTR)0 /* read-only */, &db), 0);
    CHECK_EQ(MsiDatabaseApplyTransformW(db, L"" PKG "base.mst", 0), 0);
    CHECK_EQ(MsiDatabaseOpenViewW(db, L"SELECT `Value` FROM `Registry` WHERE `Name` = 'Edition'", &view), 0);
    CHECK_EQ(MsiViewExecute(view, 0), 0);
    CHECK_EQ(MsiViewFetch(view, &rec), 0);
    char v[64] = "";
    DWORD n = sizeof(v);
    if (rec) MsiRecordGetStringA(rec, 1, v, &n);
    CHECK(!strcmp(v, "Transformed"));
    if (rec) MsiCloseHandle(rec);
    if (view) MsiCloseHandle(view);
    if (db) MsiCloseHandle(db);

    /* one made for another product is refused: nothing installed */
    CHECK_EQ(install(L"" PKG "base.msi", L"TRANSFORMS=other.mst"), 1624);
    CHECK(!key_exists(UNINSTALL BASE));
    CHECK(!exists("C:\\Programs\\NovaMsiTest\\a.txt"));

    /* installed with it, kept with the product, undone with it */
    CHECK_EQ(install(L"" PKG "base.msi", L"TRANSFORMS=base.mst"), 0);
    CHECK(!strcmp(slurp("C:\\Programs\\NovaMsiTest\\a.txt"), "one\n"));
    CHECK(!strcmp(reg("SOFTWARE\\NovaMsiTest", "Version"), "1.0.0"));
    CHECK(!strcmp(reg("SOFTWARE\\NovaMsiTest", "Edition"), "Transformed"));
    CHECK(exists("C:\\Windows\\Installer\\" BASE "\\base.mst"));
    CHECK_EQ(msiexec("/x " BASE " /qn"), 0);
    CHECK(!key_exists("SOFTWARE\\NovaMsiTest"));
    CHECK(!key_exists(UNINSTALL BASE));
    CHECK(!exists("C:\\Programs\\NovaMsiTest\\a.txt"));
    return done("transform");
}

static int t_patch(void)
{
    CHECK_EQ(msiexec("/p " PKG "patch.msp /qn"), 1642);         /* its product is not installed */
    CHECK_EQ(msiexec("/i " PKG "base.msi /qn"), 0);
    CHECK(!strcmp(slurp("C:\\Programs\\NovaMsiTest\\a.txt"), "one\n"));

    CHECK_EQ(msiexec("/p " PKG "patch.msp /qn"), 0);
    CHECK(!strcmp(slurp("C:\\Programs\\NovaMsiTest\\a.txt"), "two\n"));
    CHECK(!strcmp(reg("SOFTWARE\\NovaMsiTest", "Version"), "1.0.1"));
    CHECK(!strcmp(reg(UNINSTALL BASE, "DisplayVersion"), "1.0.1"));
    CHECK(exists("C:\\Windows\\Installer\\" BASE "\\patch.msp"));

    /* a repair keeps the patch (it is recorded with the product) */
    CHECK_EQ(msiexec("/i " PKG "base.msi /qn"), 0);
    CHECK(!strcmp(slurp("C:\\Programs\\NovaMsiTest\\a.txt"), "two\n"));

    CHECK_EQ(msiexec("/uninstall " PKG "patch.msp /qn"), 0);
    CHECK(!strcmp(slurp("C:\\Programs\\NovaMsiTest\\a.txt"), "one\n"));
    CHECK(!strcmp(reg("SOFTWARE\\NovaMsiTest", "Version"), "1.0.0"));
    CHECK(!strcmp(reg(UNINSTALL BASE, "DisplayVersion"), "1.0.0"));
    CHECK(!exists("C:\\Windows\\Installer\\" BASE "\\patch.msp"));

    CHECK_EQ(msiexec("/x " BASE " /qn"), 0);
    CHECK(!key_exists(UNINSTALL BASE));
    CHECK(!exists("C:\\Programs\\NovaMsiTest\\a.txt"));
    return done("patch");
}

static int t_rollback(void)
{
    CreateDirectoryA("C:\\Tests\\MsiOut", NULL);
    DeleteFileA(OUT "rollback-ran.txt");
    CreateDirectoryA("C:\\Programs\\NovaRbTest", NULL);
    put("C:\\Programs\\NovaRbTest\\old.txt", "before\n");

    CHECK_EQ(install(L"" PKG "rollback.msi", NULL), 1603);
    CHECK(!strcmp(slurp("C:\\Programs\\NovaRbTest\\old.txt"), "before\n"));   /* put back */
    CHECK(!exists("C:\\Programs\\NovaRbTest\\new.txt"));
    CHECK(!exists("C:\\Programs\\NovaRbTest\\tool.exe"));
    CHECK(!key_exists("SOFTWARE\\NovaRbTest"));
    CHECK(!key_exists(UNINSTALL RB));
    CHECK(exists(OUT "rollback-ran.txt"));                               /* the rollback custom action ran */

    /* a folder the failed install made goes away too */
    DeleteFileA("C:\\Programs\\NovaRbTest\\old.txt");
    RemoveDirectoryA("C:\\Programs\\NovaRbTest");
    DeleteFileA(OUT "rollback-ran.txt");
    CHECK_EQ(install(L"" PKG "rollback.msi", NULL), 1603);
    CHECK(!exists("C:\\Programs\\NovaRbTest"));
    CHECK(exists(OUT "rollback-ran.txt"));

    /* with rollback off, what was done stays (as on Windows) */
    CHECK_EQ(install(L"" PKG "rollback.msi", L"DISABLEROLLBACK=1"), 1603);
    CHECK(exists("C:\\Programs\\NovaRbTest\\new.txt"));
    DeleteFileA("C:\\Programs\\NovaRbTest\\old.txt");
    DeleteFileA("C:\\Programs\\NovaRbTest\\new.txt");
    DeleteFileA("C:\\Programs\\NovaRbTest\\tool.exe");
    RemoveDirectoryA("C:\\Programs\\NovaRbTest");
    RegDeleteTreeA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NovaRbTest");
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NovaRbTest");
    return done("rollback");
}

static DWORD svc_state(void)
{
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE h = scm ? OpenServiceA(scm, SVC_NAME, SERVICE_QUERY_STATUS) : NULL;
    SERVICE_STATUS st;
    memset(&st, 0, sizeof(st));
    if (h) QueryServiceStatus(h, &st);
    if (h) CloseServiceHandle(h);
    if (scm) CloseServiceHandle(scm);
    return h ? st.dwCurrentState : 0;
}

static int count_runs(void)
{
    int n = 0;
    for (const char *p = slurp(OUT "svc.txt"); (p = strstr(p, "running")); p++) n++;
    return n;
}

static int t_service(void)
{
    CreateDirectoryA("C:\\Tests\\MsiOut", NULL);
    DeleteFileA(OUT "svc.txt");
    CHECK_EQ(install(L"" PKG "service.msi", NULL), 0);
    CHECK_EQ(svc_state(), SERVICE_RUNNING);
    CHECK_EQ(count_runs(), 1);
    HKEY k;
    DWORD start = 0, size = 4;
    if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\" SVC_NAME, 0, KEY_READ, &k)) {
        RegQueryValueExA(k, "Start", NULL, NULL, (BYTE *)&start, &size);
        RegCloseKey(k);
    }
    CHECK_EQ(start, SERVICE_AUTO_START);
    printf("restart to see it start at boot (msitest service-boot)\n");
    return done("service");
}

static int t_service_boot(void)
{
    for (int i = 0; i < 100 && svc_state() != SERVICE_RUNNING; i++) Sleep(100);
    CHECK_EQ(svc_state(), SERVICE_RUNNING);
    CHECK_EQ(count_runs(), 2);                      /* on install, and at this boot */
    CHECK_EQ(msiexec("/x " SVC " /qn"), 0);
    CHECK_EQ(svc_state(), 0);                       /* deleted */
    CHECK(!exists("C:\\Programs\\NovaSvcTest\\svc.exe"));
    CHECK(!key_exists(UNINSTALL SVC));
    return done("service-boot");
}

static int t_script(void)
{
    static const char *outs[] = { "script-js.txt", "script-vbs.txt", "script-jsfile.txt", "script-vbsfile.txt" };
    char path[MAX_PATH];
    CreateDirectoryA("C:\\Tests\\MsiOut", NULL);
    for (int i = 0; i < 4; i++) { snprintf(path, sizeof(path), OUT "%s", outs[i]); DeleteFileA(path); }
    RegDeleteTreeA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NovaScriptShell");

    CHECK_EQ(msiexec("/i " PKG "script.msi /qn /l*v " OUT "script.log"), 0);
    /* properties the scripts set, as the Registry table wrote them */
    CHECK(!strcmp(reg("SOFTWARE\\NovaScriptTest", "JsProp"), "from JScript"));               /* type 5 */
    CHECK(!strcmp(reg("SOFTWARE\\NovaScriptTest", "VbsProp"), "from VBScript"));             /* type 6 */
    CHECK(!strcmp(reg("SOFTWARE\\NovaScriptTest", "JsInline"), "from JScript!"));            /* type 37 */
    CHECK(!strcmp(reg("SOFTWARE\\NovaScriptTest", "VbsInline"), "from VBScript!"));          /* type 38 */
    CHECK(!strcmp(reg("SOFTWARE\\NovaScriptTest", "JsFromProperty"), "yes 1.0.0"));         /* type 53 */
    CHECK(!strcmp(reg("SOFTWARE\\NovaScriptTest", "VbsFromProperty"), "yes 1.0.0"));        /* type 54 */
    /* what they read back and wrote */
    CHECK(!strcmp(slurp(OUT "script-js.txt"),
                  "JS_PROP=from JScript\r\nProductName=Nova Script Test\r\n"
                  "INSTALLDIR=C:\\Programs\\NovaScriptTest\\\r\nComplete=3\r\n"));
    CHECK(!strcmp(slurp(OUT "script-vbs.txt"), "VBS_PROP=from VBScript\r\nProductName=NOVA SCRIPT TEST\r\nWords=3\r\n"));
    CHECK(!strcmp(slurp(OUT "script-jsfile.txt"), "CustomActionData=from JScript!"));            /* type 21 */
    CHECK(!strcmp(slurp(OUT "script-vbsfile.txt"),
                  "CustomActionData=from VBScript! shell=from VBScript!"));                      /* type 22 */
    CHECK(!strcmp(reg("SOFTWARE\\NovaScriptShell", "FromVbsFile"), "from VBScript!"));
    CHECK(file_has(OUT "script.log", "JScript says: from JScript"));
    CHECK(file_has(OUT "script.log", "VBScript says: from VBScript"));

    CHECK_EQ(msiexec("/x " SCRIPT " /qn"), 0);
    CHECK(!key_exists("SOFTWARE\\NovaScriptTest"));
    CHECK(!key_exists(UNINSTALL SCRIPT));
    RegDeleteTreeA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NovaScriptShell");

    /* a script that fails fails the installation (the one marked "may
     * fail" is ignored), and it is rolled back */
    CHECK_EQ(msiexec("/i " PKG "scriptfail.msi /qn /l*v " OUT "scriptfail.log"), 1603);
    CHECK(!key_exists(UNINSTALL SCRIPT_FAIL));
    CHECK(!exists("C:\\Programs\\NovaScriptFail\\a.txt"));
    CHECK(file_has(OUT "scriptfail.log", "This script fails on purpose"));
    CHECK(file_has(OUT "scriptfail.log", "this failure is ignored"));
    return done("script");
}

/* ------------------------------------------------------------------ */
/* the service itself */

static SERVICE_STATUS_HANDLE g_h;
static HANDLE g_stop;

static void report(DWORD state)
{
    SERVICE_STATUS st;
    memset(&st, 0, sizeof(st));
    st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    st.dwCurrentState = state;
    st.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
    SetServiceStatus(g_h, &st);
}

static DWORD WINAPI handler(DWORD ctl, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)type; (void)data; (void)ctx;
    if (ctl == SERVICE_CONTROL_STOP) { report(SERVICE_STOP_PENDING); SetEvent(g_stop); }
    return 0;
}

static void note(const char *what)
{
    CreateDirectoryA("C:\\Tests\\MsiOut", NULL);
    FILE *f = fopen(OUT "svc.txt", "ab");
    if (f) { fprintf(f, "%s\n", what); fclose(f); }
}

static void WINAPI svc_main(DWORD argc, LPSTR *argv)
{
    (void)argc;
    g_h = RegisterServiceCtrlHandlerExA(argv[0], handler, NULL);
    g_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    note("running");
    report(SERVICE_RUNNING);
    WaitForSingleObject(g_stop, INFINITE);
    note("stopped");
    report(SERVICE_STOPPED);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "service") && GetEnvironmentVariableA("NOVA_SERVICE", NULL, 0)) {
        SERVICE_TABLE_ENTRYA table[] = { { (LPSTR)SVC_NAME, svc_main }, { NULL, NULL } };
        return StartServiceCtrlDispatcherA(table) ? 0 : 1;
    }
    if (!strcmp(cmd, "mark") && argc > 2) { put(argv[2], "ran\n"); return 0; }
    if (!strcmp(cmd, "transform")) return t_transform();
    if (!strcmp(cmd, "patch")) return t_patch();
    if (!strcmp(cmd, "rollback")) return t_rollback();
    if (!strcmp(cmd, "service")) return t_service();
    if (!strcmp(cmd, "service-boot")) return t_service_boot();
    if (!strcmp(cmd, "script")) return t_script();
    printf("usage: msitest transform | patch | rollback | service | service-boot | script\n");
    return 2;
}
