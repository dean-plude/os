/*
 * urlmon.dll — URL parsing for programs: CreateUri (IUri) and
 * CoInternetParseUrl.  No MIT/BSD urlmon exists (Wine's is LGPL), so this
 * is NovaOS's own: a URI is split once into scheme, user info, host, port,
 * path, query and fragment, and every property is read from those parts.
 * The download and moniker side of urlmon is not here yet.
 */
#include <windows.h>
#include <oleauto.h>
#include <wchar.h>
#include <string.h>

#define URLMONAPI __declspec(dllexport)
#define URL_UNESCAPE_ 0x10000000

/* shlwapi.dll's URL helpers */
HRESULT WINAPI UrlCreateFromPathW(LPCWSTR path, LPWSTR url, LPDWORD n, DWORD reserved);
HRESULT WINAPI PathCreateFromUrlW(LPCWSTR url, LPWSTR path, LPDWORD n, DWORD flags);
HRESULT WINAPI UrlCanonicalizeW(LPCWSTR in, LPWSTR out, LPDWORD n, DWORD flags);
#define INET_E_DEFAULT_ACTION_ ((HRESULT)0x800C0011L)

static const GUID IID_IUnknown_ = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IUri_     = { 0xa39ee748, 0x6a27, 0x4817, { 0xa6, 0xf2, 0x13, 0x91, 0x4b, 0xef, 0x58, 0x90 } };

/* (ASCII is all a scheme or an address uses) */
static int is_alpha(WCHAR c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
static int is_digit(WCHAR c) { return c >= L'0' && c <= L'9'; }

enum {  /* Uri_PROPERTY */
    P_ABSOLUTE_URI, P_AUTHORITY, P_DISPLAY_URI, P_DOMAIN, P_EXTENSION, P_FRAGMENT, P_HOST, P_PASSWORD, P_PATH,
    P_PATH_AND_QUERY, P_QUERY, P_RAW_URI, P_SCHEME_NAME, P_USER_INFO, P_USER_NAME,
    P_HOST_TYPE, P_PORT, P_SCHEME, P_ZONE
};
enum { H_UNKNOWN, H_DNS, H_IPV4, H_IPV6 };          /* Uri_HOST_TYPE */

/* URL_SCHEME numbers for the names programs meet */
static const struct { const WCHAR *name; DWORD id; WORD port; } g_schemes[] = {
    { L"ftp", 1, 21 }, { L"http", 2, 80 }, { L"gopher", 3, 70 }, { L"mailto", 4, 0 }, { L"news", 5, 0 },
    { L"nntp", 6, 119 }, { L"telnet", 7, 23 }, { L"wais", 8, 0 }, { L"file", 9, 0 }, { L"mk", 10, 0 },
    { L"https", 11, 443 }, { L"shell", 12, 0 }, { L"snews", 13, 0 }, { L"local", 14, 0 }, { L"javascript", 15, 0 },
    { L"vbscript", 16, 0 }, { L"about", 17, 0 }, { L"res", 18, 0 },
};

/* a URI's parts: [start, end) into raw for each */
typedef struct { int s, e; } Span;
typedef struct {
    WCHAR *raw;
    Span scheme, user, pass, host, port, path, query, frag;
    BOOL authority;                                 /* had "//" */
    DWORD scheme_id, port_num, host_type;
} Parts;

static BOOL parse(const WCHAR *uri, Parts *p)
{
    memset(p, 0, sizeof(*p));
    size_t n = wcslen(uri);
    p->raw = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR));
    if (!p->raw) return FALSE;
    memcpy(p->raw, uri, (n + 1) * sizeof(WCHAR));
    const WCHAR *r = p->raw;
    int i = 0;
    while ((is_alpha(r[i]) || is_digit(r[i])) || r[i] == L'+' || r[i] == L'-' || r[i] == L'.') i++;
    if (i == 0 || r[i] != L':' || !is_alpha(r[0])) return FALSE;     /* (a relative URI) */
    p->scheme = (Span){ 0, i };
    i++;
    if (r[i] == L'/' && r[i + 1] == L'/') {
        p->authority = TRUE;
        i += 2;
        int a = i, end = i;
        while (r[end] && r[end] != L'/' && r[end] != L'?' && r[end] != L'#') end++;
        int at = -1;
        for (int k = a; k < end; k++) if (r[k] == L'@') at = k;
        if (at >= 0) {
            int colon = -1;
            for (int k = a; k < at; k++) if (r[k] == L':') { colon = k; break; }
            p->user = (Span){ a, colon >= 0 ? colon : at };
            if (colon >= 0) p->pass = (Span){ colon + 1, at };
            a = at + 1;
        }
        int hend = end;
        if (r[a] == L'[') {                         /* [IPv6] */
            int k = a;
            while (k < end && r[k] != L']') k++;
            p->host = (Span){ a, k < end ? k + 1 : end };
            p->host_type = H_IPV6;
            hend = p->host.e;
        } else {
            for (int k = a; k < end; k++) if (r[k] == L':') { hend = k; break; }
            p->host = (Span){ a, hend };
        }
        if (hend < end && r[hend] == L':') p->port = (Span){ hend + 1, end };
        i = end;
    }
    int ps = i;
    while (r[i] && r[i] != L'?' && r[i] != L'#') i++;
    p->path = (Span){ ps, i };
    if (r[i] == L'?') {
        int qs = i;
        while (r[i] && r[i] != L'#') i++;
        p->query = (Span){ qs, i };
    }
    if (r[i] == L'#') p->frag = (Span){ i, (int)n };

    for (size_t k = 0; k < sizeof(g_schemes) / sizeof(g_schemes[0]); k++)
        if ((int)wcslen(g_schemes[k].name) == p->scheme.e && !_wcsnicmp(r, g_schemes[k].name, p->scheme.e)) {
            p->scheme_id = g_schemes[k].id;
            p->port_num = g_schemes[k].port;
        }
    if (p->port.e > p->port.s) p->port_num = (DWORD)wcstoul(r + p->port.s, NULL, 10);
    if (p->host.e > p->host.s && p->host_type != H_IPV6) {
        int digits = 1;
        for (int k = p->host.s; k < p->host.e; k++) if (!is_digit(r[k]) && r[k] != L'.') digits = 0;
        p->host_type = digits ? H_IPV4 : H_DNS;
    }
    return TRUE;
}

