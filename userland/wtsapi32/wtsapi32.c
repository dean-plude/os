/*
 * wtsapi32.dll — Remote Desktop Services.  NovaOS has one session, the
 * console's (1), which never connects, disconnects or locks, so session
 * notifications are accepted and never sent.
 */
#include <windows.h>

#define WTSAPI __declspec(dllexport)

WTSAPI BOOL WINAPI WTSRegisterSessionNotification(HWND h, DWORD flags) { (void)flags; return IsWindow(h); }
WTSAPI BOOL WINAPI WTSUnRegisterSessionNotification(HWND h) { (void)h; return TRUE; }
WTSAPI BOOL WINAPI WTSRegisterSessionNotificationEx(HANDLE server, HWND h, DWORD flags) { (void)server; (void)flags; return IsWindow(h); }
WTSAPI BOOL WINAPI WTSUnRegisterSessionNotificationEx(HANDLE server, HWND h) { (void)server; (void)h; return TRUE; }
WTSAPI DWORD WINAPI WTSGetActiveConsoleSessionId(void) { return 1; }
WTSAPI BOOL WINAPI WTSQueryUserToken(ULONG session, PHANDLE token)
{
    (void)session;
    return OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, token);
}
WTSAPI void WINAPI WTSFreeMemory(PVOID p) { LocalFree(p); }
WTSAPI HANDLE WINAPI WTSOpenServerW(LPWSTR name) { (void)name; return 0; }        /* WTS_CURRENT_SERVER_HANDLE */
WTSAPI void WINAPI WTSCloseServer(HANDLE h) { (void)h; }
