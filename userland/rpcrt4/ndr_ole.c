/*
 * ndr_ole.c — COM proxies and stubs built from a proxy/stub DLL's tables.
 *
 * A MIDL- or widl-generated proxy DLL exports DllGetClassObject, which
 * calls NdrDllGetClassObject with its ProxyFileInfo list: the class object
 * is an IPSFactoryBuffer whose CreateProxy and CreateStub make, for one of
 * the listed interfaces, an interface proxy (StdProxy: its vtable is the
 * DLL's, with each stubless slot pointing at one of ndr_stubless_thunks)
 * or an interface stub (CStdStubBuffer: Invoke runs NdrStubCall2 or the
 * DLL's dispatch table).  ole32 creates both while marshaling (see
 * ole32/marshal.c) and gives them an IRpcChannelBuffer to talk through.
 *
 * Interfaces derived from one in another proxy DLL (IDispatch's, say) are
 * delegated: the base interface's methods go to a proxy or stub the base
 * interface's own factory makes.
 */
#include "ndr.h"
#include "ndr_ole.h"

_Static_assert(offsetof(StdProxy, base_proxy) - offsetof(StdProxy, pProxyVtbl) == PROXY_BASE_OFFSET, "PROXY_BASE_OFFSET");

/* ---- ole32, loaded on first use ----------------------------------------- */
static Ole32Fns g_ole;
static INIT_ONCE g_ole_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK load_ole32(PINIT_ONCE once, void *param, void **ctx)
{
    (void)once; (void)param; (void)ctx;
    HMODULE m = LoadLibraryW(L"ole32.dll");
    if (!m) return TRUE;
#define GET(n) g_ole.n = (void *)GetProcAddress(m, #n)
    GET(CoMarshalInterface); GET(CoUnmarshalInterface); GET(CoGetMarshalSizeMax); GET(CoReleaseMarshalData);
    GET(CoGetPSClsid); GET(CoGetClassObject); GET(CoTaskMemAlloc); GET(CoTaskMemFree);
#undef GET
    return TRUE;
}
const Ole32Fns *ndr_ole32(void)
{
    InitOnceExecuteOnce(&g_ole_once, load_ole32, NULL, NULL);
    return g_ole.CoMarshalInterface && g_ole.CoTaskMemAlloc ? &g_ole : NULL;
}

RPCRTAPI void *RPC_ENTRY NdrOleAllocate(size_t n)
{
    const Ole32Fns *o = ndr_ole32();
    return o ? o->CoTaskMemAlloc(n) : NULL;
}
RPCRTAPI void RPC_ENTRY NdrOleFree(void *p)
{
    const Ole32Fns *o = ndr_ole32();
    if (o && p) o->CoTaskMemFree(p);
}

HRESULT ndr_ps_factory(REFIID iid, IPSFactoryBuffer **out)
{
    const Ole32Fns *o = ndr_ole32();
    CLSID clsid;
    *out = NULL;
    if (!o || !o->CoGetPSClsid) return E_UNEXPECTED;
    HRESULT hr = o->CoGetPSClsid(iid, &clsid);
    if (FAILED(hr)) return hr;
    return o->CoGetClassObject(&clsid, CLSCTX_INPROC_SERVER, NULL, &IID_IPSFactoryBuffer, (void **)out);
}

/* ---- finding an interface in a proxy file list -------------------------- */
static int find_iid(const ProxyFileInfo **list, REFIID iid, const ProxyFileInfo **file, int *index)
{
    for (; list && *list; list++) {
        const ProxyFileInfo *f = *list;
        for (int i = 0; i < f->TableSize; i++)
            if (f->pStubVtblList[i] && IsEqualIID(f->pStubVtblList[i]->header.piid, iid)) {
                *file = f;
                *index = i;
                return 1;
            }
    }
    return 0;
}
static const IID *delegated_base(const ProxyFileInfo *f, int i)
{
    return f->pDelegatedIIDs && f->pDelegatedIIDs[i] && !IsEqualIID(f->pDelegatedIIDs[i], &IID_IUnknown) ? f->pDelegatedIIDs[i] : NULL;
}

