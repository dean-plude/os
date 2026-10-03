/* msvcrt / ucrtbase: the C runtime's per-thread data
 *
 * On Windows errno, _doserrno, the floating-point error code, rand's seed,
 * strtok's position and the buffers gmtime, asctime, _wcserror and tmpnam
 * return live in a block per thread, so one thread's failing call never
 * changes what another thread reads.  The block comes from a TLS slot: made
 * on first use (every thread starts with errno 0 and rand seeded with 1),
 * freed by DllMain when the thread or the DLL goes away.
 */
#define NOVA_BUILD_MSVCRT
#include <windows.h>
#include "ptd.h"

static volatile LONG g_slot = -1;       /* TLS_OUT_OF_INDEXES until first use */
static NovaPtd g_fallback;              /* out of slots or memory: shared, as before */

static DWORD ptd_slot(void)
{
    if (g_slot != -1) return (DWORD)g_slot;
    DWORD s = TlsAlloc();
    if (s == TLS_OUT_OF_INDEXES) return s;
    if (InterlockedCompareExchange(&g_slot, (LONG)s, -1) != -1) TlsFree(s);
    return (DWORD)g_slot;
}

static NovaPtd *ptd_new(void)
{
    NovaPtd *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*p));
    if (p) p->rand = 1;
    return p;
}

NovaPtd *__nova_ptd(void)
{
    DWORD le = GetLastError();          /* errno lookups never change GetLastError */
    DWORD s = ptd_slot();
    NovaPtd *p = s == TLS_OUT_OF_INDEXES ? 0 : TlsGetValue(s);
    if (!p && s != TLS_OUT_OF_INDEXES && (p = ptd_new())) TlsSetValue(s, p);
    SetLastError(le);
    if (p) return p;
    if (!g_fallback.rand) g_fallback.rand = 1;
    return &g_fallback;
}

static void ptd_free(void)
{
    if (g_slot == -1) return;
    NovaPtd *p = TlsGetValue((DWORD)g_slot);
    if (!p) return;
    TlsSetValue((DWORD)g_slot, 0);
    HeapFree(GetProcessHeap(), 0, p);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    if (reason == DLL_THREAD_DETACH) ptd_free();
    else if (reason == DLL_PROCESS_DETACH && !reserved) {   /* FreeLibrary, not process exit */
        ptd_free();
        if (g_slot != -1) TlsFree((DWORD)g_slot);
    }
    return TRUE;
}