/* ---- IUri ---- */
typedef struct Uri Uri;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(Uri *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(Uri *);
    ULONG   (WINAPI *Release)(Uri *);
    HRESULT (WINAPI *GetPropertyBSTR)(Uri *, DWORD, BSTR *, DWORD);
    HRESULT (WINAPI *GetPropertyLength)(Uri *, DWORD, DWORD *, DWORD);
    HRESULT (WINAPI *GetPropertyDWORD)(Uri *, DWORD, DWORD *, DWORD);
    HRESULT (WINAPI *HasProperty)(Uri *, DWORD, BOOL *);
    HRESULT (WINAPI *GetAbsoluteUri)(Uri *, BSTR *);
    HRESULT (WINAPI *GetAuthority)(Uri *, BSTR *);
    HRESULT (WINAPI *GetDisplayUri)(Uri *, BSTR *);
    HRESULT (WINAPI *GetDomain)(Uri *, BSTR *);
    HRESULT (WINAPI *GetExtension)(Uri *, BSTR *);
    HRESULT (WINAPI *GetFragment)(Uri *, BSTR *);
    HRESULT (WINAPI *GetHost)(Uri *, BSTR *);
    HRESULT (WINAPI *GetPassword)(Uri *, BSTR *);
    HRESULT (WINAPI *GetPath)(Uri *, BSTR *);
    HRESULT (WINAPI *GetPathAndQuery)(Uri *, BSTR *);
    HRESULT (WINAPI *GetQuery)(Uri *, BSTR *);
    HRESULT (WINAPI *GetRawUri)(Uri *, BSTR *);
    HRESULT (WINAPI *GetSchemeName)(Uri *, BSTR *);
    HRESULT (WINAPI *GetUserInfo)(Uri *, BSTR *);
    HRESULT (WINAPI *GetUserName)(Uri *, BSTR *);
    HRESULT (WINAPI *GetHostType)(Uri *, DWORD *);
    HRESULT (WINAPI *GetPort)(Uri *, DWORD *);
    HRESULT (WINAPI *GetScheme)(Uri *, DWORD *);
    HRESULT (WINAPI *GetZone)(Uri *, DWORD *);
    HRESULT (WINAPI *GetProperties)(Uri *, LPDWORD);
    HRESULT (WINAPI *IsEqual)(Uri *, Uri *, BOOL *);
} UriVtbl;
struct Uri { const UriVtbl *vtbl; LONG refs; Parts p; };