/* ---- vtables: a heap copy of each of the DLL's tables, its gaps filled -- */
extern char ndr_stubless_thunks[], ndr_forward_thunks[];

typedef struct Patched { struct Patched *next; const void *src; void *copy; } Patched;
static Patched *g_patched;
static SRWLOCK g_patch_lock = SRWLOCK_INIT;

static void *find_patched(const void *src)
{
    void *r = NULL;
    AcquireSRWLockShared(&g_patch_lock);
    for (Patched *p = g_patched; p; p = p->next)
        if (p->src == src) { r = p->copy; break; }
    ReleaseSRWLockShared(&g_patch_lock);
    return r;
}
static void *remember_patched(const void *src, void *copy)
{
    Patched *p = HeapAlloc(GetProcessHeap(), 0, sizeof *p);
    if (!p) { HeapFree(GetProcessHeap(), 0, copy); return NULL; }
    AcquireSRWLockExclusive(&g_patch_lock);
    for (Patched *q = g_patched; q; q = q->next)
        if (q->src == src) {                       /* another thread was first */
            ReleaseSRWLockExclusive(&g_patch_lock);
            HeapFree(GetProcessHeap(), 0, copy);
            HeapFree(GetProcessHeap(), 0, p);
            return q->copy;
        }
    p->src = src;
    p->copy = copy;
    p->next = g_patched;
    g_patched = p;
    ReleaseSRWLockExclusive(&g_patch_lock);
    return copy;
}

/* the proxy vtable (header + @n methods): -1 slots become stubless thunks,
 * empty slots (a delegated base's methods) forwarding thunks */
static const CInterfaceProxyVtbl *proxy_vtbl(const CInterfaceProxyVtbl *src, unsigned n, int stubless)
{
    const CInterfaceProxyVtbl *c = find_patched(src);
    if (c) return c;
    size_t hdr = stubless ? sizeof(CInterfaceProxyHeader) : sizeof(void *);
    size_t size = sizeof(CInterfaceProxyHeader) + n * sizeof(void *);
    CInterfaceProxyVtbl *v = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!v) return NULL;
    /* (proxies built without USE_STUBLESS_PROXY have no info pointer) */
    if (stubless) v->header = src->header;
    else v->header.piid = *(const IID *const *)src;
    void *const *from = (void *const *)((const char *)src + hdr);
    for (unsigned i = 0; i < n; i++) {
        void *f = from[i];
        if (f == (void *)(INT_PTR)-1 && i < NDR_MAX_METHODS) f = ndr_stubless_thunks + 16 * i;
        else if (!f && i >= 3 && i < NDR_MAX_FORWARD) f = ndr_forward_thunks + 16 * i;
        v->Vtbl[i] = f;
    }
    return remember_patched(src, v);
}

static HRESULT STDMETHODCALLTYPE deleg_connect(IRpcStubBuffer *This, IUnknown *server);
static void STDMETHODCALLTYPE deleg_disconnect(IRpcStubBuffer *This);
static const IRpcStubBufferVtbl g_stub_defaults = {
    CStdStubBuffer_QueryInterface, CStdStubBuffer_AddRef, NULL, CStdStubBuffer_Connect, CStdStubBuffer_Disconnect,
    CStdStubBuffer_Invoke, CStdStubBuffer_IsIIDSupported, CStdStubBuffer_CountRefs,
    CStdStubBuffer_DebugServerQueryInterface, CStdStubBuffer_DebugServerRelease,
};

