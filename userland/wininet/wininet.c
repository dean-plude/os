/*
 * wininet.dll — the Internet Explorer client library.  URL parsing works
 * (InternetCrackUrl, as programs use it to split addresses); connections
 * are not made through it: InternetOpen fails, and the machine reports
 * itself offline to programs asking this library.
 */
#include <windows.h>

#define INETAPI __declspec(dllexport)
#define ERROR_INTERNET_INVALID_URL_         12005
#define ERROR_INTERNET_UNRECOGNIZED_SCHEME_ 12006
#define ERROR_INTERNET_NO_DIRECT_ACCESS_    12027

typedef struct {
    DWORD  dwStructSize;
    LPWSTR lpszScheme;    DWORD dwSchemeLength;
    int    nScheme;
    LPWSTR lpszHostName;  DWORD dwHostNameLength;
    WORD   nPort;
    LPWSTR lpszUserName;  DWORD dwUserNameLength;
    LPWSTR lpszPassword;  DWORD dwPasswordLength;
    LPWSTR lpszUrlPath;   DWORD dwUrlPathLength;
    LPWSTR lpszExtraInfo; DWORD dwExtraInfoLength;
} URL_COMPONENTSW_;

typedef struct {
    DWORD  dwStructSize;
    LPSTR  lpszScheme;    DWORD dwSchemeLength;
    int    nScheme;
    LPSTR  lpszHostName;  DWORD dwHostNameLength;
    WORD   nPort;
    LPSTR  lpszUserName;  DWORD dwUserNameLength;
    LPSTR  lpszPassword;  DWORD dwPasswordLength;
    LPSTR  lpszUrlPath;   DWORD dwUrlPathLength;
    LPSTR  lpszExtraInfo; DWORD dwExtraInfoLength;
} URL_COMPONENTSA_;

/* A part of the URL, [s, s + n): handed back as a pointer into the URL,
 * copied into the caller's buffer, or not wanted (length 0) */
static BOOL put_w(LPWSTR *dst, DWORD *len, const WCHAR *s, DWORD n)
{
    if (!*len) return TRUE;
    if (!*dst) { *dst = (LPWSTR)s; *len = n; return TRUE; }
    if (*len <= n) { *len = n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (DWORD i = 0; i < n; i++) (*dst)[i] = s[i];
    (*dst)[n] = 0;
    *len = n;
    return TRUE;
}

static int eq_scheme(const WCHAR *s, DWORD n, const char *name)
{
    DWORD i = 0;
    for (; i < n && name[i]; i++) if ((s[i] | 0x20) != name[i]) return 0;
    return i == n && !name[i];
}

INETAPI BOOL WINAPI InternetCrackUrlW(LPCWSTR url, DWORD url_len, DWORD flags, URL_COMPONENTSW_ *uc)
{
    (void)flags;
    if (!url || !uc) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!url_len) while (url[url_len]) url_len++;
    const WCHAR *end = url + url_len, *p = url;
    while (p < end && *p != ':') p++;
    if (p == end || p == url) { SetLastError(ERROR_INTERNET_UNRECOGNIZED_SCHEME_); return FALSE; }
    DWORD slen = (DWORD)(p - url);
    static const struct { const char *name; int scheme; WORD port; } schemes[] = {
        { "ftp", 1, 21 }, { "gopher", 2, 70 }, { "http", 3, 80 }, { "https", 4, 443 },
        { "file", 5, 0 }, { "news", 6, 119 }, { "mailto", 7, 0 },
    };
    int scheme = -1;
    WORD port = 0;
    for (unsigned i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++)
        if (eq_scheme(url, slen, schemes[i].name)) { scheme = schemes[i].scheme; port = schemes[i].port; }
    if (!put_w(&uc->lpszScheme, &uc->dwSchemeLength, url, slen)) return FALSE;
    uc->nScheme = scheme;
    p++;                                                    /* ':' */
    const WCHAR *user = 0, *pass = 0, *host = p, *hend = p;
    DWORD ulen = 0, plen = 0;
    if (end - p >= 2 && p[0] == '/' && p[1] == '/') {
        p += 2;
        const WCHAR *auth = p, *aend = p;
        while (aend < end && *aend != '/' && *aend != '?' && *aend != '#') aend++;
        const WCHAR *at = 0;
        for (const WCHAR *q = auth; q < aend; q++) if (*q == '@') at = q;
        host = auth;
        if (at) {
            user = auth;
            const WCHAR *colon = auth;
            while (colon < at && *colon != ':') colon++;
            ulen = (DWORD)(colon - auth);
            if (colon < at) { pass = colon + 1; plen = (DWORD)(at - pass); }
            host = at + 1;
        }
        hend = host;
        if (hend < aend && *hend == '[') { while (hend < aend && *hend != ']') hend++; if (hend < aend) hend++; }
        while (hend < aend && *hend != ':') hend++;
        if (hend < aend) {                                  /* ":port" */
            DWORD n = 0;
            for (const WCHAR *q = hend + 1; q < aend; q++) {
                if (*q < '0' || *q > '9') { SetLastError(ERROR_INTERNET_INVALID_URL_); return FALSE; }
                n = n * 10 + (DWORD)(*q - '0');
            }
            port = (WORD)n;
        }
        p = aend;
    }
    if (!put_w(&uc->lpszHostName, &uc->dwHostNameLength, host, (DWORD)(hend - host))) return FALSE;
    uc->nPort = port;
    if (!put_w(&uc->lpszUserName, &uc->dwUserNameLength, user ? user : p, ulen)) return FALSE;
    if (!put_w(&uc->lpszPassword, &uc->dwPasswordLength, pass ? pass : p, plen)) return FALSE;
    const WCHAR *extra = p;
    while (extra < end && *extra != '?' && *extra != '#') extra++;
    if (!uc->dwExtraInfoLength) extra = end;                /* not asked for: the path keeps it */
    if (!put_w(&uc->lpszUrlPath, &uc->dwUrlPathLength, p, (DWORD)(extra - p))) return FALSE;
    if (!put_w(&uc->lpszExtraInfo, &uc->dwExtraInfoLength, extra, (DWORD)(end - extra))) return FALSE;
    return TRUE;
}

