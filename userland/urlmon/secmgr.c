/*
 * urlmon.dll — the Internet security manager (CoInternetCreateSecurityManager):
 * which zone a URL belongs to and what that zone allows.  NovaOS has no
 * zone settings of its own, so the zones are Windows' defaults: files on
 * this computer are the Local Machine zone, everything reached over the
 * network the Internet zone.  In the Internet zone ActiveX controls and
 * unsigned downloads (the URLACTION_* range 0x1000-0x13FF) are refused
 * and other actions allowed; the Restricted Sites zone, which only a
 * mapping would put a URL in, refuses everything.  Zone mappings cannot be
 * changed (there is no zone store to keep them in).
 */
#include <windows.h>
#include <oleauto.h>

#define URLMONAPI __declspec(dllexport)
#define INET_E_DEFAULT_ACTION_ ((HRESULT)0x800C0011L)
#define ZONE_LOCAL_MACHINE 0
#define ZONE_INTERNET      3
#define URLPOLICY_ALLOW_    0
#define URLPOLICY_DISALLOW_ 3

static const GUID IID_IUnknown_ = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_ISecMgr_  = { 0x79eac9ee, 0xbaf9, 0x11ce, { 0x8c, 0x82, 0x00, 0xaa, 0x00, 0x4b, 0xa9, 0x0b } };

typedef struct SecMgr {
    const struct SecMgrVtbl *vt;
    LONG refs;
    IUnknown *site;
} SecMgr;

typedef struct SecMgrVtbl {
    HRESULT (WINAPI *QueryInterface)(SecMgr *, REFIID, void **);
    ULONG (WINAPI *AddRef)(SecMgr *);
    ULONG (WINAPI *Release)(SecMgr *);
    HRESULT (WINAPI *SetSecuritySite)(SecMgr *, IUnknown *);
    HRESULT (WINAPI *GetSecuritySite)(SecMgr *, IUnknown **);
    HRESULT (WINAPI *MapUrlToZone)(SecMgr *, LPCWSTR, DWORD *, DWORD);
    HRESULT (WINAPI *GetSecurityId)(SecMgr *, LPCWSTR, BYTE *, DWORD *, DWORD_PTR);
    HRESULT (WINAPI *ProcessUrlAction)(SecMgr *, LPCWSTR, DWORD, BYTE *, DWORD, BYTE *, DWORD, DWORD, DWORD);
    HRESULT (WINAPI *QueryCustomPolicy)(SecMgr *, LPCWSTR, REFGUID, BYTE **, DWORD *, BYTE *, DWORD, DWORD);
    HRESULT (WINAPI *SetZoneMapping)(SecMgr *, DWORD, LPCWSTR, DWORD);
    HRESULT (WINAPI *GetZoneMappings)(SecMgr *, DWORD, void **, DWORD);
} SecMgrVtbl;

static int lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* the scheme's length (0: none, as for a plain path) */
static int scheme_len(LPCWSTR u)
{
    int i = 0;
    while (u[i] && u[i] != ':' && u[i] != '/' && u[i] != '\\') i++;
    return u[i] == ':' && i > 1 ? i : 0;                    /* ("C:" is a drive, not a scheme) */
}

static int is_scheme(LPCWSTR u, int n, const char *name)
{
    int i = 0;
    for (; i < n && name[i]; i++) if (lower(u[i]) != name[i]) return 0;
    return i == n && !name[i];
}

static DWORD zone_of(LPCWSTR u)
{
    int n = scheme_len(u);
    return !n || is_scheme(u, n, "file") ? ZONE_LOCAL_MACHINE : ZONE_INTERNET;
}

