/*
 * mpr.dll — the network-provider router (WNet*): mapped network drives
 * and browsing network shares. NovaOS has no file-sharing client, so there
 * is no network to enumerate or connect to: the calls answer
 * ERROR_NO_NETWORK, the reply programs such as file managers expect on a
 * machine without networking.
 */

#include <windows.h>

#define MPRAPI __declspec(dllexport)
#define ERROR_NO_NETWORK_       1222
#define ERROR_NOT_CONNECTED_    2250
#ifndef NO_ERROR
#define NO_ERROR 0
#endif
#ifndef ERROR_NO_MORE_ITEMS
#define ERROR_NO_MORE_ITEMS 259
#endif

static DWORD no_net(void) { SetLastError(ERROR_NO_NETWORK_); return ERROR_NO_NETWORK_; }

MPRAPI DWORD WINAPI WNetOpenEnumW(DWORD scope, DWORD type, DWORD usage, void *res, HANDLE *h) { (void)scope; (void)type; (void)usage; (void)res; if (h) *h = 0; return no_net(); }
MPRAPI DWORD WINAPI WNetOpenEnumA(DWORD scope, DWORD type, DWORD usage, void *res, HANDLE *h) { return WNetOpenEnumW(scope, type, usage, res, h); }
MPRAPI DWORD WINAPI WNetEnumResourceW(HANDLE h, LPDWORD count, LPVOID buf, LPDWORD size) { (void)h; (void)buf; (void)size; if (count) *count = 0; return ERROR_NO_MORE_ITEMS; }
MPRAPI DWORD WINAPI WNetEnumResourceA(HANDLE h, LPDWORD count, LPVOID buf, LPDWORD size) { return WNetEnumResourceW(h, count, buf, size); }
MPRAPI DWORD WINAPI WNetCloseEnum(HANDLE h) { (void)h; return NO_ERROR; }
MPRAPI DWORD WINAPI WNetGetResourceInformationW(void *res, LPVOID buf, LPDWORD size, LPWSTR *system) { (void)res; (void)buf; (void)size; if (system) *system = 0; return no_net(); }
MPRAPI DWORD WINAPI WNetGetResourceParentW(void *res, LPVOID buf, LPDWORD size) { (void)res; (void)buf; (void)size; return no_net(); }
MPRAPI DWORD WINAPI WNetAddConnection2W(void *res, LPCWSTR pw, LPCWSTR user, DWORD flags) { (void)res; (void)pw; (void)user; (void)flags; return no_net(); }
MPRAPI DWORD WINAPI WNetAddConnection2A(void *res, LPCSTR pw, LPCSTR user, DWORD flags) { (void)res; (void)pw; (void)user; (void)flags; return no_net(); }
MPRAPI DWORD WINAPI WNetAddConnection3W(HWND w, void *res, LPCWSTR pw, LPCWSTR user, DWORD flags) { (void)w; return WNetAddConnection2W(res, pw, user, flags); }
MPRAPI DWORD WINAPI WNetCancelConnection2W(LPCWSTR name, DWORD flags, BOOL force) { (void)name; (void)flags; (void)force; SetLastError(ERROR_NOT_CONNECTED_); return ERROR_NOT_CONNECTED_; }
MPRAPI DWORD WINAPI WNetCancelConnection2A(LPCSTR name, DWORD flags, BOOL force) { (void)name; (void)flags; (void)force; SetLastError(ERROR_NOT_CONNECTED_); return ERROR_NOT_CONNECTED_; }
MPRAPI DWORD WINAPI WNetGetConnectionW(LPCWSTR local, LPWSTR remote, LPDWORD n) { (void)local; (void)remote; (void)n; SetLastError(ERROR_NOT_CONNECTED_); return ERROR_NOT_CONNECTED_; }
MPRAPI DWORD WINAPI WNetGetConnectionA(LPCSTR local, LPSTR remote, LPDWORD n) { (void)local; (void)remote; (void)n; SetLastError(ERROR_NOT_CONNECTED_); return ERROR_NOT_CONNECTED_; }
MPRAPI DWORD WINAPI WNetGetUniversalNameW(LPCWSTR local, DWORD level, LPVOID buf, LPDWORD n) { (void)local; (void)level; (void)buf; (void)n; SetLastError(ERROR_NOT_CONNECTED_); return ERROR_NOT_CONNECTED_; }
MPRAPI DWORD WINAPI WNetGetUniversalNameA(LPCSTR local, DWORD level, LPVOID buf, LPDWORD n) { (void)local; (void)level; (void)buf; (void)n; SetLastError(ERROR_NOT_CONNECTED_); return ERROR_NOT_CONNECTED_; }
MPRAPI DWORD WINAPI WNetGetUserW(LPCWSTR name, LPWSTR user, LPDWORD n)
{
    (void)name;
    return GetEnvironmentVariableW(L"USERNAME", user, n ? *n : 0) ? NO_ERROR : no_net();
}
MPRAPI DWORD WINAPI WNetGetLastErrorW(LPDWORD err, LPWSTR buf, DWORD n, LPWSTR prov, DWORD pn)
{
    if (err) *err = 0;
    if (buf && n) buf[0] = 0;
    if (prov && pn) prov[0] = 0;
    return NO_ERROR;
}
MPRAPI DWORD WINAPI WNetConnectionDialog(HWND w, DWORD type) { (void)w; (void)type; return no_net(); }
MPRAPI DWORD WINAPI WNetDisconnectDialog(HWND w, DWORD type) { (void)w; (void)type; return no_net(); }
MPRAPI DWORD WINAPI WNetGetNetworkInformationW(LPCWSTR provider, void *info) { (void)provider; (void)info; return no_net(); }
MPRAPI DWORD WINAPI WNetGetProviderNameW(DWORD type, LPWSTR name, LPDWORD size) { (void)type; (void)name; (void)size; return no_net(); }