/* The A version: cracked in UTF-16, parts mapped back to the A string
 * (offsets carry over: the conversion is one character for one byte for
 * the ASCII that URLs are made of) */
INETAPI BOOL WINAPI InternetCrackUrlA(LPCSTR url, DWORD url_len, DWORD flags, URL_COMPONENTSA_ *uc)
{
    if (!url || !uc) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!url_len) while (url[url_len]) url_len++;
    WCHAR *w = HeapAlloc(GetProcessHeap(), 0, (url_len + 1) * sizeof(WCHAR));
    if (!w) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (DWORD i = 0; i < url_len; i++) w[i] = (WCHAR)(BYTE)url[i];
    w[url_len] = 0;
    URL_COMPONENTSW_ c = { sizeof(c) };
    /* ask for pointers to every part the caller wants */
    c.dwSchemeLength = uc->dwSchemeLength ? 1 : 0;
    c.dwHostNameLength = uc->dwHostNameLength ? 1 : 0;
    c.dwUserNameLength = uc->dwUserNameLength ? 1 : 0;
    c.dwPasswordLength = uc->dwPasswordLength ? 1 : 0;
    c.dwUrlPathLength = uc->dwUrlPathLength ? 1 : 0;
    c.dwExtraInfoLength = uc->dwExtraInfoLength ? 1 : 0;
    BOOL ok = InternetCrackUrlW(w, url_len, flags, &c);
    if (ok) {
        uc->nScheme = c.nScheme;
        uc->nPort = c.nPort;
        struct { LPSTR *dst; DWORD *len; LPWSTR src; DWORD n; } parts[] = {
            { &uc->lpszScheme, &uc->dwSchemeLength, c.lpszScheme, c.dwSchemeLength },
            { &uc->lpszHostName, &uc->dwHostNameLength, c.lpszHostName, c.dwHostNameLength },
            { &uc->lpszUserName, &uc->dwUserNameLength, c.lpszUserName, c.dwUserNameLength },
            { &uc->lpszPassword, &uc->dwPasswordLength, c.lpszPassword, c.dwPasswordLength },
            { &uc->lpszUrlPath, &uc->dwUrlPathLength, c.lpszUrlPath, c.dwUrlPathLength },
            { &uc->lpszExtraInfo, &uc->dwExtraInfoLength, c.lpszExtraInfo, c.dwExtraInfoLength },
        };
        for (unsigned i = 0; ok && i < sizeof(parts) / sizeof(parts[0]); i++) {
            if (!*parts[i].len) continue;
            const char *s = parts[i].src ? url + (parts[i].src - w) : url;
            DWORD n = parts[i].src ? parts[i].n : 0;
            if (!*parts[i].dst) { *parts[i].dst = (LPSTR)s; *parts[i].len = n; continue; }
            if (*parts[i].len <= n) { *parts[i].len = n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); ok = FALSE; break; }
            for (DWORD k = 0; k < n; k++) (*parts[i].dst)[k] = s[k];
            (*parts[i].dst)[n] = 0;
            *parts[i].len = n;
        }
    }
    HeapFree(GetProcessHeap(), 0, w);
    return ok;
}