/* the stub vtable: empty methods (a delegating stub's) get the defaults */
static const CInterfaceStubVtbl *stub_vtbl(const CInterfaceStubVtbl *src, int delegating)
{
    const CInterfaceStubVtbl *c = find_patched(src);
    if (c) return c;
    CInterfaceStubVtbl *v = HeapAlloc(GetProcessHeap(), 0, sizeof *v);
    if (!v) return NULL;
    *v = *src;
    void **to = (void **)&v->Vtbl;
    void *const *def = (void *const *)&g_stub_defaults;
    for (unsigned i = 0; i < sizeof(IRpcStubBufferVtbl) / sizeof(void *); i++)
        if (!to[i]) to[i] = def[i];
    if (delegating) {
        if (v->Vtbl.Connect == CStdStubBuffer_Connect) v->Vtbl.Connect = deleg_connect;
        if (v->Vtbl.Disconnect == CStdStubBuffer_Disconnect) v->Vtbl.Disconnect = deleg_disconnect;
    }
    return remember_patched(src, v);
}

/* ---- interface proxies --------------------------------------------------- */
RPCRTAPI HRESULT STDMETHODCALLTYPE IUnknown_QueryInterface_Proxy(IUnknown *This, REFIID riid, void **ppv)
{
    StdProxy *px = PROXY_FROM_IFACE(This);
    return px->outer->lpVtbl->QueryInterface(px->outer, riid, ppv);
}
RPCRTAPI ULONG STDMETHODCALLTYPE IUnknown_AddRef_Proxy(IUnknown *This)
{
    StdProxy *px = PROXY_FROM_IFACE(This);
    return px->outer->lpVtbl->AddRef(px->outer);
}
RPCRTAPI ULONG STDMETHODCALLTYPE IUnknown_Release_Proxy(IUnknown *This)
{
    StdProxy *px = PROXY_FROM_IFACE(This);
    return px->outer->lpVtbl->Release(px->outer);
}

static HRESULT STDMETHODCALLTYPE px_qi(IRpcProxyBuffer *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRpcProxyBuffer)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE px_addref(IRpcProxyBuffer *This) { return (ULONG)InterlockedIncrement(&((StdProxy *)This)->refs); }
static void STDMETHODCALLTYPE px_disconnect(IRpcProxyBuffer *This)
{
    StdProxy *px = (StdProxy *)This;
    if (px->base_buf) px->base_buf->lpVtbl->Disconnect(px->base_buf);
    IRpcChannelBuffer *c = InterlockedExchangePointer((void **)&px->chan, NULL);
    if (c) c->lpVtbl->Release(c);
}
static ULONG STDMETHODCALLTYPE px_release(IRpcProxyBuffer *This)
{
    StdProxy *px = (StdProxy *)This;
    LONG n = InterlockedDecrement(&px->refs);
    if (n) return (ULONG)n;
    px_disconnect(This);
    if (px->base_buf) {
        /* the base proxy's interface was handed out AddRef'd on the outer
         * object, which we released at creation: take it back first */
        px->outer->lpVtbl->AddRef(px->outer);
        px->base_proxy->lpVtbl->Release(px->base_proxy);
        px->base_buf->lpVtbl->Release(px->base_buf);
    }
    if (px->factory) px->factory->lpVtbl->Release(px->factory);
    HeapFree(GetProcessHeap(), 0, px);
    return 0;
}
static HRESULT STDMETHODCALLTYPE px_connect(IRpcProxyBuffer *This, IRpcChannelBuffer *chan)
{
    StdProxy *px = (StdProxy *)This;
    if (!chan) return E_INVALIDARG;
    chan->lpVtbl->AddRef(chan);
    IRpcChannelBuffer *old = InterlockedExchangePointer((void **)&px->chan, chan);
    if (old) old->lpVtbl->Release(old);
    return px->base_buf ? px->base_buf->lpVtbl->Connect(px->base_buf, chan) : S_OK;
}
static const IRpcProxyBufferVtbl g_px_vtbl = { px_qi, px_addref, px_release, px_connect, px_disconnect };