static HRESULT WINAPI sm_qi(SecMgr *s, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown_) || IsEqualGUID(iid, &IID_ISecMgr_)) {
        *out = s;
        InterlockedIncrement(&s->refs);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG WINAPI sm_addref(SecMgr *s) { return (ULONG)InterlockedIncrement(&s->refs); }
static ULONG WINAPI sm_release(SecMgr *s)
{
    LONG r = InterlockedDecrement(&s->refs);
    if (!r) {
        if (s->site) s->site->lpVtbl->Release(s->site);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return (ULONG)r;
}
static HRESULT WINAPI sm_set_site(SecMgr *s, IUnknown *site)
{
    if (site) site->lpVtbl->AddRef(site);
    if (s->site) s->site->lpVtbl->Release(s->site);
    s->site = site;
    return S_OK;
}
static HRESULT WINAPI sm_get_site(SecMgr *s, IUnknown **site)
{
    if (!site) return E_INVALIDARG;
    *site = s->site;
    if (s->site) s->site->lpVtbl->AddRef(s->site);
    return S_OK;
}
static HRESULT WINAPI sm_map(SecMgr *s, LPCWSTR url, DWORD *zone, DWORD flags)
{
    (void)s; (void)flags;
    if (!zone) return E_INVALIDARG;
    if (!url || !*url) { *zone = (DWORD)-1; return E_INVALIDARG; }
    *zone = zone_of(url);
    return S_OK;
}
/* "scheme:host" (lower case; a file has no host) and the zone, 4 bytes */
static HRESULT WINAPI sm_id(SecMgr *s, LPCWSTR url, BYTE *id, DWORD *cb, DWORD_PTR reserved)
{
    (void)s; (void)reserved;
    if (!url || !*url || !cb) return E_INVALIDARG;
    char buf[512];
    int n = 0, k = scheme_len(url);
    const WCHAR *p = url;
    if (!k) { const char f[] = "file:"; for (int i = 0; f[i]; i++) buf[n++] = f[i]; }
    else {
        for (int i = 0; i <= k && n < 256; i++) buf[n++] = (char)lower(url[i]);
        p = url + k + 1;
        if (p[0] == '/' && p[1] == '/' && !is_scheme(url, k, "file")) {
            p += 2;
            const WCHAR *at = p;
            for (const WCHAR *q = p; *q && *q != '/' && *q != '?' && *q != '#'; q++) if (*q == '@') at = q + 1;
            for (p = at; *p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && n < 500; p++)
                buf[n++] = (char)(*p < 0x80 ? lower(*p) : '?');
        }
    }
    DWORD need = (DWORD)n + 4, zone = zone_of(url);
    if (!id || *cb < need) { *cb = need; return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); }
    for (int i = 0; i < n; i++) id[i] = (BYTE)buf[i];
    for (int i = 0; i < 4; i++) id[n + i] = (BYTE)(zone >> (8 * i));
    *cb = need;
    return S_OK;
}
static HRESULT WINAPI sm_action(SecMgr *s, LPCWSTR url, DWORD action, BYTE *policy, DWORD cb, BYTE *ctx, DWORD cctx,
                                DWORD flags, DWORD reserved)
{
    (void)s; (void)ctx; (void)cctx; (void)flags; (void)reserved;
    if (!url || !*url) return E_INVALIDARG;
    DWORD zone = zone_of(url);
    DWORD p = zone == ZONE_INTERNET && action >= 0x1000 && action < 0x1400 ? URLPOLICY_DISALLOW_ : URLPOLICY_ALLOW_;
    if (policy && cb >= sizeof(DWORD)) *(DWORD *)policy = p;
    else if (policy && cb) *policy = (BYTE)p;
    return p == URLPOLICY_ALLOW_ ? S_OK : S_FALSE;
}
static HRESULT WINAPI sm_custom(SecMgr *s, LPCWSTR url, REFGUID key, BYTE **policy, DWORD *cb, BYTE *ctx, DWORD cctx, DWORD r)
{
    (void)s; (void)url; (void)key; (void)ctx; (void)cctx; (void)r;
    if (policy) *policy = 0;
    if (cb) *cb = 0;
    return INET_E_DEFAULT_ACTION_;
}
static HRESULT WINAPI sm_set_map(SecMgr *s, DWORD zone, LPCWSTR pattern, DWORD flags)
{ (void)s; (void)zone; (void)pattern; (void)flags; return E_NOTIMPL; }
static HRESULT WINAPI sm_get_maps(SecMgr *s, DWORD zone, void **e, DWORD flags)
{ (void)s; (void)zone; (void)flags; if (e) *e = 0; return E_NOTIMPL; }

static const SecMgrVtbl sm_vtbl = { sm_qi, sm_addref, sm_release, sm_set_site, sm_get_site, sm_map, sm_id, sm_action,
                                    sm_custom, sm_set_map, sm_get_maps };

URLMONAPI HRESULT WINAPI CoInternetCreateSecurityManager(IUnknown *sp, void **out, DWORD reserved)
{
    (void)sp; (void)reserved;
    if (!out) return E_INVALIDARG;
    SecMgr *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    if (!s) { *out = 0; return E_OUTOFMEMORY; }
    s->vt = &sm_vtbl;
    s->refs = 1;
    *out = s;
    return S_OK;
}
