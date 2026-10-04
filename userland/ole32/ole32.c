/*
 * ole32.dll — the COM runtime.
 *
 * Every COM object in NovaOS lives in-process: CoCreateInstance finds the
 * class among the objects registered with CoRegisterClassObject, or in the
 * registry (HKCR\CLSID\{clsid}\InprocServer32), loads that DLL and asks its
 * DllGetClassObject for the class factory.  There is no marshaling and no
 * cross-apartment proxying: apartments are only tracked (so CoInitializeEx
 * reports RPC_E_CHANGED_MODE the way Windows does), and every interface
 * pointer can be used from any thread, as if all objects were free-threaded.
 */

#define NOVA_BUILD_OLE32
#include <objbase.h>

/* ---------------------------------------------------------------------------
 * Apartments
 * ------------------------------------------------------------------------- */
static __declspec(thread) LONG t_inits;       /* CoInitialize calls not yet undone */
static __declspec(thread) DWORD t_model;      /* COINIT_* of the first call */
static __declspec(thread) LONG t_ole_inits;
static volatile LONG g_mta_usage;             /* CoIncrementMTAUsage */

WINOLEAPI_(HRESULT) CoInitializeEx(LPVOID reserved, DWORD coinit)
{
    (void)reserved;
    DWORD model = coinit & COINIT_APARTMENTTHREADED;
    if (t_inits > 0) {
        if (model != t_model) return RPC_E_CHANGED_MODE;
        t_inits++;
        return S_FALSE;
    }
    t_model = model;
    t_inits = 1;
    return S_OK;
}

WINOLEAPI_(HRESULT) CoInitialize(LPVOID reserved) { return CoInitializeEx(reserved, COINIT_APARTMENTTHREADED); }

WINOLEAPI_(void) CoUninitialize(void)
{
    if (t_inits > 0 && --t_inits == 0) CoFreeUnusedLibraries();
}

WINOLEAPI_(HRESULT) CoGetApartmentType(APTTYPE *type, APTTYPEQUALIFIER *q)
{
    if (!type || !q) return E_INVALIDARG;
    *q = APTTYPEQUALIFIER_NONE;
    if (t_inits > 0) { *type = t_model == COINIT_APARTMENTTHREADED ? APTTYPE_STA : APTTYPE_MTA; return S_OK; }
    if (g_mta_usage > 0) { *type = APTTYPE_MTA; return S_OK; }   /* the implicit MTA */
    *type = APTTYPE_CURRENT;
    return CO_E_NOTINITIALIZED;
}

WINOLEAPI_(HRESULT) CoIncrementMTAUsage(CO_MTA_USAGE_COOKIE *cookie)
{
    if (!cookie) return E_INVALIDARG;
    InterlockedIncrement(&g_mta_usage);
    *cookie = (CO_MTA_USAGE_COOKIE)(ULONG_PTR)1;
    return S_OK;
}

WINOLEAPI_(HRESULT) CoDecrementMTAUsage(CO_MTA_USAGE_COOKIE cookie)
{
    (void)cookie;
    InterlockedDecrement(&g_mta_usage);
    return S_OK;
}

WINOLEAPI_(HRESULT) OleInitialize(LPVOID reserved)
{
    HRESULT hr = CoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return hr;
    t_ole_inits++;
    return hr;
}

WINOLEAPI_(void) OleUninitialize(void)
{
    if (t_ole_inits > 0) t_ole_inits--;
    CoUninitialize();
}

