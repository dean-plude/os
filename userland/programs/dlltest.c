/* dlltest.exe — DllMain, exported calls, static TLS, LoadLibrary, and
 * GetModuleFileName with more than 64 modules loaded */
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* Statically imported from testdll.dll */
__declspec(dllimport) int testdll_add(int a, int b);
__declspec(dllimport) int testdll_get_tls(void);
__declspec(dllimport) void testdll_set_tls(int v);
__declspec(dllimport) long testdll_process_attach(void);
__declspec(dllimport) long testdll_thread_attach(void);

static int pass, fail;
#define CHECK(c) do{ if(c) pass++; else { fail++; printf("FAIL line %d: %s\n",__LINE__,#c);} }while(0)

static volatile int tls_ok;
static DWORD WINAPI tworker(LPVOID a)
{
    (void)a;
    int before = testdll_get_tls();      /* fresh per-thread copy: initializer value */
    testdll_set_tls(0x1234);
    Sleep(2);
    if (before == 0xABCD && testdll_get_tls() == 0x1234) InterlockedIncrement((LONG*)&tls_ok);
    return 0;
}

/* A DLL the program imports refuses to load (its DllMain returns FALSE):
 * as on Windows, the process ends with STATUS_DLL_INIT_FAILED before its
 * entry point runs, here a copy of dlltest started with
 * NOVA_TESTDLL_REFUSE set, which testdll's DllMain answers with FALSE */
static void refused_start(void)
{
    char exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(0, exe, sizeof(exe));
    snprintf(cmd, sizeof(cmd), "\"%s\" child", exe);
    SetEnvironmentVariableA("NOVA_TESTDLL_REFUSE", "1");
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(exe, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi);
    SetEnvironmentVariableA("NOVA_TESTDLL_REFUSE", 0);
    CHECK(ok);
    if (!ok) return;
    CHECK(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (code != 0xC0000142) printf("refused DllMain: exit code 0x%08lx\n", (unsigned long)code);
    CHECK(code == 0xC0000142);                   /* STATUS_DLL_INIT_FAILED */
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/* GetModuleFileName and EnumProcessModules see every module, not just the
 * first 64 (GOG Galaxy's client has 89 at start, and MFC's DllMain fails
 * when GetModuleFileName does not know mfc140u.dll) */
static void many_modules(void)
{
    static const char *const names[] = {
        "activeds", "advapi32", "authz", "bcrypt", "bcryptprimitives", "cabinet", "cfgmgr32",
        "comctl32", "comdlg32", "credui", "crypt32", "dbghelp", "dwmapi", "gdi32", "imagehlp",
        "imm32", "kernelbase", "ktmw32", "mpr", "mscms", "msftedit", "msxml6", "ncrypt",
        "netapi32", "normaliz", "ole32", "oleacc", "oleaut32", "pdh", "powrprof", "propsys",
        "psapi", "riched20", "rpcrt4", "secur32", "sensapi", "setupapi", "shell32", "shfolder",
        "shlwapi", "taskschd", "user32", "userenv", "usp10", "uxtheme", "version", "wevtapi",
        "winspool", "wintab32", "wintrust", "winusb", "wldap32", "wsock32", "wtsapi32",
        "x3daudio1_7", "xinput1_1", "xinput1_2", "xinput1_3", "xinput1_4", "xinput9_1_0",
    };
    char path[MAX_PATH];
    HMODULE last = 0;
    const char *last_name = "";
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        HMODULE m = LoadLibraryA(names[i]);
        if (m) { last = m; last_name = names[i]; }
    }
    typedef BOOL (WINAPI *enumfn)(HANDLE, HMODULE *, DWORD, LPDWORD);
    enumfn en = (enumfn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32EnumProcessModules");
    CHECK(en != NULL);
    if (!en) return;
    static HMODULE mods[1024];
    DWORD need = 0;
    CHECK(en(GetCurrentProcess(), mods, sizeof(mods), &need));
    DWORD n = need / sizeof(HMODULE);
    if (n <= 64) printf("many modules: only %lu loaded\n", (unsigned long)n);
    CHECK(n > 64);
    BOOL found = FALSE;
    int unnamed = 0;
    for (DWORD i = 0; i < n && i < 1024; i++) {
        if (mods[i] == last) found = TRUE;
        if (!GetModuleFileNameA(mods[i], path, sizeof(path))) {
            if (!unnamed++) printf("many modules: no file name for module %lu\n", (unsigned long)i);
        }
    }
    CHECK(found);
    CHECK(unnamed == 0);
    DWORD k = last ? GetModuleFileNameA(last, path, sizeof(path)) : 0;
    const char *base = strrchr(path, '\\');
    CHECK(k > 0 && base && !_strnicmp(base + 1, last_name, strlen(last_name)));
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) {
        printf("FAIL: the child started although testdll refused to load\n");   /* (never runs) */
        return 3;
    }

    /* Static import works and DllMain(PROCESS_ATTACH) ran once at load */
    CHECK(testdll_add(20, 22) == 42);
    CHECK(testdll_process_attach() == 1);

    /* This (first) thread saw its TLS initializer */
    CHECK(testdll_get_tls() == 0xABCD);
    testdll_set_tls(0x9999);

    /* New threads: DLL_THREAD_ATTACH fires and each gets its own TLS */
    long attach_before = testdll_thread_attach();
    HANDLE t[3];
    for (int i = 0; i < 3; i++) t[i] = CreateThread(0, 0, tworker, 0, 0, 0);
    WaitForMultipleObjects(3, t, TRUE, 5000);
    for (int i = 0; i < 3; i++) CloseHandle(t[i]);
    CHECK(testdll_thread_attach() == attach_before + 3);
    CHECK(tls_ok == 3);
    CHECK(testdll_get_tls() == 0x9999);          /* our TLS untouched by the workers */

    /* GetProcAddress on the already-loaded module */
    HMODULE h = GetModuleHandleA("testdll.dll");
    CHECK(h != NULL);
    typedef int (*addfn)(int, int);
    addfn f = (addfn)GetProcAddress(h, "testdll_add");
    CHECK(f != NULL && f(100, 1) == 101);

    /* LoadLibrary of a not-yet-loaded DLL, then GetProcAddress */
    HMODULE k = LoadLibraryA("kernel32.dll");
    CHECK(k != NULL);
    void *p = (void *)GetProcAddress(k, "Sleep");
    CHECK(p != NULL);
    FreeLibrary(k);

    many_modules();
    refused_start();

    printf("DLL/TLS self-test: %d passed, %d failed\n", pass, fail);
    return fail;
}