static BSTR span_bstr(const Parts *p, Span s) { return SysAllocStringLen(p->raw + s.s, (UINT)(s.e - s.s)); }

/* a string property as [start, end) of raw, or FALSE when it is computed */
static BOOL prop_span(const Parts *p, DWORD prop, Span *out)
{
    switch (prop) {
    case P_RAW_URI: case P_ABSOLUTE_URI: case P_DISPLAY_URI: *out = (Span){ 0, (int)wcslen(p->raw) }; return TRUE;
    case P_SCHEME_NAME: *out = p->scheme; return TRUE;
    case P_USER_NAME: *out = p->user; return TRUE;
    case P_PASSWORD: *out = p->pass; return TRUE;
    case P_USER_INFO: *out = p->pass.e ? (Span){ p->user.s, p->pass.e } : p->user; return TRUE;
    case P_HOST: case P_DOMAIN: *out = p->host; return TRUE;
    case P_AUTHORITY:
        *out = p->authority ? (Span){ p->user.e ? p->user.s : p->host.s, p->port.e ? p->port.e : p->host.e } : (Span){ 0, 0 };
        return TRUE;
    case P_PATH: *out = p->path; return TRUE;
    case P_QUERY: *out = p->query; return TRUE;
    case P_FRAGMENT: *out = p->frag; return TRUE;
    case P_PATH_AND_QUERY: *out = (Span){ p->path.s, p->query.e ? p->query.e : p->path.e }; return TRUE;
    case P_EXTENSION: {
        int dot = -1;
        for (int k = p->path.s; k < p->path.e; k++) {
            if (p->raw[k] == L'.') dot = k;
            if (p->raw[k] == L'/') dot = -1;
        }
        *out = dot >= 0 ? (Span){ dot, p->path.e } : (Span){ 0, 0 };
        return TRUE;
    }
    }
    return FALSE;
}