INETAPI BOOL WINAPI InternetGetConnectedState(LPDWORD flags, DWORD reserved)
{
    (void)reserved;
    if (flags) *flags = 0x20;                              /* INTERNET_CONNECTION_OFFLINE */
    return FALSE;
}
INETAPI BOOL WINAPI InternetCheckConnectionW(LPCWSTR url, DWORD flags, DWORD reserved)
{ (void)url; (void)flags; (void)reserved; SetLastError(ERROR_INTERNET_NO_DIRECT_ACCESS_); return FALSE; }
INETAPI BOOL WINAPI InternetCheckConnectionA(LPCSTR url, DWORD flags, DWORD reserved)
{ (void)url; (void)flags; (void)reserved; SetLastError(ERROR_INTERNET_NO_DIRECT_ACCESS_); return FALSE; }
INETAPI HANDLE WINAPI InternetOpenW(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags)
{ (void)agent; (void)access; (void)proxy; (void)bypass; (void)flags; SetLastError(ERROR_INTERNET_NO_DIRECT_ACCESS_); return 0; }
INETAPI HANDLE WINAPI InternetOpenA(LPCSTR agent, DWORD access, LPCSTR proxy, LPCSTR bypass, DWORD flags)
{ (void)agent; (void)access; (void)proxy; (void)bypass; (void)flags; SetLastError(ERROR_INTERNET_NO_DIRECT_ACCESS_); return 0; }
INETAPI BOOL WINAPI InternetCloseHandle(HANDLE h) { (void)h; return TRUE; }
INETAPI BOOL WINAPI InternetSetOptionW(HANDLE h, DWORD opt, LPVOID buf, DWORD n) { (void)h; (void)opt; (void)buf; (void)n; return TRUE; }
INETAPI BOOL WINAPI InternetSetOptionA(HANDLE h, DWORD opt, LPVOID buf, DWORD n) { (void)h; (void)opt; (void)buf; (void)n; return TRUE; }

/* there is never a valid handle (InternetOpen gives none) to call back on */
INETAPI PVOID WINAPI InternetSetStatusCallbackW(HANDLE h, PVOID cb)
{ (void)h; (void)cb; SetLastError(12018); return (PVOID)(LONG_PTR)-1; }    /* INTERNET_INVALID_STATUS_CALLBACK */
INETAPI PVOID WINAPI InternetSetStatusCallbackA(HANDLE h, PVOID cb) { return InternetSetStatusCallbackW(h, cb); }

