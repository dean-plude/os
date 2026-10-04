/* testdll.dll — exercises DllMain, exported functions and static TLS */
#define NOVA_BUILD_TESTDLL
#include <windows.h>

__declspec(dllexport) volatile LONG g_process_attach, g_process_detach;
__declspec(dllexport) volatile LONG g_thread_attach, g_thread_detach;

/* Static (implicit) TLS: each thread gets its own copy */
static __declspec(thread) int t_value = 0xABCD;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        /* dlltest's child: refuse to load, so the process must not start */
        if (GetEnvironmentVariableA("NOVA_TESTDLL_REFUSE", 0, 0)) return FALSE;
        InterlockedIncrement(&g_process_attach);
        break;
    case DLL_PROCESS_DETACH: InterlockedIncrement(&g_process_detach); break;
    case DLL_THREAD_ATTACH:  InterlockedIncrement(&g_thread_attach);  break;
    case DLL_THREAD_DETACH:  InterlockedIncrement(&g_thread_detach);  break;
    }
    return TRUE;
}

__declspec(dllexport) int testdll_add(int a, int b) { return a + b; }
__declspec(dllexport) int testdll_get_tls(void) { return t_value; }
__declspec(dllexport) void testdll_set_tls(int v) { t_value = v; }
__declspec(dllexport) LONG testdll_process_attach(void) { return g_process_attach; }
__declspec(dllexport) LONG testdll_thread_attach(void) { return g_thread_attach; }