static HRESULT WINAPI u_qi(Uri *u, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (!memcmp(iid, &IID_IUnknown_, sizeof(GUID)) || !memcmp(iid, &IID_IUri_, sizeof(GUID))) {
        InterlockedIncrement(&u->refs);
        *out = u;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI u_addref(Uri *u) { return InterlockedIncrement(&u->refs); }
static ULONG WINAPI u_release(Uri *u)
{
    LONG n = InterlockedDecrement(&u->refs);
    if (!n) {
        HeapFree(GetProcessHeap(), 0, u->p.raw);
        HeapFree(GetProcessHeap(), 0, u);
    }
    return n;
}
static HRESULT WINAPI u_bstr(Uri *u, DWORD prop, BSTR *out, DWORD flags)
{
    (void)flags;
    if (!out) return E_POINTER;
    Span s;
    if (!prop_span(&u->p, prop, &s)) { *out = NULL; return E_INVALIDARG; }
    *out = span_bstr(&u->p, s);
    if (!*out) return E_OUTOFMEMORY;
    return s.e > s.s ? S_OK : S_FALSE;
}
static HRESULT WINAPI u_length(Uri *u, DWORD prop, DWORD *len, DWORD flags)
{
    (void)flags;
    if (!len) return E_POINTER;
    Span s;
    if (!prop_span(&u->p, prop, &s)) { *len = 0; return E_INVALIDARG; }
    *len = (DWORD)(s.e - s.s);
    return *len ? S_OK : S_FALSE;
}
static HRESULT WINAPI u_dword(Uri *u, DWORD prop, DWORD *v, DWORD flags)
{
    (void)flags;
    if (!v) return E_POINTER;
    switch (prop) {
    case P_HOST_TYPE: *v = u->p.host_type; return S_OK;
    case P_PORT: *v = u->p.port_num; return u->p.port_num ? S_OK : S_FALSE;
    case P_SCHEME: *v = u->p.scheme_id; return S_OK;
    case P_ZONE: *v = u->p.scheme_id == 9 ? 0 : 3; return S_OK;   /* URLZONE_LOCAL_MACHINE / INTERNET */
    }
    *v = 0;
    return E_INVALIDARG;
}
static HRESULT WINAPI u_has(Uri *u, DWORD prop, BOOL *has)
{
    if (!has) return E_POINTER;
    Span s;
    if (prop_span(&u->p, prop, &s)) *has = s.e > s.s;
    else *has = prop == P_HOST_TYPE || prop == P_SCHEME || prop == P_ZONE || (prop == P_PORT && u->p.port_num);
    return S_OK;
}
#define GETTER(fn, prop) static HRESULT WINAPI fn(Uri *u, BSTR *out) { return u_bstr(u, prop, out, 0); }
GETTER(u_absolute, P_ABSOLUTE_URI) GETTER(u_authority, P_AUTHORITY) GETTER(u_display, P_DISPLAY_URI)
GETTER(u_domain, P_DOMAIN) GETTER(u_extension, P_EXTENSION) GETTER(u_fragment, P_FRAGMENT) GETTER(u_host, P_HOST)
GETTER(u_password, P_PASSWORD) GETTER(u_path, P_PATH) GETTER(u_pathquery, P_PATH_AND_QUERY) GETTER(u_query, P_QUERY)
GETTER(u_rawuri, P_RAW_URI) GETTER(u_schemename, P_SCHEME_NAME) GETTER(u_userinfo, P_USER_INFO)
GETTER(u_username, P_USER_NAME)
static HRESULT WINAPI u_hosttype(Uri *u, DWORD *v) { return u_dword(u, P_HOST_TYPE, v, 0); }
static HRESULT WINAPI u_port(Uri *u, DWORD *v) { return u_dword(u, P_PORT, v, 0); }
static HRESULT WINAPI u_scheme(Uri *u, DWORD *v) { return u_dword(u, P_SCHEME, v, 0); }
static HRESULT WINAPI u_zone(Uri *u, DWORD *v) { return u_dword(u, P_ZONE, v, 0); }
static HRESULT WINAPI u_props(Uri *u, LPDWORD flags)
{
    if (!flags) return E_POINTER;
    DWORD f = 0;
    for (DWORD k = 0; k <= P_ZONE; k++) {
        BOOL has;
        u_has(u, k, &has);
        if (has) f |= 1u << k;
    }
    *flags = f;
    return S_OK;
}
static HRESULT WINAPI u_equal(Uri *u, Uri *other, BOOL *eq)
{
    if (!eq) return E_POINTER;
    *eq = other && !_wcsicmp(u->p.raw, other->p.raw);
    return S_OK;
}
static const UriVtbl g_uri = {
    u_qi, u_addref, u_release, u_bstr, u_length, u_dword, u_has, u_absolute, u_authority, u_display, u_domain,
    u_extension, u_fragment, u_host, u_password, u_path, u_pathquery, u_query, u_rawuri, u_schemename, u_userinfo,
    u_username, u_hosttype, u_port, u_scheme, u_zone, u_props, u_equal,
};

URLMONAPI HRESULT WINAPI CreateUri(LPCWSTR uri, DWORD flags, DWORD_PTR reserved, void **out)
{
    (void)flags; (void)reserved;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!uri) return E_INVALIDARG;
    Uri *u = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*u));
    if (!u) return E_OUTOFMEMORY;
    if (!parse(uri, &u->p)) {
        HeapFree(GetProcessHeap(), 0, u->p.raw);
        HeapFree(GetProcessHeap(), 0, u);
        return E_INVALIDARG;
    }
    u->vtbl = &g_uri;
    u->refs = 1;
    *out = u;
    return S_OK;
}
URLMONAPI HRESULT WINAPI CreateUriWithFragment(LPCWSTR uri, LPCWSTR frag, DWORD flags, DWORD_PTR reserved, void **out)
{
    if (!frag || !*frag) return CreateUri(uri, flags, reserved, out);
    size_t a = uri ? wcslen(uri) : 0, b = wcslen(frag);
    WCHAR *s = HeapAlloc(GetProcessHeap(), 0, (a + b + 2) * sizeof(WCHAR));
    if (!s) return E_OUTOFMEMORY;
    memcpy(s, uri, a * sizeof(WCHAR));
    size_t k = a;
    if (frag[0] != L'#') s[k++] = L'#';
    memcpy(s + k, frag, (b + 1) * sizeof(WCHAR));
    HRESULT hr = CreateUri(s, flags, reserved, out);
    HeapFree(GetProcessHeap(), 0, s);
    return hr;
}

