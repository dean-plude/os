/*
 * misc.c — advapi32's event tracing (ETW), event log and a few
 * odds and ends.  NovaOS has no trace sessions or event log service:
 * providers register successfully and are never enabled, controllers
 * are told no session can start (and that the ones they name do not
 * exist), and event-log reports go to the kernel log.  (Services are in
 * service.c.)
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
 * Trace controllers and consumers, as Windows answers when no logging
 * session is running and none can be started: StartTrace finds the
 * session limit reached, a session named or handled by StopTrace,
 * ControlTrace or EnableTrace does not exist, QueryAllTraces lists none,
 * and a real-time consumer opens but ProcessTrace finds no session to
 * read.  (Steam's service stops any old session of its own, tries to
 * start one and, when it cannot, watches processes another way.)
 * ----------------------------------------------------------------------- */
typedef ULONGLONG TRACEHANDLE;

#define ERROR_WMI_INSTANCE_NOT_FOUND 4201
#ifndef ERROR_NO_SYSTEM_RESOURCES
#define ERROR_NO_SYSTEM_RESOURCES 1450
#endif
#ifndef ERROR_BAD_LENGTH
#define ERROR_BAD_LENGTH 24
#endif

#define EVENT_TRACE_CONTROL_QUERY  0
#define EVENT_TRACE_CONTROL_STOP   1
#define EVENT_TRACE_CONTROL_UPDATE 2
#define EVENT_TRACE_CONTROL_FLUSH  3
#define EVENT_TRACE_CONTROL_INCREMENT_FILE 4
#define EVENT_TRACE_CONTROL_CONVERT_TO_REALTIME 5
#define EVENT_TRACE_REAL_TIME_MODE 0x00000100
#define PROCESS_TRACE_MODE_REAL_TIME 0x00000100
#define INVALID_PROCESSTRACE_HANDLE ((TRACEHANDLE)(ULONG_PTR)-1)

/* EVENT_TRACE_PROPERTIES: a WNODE_HEADER, then the session's settings;
 * only the size and the name offsets are read here */
typedef struct {
    ULONG BufferSize, ProviderId;
    ULONG64 HistoricalContext;
    LARGE_INTEGER TimeStamp;
    GUID Guid;
    ULONG ClientContext, Flags;
} WNODE_HEADER_;
typedef struct {
    WNODE_HEADER_ Wnode;
    ULONG BufferSize, MinimumBuffers, MaximumBuffers, MaximumFileSize;
    ULONG LogFileMode, FlushTimer, EnableFlags;
    LONG AgeLimit;
    ULONG NumberOfBuffers, FreeBuffers, EventsLost, BuffersWritten;
    ULONG LogBuffersLost, RealTimeBuffersLost;
    HANDLE LoggerThreadId;
    ULONG LogFileNameOffset, LoggerNameOffset;
} TRACE_PROPS;

/* The head of EVENT_TRACE_LOGFILEA/W: the two names and the mode */
typedef struct {
    void *LogFileName, *LoggerName;
    LONGLONG CurrentTime;
    ULONG BuffersRead;
    ULONG ProcessTraceMode;
} TRACE_LOGFILE_HEAD;

