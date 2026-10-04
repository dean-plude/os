/*
 * msxml6.dll: XMLHTTP and ServerXMLHTTP over WinHTTP.  send() runs the
 * request to completion (an asynchronous request is complete when send
 * returns, and onreadystatechange is called then).
 */
#include "msxml_private.h"
#include <winhttp.h>

typedef struct Http {
    const IServerXMLHTTPRequestVtbl *lpVtbl;
    LONG refs;
    BOOL server;
    LONG state;                   /* readyState: 0 uninitialized, 1 open, 4 complete */
    WCHAR *method, *url, *user, *password;
    WCHAR *headers;               /* "Name: value\r\n"... */
    int timeouts[4];
    LONG status;
    BSTR status_text, resp_headers;
    char *body;
    DWORD nbody;
    IDispatch *onready;
} Http;

static WCHAR *wdup(const WCHAR *s)
{
    if (!s) return 0;
    size_t n = wcslen(s);
    WCHAR *d = mem_alloc((n + 1) * sizeof(WCHAR));
    if (d) memcpy(d, s, (n + 1) * sizeof(WCHAR));
    return d;
}

static void reset_response(Http *h)
{
    SysFreeString(h->status_text);
    SysFreeString(h->resp_headers);
    mem_free(h->body);
    h->status_text = h->resp_headers = 0;
    h->body = 0;
    h->nbody = 0;
    h->status = 0;
}

/* one request, start to finish */
static HRESULT run(const WCHAR *method, const WCHAR *url, const WCHAR *headers, const void *data, DWORD ndata,
                   const int *timeouts, const WCHAR *user, const WCHAR *password,
                   LONG *status, BSTR *status_text, BSTR *resp_headers, char **body, DWORD *nbody)
{
    URL_COMPONENTS uc = { sizeof uc };
    WCHAR host[256], path[2048], extra[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return E_INVALIDARG;
    size_t pl = wcslen(path);
    for (size_t i = 0; extra[i] && pl < 2047; i++) path[pl++] = extra[i];
    path[pl] = 0;
    HRESULT hr = S_OK;
    HINTERNET s = WinHttpOpen(L"Mozilla/4.0 (compatible; Win32; WinHttp.WinHttpRequest.5)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : 0;
    HINTERNET r = c ? WinHttpOpenRequest(c, method, path[0] ? path : L"/", 0, 0, 0,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : 0;
    if (!r) { hr = HRESULT_FROM_WIN32(GetLastError()); goto out; }
    if (timeouts) WinHttpSetTimeouts(r, timeouts[0], timeouts[1], timeouts[2], timeouts[3]);
    if (user) WinHttpSetCredentials(r, WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC, user, password, 0);
    if (!WinHttpSendRequest(r, headers && *headers ? headers : 0, headers && *headers ? (DWORD)-1 : 0,
                            (void *)data, ndata, ndata, 0) ||
        !WinHttpReceiveResponse(r, 0)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto out;
    }
    DWORD code = 0, sz = sizeof code;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &code, &sz, 0);
    if (status) *status = (LONG)code;
    WCHAR text[256];
    sz = sizeof text;
    if (status_text) *status_text = WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_TEXT, 0, text, &sz, 0) ? SysAllocString(text) : SysAllocString(L"");
    if (resp_headers) {
        sz = 0;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, 0, 0, &sz, 0);
        WCHAR *all = sz ? mem_alloc(sz + 2) : 0;
        if (all && WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, 0, all, &sz, 0)) {
            WCHAR *p = wcsstr(all, L"\r\n");             /* the status line is not a header */
            *resp_headers = SysAllocString(p ? p + 2 : all);
        } else
            *resp_headers = SysAllocString(L"");
        mem_free(all);
    }
    DWORD cap = 65536, n = 0;
    char *buf = mem_alloc(cap + 1);
    for (;;) {
        if (!buf) { hr = E_OUTOFMEMORY; break; }
        if (cap - n < 16384) {
            char *b = mem_realloc(buf, (SIZE_T)cap * 2 + 1);
            if (!b) { mem_free(buf); buf = 0; hr = E_OUTOFMEMORY; break; }
            buf = b;
            cap *= 2;
        }
        DWORD got = 0;
        if (!WinHttpReadData(r, buf + n, cap - n, &got) || !got) break;
        n += got;
    }
    if (buf) { *body = buf; *nbody = n; }
out:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return hr;
}

HRESULT http_get(const WCHAR *url, char **data, DWORD *len)
{
    LONG status = 0;
    HRESULT hr = run(L"GET", url, 0, 0, 0, 0, 0, 0, &status, 0, 0, data, len);
    if (SUCCEEDED(hr) && (status < 200 || status >= 300)) { mem_free(*data); *data = 0; hr = INET_E_RESOURCE_NOT_FOUND; }
    return hr;
}

/* ---- IXMLHTTPRequest / IServerXMLHTTPRequest ---- */
static HRESULT STDMETHODCALLTYPE hr_qi(Http *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IXMLHTTPRequest) ||
        (This->server && IsEqualIID(riid, &IID_IServerXMLHTTPRequest))) {
        *out = This;
        InterlockedIncrement(&This->refs);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE hr_addref(Http *This) { return (ULONG)InterlockedIncrement(&This->refs); }
static ULONG STDMETHODCALLTYPE hr_release(Http *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (!r) {
        reset_response(This);
        mem_free(This->method); mem_free(This->url); mem_free(This->user); mem_free(This->password);
        mem_free(This->headers);
        if (This->onready) This->onready->lpVtbl->Release(This->onready);
        mem_free(This);
        InterlockedDecrement(&g_objects);
    }
    return (ULONG)r;
}

static WCHAR *opt_string(VARIANT v)
{
    if (v.vt == (VT_BYREF | VT_VARIANT)) v = *v.pvarVal;
    return v.vt == VT_BSTR && v.bstrVal ? wdup(v.bstrVal) : 0;
}

static HRESULT STDMETHODCALLTYPE hr_open(Http *This, BSTR method, BSTR url, VARIANT async, VARIANT user, VARIANT password)
{
    (void)async;
    if (!method || !url) return E_INVALIDARG;
    reset_response(This);
    mem_free(This->method); mem_free(This->url); mem_free(This->user); mem_free(This->password);
    mem_free(This->headers);
    This->headers = 0;
    This->method = wdup(method);
    This->url = wdup(url);
    This->user = opt_string(user);
    This->password = opt_string(password);
    This->state = 1;
    return This->method && This->url ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE hr_setRequestHeader(Http *This, BSTR name, BSTR value)
{
    if (!name || !*name) return E_INVALIDARG;
    if (This->state != 1) return E_FAIL;
    size_t old = This->headers ? wcslen(This->headers) : 0, add = wcslen(name) + 2 + (value ? wcslen(value) : 0) + 2;
    WCHAR *h = mem_realloc(This->headers, (old + add + 1) * sizeof(WCHAR));
    if (!h) return E_OUTOFMEMORY;
    h[old] = 0;
    wcscat(h, name);
    wcscat(h, L": ");
    if (value) wcscat(h, value);
    wcscat(h, L"\r\n");
    This->headers = h;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_getResponseHeader(Http *This, BSTR name, BSTR *out)
{
    if (!out || !name) return E_INVALIDARG;
    *out = 0;
    if (This->state != 4 || !This->resp_headers) return E_FAIL;
    size_t nl = wcslen(name);
    for (const WCHAR *p = This->resp_headers; *p; ) {
        const WCHAR *e = wcsstr(p, L"\r\n");
        size_t len = e ? (size_t)(e - p) : wcslen(p);
        if (len > nl && p[nl] == ':' && !_wcsnicmp(p, name, nl)) {
            const WCHAR *v = p + nl + 1;
            while (*v == ' ') v++;
            *out = SysAllocStringLen(v, (UINT)(p + len - v));
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        if (!e) break;
        p = e + 2;
    }
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE hr_getAllResponseHeaders(Http *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    if (This->state != 4) { *out = 0; return E_FAIL; }
    *out = SysAllocString(This->resp_headers ? This->resp_headers : L"");
    return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE hr_send(Http *This, VARIANT body)
{
    if (This->state != 1) return E_FAIL;
    const VARIANT *b = body.vt == (VT_BYREF | VT_VARIANT) ? body.pvarVal : &body;
    char *data = 0;
    DWORD n = 0;
    xmlChar *u = 0;
    switch (b->vt) {
    case VT_EMPTY: case VT_NULL: case VT_ERROR: break;
    case VT_BSTR:
        u = utf8_wide(b->bstrVal, (int)SysStringLen(b->bstrVal));
        if (!u) return E_OUTOFMEMORY;
        data = (char *)u;
        n = (DWORD)xmlStrlen(u);
        break;
    case VT_ARRAY | VT_UI1: {
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(b->parray, 1, &lo);
        SafeArrayGetUBound(b->parray, 1, &hi);
        n = (DWORD)(hi - lo + 1);
        void *p;
        if (FAILED(SafeArrayAccessData(b->parray, &p))) return E_INVALIDARG;
        u = xmlMalloc(n + 1);
        if (u) memcpy(u, p, n);
        SafeArrayUnaccessData(b->parray);
        if (!u) return E_OUTOFMEMORY;
        data = (char *)u;
        break;
    }
    case VT_UNKNOWN: case VT_DISPATCH: {
        Node *doc = node_from_iface(b->punkVal);
        if (!doc) return E_INVALIDARG;
        HRESULT hr = node_xml(doc->doc, node_x(doc), TRUE, &u);
        if (FAILED(hr)) return hr;
        data = (char *)u;
        n = (DWORD)xmlStrlen(u);
        break;
    }
    default:
        return E_INVALIDARG;
    }
    reset_response(This);
    HRESULT hr = run(This->method, This->url, This->headers, data, n, This->timeouts[0] ? This->timeouts : 0,
                     This->user, This->password, &This->status, &This->status_text, &This->resp_headers,
                     &This->body, &This->nbody);
    xmlFree(u);
    if (FAILED(hr)) return hr;
    This->state = 4;
    if (This->onready) {
        DISPPARAMS none = { 0 };
        This->onready->lpVtbl->Invoke(This->onready, 0, &IID_NULL, 0, DISPATCH_METHOD, &none, 0, 0, 0);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_abort(Http *This) { This->state = 0; reset_response(This); return S_OK; }

static HRESULT STDMETHODCALLTYPE hr_get_status(Http *This, LONG *out)
{
    if (!out) return E_INVALIDARG;
    if (This->state != 4) return E_FAIL;
    *out = This->status;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_get_statusText(Http *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    if (This->state != 4) return E_FAIL;
    *out = SysAllocString(This->status_text ? This->status_text : L"");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_get_responseText(Http *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (This->state != 4) return E_FAIL;
    const BYTE *p = (const BYTE *)This->body;
    DWORD n = This->nbody;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {        /* UTF-16 with a mark */
        *out = SysAllocStringLen((const WCHAR *)(p + 2), (n - 2) / 2);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }
    *out = bstr_utf8n(p ? p : (const BYTE *)"", (int)n);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE hr_get_responseXML(Http *This, IDispatch **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (This->state != 4) return E_FAIL;
    IXMLDOMDocument *doc;
    HRESULT hr = doc_create(6, FALSE, &IID_IXMLDOMDocument, (void **)&doc);
    if (FAILED(hr)) return hr;
    doc_load_bytes(((Node *)doc)->doc, This->body ? This->body : "", (int)This->nbody, FALSE);  /* (not XML: empty) */
    *out = (IDispatch *)doc;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_get_responseBody(Http *This, VARIANT *out)
{
    if (!out) return E_INVALIDARG;
    VariantInit(out);
    if (This->state != 4) return E_FAIL;
    SAFEARRAY *a = SafeArrayCreateVector(VT_UI1, 0, This->nbody);
    if (!a) return E_OUTOFMEMORY;
    void *p;
    SafeArrayAccessData(a, &p);
    if (This->nbody) memcpy(p, This->body, This->nbody);
    SafeArrayUnaccessData(a);
    out->vt = VT_ARRAY | VT_UI1;
    out->parray = a;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_get_responseStream(Http *This, VARIANT *out)
{
    if (!out) return E_INVALIDARG;
    VariantInit(out);
    if (This->state != 4) return E_FAIL;
    IStream *s;
    HRESULT hr = CreateStreamOnHGlobal(0, TRUE, &s);
    if (FAILED(hr)) return hr;
    ULONG put;
    s->lpVtbl->Write(s, This->body ? This->body : "", This->nbody, &put);
    LARGE_INTEGER zero = { 0 };
    s->lpVtbl->Seek(s, zero, STREAM_SEEK_SET, 0);
    out->vt = VT_UNKNOWN;
    out->punkVal = (IUnknown *)s;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_get_readyState(Http *This, LONG *out)
{
    if (!out) return E_INVALIDARG;
    *out = This->state;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_put_onreadystatechange(Http *This, IDispatch *d)
{
    if (This->onready) This->onready->lpVtbl->Release(This->onready);
    This->onready = d;
    if (d) d->lpVtbl->AddRef(d);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_setTimeouts(Http *This, LONG resolve, LONG connect, LONG send, LONG receive)
{
    This->timeouts[0] = (int)resolve;
    This->timeouts[1] = (int)connect;
    This->timeouts[2] = (int)send;
    This->timeouts[3] = (int)receive;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_waitForResponse(Http *This, VARIANT timeout, VARIANT_BOOL *done)
{
    (void)timeout;
    if (done) *done = This->state == 4 ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_getOption(Http *This, int option, VARIANT *v)
{
    (void)This; (void)option;
    if (!v) return E_INVALIDARG;
    VariantInit(v);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hr_setOption(Http *This, int option, VARIANT v) { (void)This; (void)option; (void)v; return S_OK; }

static const IServerXMLHTTPRequestVtbl http_vtbl = {
    .QueryInterface = (void *)hr_qi, .AddRef = (void *)hr_addref, .Release = (void *)hr_release, DISPATCH_SLOTS,
    .open = (void *)hr_open, .setRequestHeader = (void *)hr_setRequestHeader,
    .getResponseHeader = (void *)hr_getResponseHeader, .getAllResponseHeaders = (void *)hr_getAllResponseHeaders,
    .send = (void *)hr_send, .abort = (void *)hr_abort, .get_status = (void *)hr_get_status,
    .get_statusText = (void *)hr_get_statusText, .get_responseXML = (void *)hr_get_responseXML,
    .get_responseText = (void *)hr_get_responseText, .get_responseBody = (void *)hr_get_responseBody,
    .get_responseStream = (void *)hr_get_responseStream, .get_readyState = (void *)hr_get_readyState,
    .put_onreadystatechange = (void *)hr_put_onreadystatechange,
    .setTimeouts = (void *)hr_setTimeouts, .waitForResponse = (void *)hr_waitForResponse,
    .getOption = (void *)hr_getOption, .setOption = (void *)hr_setOption,
};

HRESULT httpreq_create(BOOL server, REFIID riid, void **out)
{
    *out = 0;
    Http *h = mem_alloc(sizeof *h);
    if (!h) return E_OUTOFMEMORY;
    h->lpVtbl = &http_vtbl;
    h->refs = 1;
    h->server = server;
    InterlockedIncrement(&g_objects);
    HRESULT hr = hr_qi(h, riid, out);
    hr_release(h);
    return hr;
}