/* ---- interface stubs ---------------------------------------------------- */
RPCRTAPI HRESULT STDMETHODCALLTYPE CStdStubBuffer_QueryInterface(IRpcStubBuffer *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRpcStubBuffer)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
RPCRTAPI ULONG STDMETHODCALLTYPE CStdStubBuffer_AddRef(IRpcStubBuffer *This)
{
    return (ULONG)InterlockedIncrement(&((CStdStubBuffer *)This)->RefCount);
}
RPCRTAPI HRESULT STDMETHODCALLTYPE CStdStubBuffer_Connect(IRpcStubBuffer *This, IUnknown *server)
{
    CStdStubBuffer *sb = (CStdStubBuffer *)This;
    IUnknown *obj = NULL;
    if (!server) return E_INVALIDARG;
    HRESULT hr = server->lpVtbl->QueryInterface(server, STUB_HEADER(This)->piid, (void **)&obj);
    if (FAILED(hr)) return hr;
    IUnknown *old = InterlockedExchangePointer((void **)&sb->pvServerObject, obj);
    if (old) old->lpVtbl->Release(old);
    return S_OK;
}
RPCRTAPI void STDMETHODCALLTYPE CStdStubBuffer_Disconnect(IRpcStubBuffer *This)
{
    IUnknown *old = InterlockedExchangePointer((void **)&((CStdStubBuffer *)This)->pvServerObject, NULL);
    if (old) old->lpVtbl->Release(old);
}
RPCRTAPI HRESULT STDMETHODCALLTYPE CStdStubBuffer_Invoke(IRpcStubBuffer *This, RPCOLEMESSAGE *msg, IRpcChannelBuffer *chan)
{
    return ndr_invoke_stub(This, msg, chan);
}
RPCRTAPI IRpcStubBuffer *STDMETHODCALLTYPE CStdStubBuffer_IsIIDSupported(IRpcStubBuffer *This, REFIID riid)
{
    if (!IsEqualIID(STUB_HEADER(This)->piid, riid)) return NULL;
    This->lpVtbl->AddRef(This);
    return This;
}
RPCRTAPI ULONG STDMETHODCALLTYPE CStdStubBuffer_CountRefs(IRpcStubBuffer *This)
{
    return ((CStdStubBuffer *)This)->pvServerObject ? 1 : 0;
}
RPCRTAPI HRESULT STDMETHODCALLTYPE CStdStubBuffer_DebugServerQueryInterface(IRpcStubBuffer *This, void **ppv)
{
    *ppv = ((CStdStubBuffer *)This)->pvServerObject;
    return *ppv ? S_OK : CO_E_OBJNOTCONNECTED;
}
RPCRTAPI void STDMETHODCALLTYPE CStdStubBuffer_DebugServerRelease(IRpcStubBuffer *This, void *pv) { (void)This; (void)pv; }

static HRESULT STDMETHODCALLTYPE deleg_connect(IRpcStubBuffer *This, IUnknown *server)
{
    HRESULT hr = CStdStubBuffer_Connect(This, server);
    IRpcStubBuffer *base = DELEGATED_BASE_STUB(This);
    if (SUCCEEDED(hr) && base) hr = base->lpVtbl->Connect(base, server);
    return hr;
}
static void STDMETHODCALLTYPE deleg_disconnect(IRpcStubBuffer *This)
{
    IRpcStubBuffer *base = DELEGATED_BASE_STUB(This);
    if (base) base->lpVtbl->Disconnect(base);
    CStdStubBuffer_Disconnect(This);
}

/* every stub is allocated as a DelegStub (its base stub NULL when it has none) */
RPCRTAPI ULONG STDMETHODCALLTYPE NdrCStdStubBuffer2_Release(IRpcStubBuffer *This, IPSFactoryBuffer *factory)
{
    CStdStubBuffer *sb = (CStdStubBuffer *)This;
    LONG n = InterlockedDecrement(&sb->RefCount);
    if (n) return (ULONG)n;
    This->lpVtbl->Disconnect(This);
    DelegStub *d = (DelegStub *)((char *)This - sizeof(void *));
    if (d->base_stub) d->base_stub->lpVtbl->Release(d->base_stub);
    if (factory) factory->lpVtbl->Release(factory);
    HeapFree(GetProcessHeap(), 0, d);
    return 0;
}
RPCRTAPI ULONG STDMETHODCALLTYPE NdrCStdStubBuffer_Release(IRpcStubBuffer *This, IPSFactoryBuffer *factory)
{
    return NdrCStdStubBuffer2_Release(This, factory);
}

