/* tlregdtr.c — __tlregdtor, which runs a C++ thread_local's destructor
 * when its thread ends (the MSVC CRT's tlregdtr.cpp).  Built into
 * tlregdtr.lib, so only a program that has such an object pulls it in,
 * with its TLS callback and per-thread list; the rest keep no TLS
 * callback.  Needs tlssup.c's TLS directory, which every program has. */
#include <windows.h>
#include <winnt.h>

/* the compiler registers each of a thread's thread_local objects with a
 * destructor as it constructs it; the callbacks run, last first, when the
 * thread exits (or the program ends, for the thread that ends it).  Up to
 * 32 a thread, more than XAML or the STL use. */
typedef void (__cdecl *tls_dtor)(void);
static __declspec(thread) tls_dtor tls_dtors[32];
static __declspec(thread) int tls_ndtors;

int __cdecl __tlregdtor(tls_dtor fn)
{
    if (tls_ndtors == (int)(sizeof tls_dtors / sizeof tls_dtors[0]))
        return -1;
    tls_dtors[tls_ndtors++] = fn;
    return 0;
}

static VOID __stdcall tls_run_dtors(PVOID module, DWORD reason, PVOID reserved)
{
    (void)module; (void)reserved;
    if (reason != DLL_THREAD_DETACH && reason != DLL_PROCESS_DETACH)
        return;
    while (tls_ndtors > 0)
        tls_dtors[--tls_ndtors]();
}

#pragma section(".CRT$XLD", long, read)
__declspec(allocate(".CRT$XLD")) PIMAGE_TLS_CALLBACK __xl_d = tls_run_dtors;
