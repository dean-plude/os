/* dlltest.exe — DllMain, exported calls, static TLS, and LoadLibrary */
#include <stdio.h>
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

int main(void)
{
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

    printf("DLL/TLS self-test: %d passed, %d failed\n", pass, fail);
    return fail;
}