/* ---- the proxy/stub class object ---------------------------------------- */
static HRESULT STDMETHODCALLTYPE cf_qi(IPSFactoryBuffer *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPSFactoryBuffer)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_addref(IPSFactoryBuffer *This) { return (ULONG)InterlockedIncrement(&((CStdPSFactoryBuffer *)This)->RefCount); }
static ULONG STDMETHODCALLTYPE cf_release(IPSFactoryBuffer *This) { return (ULONG)InterlockedDecrement(&((CStdPSFactoryBuffer *)This)->RefCount); }

static HRESULT STDMETHODCALLTYPE cf_create_proxy(IPSFactoryBuffer *This, IUnknown *outer, REFIID riid, IRpcProxyBuffer **proxy, void **ppv)
{
    CStdPSFactoryBuffer *cf = (CStdPSFactoryBuffer *)This;
    const ProxyFileInfo *file;
    int i;
    *proxy = NULL;
    *ppv = NULL;
    if (!outer) return E_INVALIDARG;
    if (!find_iid(cf->pProxyFileList, riid, &file, &i)) return E_NOINTERFACE;
    const CInterfaceStubVtbl *sv = file->pStubVtblList[i];
    const CInterfaceProxyVtbl *v = proxy_vtbl(file->pProxyVtblList[i], sv->header.DispatchTableCount, file->TableVersion & 2);
    if (!v) return E_OUTOFMEMORY;
    StdProxy *px = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *px);
    if (!px) return E_OUTOFMEMORY;
    px->lpVtbl = &g_px_vtbl;
    px->pProxyVtbl = v->Vtbl;
    px->refs = 1;
    px->outer = outer;
    px->iid = sv->header.piid;
    px->info = file->TableVersion & 2 ? v->header.pStublessProxyInfo : NULL;
    px->factory = This;
    This->lpVtbl->AddRef(This);
    const IID *base = delegated_base(file, i);
    if (base) {
        IPSFactoryBuffer *bf;
        HRESULT hr = ndr_ps_factory(base, &bf);
        if (SUCCEEDED(hr)) {
            hr = bf->lpVtbl->CreateProxy(bf, outer, base, &px->base_buf, (void **)&px->base_proxy);
            bf->lpVtbl->Release(bf);
        }
        if (FAILED(hr)) {
            px->base_buf = NULL;
            px_release((IRpcProxyBuffer *)px);
            return hr;
        }
        outer->lpVtbl->Release(outer);           /* (no cycle through the outer object) */
    }
    *proxy = (IRpcProxyBuffer *)px;
    *ppv = &px->pProxyVtbl;
    outer->lpVtbl->AddRef(outer);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cf_create_stub(IPSFactoryBuffer *This, REFIID riid, IUnknown *server, IRpcStubBuffer **stub)
{
    CStdPSFactoryBuffer *cf = (CStdPSFactoryBuffer *)This;
    const ProxyFileInfo *file;
    int i;
    *stub = NULL;
    if (!find_iid(cf->pProxyFileList, riid, &file, &i)) return E_NOINTERFACE;
    const IID *base = delegated_base(file, i);
    const CInterfaceStubVtbl *v = stub_vtbl(file->pStubVtblList[i], base != NULL);
    if (!v) return E_OUTOFMEMORY;
    DelegStub *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    if (!d) return E_OUTOFMEMORY;
    CStdStubBuffer *sb = &d->sb;
    sb->lpVtbl = &v->Vtbl;
    sb->RefCount = 1;
    sb->pPSFactory = This;
    This->lpVtbl->AddRef(This);
    if (base) {
        IPSFactoryBuffer *bf;
        HRESULT hr = ndr_ps_factory(base, &bf);
        if (SUCCEEDED(hr)) {
            hr = bf->lpVtbl->CreateStub(bf, base, NULL, &d->base_stub);
            bf->lpVtbl->Release(bf);
        }
        if (FAILED(hr)) {
            d->base_stub = NULL;
            sb->lpVtbl->Release((IRpcStubBuffer *)sb);
            return hr;
        }
    }
    if (server) {
        HRESULT hr = sb->lpVtbl->Connect((IRpcStubBuffer *)sb, server);
        if (FAILED(hr)) {
            sb->lpVtbl->Release((IRpcStubBuffer *)sb);
            return hr;
        }
    }
    *stub = (IRpcStubBuffer *)sb;
    return S_OK;
}
static const IPSFactoryBufferVtbl g_cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create_proxy, cf_create_stub };

