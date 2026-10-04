/* winsvc.h — the service control manager (advapi32, service.c), the part
 * programs use.  Not pulled in by windows.h here: include it. */
#pragma once
#include <windows.h>

#ifndef ERROR_SERVICE_ALREADY_RUNNING
#define ERROR_SERVICE_ALREADY_RUNNING 1056
#endif
#define SC_MANAGER_CONNECT           0x0001
#define SC_MANAGER_ENUMERATE_SERVICE 0x0004
#define SC_MANAGER_ALL_ACCESS        0xF003F
#define SERVICE_QUERY_CONFIG         0x0001
#define SERVICE_QUERY_STATUS         0x0004
#define SERVICE_START                0x0010
#define SERVICE_STOP                 0x0020
#define SERVICE_ALL_ACCESS           0xF01FF

#define SERVICE_KERNEL_DRIVER        0x01
#define SERVICE_FILE_SYSTEM_DRIVER   0x02
#define SERVICE_WIN32_OWN_PROCESS    0x10
#define SERVICE_WIN32_SHARE_PROCESS  0x20
#define SERVICE_BOOT_START           0
#define SERVICE_SYSTEM_START         1
#define SERVICE_AUTO_START           2
#define SERVICE_DEMAND_START         3
#define SERVICE_DISABLED             4

#define SERVICE_STOPPED              1
#define SERVICE_START_PENDING        2
#define SERVICE_STOP_PENDING         3
#define SERVICE_RUNNING              4
#define SERVICE_PAUSE_PENDING        6
#define SERVICE_PAUSED               7
#define SERVICE_CONTROL_STOP         1
#define SERVICE_CONTROL_PAUSE        2
#define SERVICE_CONTROL_CONTINUE     3
#define SERVICE_CONTROL_INTERROGATE  4
#define SERVICE_CONTROL_SHUTDOWN     5
#define SERVICE_CONTROL_PARAMCHANGE  6
#define SERVICE_ACCEPT_STOP          1
#define SERVICE_ACCEPT_PAUSE_CONTINUE 2
#define SERVICE_ACCEPT_SHUTDOWN      4
#define SERVICE_ACCEPT_PARAMCHANGE   8
#define SERVICE_ERROR_NORMAL         1
#define SC_STATUS_PROCESS_INFO       0
#ifndef ERROR_SERVICE_REQUEST_TIMEOUT
#define ERROR_SERVICE_REQUEST_TIMEOUT 1053
#endif
#ifndef ERROR_INVALID_SERVICE_CONTROL
#define ERROR_INVALID_SERVICE_CONTROL 1052
#endif
#ifndef ERROR_SERVICE_DOES_NOT_EXIST
#define ERROR_SERVICE_DOES_NOT_EXIST 1060
#endif
#ifndef ERROR_SERVICE_NOT_ACTIVE
#define ERROR_SERVICE_NOT_ACTIVE     1062
#endif
#ifndef ERROR_FAILED_SERVICE_CONTROLLER_CONNECT
#define ERROR_FAILED_SERVICE_CONTROLLER_CONNECT 1063
#endif

typedef void *SC_HANDLE;
typedef void *SERVICE_STATUS_HANDLE;
typedef struct {
    DWORD dwServiceType, dwCurrentState, dwControlsAccepted, dwWin32ExitCode, dwServiceSpecificExitCode,
          dwCheckPoint, dwWaitHint;
} SERVICE_STATUS;
typedef struct {
    DWORD dwServiceType, dwCurrentState, dwControlsAccepted, dwWin32ExitCode, dwServiceSpecificExitCode,
          dwCheckPoint, dwWaitHint, dwProcessId, dwServiceFlags;
} SERVICE_STATUS_PROCESS;
typedef void (WINAPI *LPSERVICE_MAIN_FUNCTIONA)(DWORD argc, LPSTR *argv);
typedef void (WINAPI *LPSERVICE_MAIN_FUNCTIONW)(DWORD argc, LPWSTR *argv);
typedef struct { LPSTR lpServiceName; LPSERVICE_MAIN_FUNCTIONA lpServiceProc; } SERVICE_TABLE_ENTRYA;
typedef struct { LPWSTR lpServiceName; LPSERVICE_MAIN_FUNCTIONW lpServiceProc; } SERVICE_TABLE_ENTRYW;
typedef DWORD (WINAPI *LPHANDLER_FUNCTION_EX)(DWORD control, DWORD type, LPVOID data, LPVOID ctx);

__declspec(dllimport) SC_HANDLE WINAPI OpenSCManagerA(LPCSTR machine, LPCSTR db, DWORD access);
__declspec(dllimport) SC_HANDLE WINAPI OpenSCManagerW(LPCWSTR machine, LPCWSTR db, DWORD access);
__declspec(dllimport) SC_HANDLE WINAPI OpenServiceA(SC_HANDLE scm, LPCSTR name, DWORD access);
__declspec(dllimport) SC_HANDLE WINAPI OpenServiceW(SC_HANDLE scm, LPCWSTR name, DWORD access);
__declspec(dllimport) BOOL WINAPI StartServiceA(SC_HANDLE h, DWORD argc, LPCSTR *argv);
__declspec(dllimport) BOOL WINAPI StartServiceW(SC_HANDLE h, DWORD argc, LPCWSTR *argv);
__declspec(dllimport) BOOL WINAPI ControlService(SC_HANDLE h, DWORD control, SERVICE_STATUS *st);
__declspec(dllimport) BOOL WINAPI QueryServiceStatus(SC_HANDLE h, SERVICE_STATUS *st);
__declspec(dllimport) BOOL WINAPI CloseServiceHandle(SC_HANDLE h);
__declspec(dllimport) BOOL WINAPI StartServiceCtrlDispatcherA(const SERVICE_TABLE_ENTRYA *table);
__declspec(dllimport) BOOL WINAPI StartServiceCtrlDispatcherW(const SERVICE_TABLE_ENTRYW *table);
__declspec(dllimport) SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerExA(LPCSTR name, LPHANDLER_FUNCTION_EX fn, LPVOID ctx);
__declspec(dllimport) SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerExW(LPCWSTR name, LPHANDLER_FUNCTION_EX fn, LPVOID ctx);
__declspec(dllimport) BOOL WINAPI SetServiceStatus(SERVICE_STATUS_HANDLE h, SERVICE_STATUS *st);
__declspec(dllimport) SC_HANDLE WINAPI CreateServiceW(SC_HANDLE scm, LPCWSTR name, LPCWSTR display, DWORD access, DWORD type,
                                                      DWORD start, DWORD errc, LPCWSTR path, LPCWSTR group, LPDWORD tag,
                                                      LPCWSTR deps, LPCWSTR user, LPCWSTR pw);
__declspec(dllimport) BOOL WINAPI DeleteService(SC_HANDLE h);
__declspec(dllimport) BOOL WINAPI QueryServiceStatusEx(SC_HANDLE h, int level, LPBYTE buf, DWORD n, LPDWORD need);
__declspec(dllimport) SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerW(LPCWSTR name, void (WINAPI *fn)(DWORD control));