INETAPI BOOL WINAPI InternetGetConnectedStateExW(LPDWORD flags, LPWSTR name, DWORD n, DWORD reserved)
{
    if (name && n) name[0] = 0;
    return InternetGetConnectedState(flags, reserved);
}
/* Options: nothing configured (proxy settings, timeouts...) */
INETAPI BOOL WINAPI InternetQueryOptionW(HANDLE h, DWORD opt, LPVOID buf, LPDWORD n)
{
    (void)h; (void)opt; (void)buf; (void)n;
    SetLastError(12018);                            /* ERROR_INTERNET_INCORRECT_HANDLE_TYPE */
    return FALSE;
}
INETAPI BOOL WINAPI InternetQueryOptionA(HANDLE h, DWORD opt, LPVOID buf, LPDWORD n) { return InternetQueryOptionW(h, opt, buf, n); }

/* HTTP sessions: InternetOpen gives no handle, so none of these is reached
 * with a valid one; each fails as a closed handle would */
INETAPI HANDLE WINAPI InternetConnectW(HANDLE h, LPCWSTR server, WORD port, LPCWSTR user, LPCWSTR pw, DWORD service, DWORD flags, DWORD_PTR ctx)
{ (void)h; (void)server; (void)port; (void)user; (void)pw; (void)service; (void)flags; (void)ctx; SetLastError(ERROR_INVALID_HANDLE); return 0; }
INETAPI HANDLE WINAPI InternetConnectA(HANDLE h, LPCSTR server, WORD port, LPCSTR user, LPCSTR pw, DWORD service, DWORD flags, DWORD_PTR ctx)
{ (void)h; (void)server; (void)port; (void)user; (void)pw; (void)service; (void)flags; (void)ctx; SetLastError(ERROR_INVALID_HANDLE); return 0; }
INETAPI HANDLE WINAPI HttpOpenRequestW(HANDLE h, LPCWSTR verb, LPCWSTR obj, LPCWSTR ver, LPCWSTR referrer, LPCWSTR *accept, DWORD flags, DWORD_PTR ctx)
{ (void)h; (void)verb; (void)obj; (void)ver; (void)referrer; (void)accept; (void)flags; (void)ctx; SetLastError(ERROR_INVALID_HANDLE); return 0; }
INETAPI HANDLE WINAPI HttpOpenRequestA(HANDLE h, LPCSTR verb, LPCSTR obj, LPCSTR ver, LPCSTR referrer, LPCSTR *accept, DWORD flags, DWORD_PTR ctx)
{ (void)h; (void)verb; (void)obj; (void)ver; (void)referrer; (void)accept; (void)flags; (void)ctx; SetLastError(ERROR_INVALID_HANDLE); return 0; }
INETAPI BOOL WINAPI HttpAddRequestHeadersW(HANDLE h, LPCWSTR headers, DWORD n, DWORD flags) { (void)h; (void)headers; (void)n; (void)flags; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
INETAPI BOOL WINAPI HttpAddRequestHeadersA(HANDLE h, LPCSTR headers, DWORD n, DWORD flags) { (void)h; (void)headers; (void)n; (void)flags; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
INETAPI BOOL WINAPI HttpSendRequestW(HANDLE h, LPCWSTR headers, DWORD n, LPVOID opt, DWORD optn) { (void)h; (void)headers; (void)n; (void)opt; (void)optn; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
INETAPI BOOL WINAPI HttpSendRequestA(HANDLE h, LPCSTR headers, DWORD n, LPVOID opt, DWORD optn) { (void)h; (void)headers; (void)n; (void)opt; (void)optn; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
INETAPI BOOL WINAPI HttpQueryInfoW(HANDLE h, DWORD level, LPVOID buf, LPDWORD n, LPDWORD index) { (void)h; (void)level; (void)buf; (void)n; (void)index; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
INETAPI BOOL WINAPI HttpQueryInfoA(HANDLE h, DWORD level, LPVOID buf, LPDWORD n, LPDWORD index) { (void)h; (void)level; (void)buf; (void)n; (void)index; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
INETAPI BOOL WINAPI InternetQueryDataAvailable(HANDLE h, LPDWORD n, DWORD flags, DWORD_PTR ctx) { (void)h; (void)flags; (void)ctx; if (n) *n = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
INETAPI BOOL WINAPI InternetReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read) { (void)h; (void)buf; (void)n; if (read) *read = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
INETAPI BOOL WINAPI InternetWriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written) { (void)h; (void)buf; (void)n; if (written) *written = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
