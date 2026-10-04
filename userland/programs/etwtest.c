/* etwtest.exe — advapi32's event-tracing controller and consumer
 * functions, which answer as Windows does when no logging session is
 * running and none can be started:
 *
 *   StartTrace     checks its properties block, then finds the session
 *                  limit reached (ERROR_NO_SYSTEM_RESOURCES);
 *   StopTrace, ControlTrace, QueryTrace, FlushTrace, UpdateTrace
 *                  check their arguments, then find no such session
 *                  (ERROR_WMI_INSTANCE_NOT_FOUND);
 *   EnableTrace(Ex/Ex2)  the same for the session handle;
 *   QueryAllTraces lists no sessions;
 *   OpenTrace      opens a real-time consumer (ProcessTrace then finds
 *                  no session) and fails on a missing log file;
 *   CloseTrace     closes a consumer handle once.
 *
 * Ends with the sequence Steam's service runs (stop its old session,
 * start a real-time one, and fall back when that fails).  The functions
 * are looked up by name, so a missing export fails a check rather than
 * the program's start.  Built for 64 and 32 bits.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef ULONGLONG TRACEHANDLE;
#define INVALID_PROCESSTRACE_HANDLE ((TRACEHANDLE)(ULONG_PTR)-1)
#define ERROR_WMI_INSTANCE_NOT_FOUND 4201
#ifndef ERROR_NO_SYSTEM_RESOURCES
#define ERROR_NO_SYSTEM_RESOURCES 1450
#endif
#ifndef ERROR_BAD_LENGTH
#define ERROR_BAD_LENGTH 24
#endif

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
} PROPS;

/* The properties block with room for a name after it, as callers pass it */
typedef struct { PROPS p; WCHAR name[64]; } PROPS_BUF;

/* The head of EVENT_TRACE_LOGFILE; the real structure is larger */
typedef struct {
    void *LogFileName, *LoggerName;
    LONGLONG CurrentTime;
    ULONG BuffersRead;
    ULONG ProcessTraceMode;
    BYTE rest[512];
} LOGFILE;

typedef ULONG (WINAPI *START_A)(TRACEHANDLE *, LPCSTR, PROPS *);
typedef ULONG (WINAPI *START_W)(TRACEHANDLE *, LPCWSTR, PROPS *);
typedef ULONG (WINAPI *CTRL_A)(TRACEHANDLE, LPCSTR, PROPS *);
typedef ULONG (WINAPI *CTRL_W)(TRACEHANDLE, LPCWSTR, PROPS *);
typedef ULONG (WINAPI *CONTROL_W)(TRACEHANDLE, LPCWSTR, PROPS *, ULONG);
typedef ULONG (WINAPI *QUERYALL_W)(PROPS **, ULONG, ULONG *);
typedef ULONG (WINAPI *ENABLE)(ULONG, ULONG, ULONG, const GUID *, TRACEHANDLE);
typedef ULONG (WINAPI *ENABLE_EX2)(TRACEHANDLE, const GUID *, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, void *);
typedef TRACEHANDLE (WINAPI *OPEN)(LOGFILE *);
typedef ULONG (WINAPI *PROCESS)(TRACEHANDLE *, ULONG, FILETIME *, FILETIME *);
typedef ULONG (WINAPI *CLOSE)(TRACEHANDLE);

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)
#define CHECK_RC(what, got, want) do { ULONG g_ = (got); if (g_ == (ULONG)(want)) pass++; \
    else { fail++; printf("FAIL: %s returned %lu, expected %lu (line %d)\n", what, g_, (ULONG)(want), __LINE__); } } while (0)

static void props_init(PROPS_BUF *b)
{
    memset(b, 0, sizeof(*b));
    b->p.Wnode.BufferSize = sizeof(*b);
    b->p.Wnode.Flags = 0x20000;                 /* WNODE_FLAG_TRACED_GUID */
    b->p.Wnode.ClientContext = 2;               /* system time */
    b->p.LogFileMode = 0x100;                   /* EVENT_TRACE_REAL_TIME_MODE */
    b->p.LoggerNameOffset = sizeof(PROPS);
}

/* Microsoft-Windows-Kernel-Process */
static const GUID kernel_process = { 0x22fb2cd6, 0x0e7b, 0x422b, { 0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16 } };

