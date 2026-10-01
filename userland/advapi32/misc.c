/*
 * misc.c — advapi32's event tracing (ETW), event log, services and a few
 * odds and ends.  NovaOS has no trace sessions, event log service or
 * service control manager: providers register successfully and are never
 * enabled, event-log reports go to the kernel log, and services report
 * that none exist.
 */

#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"

typedef ULONGLONG REGHANDLE;

/* -----------------------------------------------------------------------
 * Event tracing
 * ----------------------------------------------------------------------- */
static volatile LONG g_next_reg = 1;

WINADVAPI ULONG WINAPI EventRegister(const GUID *provider, PVOID cb, PVOID ctx, REGHANDLE *h)
{
    (void)provider; (void)cb; (void)ctx;
    *h = (REGHANDLE)__atomic_add_fetch(&g_next_reg, 1, __ATOMIC_RELAXED);
    return ERROR_SUCCESS;
}

WINADVAPI ULONG WINAPI EventUnregister(REGHANDLE h) { (void)h; return ERROR_SUCCESS; }
WINADVAPI BOOLEAN WINAPI EventEnabled(REGHANDLE h, const void *desc) { (void)h; (void)desc; return FALSE; }
WINADVAPI BOOLEAN WINAPI EventProviderEnabled(REGHANDLE h, UCHAR level, ULONGLONG kw) { (void)h; (void)level; (void)kw; return FALSE; }
WINADVAPI ULONG WINAPI EventWrite(REGHANDLE h, const void *desc, ULONG n, PVOID data) { (void)h; (void)desc; (void)n; (void)data; return ERROR_SUCCESS; }
WINADVAPI ULONG WINAPI EventWriteTransfer(REGHANDLE h, const void *desc, const GUID *a, const GUID *r, ULONG n, PVOID data)
{ (void)h; (void)desc; (void)a; (void)r; (void)n; (void)data; return ERROR_SUCCESS; }
WINADVAPI ULONG WINAPI EventWriteEx(REGHANDLE h, const void *desc, ULONG64 filter, ULONG flags, const GUID *a, const GUID *r, ULONG n, PVOID data)
{ (void)h; (void)desc; (void)filter; (void)flags; (void)a; (void)r; (void)n; (void)data; return ERROR_SUCCESS; }
WINADVAPI ULONG WINAPI EventWriteString(REGHANDLE h, UCHAR level, ULONGLONG kw, LPCWSTR s) { (void)h; (void)level; (void)kw; (void)s; return ERROR_SUCCESS; }
WINADVAPI ULONG WINAPI EventSetInformation(REGHANDLE h, int cls, PVOID info, ULONG n) { (void)h; (void)cls; (void)info; (void)n; return ERROR_SUCCESS; }

WINADVAPI ULONG WINAPI EventActivityIdControl(ULONG code, GUID *id)
{
    if (code == 3 /* EVENT_ACTIVITY_CTRL_CREATE_ID */ && id) SystemFunction036(id, sizeof(*id));
    else if (id && (code == 1 || code == 4)) { BYTE *b = (BYTE *)id; for (unsigned i = 0; i < sizeof(*id); i++) b[i] = 0; }
    return ERROR_SUCCESS;
}

WINADVAPI ULONG WINAPI RegisterTraceGuidsW(PVOID cb, PVOID ctx, const GUID *g, ULONG n, PVOID reg, LPCWSTR mof, LPCWSTR res, ULONGLONG *h)
{
    (void)cb; (void)ctx; (void)g; (void)n; (void)reg; (void)mof; (void)res;
    *h = 1;
    return ERROR_SUCCESS;
}
WINADVAPI ULONG WINAPI RegisterTraceGuidsA(PVOID cb, PVOID ctx, const GUID *g, ULONG n, PVOID reg, LPCSTR mof, LPCSTR res, ULONGLONG *h)
{
    (void)mof; (void)res;
    return RegisterTraceGuidsW(cb, ctx, g, n, reg, 0, 0, h);
}
WINADVAPI ULONG WINAPI UnregisterTraceGuids(ULONGLONG h) { (void)h; return ERROR_SUCCESS; }
WINADVAPI ULONG WINAPI TraceEvent(ULONGLONG h, PVOID ev) { (void)h; (void)ev; return ERROR_SUCCESS; }
WINADVAPI ULONGLONG WINAPI GetTraceLoggerHandle(PVOID buf) { (void)buf; SetLastError(ERROR_INVALID_HANDLE); return (ULONGLONG)-1; }
WINADVAPI UCHAR WINAPI GetTraceEnableLevel(ULONGLONG h) { (void)h; return 0; }
WINADVAPI ULONG WINAPI GetTraceEnableFlags(ULONGLONG h) { (void)h; return 0; }

/* -----------------------------------------------------------------------
 * Event log: reports go to the kernel log
 * ----------------------------------------------------------------------- */
