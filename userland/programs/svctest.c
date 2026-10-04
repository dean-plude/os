/* svctest.exe — a real Windows service through the service control
 * manager (advapi32), the way Steam's service is installed and started:
 *
 *   svctest            installs itself as an own-process service under a
 *                      name with spaces and an ImagePath of the form
 *                      "path" /service, starts it with arguments, waits
 *                      for SERVICE_RUNNING, sends it controls, stops and
 *                      deletes it, and checks what the service saw;
 *   svctest /service   (started by the control manager) the service:
 *                      StartServiceCtrlDispatcher, ServiceMain, the
 *                      SERVICE_STATUS handshake and a control handler;
 *   svctest /child     a program the service starts before it reports
 *                      itself running: it must not get to connect as the
 *                      service (Steam's service starts its updater first).
 *
 * Also: run any other way, StartServiceCtrlDispatcher fails with
 * ERROR_FAILED_SERVICE_CONTROLLER_CONNECT; CopyFile keeps the source's
 * last-write time and attributes (Steam's service compares them to tell
 * whether its copy is current).  Built for 64 and 32 bits; each bitness
 * uses its own service name, so both can run on one machine.
 */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winsvc.h>

#ifdef _WIN64
#define SVC_NAME L"Nova Test Service (64-bit)"
#else
#define SVC_NAME L"Nova Test Service (32-bit)"
#endif
#define USER_CONTROL 200
#ifndef NO_ERROR
#define NO_ERROR 0
#endif
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d, error %lu)\n", what, __LINE__, GetLastError()); } } while (0)

/* The service's notes for the test, in the temporary folder */
static void log_path(WCHAR *out, DWORD cap)
{
    WCHAR tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) wcscpy(tmp, L"C:\\");
#ifdef _WIN64
    _snwprintf(out, cap, L"%ssvctest64.txt", tmp);
#else
    _snwprintf(out, cap, L"%ssvctest32.txt", tmp);
#endif
}

static void note(const char *fmt, ...)
{
    WCHAR path[MAX_PATH + 32];
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    strcat(line, "\r\n");
    log_path(path, MAX_PATH + 32);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    SetFilePointer(f, 0, NULL, FILE_END);
    WriteFile(f, line, (DWORD)strlen(line), &n, NULL);
    CloseHandle(f);
}

/* -----------------------------------------------------------------------
 * The service
 * ----------------------------------------------------------------------- */
static SERVICE_STATUS_HANDLE g_status;
static SERVICE_STATUS g_st;
static HANDLE g_stop;

static void report(DWORD state, DWORD accepts, DWORD checkpoint, DWORD hint)
{
    g_st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_st.dwCurrentState = state;
    g_st.dwControlsAccepted = accepts;
    g_st.dwWin32ExitCode = 0;
    g_st.dwCheckPoint = checkpoint;
    g_st.dwWaitHint = hint;
    SetServiceStatus(g_status, &g_st);
}

static DWORD WINAPI handler(DWORD control, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)type; (void)data;
    note("control %lu ctx %s", (unsigned long)control, ctx == (LPVOID)&g_st ? "ok" : "wrong");
    switch (control) {
    case SERVICE_CONTROL_STOP:
        report(SERVICE_STOP_PENDING, 0, 1, 3000);
        SetEvent(g_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
    case USER_CONTROL:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/* A program started with this process's environment, as Steam's service
 * starts its updater; its exit code */
static DWORD run_self(const WCHAR *arg)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf(cmd, MAX_PATH + 32, L"\"%s\" %s", exe, arg);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 0xFFFFFFFF;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0xFFFFFFFE;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    g_status = RegisterServiceCtrlHandlerExW(argv[0], handler, &g_st);
    note("registered %d", g_status != NULL);
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    report(SERVICE_START_PENDING, 0, 1, 5000);
    char line[400];
    int at = snprintf(line, sizeof(line), "argc %lu", (unsigned long)argc);
    for (DWORD i = 0; i < argc && at < (int)sizeof(line) - 64; i++)
        at += snprintf(line + at, sizeof(line) - at, " [%ls]", argv[i]);
    note("%s", line);
    note("child %lu", (unsigned long)run_self(L"/child"));
    report(SERVICE_RUNNING, SERVICE_ACCEPT_STOP, 0, 0);
    WaitForSingleObject(g_stop, INFINITE);
    note("stopping");
    report(SERVICE_STOPPED, 0, 0, 0);
}

static int as_service(void)
{
    SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)SVC_NAME, service_main }, { NULL, NULL } };
    if (!StartServiceCtrlDispatcherW(table)) {
        note("dispatcher failed %lu", GetLastError());
        return 1;
    }
    note("dispatcher returned");
    return 0;
}