RPCRTAPI HRESULT RPC_ENTRY NdrDllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv, const ProxyFileInfo **list,
                                                const CLSID *psclsid, CStdPSFactoryBuffer *cf)
{
    const ProxyFileInfo *file;
    int i;
    *ppv = NULL;
    if (!cf->lpVtbl) {
        cf->pProxyFileList = list;
        InterlockedCompareExchangePointer((void **)&cf->lpVtbl, (void *)&g_cf_vtbl, NULL);
    }
    if ((psclsid && IsEqualCLSID(clsid, psclsid)) || find_iid(list, clsid, &file, &i))
        return cf_qi((IPSFactoryBuffer *)cf, riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}
RPCRTAPI HRESULT RPC_ENTRY NdrDllCanUnloadNow(CStdPSFactoryBuffer *cf)
{
    return cf->RefCount ? S_FALSE : S_OK;
}

/* ---- registration (regsvr32) -------------------------------------------- */
static WCHAR *put_hex(WCHAR *o, unsigned long v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) *o++ = L"0123456789ABCDEF"[(v >> (4 * i)) & 15];
    return o;
}
static void guid_text(const GUID *g, WCHAR *o)
{
    *o++ = '{';
    o = put_hex(o, g->Data1, 8);
    *o++ = '-';
    o = put_hex(o, g->Data2, 4);
    *o++ = '-';
    o = put_hex(o, g->Data3, 4);
    *o++ = '-';
    for (int i = 0; i < 8; i++) {
        if (i == 2) *o++ = '-';
        o = put_hex(o, g->Data4[i], 2);
    }
    *o++ = '}';
    *o = 0;
}
/* "a" "b" "c" -> out */
static void cat3(WCHAR *out, const WCHAR *a, const WCHAR *b, const WCHAR *c)
{
    lstrcpyW(out, a);
    lstrcatW(out, b);
    if (c) lstrcatW(out, c);
}
static LSTATUS set_value(HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *value)
{
    HKEY k;
    LSTATUS e = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
    if (e) return e;
    e = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value, (DWORD)(lstrlenW(value) + 1) * sizeof(WCHAR));
    RegCloseKey(k);
    return e;
}