WINADVAPI HANDLE WINAPI RegisterEventSourceW(LPCWSTR server, LPCWSTR source) { (void)server; (void)source; return (HANDLE)(ULONG_PTR)0x4E4F; }
WINADVAPI HANDLE WINAPI RegisterEventSourceA(LPCSTR server, LPCSTR source) { (void)server; (void)source; return (HANDLE)(ULONG_PTR)0x4E4F; }
WINADVAPI BOOL WINAPI DeregisterEventSource(HANDLE h) { (void)h; return TRUE; }

WINADVAPI BOOL WINAPI ReportEventW(HANDLE h, WORD type, WORD cat, DWORD id, PSID sid, WORD n, DWORD size, LPCWSTR *strings, LPVOID data)
{
    (void)h; (void)cat; (void)id; (void)sid; (void)size; (void)data;
    char line[256];
    int o = 0;
    const char *kind = type == 1 ? "error" : type == 2 ? "warning" : "event";
    while (*kind && o < 40) line[o++] = *kind++;
    line[o++] = ':';
    for (WORD i = 0; i < n && strings && o < 250; i++) {
        line[o++] = ' ';
        for (const WCHAR *s = strings[i]; s && *s && o < 250; s++) line[o++] = *s < 0x80 ? (char)*s : '?';
    }
    NtNovaDebugPrint(line, (ULONG)o);
    return TRUE;
}

WINADVAPI BOOL WINAPI ReportEventA(HANDLE h, WORD type, WORD cat, DWORD id, PSID sid, WORD n, DWORD size, LPCSTR *strings, LPVOID data)
{
    (void)h; (void)type; (void)cat; (void)id; (void)sid; (void)size; (void)data;
    for (WORD i = 0; i < n && strings; i++) NtNovaDebugPrint(strings[i], (ULONG)strlen_(strings[i]));
    return TRUE;
}

WINADVAPI HANDLE WINAPI OpenEventLogW(LPCWSTR server, LPCWSTR source) { (void)server; (void)source; return (HANDLE)(ULONG_PTR)0x4E4F; }
WINADVAPI BOOL WINAPI CloseEventLog(HANDLE h) { (void)h; return TRUE; }

/* -----------------------------------------------------------------------
 * Services: a control manager with no services in it
 * ----------------------------------------------------------------------- */
typedef void *SC_HANDLE;
#define SCM_HANDLE ((SC_HANDLE)(ULONG_PTR)0x5C4D)

WINADVAPI SC_HANDLE WINAPI OpenSCManagerW(LPCWSTR machine, LPCWSTR db, DWORD access) { (void)machine; (void)db; (void)access; return SCM_HANDLE; }
WINADVAPI SC_HANDLE WINAPI OpenSCManagerA(LPCSTR machine, LPCSTR db, DWORD access) { (void)machine; (void)db; (void)access; return SCM_HANDLE; }
WINADVAPI SC_HANDLE WINAPI OpenServiceW(SC_HANDLE scm, LPCWSTR name, DWORD access) { (void)scm; (void)name; (void)access; SetLastError(1060 /* ERROR_SERVICE_DOES_NOT_EXIST */); return 0; }
WINADVAPI SC_HANDLE WINAPI OpenServiceA(SC_HANDLE scm, LPCSTR name, DWORD access) { (void)scm; (void)name; (void)access; SetLastError(1060); return 0; }
WINADVAPI SC_HANDLE WINAPI CreateServiceW(SC_HANDLE scm, LPCWSTR n, LPCWSTR d, DWORD a, DWORD t, DWORD s, DWORD e, LPCWSTR p,
                                          LPCWSTR g, LPDWORD tag, LPCWSTR dep, LPCWSTR user, LPCWSTR pw)
{
    (void)scm; (void)n; (void)d; (void)a; (void)t; (void)s; (void)e; (void)p; (void)g; (void)tag; (void)dep; (void)user; (void)pw;
    SetLastError(ERROR_ACCESS_DENIED);
    return 0;
}
WINADVAPI BOOL WINAPI CloseServiceHandle(SC_HANDLE h) { (void)h; return TRUE; }
WINADVAPI BOOL WINAPI EnumServicesStatusExW(SC_HANDLE scm, int level, DWORD type, DWORD state, LPBYTE buf, DWORD n, LPDWORD need,
                                            LPDWORD count, LPDWORD resume, LPCWSTR group)
{
    (void)scm; (void)level; (void)type; (void)state; (void)buf; (void)n; (void)group;
    *need = 0; *count = 0;
    if (resume) *resume = 0;
    return TRUE;
}

