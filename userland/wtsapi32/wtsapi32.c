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

/* A few facts about the one session: its id, state (active), user name */
WTSAPI BOOL WINAPI WTSQuerySessionInformationW(HANDLE server, DWORD session, int cls, LPWSTR *buf, DWORD *n)
{
    (void)server; (void)session;
    if (!buf || !n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *buf = NULL; *n = 0;
    if (cls == 4 /* WTSSessionId */ || cls == 8 /* WTSConnectState: WTSActive (0) */) {
        DWORD *d = LocalAlloc(LPTR, sizeof(DWORD));
        if (!d) return FALSE;
        *d = cls == 4 ? 1 : 0;
        *buf = (LPWSTR)d; *n = sizeof(DWORD);
        return TRUE;
    }
    if (cls == 5 /* WTSUserName */) {
        WCHAR name[256];
        DWORD len = 256;
        if (!GetUserNameW(name, &len)) return FALSE;
        WCHAR *w = LocalAlloc(LPTR, len * sizeof(WCHAR));
        if (!w) return FALSE;
        lstrcpyW(w, name);
        *buf = w; *n = len * sizeof(WCHAR);
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