/* Windows checks the properties block before it looks for the session */
static ULONG check_props(const TRACE_PROPS *p, BOOL need_name, BOOL have_name)
{
    if (!p) return ERROR_INVALID_PARAMETER;
    if (p->Wnode.BufferSize < sizeof(TRACE_PROPS)) return ERROR_BAD_LENGTH;
    if (p->LoggerNameOffset && p->LoggerNameOffset < sizeof(TRACE_PROPS)) return ERROR_INVALID_PARAMETER;
    if (p->LogFileNameOffset && p->LogFileNameOffset < sizeof(TRACE_PROPS)) return ERROR_INVALID_PARAMETER;
    if (need_name && !have_name) return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

static ULONG start_trace(TRACEHANDLE *h, BOOL have_name, TRACE_PROPS *p)
{
    if (h) *h = 0;
    if (!h) return ERROR_INVALID_PARAMETER;
    ULONG e = check_props(p, TRUE, have_name);
    if (e) return e;
    return ERROR_NO_SYSTEM_RESOURCES;      /* no logging session can start */
}

WINADVAPI ULONG WINAPI StartTraceW(TRACEHANDLE *h, LPCWSTR name, PVOID props)
{
    return start_trace(h, name && *name, props);
}
WINADVAPI ULONG WINAPI StartTraceA(TRACEHANDLE *h, LPCSTR name, PVOID props)
{
    return start_trace(h, name && *name, props);
}

static ULONG control_trace(TRACEHANDLE h, BOOL have_name, TRACE_PROPS *p, ULONG code)
{
    if (code > EVENT_TRACE_CONTROL_CONVERT_TO_REALTIME) return ERROR_INVALID_PARAMETER;
    ULONG e = check_props(p, !h, have_name);
    if (e) return e;
    return ERROR_WMI_INSTANCE_NOT_FOUND;   /* no such session is running */
}

WINADVAPI ULONG WINAPI ControlTraceW(TRACEHANDLE h, LPCWSTR name, PVOID props, ULONG code)
{ return control_trace(h, name && *name, props, code); }
WINADVAPI ULONG WINAPI ControlTraceA(TRACEHANDLE h, LPCSTR name, PVOID props, ULONG code)
{ return control_trace(h, name && *name, props, code); }
WINADVAPI ULONG WINAPI StopTraceW(TRACEHANDLE h, LPCWSTR name, PVOID props) { return ControlTraceW(h, name, props, EVENT_TRACE_CONTROL_STOP); }
WINADVAPI ULONG WINAPI StopTraceA(TRACEHANDLE h, LPCSTR name, PVOID props) { return ControlTraceA(h, name, props, EVENT_TRACE_CONTROL_STOP); }
WINADVAPI ULONG WINAPI QueryTraceW(TRACEHANDLE h, LPCWSTR name, PVOID props) { return ControlTraceW(h, name, props, EVENT_TRACE_CONTROL_QUERY); }
WINADVAPI ULONG WINAPI QueryTraceA(TRACEHANDLE h, LPCSTR name, PVOID props) { return ControlTraceA(h, name, props, EVENT_TRACE_CONTROL_QUERY); }
WINADVAPI ULONG WINAPI UpdateTraceW(TRACEHANDLE h, LPCWSTR name, PVOID props) { return ControlTraceW(h, name, props, EVENT_TRACE_CONTROL_UPDATE); }
WINADVAPI ULONG WINAPI UpdateTraceA(TRACEHANDLE h, LPCSTR name, PVOID props) { return ControlTraceA(h, name, props, EVENT_TRACE_CONTROL_UPDATE); }
WINADVAPI ULONG WINAPI FlushTraceW(TRACEHANDLE h, LPCWSTR name, PVOID props) { return ControlTraceW(h, name, props, EVENT_TRACE_CONTROL_FLUSH); }
WINADVAPI ULONG WINAPI FlushTraceA(TRACEHANDLE h, LPCSTR name, PVOID props) { return ControlTraceA(h, name, props, EVENT_TRACE_CONTROL_FLUSH); }

/* No sessions to list */
static ULONG query_all(PVOID *props, ULONG n, ULONG *count)
{
    if (!count || (n && !props) || n > 64) return ERROR_INVALID_PARAMETER;
    *count = 0;
    return ERROR_SUCCESS;
}
WINADVAPI ULONG WINAPI QueryAllTracesW(PVOID *props, ULONG n, ULONG *count) { return query_all(props, n, count); }
WINADVAPI ULONG WINAPI QueryAllTracesA(PVOID *props, ULONG n, ULONG *count) { return query_all(props, n, count); }

/* Enabling a provider needs a session to send its events to */
static ULONG enable_trace(const GUID *guid, TRACEHANDLE session)
{
    if (!guid || !session) return ERROR_INVALID_PARAMETER;
    return ERROR_WMI_INSTANCE_NOT_FOUND;
}
WINADVAPI ULONG WINAPI EnableTrace(ULONG enable, ULONG flags, ULONG level, const GUID *guid, TRACEHANDLE session)
{
    (void)flags; (void)level;
    if (enable > 1) return ERROR_INVALID_PARAMETER;
    return enable_trace(guid, session);
}
WINADVAPI ULONG WINAPI EnableTraceEx(const GUID *provider, const GUID *source, TRACEHANDLE session, ULONG enable,
                                     UCHAR level, ULONGLONG any, ULONGLONG all, ULONG props, PVOID filter)
{
    (void)source; (void)level; (void)any; (void)all; (void)props; (void)filter;
    if (enable > 2) return ERROR_INVALID_PARAMETER;
    return enable_trace(provider, session);
}
WINADVAPI ULONG WINAPI EnableTraceEx2(TRACEHANDLE session, const GUID *provider, ULONG control, UCHAR level,
                                      ULONGLONG any, ULONGLONG all, ULONG timeout, PVOID params)
{
    (void)level; (void)any; (void)all; (void)timeout; (void)params;
    if (control > 3) return ERROR_INVALID_PARAMETER;
    return enable_trace(provider, session);
}

/* Consumers: OpenTrace hands out a handle for a real-time session (that
 * opens without the session; ProcessTrace finds it missing) or an
 * existing log file (NovaOS cannot read .etl files) */
#define MAX_CONSUMERS 64
#define CONSUMER_TAG  0x4E540000ULL
static volatile LONG g_consumer[MAX_CONSUMERS];       /* 0 free, 1 real-time, 2 log file */

static TRACEHANDLE open_trace(int kind)
{
    for (int i = 0; i < MAX_CONSUMERS; i++)
        if (__sync_bool_compare_and_swap(&g_consumer[i], 0, kind)) return CONSUMER_TAG + (TRACEHANDLE)i;
    SetLastError(ERROR_NO_SYSTEM_RESOURCES);
    return INVALID_PROCESSTRACE_HANDLE;
}

static int consumer_slot(TRACEHANDLE h)
{
    if (h < CONSUMER_TAG || h >= CONSUMER_TAG + MAX_CONSUMERS) return -1;
    int i = (int)(h - CONSUMER_TAG);
    return g_consumer[i] ? i : -1;
}

static TRACEHANDLE open_log_file(HANDLE f)
{
    if (f == INVALID_HANDLE_VALUE) return INVALID_PROCESSTRACE_HANDLE;    /* CreateFile's error */
    CloseHandle(f);
    return open_trace(2);
}

WINADVAPI TRACEHANDLE WINAPI OpenTraceW(PVOID logfile)
{
    TRACE_LOGFILE_HEAD *lf = logfile;
    if (!lf) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_PROCESSTRACE_HANDLE; }
    if (lf->ProcessTraceMode & PROCESS_TRACE_MODE_REAL_TIME) {
        const WCHAR *n = lf->LoggerName;
        if (!n || !*n) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_PROCESSTRACE_HANDLE; }
        return open_trace(1);
    }
    const WCHAR *path = lf->LogFileName;
    if (!path || !*path) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_PROCESSTRACE_HANDLE; }
    return open_log_file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0));
}

