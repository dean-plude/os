/* svc.c — a Windows service for the services test package: it writes
 * C:\ProgramData\NovaSvc\state.txt when it starts and when it stops.
 *   x86_64-w64-mingw32-gcc -O2 -o novasvc.exe svc.c -ladvapi32 */
#include <windows.h>
#include <stdio.h>

static SERVICE_STATUS_HANDLE g_h;
static SERVICE_STATUS g_st;
static HANDLE g_stop;

static void note(const char *what)
{
    CreateDirectoryA("C:\\ProgramData", NULL);
    CreateDirectoryA("C:\\ProgramData\\NovaSvc", NULL);
    FILE *f = fopen("C:\\ProgramData\\NovaSvc\\state.txt", "a");
    if (f) { fprintf(f, "%s\n", what); fclose(f); }
}

static void report(DWORD state)
{
    g_st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_st.dwCurrentState = state;
    g_st.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
    SetServiceStatus(g_h, &g_st);
}

static DWORD WINAPI handler(DWORD ctl, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)type; (void)data; (void)ctx;
    if (ctl == SERVICE_CONTROL_STOP) { report(SERVICE_STOP_PENDING); SetEvent(g_stop); }
    return NO_ERROR;
}

static void WINAPI svc_main(DWORD argc, LPSTR *argv)
{
    char line[300];
    g_h = RegisterServiceCtrlHandlerExA(argv[0], handler, NULL);
    g_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    report(SERVICE_RUNNING);
    snprintf(line, sizeof(line), "running %s (%lu args)", argv[0], (unsigned long)argc);
    note(line);
    WaitForSingleObject(g_stop, INFINITE);
    note("stopped");
    report(SERVICE_STOPPED);
}

int main(void)
{
    SERVICE_TABLE_ENTRYA table[] = { { "NovaTestSvc", svc_main }, { NULL, NULL } };
    if (!StartServiceCtrlDispatcherA(table)) {
        printf("not started as a service (error %lu)\n", GetLastError());
        return 1;
    }
    return 0;
}