/* Not started by a service control manager */
WINADVAPI BOOL WINAPI StartServiceCtrlDispatcherW(const void *table) { (void)table; SetLastError(1063 /* ERROR_FAILED_SERVICE_CONTROLLER_CONNECT */); return FALSE; }
WINADVAPI BOOL WINAPI StartServiceCtrlDispatcherA(const void *table) { (void)table; SetLastError(1063); return FALSE; }
WINADVAPI HANDLE WINAPI RegisterServiceCtrlHandlerW(LPCWSTR name, PVOID fn) { (void)name; (void)fn; SetLastError(1063); return 0; }
WINADVAPI HANDLE WINAPI RegisterServiceCtrlHandlerExW(LPCWSTR name, PVOID fn, PVOID ctx) { (void)name; (void)fn; (void)ctx; SetLastError(1063); return 0; }
WINADVAPI BOOL WINAPI SetServiceStatus(HANDLE h, PVOID st) { (void)h; (void)st; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

/* -----------------------------------------------------------------------
 * Odds and ends
 * ----------------------------------------------------------------------- */
/* Guess whether a buffer is UTF-16 text (IS_TEXT_UNICODE_* tests) */
WINADVAPI BOOL WINAPI IsTextUnicode(const void *buf, int n, LPINT result)
{
    const BYTE *b = buf;
    int want = result ? *result : 0xFFFF, found = 0;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) found |= 0x0008;          /* IS_TEXT_UNICODE_SIGNATURE */
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) found |= 0x0080;          /* REVERSE_SIGNATURE */
    int zeros_odd = 0, zeros_even = 0, pairs = n / 2;
    for (int i = 0; i + 1 < n; i += 2) { if (!b[i + 1]) zeros_odd++; if (!b[i]) zeros_even++; }
    if (pairs && zeros_odd * 2 > pairs && zeros_even * 8 < pairs) found |= 0x0002;   /* STATISTICS */
    if (n & 1) found |= 0x0200;                                             /* ODD_LENGTH (not unicode) */
    if (result) *result = found & want;
    return (found & 0x000A) && !(found & 0x0200);
}

WINADVAPI BOOL WINAPI GetCurrentHwProfileW(PVOID info) { (void)info; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }
/* InitiateSystemShutdown: this machine only, at once (no countdown to
 * abort), saving drive C: first */
WINADVAPI BOOL WINAPI InitiateSystemShutdownExW(LPWSTR m, LPWSTR msg, DWORD t, BOOL f, BOOL r, DWORD reason)
{
    (void)m; (void)msg; (void)t; (void)f; (void)reason;
    return NtShutdownSystem(r ? 1 : 2) == 0;
}
WINADVAPI BOOL WINAPI InitiateSystemShutdownExA(LPSTR m, LPSTR msg, DWORD t, BOOL f, BOOL r, DWORD reason)
{ (void)m; (void)msg; return InitiateSystemShutdownExW(NULL, NULL, t, f, r, reason); }
WINADVAPI BOOL WINAPI InitiateSystemShutdownW(LPWSTR m, LPWSTR msg, DWORD t, BOOL f, BOOL r)
{ return InitiateSystemShutdownExW(m, msg, t, f, r, 0); }
WINADVAPI BOOL WINAPI InitiateSystemShutdownA(LPSTR m, LPSTR msg, DWORD t, BOOL f, BOOL r)
{ (void)m; (void)msg; return InitiateSystemShutdownExW(NULL, NULL, t, f, r, 0); }
WINADVAPI BOOL WINAPI AbortSystemShutdownW(LPWSTR m) { (void)m; SetLastError(ERROR_NO_SHUTDOWN_IN_PROGRESS); return FALSE; }
WINADVAPI BOOL WINAPI AbortSystemShutdownA(LPSTR m) { (void)m; SetLastError(ERROR_NO_SHUTDOWN_IN_PROGRESS); return FALSE; }

WINADVAPI BOOL WINAPI QueryServiceStatusEx(SC_HANDLE s, int level, LPBYTE buf, DWORD n, LPDWORD need)
{
    (void)s; (void)level; (void)buf; (void)n;
    if (need) *need = 0;
    SetLastError(ERROR_INVALID_HANDLE);             /* (there are no services to open) */
    return FALSE;
}

/* SetEntriesInAcl: files carry no ACLs here, so the new ACL is an empty
 * one the caller frees with LocalFree */
static DWORD entries_in_acl(PACL *out)
{
    PACL a = LocalAlloc(LMEM_FIXED, 8);
    if (!a) return ERROR_NOT_ENOUGH_MEMORY;
    InitializeAcl(a, 8, 2);
    *out = a;
    return ERROR_SUCCESS;
}
WINADVAPI DWORD WINAPI SetEntriesInAclA(ULONG n, void *entries, PACL old, PACL *out) { (void)n; (void)entries; (void)old; return entries_in_acl(out); }
WINADVAPI DWORD WINAPI SetEntriesInAclW(ULONG n, void *entries, PACL old, PACL *out) { (void)n; (void)entries; (void)old; return entries_in_acl(out); }

NTSYSAPI NTSTATUS NTAPI NtAllocateLocallyUniqueId(PLUID luid);
WINADVAPI BOOL WINAPI AllocateLocallyUniqueId(PLUID luid)
{
    if (!luid) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    NtAllocateLocallyUniqueId(luid);
    return TRUE;
}