int main(void)
{
    HMODULE adv = LoadLibraryA("advapi32.dll");
    START_A StartTraceA = (START_A)GetProcAddress(adv, "StartTraceA");
    START_W StartTraceW = (START_W)GetProcAddress(adv, "StartTraceW");
    CTRL_A StopTraceA = (CTRL_A)GetProcAddress(adv, "StopTraceA");
    CTRL_W StopTraceW = (CTRL_W)GetProcAddress(adv, "StopTraceW");
    CTRL_W QueryTraceW = (CTRL_W)GetProcAddress(adv, "QueryTraceW");
    CTRL_W FlushTraceW = (CTRL_W)GetProcAddress(adv, "FlushTraceW");
    CTRL_W UpdateTraceW = (CTRL_W)GetProcAddress(adv, "UpdateTraceW");
    CONTROL_W ControlTraceW = (CONTROL_W)GetProcAddress(adv, "ControlTraceW");
    QUERYALL_W QueryAllTracesW = (QUERYALL_W)GetProcAddress(adv, "QueryAllTracesW");
    ENABLE EnableTrace = (ENABLE)GetProcAddress(adv, "EnableTrace");
    ENABLE_EX2 EnableTraceEx2 = (ENABLE_EX2)GetProcAddress(adv, "EnableTraceEx2");
    OPEN OpenTraceA = (OPEN)GetProcAddress(adv, "OpenTraceA");
    OPEN OpenTraceW = (OPEN)GetProcAddress(adv, "OpenTraceW");
    PROCESS ProcessTrace = (PROCESS)GetProcAddress(adv, "ProcessTrace");
    CLOSE CloseTrace = (CLOSE)GetProcAddress(adv, "CloseTrace");
    static const char *more[] = { "ControlTraceA", "QueryTraceA", "FlushTraceA", "UpdateTraceA",
                                  "QueryAllTracesA", "EnableTraceEx", 0 };

    CHECK("all exported", StartTraceA && StartTraceW && StopTraceA && StopTraceW && QueryTraceW && FlushTraceW &&
          UpdateTraceW && ControlTraceW && QueryAllTracesW && EnableTrace && EnableTraceEx2 && OpenTraceA &&
          OpenTraceW && ProcessTrace && CloseTrace);
    for (int i = 0; more[i]; i++) CHECK(more[i], GetProcAddress(adv, more[i]) != NULL);
    if (fail) { printf("etwtest: %d passed, %d failed\n", pass, fail); return 1; }

    PROPS_BUF b;
    TRACEHANDLE h;

    /* StartTrace: the properties are checked first, then no session can start */
    props_init(&b);
    h = 123;
    CHECK_RC("StartTraceW", StartTraceW(&h, L"Nova ETW Test", &b.p), ERROR_NO_SYSTEM_RESOURCES);
    CHECK("StartTraceW clears the handle", h == 0);
    CHECK_RC("StartTraceA", StartTraceA(&h, "Nova ETW Test", &b.p), ERROR_NO_SYSTEM_RESOURCES);
    CHECK_RC("StartTrace without properties", StartTraceW(&h, L"Nova ETW Test", NULL), ERROR_INVALID_PARAMETER);
    CHECK_RC("StartTrace without a handle", StartTraceW(NULL, L"Nova ETW Test", &b.p), ERROR_INVALID_PARAMETER);
    CHECK_RC("StartTrace without a name", StartTraceW(&h, NULL, &b.p), ERROR_INVALID_PARAMETER);
    b.p.Wnode.BufferSize = sizeof(PROPS) - 8;
    CHECK_RC("StartTrace with a short block", StartTraceW(&h, L"Nova ETW Test", &b.p), ERROR_BAD_LENGTH);
    props_init(&b);
    b.p.LoggerNameOffset = 8;
    CHECK_RC("StartTrace with a name inside the header", StartTraceW(&h, L"Nova ETW Test", &b.p), ERROR_INVALID_PARAMETER);

    /* Controlling a session that does not exist */
    props_init(&b);
    CHECK_RC("StopTraceW by name", StopTraceW(0, L"Nova ETW Test", &b.p), ERROR_WMI_INSTANCE_NOT_FOUND);
    CHECK_RC("StopTraceA by name", StopTraceA(0, "Nova ETW Test", &b.p), ERROR_WMI_INSTANCE_NOT_FOUND);
    CHECK_RC("StopTrace by handle", StopTraceW(0x1234, NULL, &b.p), ERROR_WMI_INSTANCE_NOT_FOUND);
    CHECK_RC("StopTrace with neither", StopTraceW(0, NULL, &b.p), ERROR_INVALID_PARAMETER);
    CHECK_RC("StopTrace without properties", StopTraceW(0, L"Nova ETW Test", NULL), ERROR_INVALID_PARAMETER);
    CHECK_RC("QueryTraceW", QueryTraceW(0, L"NT Kernel Logger", &b.p), ERROR_WMI_INSTANCE_NOT_FOUND);
    CHECK_RC("FlushTraceW", FlushTraceW(0, L"Nova ETW Test", &b.p), ERROR_WMI_INSTANCE_NOT_FOUND);
    CHECK_RC("UpdateTraceW", UpdateTraceW(0, L"Nova ETW Test", &b.p), ERROR_WMI_INSTANCE_NOT_FOUND);
    CHECK_RC("ControlTraceW query", ControlTraceW(0, L"Nova ETW Test", &b.p, 0), ERROR_WMI_INSTANCE_NOT_FOUND);
    CHECK_RC("ControlTraceW unknown code", ControlTraceW(0, L"Nova ETW Test", &b.p, 99), ERROR_INVALID_PARAMETER);

    /* No sessions to list */
    PROPS_BUF all[4];
    PROPS *ptrs[4];
    for (int i = 0; i < 4; i++) { props_init(&all[i]); ptrs[i] = &all[i].p; }
    ULONG count = 77;
    CHECK_RC("QueryAllTracesW", QueryAllTracesW(ptrs, 4, &count), ERROR_SUCCESS);
    CHECK("QueryAllTracesW lists none", count == 0);
    CHECK_RC("QueryAllTracesW without a count", QueryAllTracesW(ptrs, 4, NULL), ERROR_INVALID_PARAMETER);

    /* Enabling a provider needs a running session */
    CHECK_RC("EnableTrace", EnableTrace(1, 0x10, 4, &kernel_process, 0x1234), ERROR_WMI_INSTANCE_NOT_FOUND);
    CHECK_RC("EnableTrace without a GUID", EnableTrace(1, 0x10, 4, NULL, 0x1234), ERROR_INVALID_PARAMETER);
    CHECK_RC("EnableTrace without a session", EnableTrace(1, 0x10, 4, &kernel_process, 0), ERROR_INVALID_PARAMETER);
    CHECK_RC("EnableTraceEx2", EnableTraceEx2(0x1234, &kernel_process, 1, 4, 0x10, 0, 0, NULL), ERROR_WMI_INSTANCE_NOT_FOUND);

    /* A real-time consumer opens; there is no session to read */
    LOGFILE lf;
    memset(&lf, 0, sizeof(lf));
    lf.LoggerName = "Nova ETW Test";
    lf.ProcessTraceMode = 0x10000100;           /* EVENT_RECORD | REAL_TIME */
    TRACEHANDLE c = OpenTraceA(&lf);
    CHECK("OpenTraceA real-time", c != INVALID_PROCESSTRACE_HANDLE && c != 0);
    CHECK_RC("ProcessTrace", ProcessTrace(&c, 1, NULL, NULL), ERROR_WMI_INSTANCE_NOT_FOUND);
    CHECK_RC("ProcessTrace without handles", ProcessTrace(NULL, 1, NULL, NULL), ERROR_INVALID_PARAMETER);
    CHECK_RC("CloseTrace", CloseTrace(c), ERROR_SUCCESS);
    CHECK_RC("CloseTrace twice", CloseTrace(c), ERROR_INVALID_HANDLE);
    CHECK_RC("ProcessTrace after CloseTrace", ProcessTrace(&c, 1, NULL, NULL), ERROR_INVALID_HANDLE);

    memset(&lf, 0, sizeof(lf));
    lf.LogFileName = L"C:\\nova-etwtest-missing.etl";
    SetLastError(0);
    c = OpenTraceW(&lf);
    CHECK("OpenTraceW of a missing log file fails", c == INVALID_PROCESSTRACE_HANDLE);
    CHECK("... with ERROR_FILE_NOT_FOUND", GetLastError() == ERROR_FILE_NOT_FOUND);
#ifndef _WIN64
    CHECK("INVALID_PROCESSTRACE_HANDLE is 0xFFFFFFFF on 32 bits", c == 0xFFFFFFFFull);
#endif

    /* Steam's service: stop its old session, start a real-time one; when
     * that fails it watches processes without event tracing */
    props_init(&b);
    StopTraceA(0, "Steam Process Watch", &b.p);
    props_init(&b);
    b.p.MaximumFileSize = 0x400;
    CHECK("Steam's StartTraceA fails, so Steam falls back",
          StartTraceA(&h, "Steam Process Watch", &b.p) != ERROR_SUCCESS);

    printf("etwtest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