WINADVAPI TRACEHANDLE WINAPI OpenTraceA(PVOID logfile)
{
    TRACE_LOGFILE_HEAD *lf = logfile;
    if (!lf) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_PROCESSTRACE_HANDLE; }
    if (lf->ProcessTraceMode & PROCESS_TRACE_MODE_REAL_TIME) {
        const char *n = lf->LoggerName;
        if (!n || !*n) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_PROCESSTRACE_HANDLE; }
        return open_trace(1);
    }
    const char *path = lf->LogFileName;
    if (!path || !*path) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_PROCESSTRACE_HANDLE; }
    return open_log_file(CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0));
}

WINADVAPI ULONG WINAPI ProcessTrace(TRACEHANDLE *handles, ULONG n, FILETIME *start, FILETIME *end)
{
    (void)start; (void)end;
    if (!handles || !n || n > MAX_CONSUMERS) return ERROR_INVALID_PARAMETER;
    for (ULONG i = 0; i < n; i++)
        if (consumer_slot(handles[i]) < 0) return ERROR_INVALID_HANDLE;
    for (ULONG i = 0; i < n; i++)
        if (g_consumer[consumer_slot(handles[i])] == 1) return ERROR_WMI_INSTANCE_NOT_FOUND;
    return ERROR_NOT_SUPPORTED;            /* an .etl file NovaOS cannot read */
}

