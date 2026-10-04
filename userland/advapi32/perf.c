/*
 * perf.c — the performance counter provider API (PerfStartProvider and
 * friends, Windows Vista's "V2" counters), which Chromium and Edge use to
 * publish their own counters.  NovaOS has no performance counter consumers
 * (no perfmon, no PDH data source), so a provider starts, describes its
 * counter sets, makes instances (with their names, as Windows lays them
 * out) and points counters at its own memory, and nothing reads them.
 */

#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"

typedef struct {
    GUID CounterSetGuid;
    ULONG dwSize;
    ULONG InstanceId;
    ULONG InstanceNameOffset;
    ULONG InstanceNameSize;
} PERF_COUNTERSET_INSTANCE;

#define PROVIDER_MAGIC 0x50524656           /* "VFRP" */
typedef struct {
    ULONG magic;
    GUID guid;
} Provider;

static Provider *provider_of(HANDLE h)
{
    Provider *p = (Provider *)h;
    return p && p->magic == PROVIDER_MAGIC ? p : 0;
}

WINADVAPI ULONG WINAPI PerfStartProviderEx(GUID *guid, void *context, HANDLE *out)
{
    (void)context;
    if (!guid || !out) return ERROR_INVALID_PARAMETER;
    Provider *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Provider));
    if (!p) return ERROR_OUTOFMEMORY;
    p->magic = PROVIDER_MAGIC;
    p->guid = *guid;
    *out = (HANDLE)p;
    return ERROR_SUCCESS;
}

WINADVAPI ULONG WINAPI PerfStartProvider(GUID *guid, void *callback, HANDLE *out)
{
    (void)callback;
    return PerfStartProviderEx(guid, 0, out);
}

WINADVAPI ULONG WINAPI PerfStopProvider(HANDLE h)
{
    Provider *p = provider_of(h);
    if (!p) return ERROR_INVALID_HANDLE;
    p->magic = 0;
    HeapFree(GetProcessHeap(), 0, p);
    return ERROR_SUCCESS;
}

/* PERF_COUNTERSET_INFO, then its PERF_COUNTER_INFO array: checked for
 * size only, since nothing will ask about the counters */
WINADVAPI ULONG WINAPI PerfSetCounterSetInfo(HANDLE h, void *info, ULONG size)
{
    if (!provider_of(h)) return ERROR_INVALID_HANDLE;
    if (!info || size < 32) return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

/* An instance: the header, then its name (UTF-16, 8-byte aligned), then
 * room for the counters' values the provider sets */
WINADVAPI PERF_COUNTERSET_INSTANCE *WINAPI PerfCreateInstance(HANDLE h, const GUID *set, PCWSTR name, ULONG id)
{
    if (!provider_of(h) || !set || !name) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    ULONG chars = 0;
    while (name[chars]) chars++;
    ULONG name_size = (chars + 1) * sizeof(WCHAR);
    ULONG size = (ULONG)sizeof(PERF_COUNTERSET_INSTANCE) + ((name_size + 7) & ~7u);
    PERF_COUNTERSET_INSTANCE *in = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!in) { SetLastError(ERROR_OUTOFMEMORY); return 0; }
    in->CounterSetGuid = *set;
    in->dwSize = size;
    in->InstanceId = id;
    in->InstanceNameOffset = sizeof(PERF_COUNTERSET_INSTANCE);
    in->InstanceNameSize = name_size;
    WCHAR *n = (WCHAR *)((BYTE *)in + in->InstanceNameOffset);
    for (ULONG i = 0; i <= chars; i++) n[i] = name[i];
    return in;
}

WINADVAPI ULONG WINAPI PerfDeleteInstance(HANDLE h, PERF_COUNTERSET_INSTANCE *in)
{
    if (!provider_of(h)) return ERROR_INVALID_HANDLE;
    if (!in) return ERROR_INVALID_PARAMETER;
    HeapFree(GetProcessHeap(), 0, in);
    return ERROR_SUCCESS;
}

/* Counter values: the provider keeps them; nobody collects them */
WINADVAPI ULONG WINAPI PerfSetCounterRefValue(HANDLE h, PERF_COUNTERSET_INSTANCE *in, ULONG counter, void *address)
{
    (void)counter; (void)address;
    if (!provider_of(h)) return ERROR_INVALID_HANDLE;
    return in ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}

WINADVAPI ULONG WINAPI PerfSetULongCounterValue(HANDLE h, PERF_COUNTERSET_INSTANCE *in, ULONG counter, ULONG v)
{
    (void)counter; (void)v;
    if (!provider_of(h)) return ERROR_INVALID_HANDLE;
    return in ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}

WINADVAPI ULONG WINAPI PerfSetULongLongCounterValue(HANDLE h, PERF_COUNTERSET_INSTANCE *in, ULONG counter, ULONGLONG v)
{
    (void)counter; (void)v;
    if (!provider_of(h)) return ERROR_INVALID_HANDLE;
    return in ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}
