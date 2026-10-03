/* httptest.exe — WinHTTP client and self-test
 *
 *   httptest [-2] [-k] [-a] [-X verb] [-d data] <url>
 *       fetch @url and print the status, the protocol and the body's size
 *       (-2: allow HTTP/2, -k: accept any certificate, -a: asynchronous)
 *   httptest suite <https-base> <http-base>
 *       the checks tools/selftest.py runs against tools/h2server.js:
 *       HTTP/2 negotiated by ALPN, a large body, a POST, a redirect,
 *       headers, chunked HTTP/1.1, and the asynchronous API
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winhttp.h>

typedef struct {
    DWORD status, protocol, bytes;
    unsigned sum;                        /* a checksum of the body */
    char head[256];                      /* its start */
    WCHAR server_proto[32];              /* the X-Protocol header (what the server saw) */
    DWORD err;
} Result;

static int g_passed, g_failed;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_passed++; else g_failed++;
}

/* --- the asynchronous form: callbacks drive the request ---------------- */
typedef struct { HANDLE done; Result *res; BYTE buf[8192]; int ended; } Async;

static void CALLBACK on_status(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len)
{
    Async *a = (Async *)ctx;
    if (!a) return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        if (!WinHttpReceiveResponse(h, 0)) { a->res->err = GetLastError(); SetEvent(a->done); }
        break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: {
        DWORD n = sizeof(DWORD);
        WinHttpQueryHeaders(h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &a->res->status, &n, 0);
        n = sizeof(DWORD);
        WinHttpQueryOption(h, WINHTTP_OPTION_HTTP_PROTOCOL_USED, &a->res->protocol, &n);
        if (!WinHttpReadData(h, a->buf, sizeof(a->buf), 0)) { a->res->err = GetLastError(); SetEvent(a->done); }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        if (!len) { SetEvent(a->done); break; }
        for (DWORD i = 0; i < len; i++) a->res->sum = a->res->sum * 31 + ((BYTE *)info)[i];
        a->res->bytes += len;
        if (!WinHttpReadData(h, a->buf, sizeof(a->buf), 0)) { a->res->err = GetLastError(); SetEvent(a->done); }
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        a->res->err = ((WINHTTP_ASYNC_RESULT *)info)->dwError;
        SetEvent(a->done);
        break;
    }
}

/* Fetch @url; the body is summed, its start kept */
static void fetch(const WCHAR *url, const WCHAR *verb, const char *data, int http2, int insecure, int async,
                  Result *res)
{
    memset(res, 0, sizeof(*res));
    WCHAR host[256], path[1024];
    URL_COMPONENTS uc = { sizeof(uc) };
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    uc.dwExtraInfoLength = 0;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { res->err = GetLastError(); return; }
    HINTERNET s = WinHttpOpen(L"NovaOS-httptest/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, 0, 0, async ? WINHTTP_FLAG_ASYNC : 0);
    if (http2) {
        DWORD p = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(s, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &p, sizeof(p));
    }
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, verb, path, 0, 0, 0, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : 0;
    if (!r) { res->err = GetLastError(); goto out; }
    if (insecure) {
        DWORD f = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(r, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof(f));
    }
    DWORD dlen = data ? (DWORD)strlen(data) : 0;
    if (async) {
        Async a = { CreateEventW(0, TRUE, FALSE, 0), res };
        WinHttpSetStatusCallback(r, on_status, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, 0);
        if (!WinHttpSendRequest(r, 0, 0, (void *)data, dlen, dlen, (DWORD_PTR)&a)) res->err = GetLastError();
        else if (WaitForSingleObject(a.done, 30000)) res->err = ERROR_WINHTTP_TIMEOUT;
        CloseHandle(a.done);
        goto out;
    }
    if (!WinHttpSendRequest(r, data ? L"Content-Type: text/plain\r\n" : 0, (DWORD)-1, (void *)data, dlen, dlen, 0) ||
        !WinHttpReceiveResponse(r, 0)) { res->err = GetLastError(); goto out; }
    DWORD n = sizeof(DWORD);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &res->status, &n, 0);
    n = sizeof(DWORD);
    WinHttpQueryOption(r, WINHTTP_OPTION_HTTP_PROTOCOL_USED, &res->protocol, &n);
    n = sizeof(res->server_proto);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_CUSTOM, L"X-Protocol", res->server_proto, &n, 0);
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(r, &avail)) { res->err = GetLastError(); break; }
        if (!avail) break;
        BYTE buf[16384];
        if (avail > sizeof(buf)) avail = sizeof(buf);
        if (!WinHttpReadData(r, buf, avail, &got)) { res->err = GetLastError(); break; }
        if (!got) break;
        for (DWORD i = 0; i < got; i++) {
            res->sum = res->sum * 31 + buf[i];
            if (res->bytes + i < sizeof(res->head) - 1) res->head[res->bytes + i] = (char)buf[i];
        }
        res->bytes += got;
    }
out:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
}

/* The sum of h2server.js's /big body: byte i is i * 7 % 251 */
static unsigned big_sum(DWORD n)
{
    unsigned s = 0;
    for (DWORD i = 0; i < n; i++) s = s * 31 + (BYTE)(i * 7 % 251);
    return s;
}

static WCHAR *join(const char *base, const char *path)
{
    static WCHAR w[512];
    _snwprintf(w, 512, L"%hs%hs", base, path);
    return w;
}

static int suite(const char *https, const char *http)
{
    Result r;
    fetch(join(https, "/hello"), L"GET", 0, 1, 1, 0, &r);
    check(!r.err && r.status == 200 && r.protocol == WINHTTP_PROTOCOL_FLAG_HTTP2 && !wcscmp(r.server_proto, L"2.0") &&
          strstr(r.head, "Hello over HTTP/2"), "GET over HTTP/2 (ALPN h2)");
    printf("  status %lu, protocol %s, server saw HTTP/%ls, %lu bytes, error %lu\n", r.status,
           r.protocol ? "HTTP/2" : "HTTP/1.1", r.server_proto, r.bytes, r.err);

    fetch(join(https, "/hello"), L"GET", 0, 0, 1, 0, &r);
    check(!r.err && r.status == 200 && r.protocol == 0 && !wcscmp(r.server_proto, L"1.1"),
          "HTTP/1.1 over TLS when HTTP/2 is not enabled");

    fetch(join(https, "/hello"), L"GET", 0, 1, 0, 0, &r);
    check(r.err == ERROR_WINHTTP_SECURE_INVALID_CA, "an untrusted certificate is refused");
    printf("  error %lu\n", r.err);

    fetch(join(https, "/big"), L"GET", 0, 1, 1, 0, &r);
    check(!r.err && r.status == 200 && r.protocol && r.bytes == 300000 && r.sum == big_sum(300000),
          "300000-byte body over HTTP/2 (flow control)");
    printf("  %lu bytes, error %lu\n", r.bytes, r.err);

    fetch(join(https, "/echo"), L"POST", "posted over HTTP/2", 1, 1, 0, &r);
    check(!r.err && r.status == 200 && r.protocol && !strcmp(r.head, "echo: posted over HTTP/2"), "POST over HTTP/2");
    printf("  \"%s\"\n", r.head);

    fetch(join(https, "/redirect"), L"GET", 0, 1, 1, 0, &r);
    check(!r.err && r.status == 200 && strstr(r.head, "Hello over HTTP/2"), "a redirect is followed");

    fetch(join(https, "/missing"), L"GET", 0, 1, 1, 0, &r);
    check(!r.err && r.status == 404, "404 status");

    fetch(join(http, "/chunked"), L"GET", 0, 1, 0, 0, &r);
    check(!r.err && r.status == 200 && r.protocol == 0 && r.bytes == 3 * 1000 &&
          !strncmp(r.head, "chunk 0 ", 8), "chunked HTTP/1.1 body over plain TCP");
    printf("  %lu bytes, error %lu\n", r.bytes, r.err);

    fetch(join(https, "/big"), L"GET", 0, 1, 1, 1, &r);
    check(!r.err && r.status == 200 && r.protocol && r.bytes == 300000 && r.sum == big_sum(300000),
          "asynchronous API (status callbacks) over HTTP/2");
    printf("  %lu bytes, error %lu\n", r.bytes, r.err);

    printf("httptest: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "suite")) return suite(argv[2], argv[3]);
    int http2 = 0, insecure = 0, async = 0, i = 1;
    const char *verb = "GET", *data = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-2")) http2 = 1;
        else if (!strcmp(argv[i], "-k")) insecure = 1;
        else if (!strcmp(argv[i], "-a")) async = 1;
        else if (!strcmp(argv[i], "-X") && i + 1 < argc) verb = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) { data = argv[++i]; if (!strcmp(verb, "GET")) verb = "POST"; }
        else break;
    }
    if (i >= argc) {
        printf("usage: httptest [-2] [-k] [-a] [-X verb] [-d data] <url>\n"
               "       httptest suite <https-base> <http-base>\n");
        return 1;
    }
    WCHAR url[1024], wverb[16];
    MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, url, 1024);
    MultiByteToWideChar(CP_UTF8, 0, verb, -1, wverb, 16);
    Result r;
    fetch(url, wverb, data, http2, insecure, async, &r);
    if (r.err) { printf("error %lu\n", r.err); return 1; }
    printf("%lu, %s, %lu bytes\n%s\n", r.status, r.protocol ? "HTTP/2" : "HTTP/1.1", r.bytes, r.head);
    return 0;
}