WINADVAPI ULONG WINAPI CloseTrace(TRACEHANDLE h)
{
    int i = consumer_slot(h);
    if (i < 0) return ERROR_INVALID_HANDLE;
    __atomic_store_n(&g_consumer[i], 0, __ATOMIC_RELEASE);
    return ERROR_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Event log: reports go to the kernel log
 * ----------------------------------------------------------------------- */
WINADVAPI HANDLE WINAPI RegisterEventSourceW(LPCWSTR server, LPCWSTR source) { (void)server; (void)source; return (HANDLE)(ULONG_PTR)0x4E4F; }
WINADVAPI HANDLE WINAPI RegisterEventSourceA(LPCSTR server, LPCSTR source) { (void)server; (void)source; return (HANDLE)(ULONG_PTR)0x4E4F; }
WINADVAPI BOOL WINAPI DeregisterEventSource(HANDLE h) { (void)h; return TRUE; }

WINADVAPI BOOL WINAPI ReportEventW(HANDLE h, WORD type, WORD cat, DWORD id, PSID sid, WORD n, DWORD size, LPCWSTR *strings, LPVOID data)
{
    (void)h; (void)cat; (void)sid;
    char line[256];
    int o = 0;
    const char *kind = type == 1 ? "error" : type == 2 ? "warning" : "event";
    while (*kind && o < 40) line[o++] = *kind++;
    line[o++] = ' ';
    static const char hex[] = "0123456789abcdef";      /* the event ID (often an HRESULT) */
    for (int k = 28; k >= 0; k -= 4) line[o++] = hex[(id >> k) & 15];
    line[o++] = ':';
    for (WORD i = 0; i < n && strings && o < 250; i++) {
        line[o++] = ' ';
        for (const WCHAR *s = strings[i]; s && *s && o < 250; s++) line[o++] = *s < 0x80 ? (char)*s : '?';
    }
    if (data && size) {                    /* binary data (Firefox's launcher: HRESULT, line, source file) */
        const BYTE *d = data;
        line[o++] = ' ';
        line[o++] = '[';
        for (DWORD i = 0; i < size && o < 250; i++) line[o++] = d[i] >= 0x20 && d[i] < 0x7F ? (char)d[i] : '.';
        line[o++] = ']';
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
WINADVAPI BOOL WINAPI GetCurrentHwProfileA(PVOID info) { (void)info; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }
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

NTSYSAPI NTSTATUS NTAPI NtAllocateLocallyUniqueId(PLUID luid);
WINADVAPI BOOL WINAPI AllocateLocallyUniqueId(PLUID luid)
{
    if (!luid) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    NtAllocateLocallyUniqueId(luid);
    return TRUE;
}

/* Transacted registry calls (ktmw32): the change is made at once, and the
 * transaction's commit (CommitTransaction) has nothing left to do */
WINADVAPI LSTATUS WINAPI RegCreateKeyTransactedW(HKEY key, LPCWSTR sub, DWORD reserved, LPWSTR cls, DWORD options, REGSAM sam,
                                                 LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disposition, HANDLE trans, PVOID ext)
{
    (void)trans; (void)ext;
    return RegCreateKeyExW(key, sub, reserved, cls, options, sam, sa, out, disposition);
}
WINADVAPI LSTATUS WINAPI RegOpenKeyTransactedW(HKEY key, LPCWSTR sub, DWORD options, REGSAM sam, PHKEY out, HANDLE trans, PVOID ext)
{
    (void)trans; (void)ext;
    return RegOpenKeyExW(key, sub, options, sam, out);
}
WINADVAPI LSTATUS WINAPI RegDeleteKeyTransactedW(HKEY key, LPCWSTR sub, REGSAM sam, DWORD reserved, HANDLE trans, PVOID ext)
{
    (void)trans; (void)ext;
    return RegDeleteKeyExW(key, sub, sam, reserved);
}