/* Not started by the control manager: the dispatcher refuses */
static int as_child(void)
{
    SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)SVC_NAME, service_main }, { NULL, NULL } };
    if (StartServiceCtrlDispatcherW(table)) return 0;
    return (int)GetLastError();
}

/* -----------------------------------------------------------------------
 * The test
 * ----------------------------------------------------------------------- */
static BOOL status(SC_HANDLE s, SERVICE_STATUS_PROCESS *st)
{
    DWORD need = 0;
    return QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, (LPBYTE)st, sizeof(*st), &need);
}

static DWORD wait_state(SC_HANDLE s, DWORD want, SERVICE_STATUS_PROCESS *st)
{
    for (int i = 0; i < 300; i++) {
        if (status(s, st) && st->dwCurrentState == want) return want;
        Sleep(100);
    }
    return st->dwCurrentState;
}

static char *read_log(void)
{
    WCHAR path[MAX_PATH + 32];
    log_path(path, MAX_PATH + 32);
    static char buf[8192];
    buf[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return buf;
    DWORD n = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &n, NULL);
    buf[n] = 0;
    CloseHandle(f);
    return buf;
}

static void copy_test(void)
{
    WCHAR tmp[MAX_PATH], a[MAX_PATH + 16], b[MAX_PATH + 16];
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf(a, MAX_PATH + 16, L"%ssvccopy-a.bin", tmp);
    _snwprintf(b, MAX_PATH + 16, L"%ssvccopy-b.bin", tmp);
    SetFileAttributesW(b, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(b);
    HANDLE f = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n;
    WriteFile(f, "service binary", 14, &n, NULL);
    FILETIME old;                            /* 2020-09-15 15:29:00 UTC, an even second */
    SYSTEMTIME sys = { 2020, 9, 2, 15, 15, 29, 0, 0 };
    SystemTimeToFileTime(&sys, &old);
    CHECK("set the source's last-write time", SetFileTime(f, NULL, NULL, &old));
    CloseHandle(f);
    SetFileAttributesW(a, FILE_ATTRIBUTE_READONLY);
    CHECK("CopyFile", CopyFileW(a, b, FALSE));
    WIN32_FILE_ATTRIBUTE_DATA da, db;
    CHECK("attributes of both", GetFileAttributesExW(a, GetFileExInfoStandard, &da) &&
                                GetFileAttributesExW(b, GetFileExInfoStandard, &db));
    CHECK("the copy keeps the last-write time", !memcmp(&da.ftLastWriteTime, &db.ftLastWriteTime, sizeof(FILETIME)) &&
                                                !memcmp(&db.ftLastWriteTime, &old, sizeof(FILETIME)));
    CHECK("the copy keeps the size", da.nFileSizeLow == db.nFileSizeLow && db.nFileSizeLow == 14);
    CHECK("the copy keeps read-only", (db.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0);
    SetFileAttributesW(a, FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(b, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(a);
    DeleteFileW(b);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "/service")) return as_service();
    if (argc > 1 && !strcmp(argv[1], "/child")) return as_child();

    copy_test();

    WCHAR exe[MAX_PATH], path[MAX_PATH + 32], logp[MAX_PATH + 32];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf(path, MAX_PATH + 32, L"\"%s\" /service", exe);
    log_path(logp, MAX_PATH + 32);
    DeleteFileW(logp);

    CHECK("not started as a service: ERROR_FAILED_SERVICE_CONTROLLER_CONNECT",
          run_self(L"/child") == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    CHECK("OpenSCManager", scm != NULL);
    if (!scm) goto done;
    SC_HANDLE old = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (old) {                                /* left over from a run that failed */
        SERVICE_STATUS st;
        ControlService(old, SERVICE_CONTROL_STOP, &st);
        DeleteService(old);
        CloseServiceHandle(old);
    }
    SC_HANDLE s = CreateServiceW(scm, SVC_NAME, L"Nova test service", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, path, NULL, NULL, NULL, NULL, NULL);
    CHECK("CreateService", s != NULL);
    if (!s) goto done;
    SERVICE_STATUS_PROCESS sp;
    CHECK("a new service is stopped", status(s, &sp) && sp.dwCurrentState == SERVICE_STOPPED && !sp.dwProcessId);
    SERVICE_STATUS st;
    SetLastError(0);
    CHECK("controlling a stopped service: ERROR_SERVICE_NOT_ACTIVE",
          !ControlService(s, SERVICE_CONTROL_INTERROGATE, &st) && GetLastError() == ERROR_SERVICE_NOT_ACTIVE);

    LPCWSTR args[] = { L"alpha", L"beta gamma" };
    CHECK("StartService", StartServiceW(s, 2, args));
    CHECK("the service reports SERVICE_RUNNING", wait_state(s, SERVICE_RUNNING, &sp) == SERVICE_RUNNING);
    CHECK("it runs in its own process", sp.dwProcessId && sp.dwProcessId != GetCurrentProcessId());
    CHECK("it accepts stop", sp.dwControlsAccepted == SERVICE_ACCEPT_STOP);
    DWORD pid = sp.dwProcessId;
    SetLastError(0);
    CHECK("starting it again: ERROR_SERVICE_ALREADY_RUNNING",
          !StartServiceW(s, 0, NULL) && GetLastError() == ERROR_SERVICE_ALREADY_RUNNING);
    CHECK("SERVICE_CONTROL_INTERROGATE", ControlService(s, SERVICE_CONTROL_INTERROGATE, &st) &&
                                         st.dwCurrentState == SERVICE_RUNNING);
    CHECK("a user-defined control", ControlService(s, USER_CONTROL, &st));
    SetLastError(0);
    CHECK("a control it does not accept: ERROR_INVALID_SERVICE_CONTROL",
          !ControlService(s, SERVICE_CONTROL_PAUSE, &st) && GetLastError() == ERROR_INVALID_SERVICE_CONTROL);

    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    CHECK("SERVICE_CONTROL_STOP", ControlService(s, SERVICE_CONTROL_STOP, &st));
    CHECK("the service reports SERVICE_STOPPED", wait_state(s, SERVICE_STOPPED, &sp) == SERVICE_STOPPED && !sp.dwProcessId);
    CHECK("its process ends", proc && WaitForSingleObject(proc, 30000) == WAIT_OBJECT_0);
    if (proc) CloseHandle(proc);

    const char *log = read_log();
    char want[200];
    snprintf(want, sizeof(want), "argc 3 [%ls] [alpha] [beta gamma]", SVC_NAME);
    CHECK("RegisterServiceCtrlHandlerEx", strstr(log, "registered 1") != NULL);
    CHECK("ServiceMain gets the name and the start arguments", strstr(log, want) != NULL);
    snprintf(want, sizeof(want), "child %d", ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
    CHECK("a program the service starts cannot connect as the service", strstr(log, want) != NULL);
    snprintf(want, sizeof(want), "control %d ctx ok", USER_CONTROL);
    CHECK("the handler gets its context and the user control", strstr(log, want) != NULL);
    CHECK("the handler never sees a control the service does not accept", strstr(log, "control 2 ") == NULL);
    CHECK("the dispatcher returns once the service stops", strstr(log, "stopping") && strstr(log, "dispatcher returned"));

    CHECK("DeleteService", DeleteService(s));
    CloseServiceHandle(s);
    SetLastError(0);
    CHECK("a deleted service: ERROR_SERVICE_DOES_NOT_EXIST",
          !OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS) && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST);
    CloseServiceHandle(scm);
    DeleteFileW(logp);
done:
    if (fail) printf("service log:\n%s", read_log());
    printf("svctest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
