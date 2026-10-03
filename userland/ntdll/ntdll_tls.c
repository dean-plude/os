#define NOVA_BUILD_NTDLL
/*
 * ntdll_tls.c — TlsAlloc's indexes
 *
 * A process has 64 + 1024 TLS indexes, as on Windows.  The first 64 are
 * the TEB's TlsSlots; the other 1024 live in a per-thread array the TEB's
 * TlsExpansionSlots points at, allocated the first time the thread sets
 * one of them and freed when the thread ends (after the DLLs'
 * DLL_THREAD_DETACH, which may still read them).  A freed index reads
 * zero again in every thread: the kernel clears it in each TEB or array
 * (NtSetInformationThread ThreadZeroTlsCell), under this lock so that no
 * ending thread frees its array meanwhile.  Programs with many DLLs
 * (Firefox's) run past the first 64.
 */

#include <winternl.h>
#include <winnt.h>

#ifndef HEAP_ZERO_MEMORY
#define HEAP_ZERO_MEMORY 0x8
#endif
#define TLS_ALL (TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS)
#define ThreadZeroTlsCell 10

static RTL_CRITICAL_SECTION g_lock = { 0, -1, 0, 0, 0, 0 };
static BYTE g_used[TLS_ALL];

static PVOID **exp_ptr(void) { return (PVOID **)(NtCurrentTebBytes() + TEB_TLS_EXPANSION); }
static PVOID *slot_ptr(ULONG i) { return (PVOID *)(NtCurrentTebBytes() + TEB_TLS_SLOTS) + i; }

NTSYSAPI NTSTATUS NTAPI RtlTlsAlloc(ULONG *index)
{
    if (!index) return STATUS_INVALID_PARAMETER;
    NTSTATUS s = STATUS_NO_MEMORY;
    RtlEnterCriticalSection(&g_lock);
    for (ULONG i = 0; i < TLS_ALL; i++) {
        if (g_used[i]) continue;
        g_used[i] = 1;
        if (i < TLS_MINIMUM_AVAILABLE) *slot_ptr(i) = 0;
        else if (*exp_ptr()) (*exp_ptr())[i - TLS_MINIMUM_AVAILABLE] = 0;
        *index = i;
        s = STATUS_SUCCESS;
        break;
    }
    RtlLeaveCriticalSection(&g_lock);
    return s;
}

NTSYSAPI NTSTATUS NTAPI RtlTlsFree(ULONG i)
{
    if (i >= TLS_ALL) return STATUS_INVALID_PARAMETER;
    RtlEnterCriticalSection(&g_lock);
    if (!g_used[i]) { RtlLeaveCriticalSection(&g_lock); return STATUS_INVALID_PARAMETER; }
    NtSetInformationThread(NtCurrentThread(), ThreadZeroTlsCell, &i, sizeof(i));
    g_used[i] = 0;
    RtlLeaveCriticalSection(&g_lock);
    return STATUS_SUCCESS;
}

/* TlsGetValue reads the TEB itself; setting an expansion index may first
 * need the thread's array */
NTSYSAPI NTSTATUS NTAPI RtlTlsSetValue(ULONG i, PVOID v)
{
    if (i >= TLS_ALL) return STATUS_INVALID_PARAMETER;
    if (i < TLS_MINIMUM_AVAILABLE) { *slot_ptr(i) = v; return STATUS_SUCCESS; }
    PVOID *a = *exp_ptr();
    if (!a) {
        if (!g_used[i]) return STATUS_INVALID_PARAMETER;
        a = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, TLS_EXPANSION_SLOTS * sizeof(PVOID));
        if (!a) return STATUS_NO_MEMORY;
        *exp_ptr() = a;
    }
    a[i - TLS_MINIMUM_AVAILABLE] = v;
    return STATUS_SUCCESS;
}

/* The ending thread's array goes after its DLL_THREAD_DETACH calls */
void nova_tls_thread_exit(void)
{
    if (!*exp_ptr()) return;
    RtlEnterCriticalSection(&g_lock);
    PVOID *a = *exp_ptr();
    *exp_ptr() = 0;
    RtlLeaveCriticalSection(&g_lock);
    if (a) RtlFreeHeap(RtlGetProcessHeap(), 0, a);
}
