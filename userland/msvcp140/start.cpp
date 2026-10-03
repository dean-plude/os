/*
 * msvcp140.dll's start-up, and the pieces of the VC runtime's static
 * library (vcstartup) that every MSVC-built module links into itself:
 * the C and C++ initializers, atexit for this module and the type_info
 * name list (operator new and delete are in new.cpp).  Written for NovaOS.
 */
#include <stdlib.h>
#include <malloc.h>
#include <windows.h>

extern "C" {
typedef int(__cdecl *_PIFV)(void);
typedef void(__cdecl *_PVFV)(void);

/* the linker sorts .CRT$X?? by name: these bracket each table */
#pragma section(".CRT$XIA", long, read)
#pragma section(".CRT$XIZ", long, read)
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)
__declspec(allocate(".CRT$XIA")) _PIFV __xi_a[] = { nullptr };
__declspec(allocate(".CRT$XIZ")) _PIFV __xi_z[] = { nullptr };
__declspec(allocate(".CRT$XCA")) _PVFV __xc_a[] = { nullptr };
__declspec(allocate(".CRT$XCZ")) _PVFV __xc_z[] = { nullptr };

/* this module's atexit list (static destructors), run when it unloads */
static _PVFV *g_atexit;
static int g_natexit, g_capexit;
static CRITICAL_SECTION g_atexit_lock;

int __cdecl atexit(_PVFV fn)
{
    EnterCriticalSection(&g_atexit_lock);
    if (g_natexit == g_capexit) {
        int cap = g_capexit ? g_capexit * 2 : 64;
        _PVFV *n = static_cast<_PVFV *>(realloc(g_atexit, cap * sizeof(_PVFV)));
        if (!n) { LeaveCriticalSection(&g_atexit_lock); return -1; }
        g_atexit = n;
        g_capexit = cap;
    }
    g_atexit[g_natexit++] = fn;
    LeaveCriticalSection(&g_atexit_lock);
    return 0;
}

_onexit_t __cdecl _onexit(_onexit_t fn)
{
    return atexit(reinterpret_cast<_PVFV>(fn)) == 0 ? fn : nullptr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID);

BOOL WINAPI _DllMainCRTStartup(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_atexit_lock);
        for (_PIFV *p = __xi_a; p < __xi_z; p++)
            if (*p && (*p)() != 0) return FALSE;
        for (_PVFV *p = __xc_a; p < __xc_z; p++)
            if (*p) (*p)();
    }
    BOOL ok = DllMain(inst, reason, reserved);
    if (reason == DLL_PROCESS_DETACH) {
        while (g_natexit > 0) g_atexit[--g_natexit]();
        free(g_atexit);
        g_atexit = nullptr;
    }
    return ok;
}

/* floating point is in use (the linker's marker) */
int _fltused = 0x9875;

/* type_info's vftable, which the RTTI type descriptors the compiler emits
 * point at.  Only its address is taken (type_info objects are never
 * deleted through it); vcruntime140 exports the real one as data. */
static void __cdecl type_info_nodelete(void) {}
extern const void *const type_info_vftable[1] __asm__("??_7type_info@@6B@");
const void *const type_info_vftable[1] = { (const void *)type_info_nodelete };

/* type_info::name()'s list of undecorated names */
struct __type_info_node { void *_MemPtr; __type_info_node *_Next; };
__type_info_node __type_info_root_node;
}