/* PARSE_ACTION numbers */
enum { PA_CANONICALIZE = 1, PA_FRIENDLY, PA_SECURITY_URL, PA_ROOTDOCUMENT, PA_DOCUMENT, PA_ANCHOR, PA_ENCODE, PA_DECODE,
       PA_PATH_FROM_URL, PA_URL_FROM_PATH, PA_MIME, PA_SERVER, PA_SCHEMA, PA_SITE, PA_DOMAIN, PA_LOCATION,
       PA_SECURITY_DOMAIN, PA_ESCAPE, PA_UNESCAPE };

static HRESULT give(const WCHAR *s, int n, LPWSTR out, DWORD cch, DWORD *need)
{
    if (need) *need = (DWORD)n + 1;
    if (!out || cch < (DWORD)n + 1) return E_POINTER;   /* (Windows: the buffer is too small) */
    memcpy(out, s, n * sizeof(WCHAR));
    out[n] = 0;
    return S_OK;
}

URLMONAPI HRESULT WINAPI CoInternetParseUrl(LPCWSTR url, DWORD action, DWORD flags, LPWSTR out, DWORD cch, DWORD *need,
                                            DWORD reserved)
{
    (void)reserved;
    if (!url) return E_INVALIDARG;
    if (action == PA_URL_FROM_PATH || action == PA_PATH_FROM_URL) {
        DWORD n = cch;
        HRESULT hr = action == PA_URL_FROM_PATH ? UrlCreateFromPathW(url, out, &n, 0) : PathCreateFromUrlW(url, out, &n, 0);
        if (need) *need = n + (SUCCEEDED(hr) ? 1 : 0);
        return hr;
    }
    if (action == PA_CANONICALIZE || action == PA_ESCAPE || action == PA_UNESCAPE || action == PA_ENCODE || action == PA_DECODE) {
        DWORD n = cch;
        DWORD f = action == PA_UNESCAPE || action == PA_DECODE ? URL_UNESCAPE_ : flags;
        HRESULT hr = UrlCanonicalizeW(url, out, &n, f);
        if (need) *need = n + 1;
        return hr;
    }
    Parts p;
    if (!parse(url, &p)) { HeapFree(GetProcessHeap(), 0, p.raw); return INET_E_DEFAULT_ACTION_; }
    HRESULT hr;
    switch (action) {
    case PA_SCHEMA: hr = give(p.raw + p.scheme.s, p.scheme.e - p.scheme.s, out, cch, need); break;
    case PA_DOMAIN: case PA_SERVER: case PA_SITE:
        hr = give(p.raw + p.host.s, p.host.e - p.host.s, out, cch, need); break;
    case PA_ANCHOR: hr = give(p.raw + p.frag.s, p.frag.e - p.frag.s, out, cch, need); break;
    case PA_DOCUMENT: hr = give(p.raw, p.frag.e ? p.frag.s : (int)wcslen(p.raw), out, cch, need); break;
    case PA_ROOTDOCUMENT: case PA_SECURITY_URL: case PA_SECURITY_DOMAIN:
        hr = give(p.raw, p.path.s, out, cch, need); break;
    case PA_LOCATION: hr = give(p.raw + p.frag.s, p.frag.e - p.frag.s, out, cch, need); break;
    default: hr = INET_E_DEFAULT_ACTION_;
    }
    HeapFree(GetProcessHeap(), 0, p.raw);
    return hr;
}