/* security has nothing to configure: every caller is the same user */
WINOLEAPI_(HRESULT) CoInitializeSecurity(PVOID sd, LONG n, PVOID auth, void *res, DWORD authn, DWORD imp, void *list,
                                         DWORD caps, void *res3)
{
    (void)sd; (void)n; (void)auth; (void)res; (void)authn; (void)imp; (void)list; (void)caps; (void)res3;
    return S_OK;
}
WINOLEAPI_(HRESULT) CoSetProxyBlanket(IUnknown *p, DWORD a, DWORD b, void *c, DWORD d, DWORD e, void *f, DWORD g)
{
    (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return S_OK;
}
WINOLEAPI_(HRESULT) CoQueryProxyBlanket(IUnknown *p, DWORD *a, DWORD *b, LPOLESTR *c, DWORD *d, DWORD *e, void **f, DWORD *g)
{
    (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return E_NOINTERFACE;       /* not a proxy */
}
WINOLEAPI_(HRESULT) CoImpersonateClient(void) { return S_OK; }
WINOLEAPI_(HRESULT) CoRevertToSelf(void) { return S_OK; }

WINOLEAPI_(DWORD) CoGetCurrentProcess(void) { return GetCurrentProcessId(); }

WINOLEAPI_(HRESULT) CoGetCallerTID(LPDWORD tid) { if (tid) *tid = GetCurrentThreadId(); return S_FALSE; }

/* -----------------------------------------------------------------------
 * The object context: one per apartment kind (there is no cross-apartment
 * marshaling).  Its token is the object itself, as on Windows, and it
 * answers IUnknown, IContextCallback (callbacks run directly) and
 * IComThreadingInfo.
 * ----------------------------------------------------------------------- */
static const IID IID_IContextCallback_ = { 0x000001da, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID IID_IComThreadingInfo_ = { 0x000001ce, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
typedef struct { const void *vtbl; APTTYPE type; } ObjCtx;
typedef HRESULT (__stdcall *PFNCONTEXTCALL_)(void *data);

static HRESULT __stdcall ctx_qi(ObjCtx *This, REFIID riid, void **ppv);
static ULONG __stdcall ctx_addref(ObjCtx *This) { (void)This; return 2; }
static ULONG __stdcall ctx_release(ObjCtx *This) { (void)This; return 1; }
static HRESULT __stdcall ctx_callback(ObjCtx *This, PFNCONTEXTCALL_ fn, void *data, REFIID riid, int method, IUnknown *unk)
{
    (void)This; (void)riid; (void)method; (void)unk;
    return fn ? fn(data) : E_INVALIDARG;
}
static HRESULT __stdcall cti_apt_type(ObjCtx *This, APTTYPE *t) { if (!t) return E_POINTER; *t = This->type; return S_OK; }
static HRESULT __stdcall cti_thread_type(ObjCtx *This, int *t) { (void)This; if (!t) return E_POINTER; *t = 0; return S_OK; }   /* THDTYPE_BLOCKMESSAGES */
static HRESULT __stdcall cti_get_logical(ObjCtx *This, GUID *g) { (void)This; if (!g) return E_POINTER; ZeroMemory(g, sizeof(*g)); return S_OK; }
static HRESULT __stdcall cti_set_logical(ObjCtx *This, REFGUID g) { (void)This; (void)g; return S_OK; }

static const void *const g_ctx_callback_vtbl[] = { (void *)ctx_qi, (void *)ctx_addref, (void *)ctx_release, (void *)ctx_callback };
static const void *const g_ctx_threading_vtbl[] = { (void *)ctx_qi, (void *)ctx_addref, (void *)ctx_release,
                                                    (void *)cti_apt_type, (void *)cti_thread_type,
                                                    (void *)cti_get_logical, (void *)cti_set_logical };
/* per apartment kind: the object (IUnknown/IContextCallback) and its threading-info view */
static ObjCtx g_ctx_sta = { g_ctx_callback_vtbl, APTTYPE_STA }, g_ctx_mta = { g_ctx_callback_vtbl, APTTYPE_MTA };
static ObjCtx g_cti_sta = { g_ctx_threading_vtbl, APTTYPE_STA }, g_cti_mta = { g_ctx_threading_vtbl, APTTYPE_MTA };

static HRESULT __stdcall ctx_qi(ObjCtx *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    int sta = This->type == APTTYPE_STA;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IContextCallback_)) { *ppv = sta ? &g_ctx_sta : &g_ctx_mta; return S_OK; }
    if (IsEqualIID(riid, &IID_IComThreadingInfo_)) { *ppv = sta ? &g_cti_sta : &g_cti_mta; return S_OK; }
    *ppv = 0;
    return E_NOINTERFACE;
}

static ObjCtx *current_ctx(void)
{
    if (t_inits > 0) return t_model == COINIT_APARTMENTTHREADED ? &g_ctx_sta : &g_ctx_mta;
    return g_mta_usage > 0 ? &g_ctx_mta : 0;
}

WINOLEAPI_(HRESULT) CoGetContextToken(ULONG_PTR *token)
{
    if (!token) return E_POINTER;
    ObjCtx *c = current_ctx();
    if (!c) { *token = 0; return CO_E_NOTINITIALIZED; }
    *token = (ULONG_PTR)c;
    return S_OK;
}

WINOLEAPI_(HRESULT) CoWaitForMultipleHandles(DWORD flags, DWORD ms, ULONG n, LPHANDLE h, LPDWORD index)
{
    if (!h || !index || !n) return E_INVALIDARG;
    DWORD r = WaitForMultipleObjectsEx(n, h, (flags & 1) != 0 /* COWAIT_WAITALL */, ms, (flags & 8) != 0 /* ALERTABLE */);
    if (r == WAIT_TIMEOUT) return (HRESULT)0x8001011FL;       /* RPC_S_CALLPENDING */
    if (r == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
    *index = r == WAIT_IO_COMPLETION ? 0 : r - WAIT_OBJECT_0;
    return r == WAIT_IO_COMPLETION ? (HRESULT)0x000400C0L : S_OK;
}

WINOLEAPI_(HRESULT) CoLockObjectExternal(IUnknown *p, BOOL lock, BOOL last)
{
    (void)last;
    if (!p) return E_INVALIDARG;
    if (lock) p->lpVtbl->AddRef(p); else p->lpVtbl->Release(p);
    return S_OK;
}

WINOLEAPI_(HRESULT) CoDisconnectObject(IUnknown *p, DWORD reserved) { (void)p; (void)reserved; return S_OK; }
WINOLEAPI_(BOOL)    CoIsHandlerConnected(IUnknown *p) { (void)p; return TRUE; }
WINOLEAPI_(HRESULT) CoAllowSetForegroundWindow(IUnknown *p, LPVOID r) { (void)p; (void)r; return S_OK; }
WINOLEAPI_(HRESULT) CoRegisterMessageFilter(LPVOID filter, LPVOID *old) { (void)filter; if (old) *old = 0; return S_OK; }
WINOLEAPI_(HRESULT) CoEnableCallCancellation(LPVOID r) { (void)r; return S_OK; }
WINOLEAPI_(HRESULT) CoDisableCallCancellation(LPVOID r) { (void)r; return S_OK; }

/* marshaling would need an RPC runtime; everything is in-process */
WINOLEAPI_(HRESULT) CoMarshalInterface(IStream *s, REFIID riid, IUnknown *p, DWORD ctx, void *pv, DWORD flags)
{
    (void)s; (void)riid; (void)p; (void)ctx; (void)pv; (void)flags;
    return E_NOTIMPL;
}
WINOLEAPI_(HRESULT) CoUnmarshalInterface(IStream *s, REFIID riid, void **ppv)
{
    (void)s; (void)riid;
    if (ppv) *ppv = 0;
    return E_NOTIMPL;
}

WINOLEAPI_(HRESULT) CoGetStdMarshalEx(IUnknown *outer, DWORD flags, IUnknown **out)
{
    (void)outer; (void)flags;
    if (out) *out = 0;
    return E_NOTIMPL;
}

/* no call ever arrives from another apartment or process, so no code runs
 * inside one: Windows answers this outside a call */
WINOLEAPI_(HRESULT) CoGetCallContext(REFIID riid, void **ppv)
{
    (void)riid;
    if (ppv) *ppv = 0;
    return RPC_E_CALL_COMPLETE;
}

/* CoRegisterPSClsid: which proxy/stub class serves an interface in this
 * process (HKCR\Interface\{iid}\ProxyStubClsid32 for everyone else); kept
 * for the marshaling a cross-process call would need */
typedef struct PsClsid { IID iid; CLSID clsid; struct PsClsid *next; } PsClsid;
static PsClsid *g_ps_clsids;
static SRWLOCK g_ps_lock = SRWLOCK_INIT;

WINOLEAPI_(HRESULT) CoRegisterPSClsid(REFIID riid, REFCLSID rclsid)
{
    if (!riid || !rclsid) return E_INVALIDARG;
    if (!t_inits) return CO_E_NOTINITIALIZED;
    AcquireSRWLockExclusive(&g_ps_lock);
    PsClsid *p = g_ps_clsids;
    while (p && !IsEqualIID(&p->iid, riid)) p = p->next;
    if (!p && (p = HeapAlloc(GetProcessHeap(), 0, sizeof *p))) {
        p->iid = *riid;
        p->next = g_ps_clsids;
        g_ps_clsids = p;
    }
    if (p) p->clsid = *rclsid;
    ReleaseSRWLockExclusive(&g_ps_lock);
    return p ? S_OK : E_OUTOFMEMORY;
}

/* the "proxy" for another apartment is the object itself */
WINOLEAPI_(HRESULT) CoMarshalInterThreadInterfaceInStream(REFIID riid, IUnknown *p, IStream **out)
{
    if (!p || !out) return E_INVALIDARG;
    HRESULT hr = CreateStreamOnHGlobal(0, TRUE, out);
    if (FAILED(hr)) return hr;
    void *q = 0;
    hr = p->lpVtbl->QueryInterface(p, riid, &q);
    if (SUCCEEDED(hr)) hr = (*out)->lpVtbl->Write(*out, &q, sizeof q, 0);
    if (FAILED(hr)) { (*out)->lpVtbl->Release(*out); *out = 0; }
    return hr;
}

WINOLEAPI_(HRESULT) CoGetInterfaceAndReleaseStream(IStream *s, REFIID riid, void **ppv)
{
    if (!s || !ppv) return E_INVALIDARG;
    IUnknown *p = 0;
    LARGE_INTEGER zero = { 0 };
    ULONG got = 0;
    s->lpVtbl->Seek(s, zero, STREAM_SEEK_SET, 0);
    HRESULT hr = s->lpVtbl->Read(s, &p, sizeof p, &got);
    s->lpVtbl->Release(s);
    if (FAILED(hr) || got != sizeof p || !p) return E_INVALIDARG;
    hr = p->lpVtbl->QueryInterface(p, riid, ppv);
    p->lpVtbl->Release(p);
    return hr;
}

/* ---------------------------------------------------------------------------
 * Task memory
 * ------------------------------------------------------------------------- */
WINOLEAPI_(LPVOID) CoTaskMemAlloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
WINOLEAPI_(void)   CoTaskMemFree(LPVOID p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
WINOLEAPI_(LPVOID) CoTaskMemRealloc(LPVOID p, SIZE_T n)
{
    if (!p) return CoTaskMemAlloc(n);
    if (!n) { CoTaskMemFree(p); return 0; }
    return HeapReAlloc(GetProcessHeap(), 0, p, n);
}

static HRESULT STDMETHODCALLTYPE malloc_qi(IMalloc *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMalloc)) { *ppv = This; return S_OK; }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE malloc_addref(IMalloc *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE malloc_release(IMalloc *This) { (void)This; return 1; }
static void *STDMETHODCALLTYPE malloc_alloc(IMalloc *This, SIZE_T n) { (void)This; return CoTaskMemAlloc(n); }
static void *STDMETHODCALLTYPE malloc_realloc(IMalloc *This, void *p, SIZE_T n) { (void)This; return CoTaskMemRealloc(p, n); }
static void STDMETHODCALLTYPE malloc_free(IMalloc *This, void *p) { (void)This; CoTaskMemFree(p); }
static SIZE_T STDMETHODCALLTYPE malloc_size(IMalloc *This, void *p) { (void)This; return p ? HeapSize(GetProcessHeap(), 0, p) : (SIZE_T)-1; }
static int STDMETHODCALLTYPE malloc_didalloc(IMalloc *This, void *p) { (void)This; return p ? -1 : 0; }
static void STDMETHODCALLTYPE malloc_minimize(IMalloc *This) { (void)This; }

static const IMallocVtbl g_malloc_vtbl = {
    malloc_qi, malloc_addref, malloc_release, malloc_alloc, malloc_realloc,
    malloc_free, malloc_size, malloc_didalloc, malloc_minimize,
};
static IMalloc g_malloc = { &g_malloc_vtbl };

WINOLEAPI_(HRESULT) CoGetMalloc(DWORD ctx, LPMALLOC *out)
{
    if (ctx != 1 /* MEMCTX_TASK */) { *out = 0; return E_INVALIDARG; }
    *out = &g_malloc;
    return S_OK;
}

/* ---------------------------------------------------------------------------
 * GUIDs
 * ------------------------------------------------------------------------- */
WINOLEAPI_(HRESULT) CoCreateGuid(GUID *g)
{
    if (!g) return E_INVALIDARG;
    if (!SystemFunction036(g, sizeof *g)) return E_FAIL;
    g->Data3 = (WORD)((g->Data3 & 0x0FFF) | 0x4000);           /* version 4 (random) */
    g->Data4[0] = (BYTE)((g->Data4[0] & 0x3F) | 0x80);          /* RFC 4122 variant */
    return S_OK;
}

WINOLEAPI_(int) StringFromGUID2(REFGUID g, LPOLESTR out, int cch)
{
    static const char hx[] = "0123456789ABCDEF";
    WCHAR s[39];
    int k = 0;
    s[k++] = '{';
    for (int i = 7; i >= 0; i--) s[k++] = hx[(g->Data1 >> (4 * i)) & 15];
    s[k++] = '-';
    for (int i = 3; i >= 0; i--) s[k++] = hx[(g->Data2 >> (4 * i)) & 15];
    s[k++] = '-';
    for (int i = 3; i >= 0; i--) s[k++] = hx[(g->Data3 >> (4 * i)) & 15];
    s[k++] = '-';
    for (int i = 0; i < 8; i++) {
        if (i == 2) s[k++] = '-';
        s[k++] = hx[g->Data4[i] >> 4];
        s[k++] = hx[g->Data4[i] & 15];
    }
    s[k++] = '}';
    s[k++] = 0;
    if (!out || cch < k) return 0;
    for (int i = 0; i < k; i++) out[i] = s[i];
    return k;
}

static HRESULT guid_to_new_string(REFGUID g, LPOLESTR *out)
{
    if (!out) return E_INVALIDARG;
    *out = CoTaskMemAlloc(39 * sizeof(WCHAR));
    if (!*out) return E_OUTOFMEMORY;
    StringFromGUID2(g, *out, 39);
    return S_OK;
}
WINOLEAPI_(HRESULT) StringFromCLSID(REFCLSID clsid, LPOLESTR *out) { return guid_to_new_string(clsid, out); }
WINOLEAPI_(HRESULT) StringFromIID(REFIID iid, LPOLESTR *out) { return guid_to_new_string(iid, out); }

static int hexv(WCHAR c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (braces required, as in Windows) */
static BOOL parse_guid(LPCOLESTR s, GUID *g)
{
    static const int dash[] = { 9, 14, 19, 24 };
    if (!s || s[0] != '{') return FALSE;
    for (int i = 1; i < 37; i++) {
        BOOL d = i == dash[0] || i == dash[1] || i == dash[2] || i == dash[3];
        if (d ? s[i] != '-' : hexv(s[i]) < 0) return FALSE;
    }
    if (s[37] != '}' || s[38]) return FALSE;
    unsigned long long v = 0;
    for (int i = 1; i < 9; i++) v = v << 4 | (unsigned)hexv(s[i]);
    g->Data1 = (DWORD)v;
    v = 0;
    for (int i = 10; i < 14; i++) v = v << 4 | (unsigned)hexv(s[i]);
    g->Data2 = (WORD)v;
    v = 0;
    for (int i = 15; i < 19; i++) v = v << 4 | (unsigned)hexv(s[i]);
    g->Data3 = (WORD)v;
    static const int pos[8] = { 20, 22, 25, 27, 29, 31, 33, 35 };
    for (int i = 0; i < 8; i++) g->Data4[i] = (BYTE)(hexv(s[pos[i]]) << 4 | hexv(s[pos[i] + 1]));
    return TRUE;
}

WINOLEAPI_(HRESULT) IIDFromString(LPCOLESTR s, LPIID out)
{
    if (!out) return E_INVALIDARG;
    if (!s || !*s) { *out = GUID_NULL; return S_OK; }
    return parse_guid(s, out) ? S_OK : E_INVALIDARG;
}

WINOLEAPI_(HRESULT) CLSIDFromProgID(LPCOLESTR progid, LPCLSID out)
{
    if (!progid || !out) return E_INVALIDARG;
    WCHAR sub[300], val[64];
    int n = 0;
    for (; progid[n] && n < 250; n++) sub[n] = progid[n];
    if (progid[n]) return CO_E_CLASSSTRING;
    const WCHAR *tail = L"\\CLSID";
    for (int i = 0; tail[i]; i++) sub[n++] = tail[i];
    sub[n] = 0;
    DWORD size = sizeof val;
    if (RegGetValueW(HKEY_CLASSES_ROOT, sub, 0, RRF_RT_REG_SZ, 0, val, &size)) return CO_E_CLASSSTRING;
    return parse_guid(val, out) ? S_OK : CO_E_CLASSSTRING;
}

WINOLEAPI_(HRESULT) CLSIDFromProgIDEx(LPCOLESTR progid, LPCLSID out) { return CLSIDFromProgID(progid, out); }

WINOLEAPI_(HRESULT) CLSIDFromString(LPCOLESTR s, LPCLSID out)
{
    if (!out) return E_INVALIDARG;
    if (!s || !*s) { *out = CLSID_NULL; return S_OK; }
    if (parse_guid(s, out)) return S_OK;
    return SUCCEEDED(CLSIDFromProgID(s, out)) ? S_OK : CO_E_CLASSSTRING;
}

/* "CLSID\{clsid}\" followed by @leaf */
static void clsid_key(REFCLSID clsid, const WCHAR *leaf, WCHAR *out)
{
    static const WCHAR pre[] = L"CLSID\\";
    int n = 0;
    for (int i = 0; pre[i]; i++) out[n++] = pre[i];
    n += StringFromGUID2(clsid, out + n, 39) - 1;
    if (leaf) {
        out[n++] = '\\';
        for (int i = 0; leaf[i]; i++) out[n++] = leaf[i];
    }
    out[n] = 0;
}

WINOLEAPI_(HRESULT) ProgIDFromCLSID(REFCLSID clsid, LPOLESTR *out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    WCHAR key[80];
    clsid_key(clsid, L"ProgID", key);
    DWORD size = 0;
    if (RegGetValueW(HKEY_CLASSES_ROOT, key, 0, RRF_RT_REG_SZ, 0, 0, &size)) return REGDB_E_CLASSNOTREG;
    *out = CoTaskMemAlloc(size);
    if (!*out) return E_OUTOFMEMORY;
    if (RegGetValueW(HKEY_CLASSES_ROOT, key, 0, RRF_RT_REG_SZ, 0, *out, &size)) {
        CoTaskMemFree(*out);
        *out = 0;
        return REGDB_E_CLASSNOTREG;
    }
    return S_OK;
}

WINOLEAPI_(HRESULT) CoGetTreatAsClass(REFCLSID clsid, LPCLSID out)
{
    WCHAR key[80], val[64];
    DWORD size = sizeof val;
    *out = *clsid;
    clsid_key(clsid, L"TreatAs", key);
    if (RegGetValueW(HKEY_CLASSES_ROOT, key, 0, RRF_RT_REG_SZ, 0, val, &size) || !parse_guid(val, out)) {
        *out = *clsid;
        return S_FALSE;
    }
    return S_OK;
}

/* ---------------------------------------------------------------------------
 * Class objects: registered in-process, or in-process servers from the registry
 * ------------------------------------------------------------------------- */
typedef HRESULT (STDAPICALLTYPE *GETCLASSOBJECT)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (STDAPICALLTYPE *CANUNLOADNOW)(void);

typedef struct RegClass {
    struct RegClass *next;
    CLSID clsid;
    IUnknown *obj;
    DWORD ctx, flags, cookie;
    BOOL used;                        /* REGCLS_SINGLEUSE: handed out once */
} RegClass;

typedef struct Server {
    struct Server *next;
    HMODULE mod;
    CANUNLOADNOW can_unload;
} Server;

static SRWLOCK g_lock = SRWLOCK_INIT;
static RegClass *g_classes;
static Server *g_servers;
static DWORD g_next_cookie = 1;

WINOLEAPI_(HRESULT) CoRegisterClassObject(REFCLSID clsid, IUnknown *obj, DWORD ctx, DWORD flags, LPDWORD cookie)
{
    if (!obj || !cookie) return E_INVALIDARG;
    RegClass *r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *r);
    if (!r) return E_OUTOFMEMORY;
    r->clsid = *clsid;
    r->obj = obj;
    r->ctx = ctx;
    r->flags = flags;
    obj->lpVtbl->AddRef(obj);
    AcquireSRWLockExclusive(&g_lock);
    r->cookie = g_next_cookie++;
    r->next = g_classes;
    g_classes = r;
    ReleaseSRWLockExclusive(&g_lock);
    *cookie = r->cookie;
    return S_OK;
}

WINOLEAPI_(HRESULT) CoRevokeClassObject(DWORD cookie)
{
    AcquireSRWLockExclusive(&g_lock);
    RegClass **pp = &g_classes, *r = 0;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->cookie == cookie) { r = *pp; *pp = r->next; break; }
    ReleaseSRWLockExclusive(&g_lock);
    if (!r) return CO_E_OBJNOTREG;
    r->obj->lpVtbl->Release(r->obj);
    HeapFree(GetProcessHeap(), 0, r);
    return S_OK;
}

WINOLEAPI_(HRESULT) CoResumeClassObjects(void) { return S_OK; }
WINOLEAPI_(HRESULT) CoSuspendClassObjects(void) { return S_OK; }
WINOLEAPI_(ULONG)   CoAddRefServerProcess(void) { return 1; }
WINOLEAPI_(ULONG)   CoReleaseServerProcess(void) { return 0; }

static IUnknown *find_registered(REFCLSID clsid, DWORD ctx)
{
    IUnknown *obj = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (RegClass *r = g_classes; r; r = r->next)
        if (IsEqualCLSID(&r->clsid, clsid) && (r->ctx & ctx) && !(r->flags & REGCLS_SUSPENDED) && !r->used) {
            obj = r->obj;
            obj->lpVtbl->AddRef(obj);
            if ((r->flags & 3) == REGCLS_SINGLEUSE) r->used = TRUE;
            break;
        }
    ReleaseSRWLockExclusive(&g_lock);
    return obj;
}

static void remember_server(HMODULE mod)
{
    AcquireSRWLockExclusive(&g_lock);
    for (Server *s = g_servers; s; s = s->next)
        if (s->mod == mod) {                 /* already loaded once: keep one reference */
            ReleaseSRWLockExclusive(&g_lock);
            FreeLibrary(mod);
            return;
        }
    Server *s = HeapAlloc(GetProcessHeap(), 0, sizeof *s);
    if (s) {
        s->mod = mod;
        s->can_unload = (CANUNLOADNOW)GetProcAddress(mod, "DllCanUnloadNow");
        s->next = g_servers;
        g_servers = s;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* HKCR\CLSID\{clsid}\InprocServer32 -> the DLL's DllGetClassObject */
static HRESULT inproc_class_object(REFCLSID clsid, REFIID riid, void **ppv)
{
    WCHAR key[100], path[MAX_PATH];
    DWORD size = sizeof path;
    clsid_key(clsid, L"InprocServer32", key);
    LSTATUS e = RegGetValueW(HKEY_CLASSES_ROOT, key, 0, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, 0, path, &size);
    if (e) {
        clsid_key(clsid, 0, key);
        { char m[140]; int n = 0; const char *pre = "ole32: class not registered: ";
          while (*pre) m[n++] = *pre++;
          for (const WCHAR *q = key; *q && n < 136; q++) m[n++] = (char)*q;
          m[n++] = '\n'; m[n] = 0; OutputDebugStringA(m); }
        HKEY k;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &k)) return REGDB_E_CLASSNOTREG;
        RegCloseKey(k);
        return REGDB_E_CLASSNOTREG;                           /* registered, but not in-process */
    }
    HMODULE mod = LoadLibraryExW(path, 0, 0);
    if (!mod) return CO_E_DLLNOTFOUND;
    GETCLASSOBJECT get = (GETCLASSOBJECT)GetProcAddress(mod, "DllGetClassObject");
    if (!get) { FreeLibrary(mod); return CO_E_ERRORINDLL; }
    HRESULT hr = get(clsid, riid, ppv);
    if (FAILED(hr)) { FreeLibrary(mod); return hr; }
    remember_server(mod);
    return hr;
}

WINOLEAPI_(HRESULT) CoGetClassObject(REFCLSID clsid, DWORD ctx, LPVOID server, REFIID riid, LPVOID *ppv)
{
    (void)server;
    if (!ppv) return E_INVALIDARG;
    *ppv = 0;
    if (!clsid) return E_INVALIDARG;
    CLSID real;
    CoGetTreatAsClass(clsid, &real);
    IUnknown *obj = find_registered(&real, ctx ? ctx : CLSCTX_ALL);
    if (obj) {
        HRESULT hr = obj->lpVtbl->QueryInterface(obj, riid, ppv);
        obj->lpVtbl->Release(obj);
        return hr;
    }
    if (!(ctx & (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER))) return REGDB_E_CLASSNOTREG;
    return inproc_class_object(&real, riid, ppv);
}

WINOLEAPI_(HRESULT) CoCreateInstance(REFCLSID clsid, IUnknown *outer, DWORD ctx, REFIID riid, LPVOID *ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = 0;
    IClassFactory *cf;
    HRESULT hr = CoGetClassObject(clsid, ctx, 0, &IID_IClassFactory, (void **)&cf);
    if (FAILED(hr)) return hr;
    hr = cf->lpVtbl->CreateInstance(cf, outer, riid, ppv);
    cf->lpVtbl->Release(cf);
    return hr;
}

WINOLEAPI_(HRESULT) CoCreateInstanceEx(REFCLSID clsid, IUnknown *outer, DWORD ctx, COSERVERINFO *server, DWORD n, MULTI_QI *res)
{
    (void)server;
    if (!n || !res) return E_INVALIDARG;
    for (DWORD i = 0; i < n; i++) { res[i].pItf = 0; res[i].hr = E_NOINTERFACE; }
    IUnknown *unk;
    HRESULT hr = CoCreateInstance(clsid, outer, ctx, &IID_IUnknown, (void **)&unk);
    if (FAILED(hr)) {
        for (DWORD i = 0; i < n; i++) res[i].hr = hr;
        return hr;
    }
    DWORD got = 0;
    for (DWORD i = 0; i < n; i++) {
        res[i].hr = unk->lpVtbl->QueryInterface(unk, res[i].pIID, (void **)&res[i].pItf);
        if (SUCCEEDED(res[i].hr)) got++;
    }
    unk->lpVtbl->Release(unk);
    return !got ? E_NOINTERFACE : got < n ? (HRESULT)0x00040012L /* CO_S_NOTALLINTERFACES */ : S_OK;
}

WINOLEAPI_(HRESULT) CoCreateInstanceFromApp(REFCLSID clsid, IUnknown *outer, DWORD ctx, PVOID reserved, DWORD n, MULTI_QI *res)
{
    (void)reserved;
    return CoCreateInstanceEx(clsid, outer, ctx, 0, n, res);
}

WINOLEAPI_(void) CoFreeUnusedLibrariesEx(DWORD delay, DWORD reserved)
{
    (void)delay; (void)reserved;
    AcquireSRWLockExclusive(&g_lock);
    Server **pp = &g_servers;
    while (*pp) {
        Server *s = *pp;
        if (s->can_unload && s->can_unload() == S_OK) {
            *pp = s->next;
            FreeLibrary(s->mod);
            HeapFree(GetProcessHeap(), 0, s);
        } else pp = &s->next;
    }
    ReleaseSRWLockExclusive(&g_lock);
}
WINOLEAPI_(void) CoFreeUnusedLibraries(void) { CoFreeUnusedLibrariesEx(0, 0); }
WINOLEAPI_(void) CoFreeAllLibraries(void) { CoFreeUnusedLibraries(); }

WINOLEAPI_(HINSTANCE) CoLoadLibrary(LPOLESTR name, BOOL autofree) { (void)autofree; return LoadLibraryExW(name, 0, 0); }
WINOLEAPI_(void)      CoFreeLibrary(HINSTANCE h) { FreeLibrary(h); }

WINOLEAPI_(HRESULT) CoFileTimeNow(FILETIME *ft) { GetSystemTimeAsFileTime(ft); return S_OK; }

/* ---------------------------------------------------------------------------
 * IStream over an HGLOBAL (CreateStreamOnHGlobal)
 * ------------------------------------------------------------------------- */
typedef struct HStream {
    IStream iface;
    LONG refs;
    HGLOBAL mem;
    ULONG size, cap;
    ULONG pos;
    BOOL del;
} HStream;

static HRESULT STDMETHODCALLTYPE hs_qi(IStream *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IStream) || IsEqualIID(riid, &IID_ISequentialStream)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE hs_addref(IStream *This) { return (ULONG)InterlockedIncrement(&((HStream *)This)->refs); }
static ULONG STDMETHODCALLTYPE hs_release(IStream *This)
{
    HStream *s = (HStream *)This;
    LONG r = InterlockedDecrement(&s->refs);
    if (!r) {
        if (s->del) GlobalFree(s->mem);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return (ULONG)r;
}

static BOOL hs_reserve(HStream *s, ULONG need)
{
    if (need <= s->cap) return TRUE;
    ULONG cap = s->cap ? s->cap : 256;
    while (cap < need) cap = cap > 0x7FFFFFFF / 2 ? need : cap * 2;
    HGLOBAL m = s->mem ? GlobalReAlloc(s->mem, cap, GMEM_MOVEABLE) : GlobalAlloc(GMEM_MOVEABLE, cap);
    if (!m) return FALSE;
    s->mem = m;
    s->cap = cap;
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE hs_read(IStream *This, void *pv, ULONG cb, ULONG *read)
{
    HStream *s = (HStream *)This;
    ULONG n = s->pos < s->size ? s->size - s->pos : 0;
    if (n > cb) n = cb;
    if (n) CopyMemory(pv, (BYTE *)s->mem + s->pos, n);
    s->pos += n;
    if (read) *read = n;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hs_write(IStream *This, const void *pv, ULONG cb, ULONG *written)
{
    HStream *s = (HStream *)This;
    if (written) *written = 0;
    if (!cb) return S_OK;
    if (s->pos + cb < s->pos || !hs_reserve(s, s->pos + cb)) return STG_E_MEDIUMFULL;
    if (s->pos > s->size) ZeroMemory((BYTE *)s->mem + s->size, s->pos - s->size);
    CopyMemory((BYTE *)s->mem + s->pos, pv, cb);
    s->pos += cb;
    if (s->pos > s->size) s->size = s->pos;
    if (written) *written = cb;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hs_seek(IStream *This, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *pos)
{
    HStream *s = (HStream *)This;
    LONGLONG base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? (LONGLONG)s->pos :
                    origin == STREAM_SEEK_END ? (LONGLONG)s->size : -1;
    if (base < 0) return STG_E_INVALIDFUNCTION;
    LONGLONG to = base + move.QuadPart;
    if (to < 0 || to > 0xFFFFFFFFLL) return STG_E_INVALIDFUNCTION;
    s->pos = (ULONG)to;
    if (pos) pos->QuadPart = (ULONGLONG)to;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hs_setsize(IStream *This, ULARGE_INTEGER size)
{
    HStream *s = (HStream *)This;
    if (size.QuadPart > 0xFFFFFFFFULL) return STG_E_MEDIUMFULL;
    ULONG n = (ULONG)size.QuadPart;
    if (!hs_reserve(s, n)) return STG_E_MEDIUMFULL;
    if (n > s->size) ZeroMemory((BYTE *)s->mem + s->size, n - s->size);
    s->size = n;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hs_copyto(IStream *This, IStream *to, ULARGE_INTEGER cb, ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
    HStream *s = (HStream *)This;
    ULONG n = s->pos < s->size ? s->size - s->pos : 0;
    if (cb.QuadPart < n) n = (ULONG)cb.QuadPart;
    ULONG w = 0;
    HRESULT hr = n ? to->lpVtbl->Write(to, (BYTE *)s->mem + s->pos, n, &w) : S_OK;
    s->pos += n;
    if (read) read->QuadPart = n;
    if (written) written->QuadPart = w;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hs_commit(IStream *This, DWORD flags) { (void)This; (void)flags; return S_OK; }
static HRESULT STDMETHODCALLTYPE hs_revert(IStream *This) { (void)This; return S_OK; }
static HRESULT STDMETHODCALLTYPE hs_lock(IStream *This, ULARGE_INTEGER o, ULARGE_INTEGER n, DWORD t)
{
    (void)This; (void)o; (void)n; (void)t;
    return STG_E_INVALIDFUNCTION;
}

static HRESULT STDMETHODCALLTYPE hs_stat(IStream *This, STATSTG *st, DWORD flags)
{
    (void)flags;
    HStream *s = (HStream *)This;
    ZeroMemory(st, sizeof *st);
    st->type = STGTY_STREAM;
    st->cbSize.QuadPart = s->size;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hs_clone(IStream *This, IStream **out);

static const IStreamVtbl g_hs_vtbl = {
    hs_qi, hs_addref, hs_release, hs_read, hs_write, hs_seek, hs_setsize, hs_copyto,
    hs_commit, hs_revert, hs_lock, hs_lock, hs_stat, hs_clone,
};

/* a clone shares the memory but has its own position; the original keeps ownership */
static HRESULT STDMETHODCALLTYPE hs_clone(IStream *This, IStream **out)
{
    HStream *s = (HStream *)This, *c = HeapAlloc(GetProcessHeap(), 0, sizeof *c);
    if (!c) return E_OUTOFMEMORY;
    *c = *s;
    c->refs = 1;
    c->del = FALSE;
    *out = &c->iface;
    return S_OK;
}

WINOLEAPI_(HRESULT) CreateStreamOnHGlobal(HGLOBAL h, BOOL del, LPSTREAM *out)
{
    if (!out) return E_INVALIDARG;
    HStream *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    if (!s) return E_OUTOFMEMORY;
    s->iface.lpVtbl = &g_hs_vtbl;
    s->refs = 1;
    s->del = del;
    if (h) {
        s->mem = h;
        s->cap = s->size = (ULONG)GlobalSize(h);
    } else {
        s->del = del;
        if (!hs_reserve(s, 256)) { HeapFree(GetProcessHeap(), 0, s); return E_OUTOFMEMORY; }
    }
    *out = &s->iface;
    return S_OK;
}

WINOLEAPI_(HRESULT) GetHGlobalFromStream(LPSTREAM st, HGLOBAL *out)
{
    if (!st || !out || st->lpVtbl != &g_hs_vtbl) return E_INVALIDARG;
    *out = ((HStream *)st)->mem;
    return S_OK;
}

/* ---------------------------------------------------------------------------
 * OLE odds and ends (drag and drop: dragdrop.c; the clipboard: clipbrd.c)
 * ------------------------------------------------------------------------- */

WINOLEAPI_(void) ReleaseStgMedium(STGMEDIUM *m)
{
    if (!m) return;
    if (m->pUnkForRelease) {
        m->pUnkForRelease->lpVtbl->Release(m->pUnkForRelease);
    } else switch (m->tymed) {
        case 1: GlobalFree(m->hGlobal); break;                   /* TYMED_HGLOBAL */
        case 2: CoTaskMemFree(m->lpszFileName); break;           /* TYMED_FILE */
        case 4: case 8: if (m->pstm) ((IUnknown *)m->pstm)->lpVtbl->Release((IUnknown *)m->pstm); break;   /* ISTREAM / ISTORAGE */
        }
    m->tymed = 0;
}

/* PROPVARIANT: VT_LPWSTR, VT_LPSTR and VT_CLSID own task memory, others need oleaut32 */
typedef struct { WORD vt, r1, r2, r3; union { LPWSTR pwszVal; LPSTR pszVal; CLSID *puuid; IUnknown *punkVal; ULONGLONG uhVal; }; } PROPVARIANT_;
WINOLEAPI_(HRESULT) PropVariantClear(PROPVARIANT_ *pv)
{
    if (!pv) return S_OK;
    switch (pv->vt) {
    case 30: case 31: case 72: CoTaskMemFree(pv->pwszVal); break;   /* LPSTR, LPWSTR, CLSID */
    case 9: case 13: if (pv->punkVal) pv->punkVal->lpVtbl->Release(pv->punkVal); break;
    case 8: {                                                         /* BSTR: SysFreeString */
        typedef void (WINAPI *freestr_t)(LPWSTR);
        HMODULE oa = LoadLibraryA("oleaut32.dll");
        freestr_t f = oa ? (freestr_t)GetProcAddress(oa, "SysFreeString") : 0;
        if (f) f(pv->pwszVal);
        break;
    }
    }
    ZeroMemory(pv, sizeof *pv);
    return S_OK;
}

WINOLEAPI_(HRESULT) PropVariantCopy(PROPVARIANT_ *dst, const PROPVARIANT_ *src)
{
    *dst = *src;
    switch (src->vt) {
    case 31: {
        SIZE_T n = (lstrlenW(src->pwszVal) + 1) * sizeof(WCHAR);
        if (!(dst->pwszVal = CoTaskMemAlloc(n))) return E_OUTOFMEMORY;
        CopyMemory(dst->pwszVal, src->pwszVal, n);
        break;
    }
    case 30: {
        SIZE_T n = (SIZE_T)lstrlenA(src->pszVal) + 1;
        if (!(dst->pszVal = CoTaskMemAlloc(n))) return E_OUTOFMEMORY;
        CopyMemory(dst->pszVal, src->pszVal, n);
        break;
    }
    case 72:
        if (!(dst->puuid = CoTaskMemAlloc(sizeof(CLSID)))) return E_OUTOFMEMORY;
        *dst->puuid = *src->puuid;
        break;
    case 9: case 13:
        if (dst->punkVal) dst->punkVal->lpVtbl->AddRef(dst->punkVal);
        break;
    }
    return S_OK;
}

/* -----------------------------------------------------------------------
 * For the .NET runtime's COM interop: initialize spies are accepted (and
 * never called), and there is no marshaling across apartments
 * ----------------------------------------------------------------------- */
WINOLEAPI_(HRESULT) CoRegisterInitializeSpy(IUnknown *spy, ULARGE_INTEGER *cookie)
{
    static LONG next;
    if (!spy || !cookie) return E_INVALIDARG;
    cookie->QuadPart = (ULONGLONG)InterlockedIncrement(&next);
    return S_OK;
}
WINOLEAPI_(HRESULT) CoRevokeInitializeSpy(ULARGE_INTEGER cookie) { (void)cookie; return S_OK; }
WINOLEAPI_(HRESULT) CoGetObjectContext(REFIID riid, LPVOID *ppv)
{
    if (!ppv) return E_POINTER;
    ObjCtx *c = current_ctx();
    if (!c) { *ppv = 0; return CO_E_NOTINITIALIZED; }
    return ctx_qi(c, riid, ppv);
}
WINOLEAPI_(HRESULT) CoCreateFreeThreadedMarshaler(IUnknown *outer, IUnknown **marshal)
{
    (void)outer;
    if (marshal) *marshal = 0;
    return E_NOTIMPL;
}
WINOLEAPI_(HRESULT) CoGetMarshalSizeMax(ULONG *size, REFIID riid, IUnknown *unk, DWORD ctx, LPVOID pv, DWORD flags)
{
    (void)riid; (void)unk; (void)ctx; (void)pv; (void)flags;
    if (size) *size = 0;
    return E_NOTIMPL;
}
WINOLEAPI_(HRESULT) CoReleaseMarshalData(IStream *stm) { (void)stm; return E_NOTIMPL; }

/* -----------------------------------------------------------------------
 * OLE in-place activation helpers (MFC's container code links them).
 * No OLE object is ever activated in place in another program's window
 * here, so there are no shared menus and no object's accelerators to try.
 * ----------------------------------------------------------------------- */
WINOLEAPI_(BOOL) IsAccelerator(HACCEL acc, int n, LPMSG msg, WORD *cmd)
{
    (void)acc; (void)n; (void)msg;
    if (cmd) *cmd = 0;
    return FALSE;
}
WINOLEAPI_(HRESULT) OleTranslateAccelerator(void *frame, void *info, LPMSG msg) { (void)frame; (void)info; (void)msg; return S_FALSE; }
WINOLEAPI_(HANDLE) OleCreateMenuDescriptor(HMENU combined, void *widths) { (void)widths; return combined ? (HANDLE)combined : NULL; }
WINOLEAPI_(HRESULT) OleDestroyMenuDescriptor(HANDLE h) { (void)h; return S_OK; }
WINOLEAPI_(HRESULT) OleSetMenuDescriptor(HANDLE h, HWND frame, HWND active, void *ipframe, void *ipobj)
{ (void)h; (void)frame; (void)active; (void)ipframe; (void)ipobj; return S_OK; }

/* running objects: an object is running once it exists */
WINOLEAPI_(HRESULT) OleRun(IUnknown *obj) { return obj ? S_OK : E_INVALIDARG; }
WINOLEAPI_(BOOL) OleIsRunning(IUnknown *obj) { return obj != NULL; }
WINOLEAPI_(HRESULT) OleLockRunning(IUnknown *obj, BOOL lock, BOOL last) { (void)lock; (void)last; return obj ? S_OK : E_INVALIDARG; }
/* CoGetObject("Elevation:...", "WinNT://...", monikers by display name): no moniker
 * namespaces are registered, so no name parses */
WINOLEAPI_(HRESULT) CoGetObject(LPCWSTR name, void *opts, REFIID iid, void **out)
{ (void)name; (void)opts; (void)iid; if (out) *out = NULL; return (HRESULT)0x800401E4L; }   /* MK_E_SYNTAX */
WINOLEAPI_(HRESULT) OleSetContainedObject(IUnknown *obj, BOOL contained) { (void)contained; return obj ? S_OK : E_INVALIDARG; }
WINOLEAPI_(HRESULT) OleNoteObjectVisible(IUnknown *obj, BOOL visible) { (void)visible; return obj ? S_OK : E_INVALIDARG; }
