#define NOVA_BUILD_NTDLL
/*
 * ntdll_fls.c — fiber-local storage
 *
 * FLS slots are separate from TLS slots, as on Windows: 128 indexes, each
 * with an optional callback.  Every fiber (a thread that never made one
 * counts as a fiber) has its own block of values, reached from the TEB's
 * FlsData field; kernel32's SwitchToFiber swaps that field with the
 * fiber.  A slot's callback runs on each value still set when the thread
 * exits (before the DLLs' DLL_THREAD_DETACH, as on Windows), when the
 * fiber is deleted, and on every thread's value when the slot is freed.
 * The C runtime Microsoft ships (vcruntime140, ucrtbase) keeps its
 * per-thread data this way, so without the callbacks each ended thread
 * leaks it.
 */

#include <winternl.h>
#include <winnt.h>

#ifndef HEAP_ZERO_MEMORY
#define HEAP_ZERO_MEMORY 0x8
#endif
#define FLS_SLOTS 128                   /* FLS_MAXIMUM_AVAILABLE */

typedef struct FlsData {
    struct FlsData *next, *prev;        /* every live block, so FlsFree reaches them all */
    PVOID slot[FLS_SLOTS];
} FlsData;

typedef VOID (NTAPI *FLS_CALLBACK)(PVOID);

/* Callbacks run under the lock (a critical section, so a callback may set
 * or read FLS values itself): FlsFree on one thread and the exit of
 * another never both see the same value */
static RTL_CRITICAL_SECTION g_lock = { 0, -1, 0, 0, 0, 0 };
static FLS_CALLBACK g_cb[FLS_SLOTS];
static BYTE g_used[FLS_SLOTS];
static FlsData g_all = { &g_all, &g_all, { 0 } };

static FlsData **fls_ptr(void) { return (FlsData **)(NtCurrentTebBytes() + TEB_FLS_DATA); }

NTSYSAPI NTSTATUS NTAPI RtlFlsAlloc(FLS_CALLBACK cb, ULONG *index)
{
    if (!index) return STATUS_INVALID_PARAMETER;
    NTSTATUS s = STATUS_NO_MEMORY;
    RtlEnterCriticalSection(&g_lock);
    for (ULONG i = 0; i < FLS_SLOTS; i++) {
        if (g_used[i]) continue;
        g_used[i] = 1;
        g_cb[i] = cb;
        for (FlsData *d = g_all.next; d != &g_all; d = d->next) d->slot[i] = 0;
        *index = i;
        s = STATUS_SUCCESS;
        break;
    }
    RtlLeaveCriticalSection(&g_lock);
    return s;
}

NTSYSAPI NTSTATUS NTAPI RtlFlsFree(ULONG i)
{
    if (i >= FLS_SLOTS) return STATUS_INVALID_PARAMETER;
    RtlEnterCriticalSection(&g_lock);
    if (!g_used[i]) { RtlLeaveCriticalSection(&g_lock); return STATUS_INVALID_PARAMETER; }
    for (FlsData *d = g_all.next; d != &g_all; d = d->next) {
        PVOID v = d->slot[i];
        d->slot[i] = 0;
        if (v && g_cb[i]) g_cb[i](v);
    }
    g_used[i] = 0;
    g_cb[i] = 0;
    RtlLeaveCriticalSection(&g_lock);
    return STATUS_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlFlsGetValue(ULONG i, PVOID *v)
{
    if (i >= FLS_SLOTS || !g_used[i] || !v) return STATUS_INVALID_PARAMETER;
    FlsData *d = *fls_ptr();
    *v = d ? d->slot[i] : 0;
    return STATUS_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlFlsSetValue(ULONG i, PVOID v)
{
    if (i >= FLS_SLOTS || !g_used[i]) return STATUS_INVALID_PARAMETER;
    FlsData *d = *fls_ptr();
    if (!d) {
        if (!v) return STATUS_SUCCESS;
        d = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
        if (!d) return STATUS_NO_MEMORY;
        RtlEnterCriticalSection(&g_lock);
        d->next = g_all.next;
        d->prev = &g_all;
        g_all.next->prev = d;
        g_all.next = d;
        RtlLeaveCriticalSection(&g_lock);
        *fls_ptr() = d;
    }
    d->slot[i] = v;
    return STATUS_SUCCESS;
}

/* flags 1: run the callbacks on the values still set; 2: free the block */
NTSYSAPI VOID NTAPI RtlProcessFlsData(PVOID data, ULONG flags)
{
    FlsData *d = data;
    if (!d) return;
    RtlEnterCriticalSection(&g_lock);
    if (flags & 1)
        for (ULONG i = 0; i < FLS_SLOTS; i++) {
            PVOID v = d->slot[i];
            if (!v || !g_used[i]) continue;
            d->slot[i] = 0;
            if (g_cb[i]) g_cb[i](v);
        }
    if (flags & 2) {
        d->prev->next = d->next;
        d->next->prev = d->prev;
    }
    RtlLeaveCriticalSection(&g_lock);
    if (flags & 2) RtlFreeHeap(RtlGetProcessHeap(), 0, d);
}

/* The exiting thread's (its running fiber's) block: callbacks first, then
 * the DLLs' DLL_THREAD_DETACH, then the block goes */
void nova_fls_thread_exit(int stage)
{
    FlsData *d = *fls_ptr();
    if (!d) return;
    if (stage == 0) { RtlProcessFlsData(d, 1); return; }
    *fls_ptr() = 0;
    RtlProcessFlsData(d, 2);
}
