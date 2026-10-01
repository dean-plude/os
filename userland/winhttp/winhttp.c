/*
 * winhttp.dll — Windows' HTTP client.  What exists answers the questions
 * programs ask before connecting: there is no proxy, and automatic proxy
 * discovery finds nothing.  Sessions can be opened and closed; requests
 * are not carried out (programs with their own HTTP stack, such as the
 * Java runtime, only look up proxies here).
 */
#include <windows.h>

#define WINHTTPAPI __declspec(dllexport)
#define ERROR_WINHTTP_AUTODETECTION_FAILED_ 12180
#define ERROR_WINHTTP_INVALID_HANDLE_      12017
#define ERROR_WINHTTP_CANNOT_CONNECT_      12029

typedef struct { BOOL fAutoDetect; LPWSTR lpszAutoConfigUrl, lpszProxy, lpszProxyBypass; } IE_PROXY_CONFIG_;
typedef struct { DWORD dwAccessType; LPWSTR lpszProxy, lpszProxyBypass; } PROXY_INFO_;

static DWORD g_magic = 0x48545450;                  /* "HTTP": what a handle points at */

WINHTTPAPI BOOL WINAPI WinHttpGetIEProxyConfigForCurrentUser(IE_PROXY_CONFIG_ *c)
{
    if (!c) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ZeroMemory(c, sizeof(*c));                       /* direct connections */
    return TRUE;
}
WINHTTPAPI BOOL WINAPI WinHttpGetDefaultProxyConfiguration(PROXY_INFO_ *p)
{
    if (!p) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ZeroMemory(p, sizeof(*p));
    p->dwAccessType = 1;                             /* WINHTTP_ACCESS_TYPE_NO_PROXY */
    return TRUE;
}
WINHTTPAPI BOOL WINAPI WinHttpGetProxyForUrl(HANDLE s, LPCWSTR url, PVOID opts, PROXY_INFO_ *p)
{
    (void)s; (void)url; (void)opts; (void)p;
    SetLastError(ERROR_WINHTTP_AUTODETECTION_FAILED_);
    return FALSE;
}
WINHTTPAPI BOOL WINAPI WinHttpDetectAutoProxyConfigUrl(DWORD flags, LPWSTR *url)
{
    (void)flags; if (url) *url = 0;
    SetLastError(ERROR_WINHTTP_AUTODETECTION_FAILED_);
    return FALSE;
}
WINHTTPAPI HANDLE WINAPI WinHttpOpen(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags)
{
    (void)agent; (void)access; (void)proxy; (void)bypass; (void)flags;
    return &g_magic;
}
WINHTTPAPI BOOL WINAPI WinHttpCloseHandle(HANDLE h)
{
    if (h != &g_magic) { SetLastError(ERROR_WINHTTP_INVALID_HANDLE_); return FALSE; }
    return TRUE;
}
WINHTTPAPI BOOL WINAPI WinHttpSetTimeouts(HANDLE h, int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; return h == &g_magic; }
WINHTTPAPI BOOL WINAPI WinHttpSetOption(HANDLE h, DWORD o, LPVOID b, DWORD n) { (void)o; (void)b; (void)n; return h == &g_magic; }
WINHTTPAPI BOOL WINAPI WinHttpQueryOption(HANDLE h, DWORD o, LPVOID b, LPDWORD n) { (void)h; (void)o; (void)b; (void)n; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
WINHTTPAPI HANDLE WINAPI WinHttpConnect(HANDLE s, LPCWSTR server, WORD port, DWORD r)
{
    (void)s; (void)server; (void)port; (void)r;
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT_);
    return 0;
}
WINHTTPAPI BOOL WINAPI WinHttpCheckPlatform(void) { return TRUE; }