RPCRTAPI HRESULT RPC_ENTRY NdrDllRegisterProxy(HMODULE dll, const ProxyFileInfo **list, const CLSID *psclsid)
{
    WCHAR path[MAX_PATH], key[128], clsid[40], iid[40], num[16];
    if (!list || !*list) return E_INVALIDARG;
    if (!GetModuleFileNameW(dll, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    const IID *first = (*list)->pStubVtblList[0] ? (*list)->pStubVtblList[0]->header.piid : NULL;
    guid_text(psclsid ? psclsid : first, clsid);
    for (const ProxyFileInfo **l = list; *l; l++)
        for (int i = 0; i < (*l)->TableSize; i++) {
            const CInterfaceStubVtbl *sv = (*l)->pStubVtblList[i];
            WCHAR name[128];
            int n = 0;
            for (const char *c = (*l)->pNamesArray[i]; c && *c && n < 127; c++) name[n++] = (WCHAR)(BYTE)*c;
            name[n] = 0;
            guid_text(sv->header.piid, iid);
            cat3(key, L"Interface\\", iid, NULL);
            set_value(HKEY_CLASSES_ROOT, key, NULL, name);
            cat3(key, L"Interface\\", iid, L"\\ProxyStubClsid32");
            set_value(HKEY_CLASSES_ROOT, key, NULL, clsid);
            cat3(key, L"Interface\\", iid, L"\\NumMethods");
            { unsigned long v = sv->header.DispatchTableCount; WCHAR t[12], *q = t + 11; *q = 0; do *--q = (WCHAR)(L'0' + v % 10); while (v /= 10); lstrcpyW(num, q); }
            set_value(HKEY_CLASSES_ROOT, key, NULL, num);
        }
    cat3(key, L"CLSID\\", clsid, NULL);
    set_value(HKEY_CLASSES_ROOT, key, NULL, L"PSFactoryBuffer");
    cat3(key, L"CLSID\\", clsid, L"\\InprocServer32");
    LSTATUS e = set_value(HKEY_CLASSES_ROOT, key, NULL, path);
    if (!e) e = set_value(HKEY_CLASSES_ROOT, key, L"ThreadingModel", L"Both");
    return e ? HRESULT_FROM_WIN32(e) : S_OK;
}

RPCRTAPI HRESULT RPC_ENTRY NdrDllUnregisterProxy(HMODULE dll, const ProxyFileInfo **list, const CLSID *psclsid)
{
    (void)dll;
    WCHAR key[128], clsid[40], iid[40];
    if (!list || !*list) return E_INVALIDARG;
    const IID *first = (*list)->pStubVtblList[0] ? (*list)->pStubVtblList[0]->header.piid : NULL;
    guid_text(psclsid ? psclsid : first, clsid);
    for (const ProxyFileInfo **l = list; *l; l++)
        for (int i = 0; i < (*l)->TableSize; i++) {
            guid_text((*l)->pStubVtblList[i]->header.piid, iid);
            cat3(key, L"Interface\\", iid, NULL);
            RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
        }
    cat3(key, L"CLSID\\", clsid, NULL);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
    return S_OK;
}

/* ---- the older (/Os) proxies and stubs ---------------------------------- */
RPCRTAPI void RPC_ENTRY NdrProxyInitialize(void *This, PRPC_MESSAGE msg, PMIDL_STUB_MESSAGE sm, PMIDL_STUB_DESC desc, unsigned int proc)
{
    memset(msg, 0, sizeof *msg);
    memset(sm, 0, sizeof *sm);
    sm->RpcMsg = msg;
    sm->StubDesc = desc;
    sm->IsClient = 1;
    sm->pfnAllocate = desc->pfnAllocate;
    sm->pfnFree = desc->pfnFree;
    msg->ProcNum = proc;
    msg->RpcInterfaceInformation = desc->RpcInterfaceInformation;
    msg->DataRepresentation = NDR_LOCAL_DATA_REPRESENTATION;
    IRpcChannelBuffer *chan = PROXY_FROM_IFACE(This)->chan;
    if (!chan) RpcRaiseException(CO_E_OBJNOTCONNECTED);
    sm->pRpcChannelBuffer = chan;
    chan->lpVtbl->AddRef(chan);
    chan->lpVtbl->GetDestCtx(chan, &sm->dwDestContext, &sm->pvDestContext);
}
RPCRTAPI void RPC_ENTRY NdrProxyGetBuffer(void *This, PMIDL_STUB_MESSAGE sm)
{
    StdProxy *px = PROXY_FROM_IFACE(This);
    sm->RpcMsg->BufferLength = sm->BufferLength;
    HRESULT hr = sm->pRpcChannelBuffer->lpVtbl->GetBuffer(sm->pRpcChannelBuffer, (RPCOLEMESSAGE *)sm->RpcMsg, px->iid);
    if (FAILED(hr)) RpcRaiseException(hr);
    sm->fBufferValid = 1;
    sm->Buffer = sm->BufferStart = sm->RpcMsg->Buffer;
    sm->BufferEnd = sm->Buffer + sm->RpcMsg->BufferLength;
}
RPCRTAPI void RPC_ENTRY NdrProxySendReceive(void *This, PMIDL_STUB_MESSAGE sm)
{
    (void)This;
    ULONG status = 0;
    sm->RpcMsg->BufferLength = (unsigned)(sm->Buffer - (unsigned char *)sm->RpcMsg->Buffer);
    HRESULT hr = sm->pRpcChannelBuffer->lpVtbl->SendReceive(sm->pRpcChannelBuffer, (RPCOLEMESSAGE *)sm->RpcMsg, &status);
    if (FAILED(hr)) RpcRaiseException(hr);
    sm->Buffer = sm->BufferStart = sm->RpcMsg->Buffer;
    sm->BufferEnd = sm->Buffer + sm->RpcMsg->BufferLength;
}
RPCRTAPI void RPC_ENTRY NdrProxyFreeBuffer(void *This, PMIDL_STUB_MESSAGE sm)
{
    (void)This;
    if (!sm->pRpcChannelBuffer) return;
    if (sm->fBufferValid) sm->pRpcChannelBuffer->lpVtbl->FreeBuffer(sm->pRpcChannelBuffer, (RPCOLEMESSAGE *)sm->RpcMsg);
    sm->fBufferValid = 0;
    sm->pRpcChannelBuffer->lpVtbl->Release(sm->pRpcChannelBuffer);
    sm->pRpcChannelBuffer = NULL;
}
RPCRTAPI HRESULT RPC_ENTRY NdrProxyErrorHandler(DWORD code)
{
    return (LONG)code < 0 ? (HRESULT)code : HRESULT_FROM_WIN32(code);
}
RPCRTAPI void RPC_ENTRY NdrStubInitialize(PRPC_MESSAGE msg, PMIDL_STUB_MESSAGE sm, PMIDL_STUB_DESC desc, IRpcChannelBuffer *chan)
{
    memset(sm, 0, sizeof *sm);
    sm->RpcMsg = msg;
    sm->StubDesc = desc;
    sm->pfnAllocate = desc->pfnAllocate;
    sm->pfnFree = desc->pfnFree;
    sm->pRpcChannelBuffer = chan;
    sm->Buffer = sm->BufferStart = msg->Buffer;
    sm->BufferEnd = sm->Buffer + msg->BufferLength;
    chan->lpVtbl->GetDestCtx(chan, &sm->dwDestContext, &sm->pvDestContext);
}
RPCRTAPI void RPC_ENTRY NdrStubGetBuffer(IRpcStubBuffer *This, IRpcChannelBuffer *chan, PMIDL_STUB_MESSAGE sm)
{
    sm->RpcMsg->BufferLength = sm->BufferLength;
    HRESULT hr = chan->lpVtbl->GetBuffer(chan, (RPCOLEMESSAGE *)sm->RpcMsg, STUB_HEADER(This)->piid);
    if (FAILED(hr)) RpcRaiseException(hr);
    sm->Buffer = sm->BufferStart = sm->RpcMsg->Buffer;
    sm->BufferEnd = sm->Buffer + sm->RpcMsg->BufferLength;
}
RPCRTAPI void RPC_ENTRY NdrClearOutParameters(PMIDL_STUB_MESSAGE sm, PFORMAT_STRING f, void *arg)
{
    if (!arg) return;
    unsigned long n = ndr_is_pointer(f[0]) ? sizeof(void *) : ndr_memsize(sm, f);
    if (n) memset(arg, 0, n);
}

RPCRTAPI void RPC_ENTRY RpcRaiseException(RPC_STATUS status)
{
    RaiseException((DWORD)status, EXCEPTION_NONCONTINUABLE, 0, NULL);
}
