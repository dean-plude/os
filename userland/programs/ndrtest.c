/* ndrtest.exe — rpcrt4's NDR engine (what COM proxy/stub DLLs such as
 * Edge Update's psmachine.dll run on), in one process: ndrtestps.dll is a
 * proxy/stub DLL made with widl from ../ndrtestps/idl, and this program
 * registers it (DllRegisterServer: NdrDllRegisterProxy), finds it again
 * through ole32 (CoGetPSClsid, CoGetClassObject: NdrDllGetClassObject),
 * makes an interface stub around a server object (CStdStubBuffer) and an
 * interface proxy (stubless thunks) and joins the two with a channel that
 * hands each request straight to the stub's Invoke (NdrStubCall2).  Every
 * call is then marshaled into a buffer, unmarshaled on the "server" side,
 * run, and its results marshaled back, as across processes.
 *
 * INdrTest derives from INdrBase, which is in another proxy file, so its
 * base methods go through a delegated proxy and a forwarding stub. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#define COBJMACROS
#include <objbase.h>
#include "../ndrtestps/ndrtest.h"

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static const CLSID CLSID_NdrTestPS = { 0x6e0f3a10, 0x4d2b, 0x4c55, { 0x9a, 0x51, 0x7c, 0x0d, 0xe0, 0xa1, 0xb0, 0xff } };
#define HR_CRASH ((HRESULT)0x8badf00dL)

/* ---- the server object ---------------------------------------------------- */
typedef struct { INdrTest iface; LONG refs; LONG calls; } Server;
static Server *srv_of(INdrTest *p) { return (Server *)p; }

static HRESULT STDMETHODCALLTYPE s_qi(INdrTest *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_INdrBase) || IsEqualIID(riid, &IID_INdrTest)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE s_addref(INdrTest *This) { return (ULONG)InterlockedIncrement(&srv_of(This)->refs); }
static ULONG STDMETHODCALLTYPE s_release(INdrTest *This) { return (ULONG)InterlockedDecrement(&srv_of(This)->refs); }
static HRESULT STDMETHODCALLTYPE s_add(INdrTest *This, LONG a, LONG b, LONG *sum)
{
    srv_of(This)->calls++;
    *sum = a + b;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_negate(INdrTest *This, LONG *v)
{
    srv_of(This)->calls++;
    *v = -*v;
    return S_FALSE;
}
static HRESULT STDMETHODCALLTYPE s_echo(INdrTest *This, const WCHAR *in, WCHAR **out)
{
    srv_of(This)->calls++;
    size_t n = wcslen(in);
    *out = CoTaskMemAlloc((n + 8) * sizeof(WCHAR));
    if (!*out) return E_OUTOFMEMORY;
    wcscpy(*out, L"echo: ");
    wcscat(*out, in);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_sum(INdrTest *This, ULONG n, const LONG *v, hyper *total)
{
    srv_of(This)->calls++;
    hyper t = 0;
    for (ULONG i = 0; i < n; i++) t += v[i];
    *total = t * 1000000007LL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_fill(INdrTest *This, ULONG n, BYTE seed, BYTE *buf)
{
    srv_of(This)->calls++;
    for (ULONG i = 0; i < n; i++) buf[i] = (BYTE)(seed + i * 7);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_scale(INdrTest *This, double d, float f, short s, BYTE c, hyper h, double e, double *r)
{
    srv_of(This)->calls++;
    *r = d * f + s + c + (double)h + e;
    return d == 1.5 && f == 2.25f && s == -3 && c == 200 && h == 0x123456789LL && e == 0.125 ? S_OK : E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE s_move(INdrTest *This, NdrPoint by, NdrPoint *p)
{
    srv_of(This)->calls++;
    p->x += by.x;
    p->y += by.y;
    p->z += by.z;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_maybe(INdrTest *This, LONG *v, LONG *got)
{
    srv_of(This)->calls++;
    *got = v ? *v : -1;
    return v ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE s_list(INdrTest *This, ULONG n, ULONG *count, LONG **items)
{
    srv_of(This)->calls++;
    *count = n;
    *items = CoTaskMemAlloc(n * sizeof(LONG) + 1);
    for (ULONG i = 0; i < n; i++) (*items)[i] = (LONG)(i * i);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_rename(INdrTest *This, NdrNamed *n)
{
    srv_of(This)->calls++;
    n->id++;
    size_t len = n->name ? wcslen(n->name) : 0;
    WCHAR *s = CoTaskMemAlloc((len + 2) * sizeof(WCHAR));
    s[0] = L'~';
    if (n->name) wcscpy(s + 1, n->name);
    else s[1] = 0;
    CoTaskMemFree(n->name);                      /* [in, out]: the stub frees what is there after */
    n->name = s;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_total(INdrTest *This, NdrVec *v, LONG *total)
{
    srv_of(This)->calls++;
    LONG t = 0;
    for (ULONG i = 0; i < v->n; i++) t += v->items[i];
    *total = t;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_fail(INdrTest *This, HRESULT hr)
{
    srv_of(This)->calls++;
    if (hr == HR_CRASH) *(volatile int *)0 = 1;  /* the stub catches it: RPC_E_SERVERFAULT */
    return hr;
}
static INdrTestVtbl g_server_vtbl = {
    s_qi, s_addref, s_release, s_add, s_negate,
    s_echo, s_sum, s_fill, s_scale, s_move, s_maybe, s_list, s_rename, s_total, s_fail,
};

/* ---- the proxy's controlling ("outer") object ------------------------------ */
typedef struct { IUnknown iface; LONG refs; void *proxy; } Outer;
static HRESULT STDMETHODCALLTYPE o_qi(IUnknown *This, REFIID riid, void **ppv)
{
    Outer *o = (Outer *)This;
    if (IsEqualIID(riid, &IID_IUnknown)) *ppv = This;
    else if ((IsEqualIID(riid, &IID_INdrTest) || IsEqualIID(riid, &IID_INdrBase)) && o->proxy) *ppv = o->proxy;
    else { *ppv = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&o->refs);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE o_addref(IUnknown *This) { return (ULONG)InterlockedIncrement(&((Outer *)This)->refs); }
static ULONG STDMETHODCALLTYPE o_release(IUnknown *This) { return (ULONG)InterlockedDecrement(&((Outer *)This)->refs); }
static IUnknownVtbl g_outer_vtbl = { o_qi, o_addref, o_release };

/* ---- a channel that runs each call on the stub right away ----------------- */
typedef struct { IRpcChannelBuffer iface; LONG refs; IRpcStubBuffer *stub; LONG buffers; LONG sends; } Loop;
static HRESULT STDMETHODCALLTYPE c_qi(IRpcChannelBuffer *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRpcChannelBuffer)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE c_addref(IRpcChannelBuffer *This) { return (ULONG)InterlockedIncrement(&((Loop *)This)->refs); }
static ULONG STDMETHODCALLTYPE c_release(IRpcChannelBuffer *This) { return (ULONG)InterlockedDecrement(&((Loop *)This)->refs); }
static HRESULT STDMETHODCALLTYPE c_getbuffer(IRpcChannelBuffer *This, RPCOLEMESSAGE *msg, REFIID riid)
{
    (void)riid;
    /* a little more than asked for, filled with garbage: nothing may rely
     * on the buffer starting zeroed */
    unsigned char *b = HeapAlloc(GetProcessHeap(), 0, msg->cbBuffer + 16);
    if (!b) return E_OUTOFMEMORY;
    memset(b, 0xa5, msg->cbBuffer + 16);
    msg->Buffer = b;
    InterlockedIncrement(&((Loop *)This)->buffers);
    return S_OK;
}
static void put_back(Loop *l, void *b, ULONG n)
{
    memset(b, 0xdd, n);                          /* catch reads of freed buffers */
    HeapFree(GetProcessHeap(), 0, b);
    InterlockedDecrement(&l->buffers);
}
static HRESULT STDMETHODCALLTYPE c_sendreceive(IRpcChannelBuffer *This, RPCOLEMESSAGE *msg, ULONG *status)
{
    Loop *l = (Loop *)This;
    l->sends++;
    RPCOLEMESSAGE call = *msg;                   /* what the server's side sees */
    void *request = msg->Buffer;
    ULONG request_len = msg->cbBuffer;
    HRESULT hr = l->stub->lpVtbl->Invoke(l->stub, &call, This);
    *status = 0;
    if (FAILED(hr)) return hr;                   /* (the request is the caller's to free) */
    put_back(l, request, request_len);
    msg->Buffer = call.Buffer;                   /* the reply */
    msg->cbBuffer = call.cbBuffer;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_freebuffer(IRpcChannelBuffer *This, RPCOLEMESSAGE *msg)
{
    if (msg->Buffer) put_back((Loop *)This, msg->Buffer, msg->cbBuffer);
    msg->Buffer = NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_getdestctx(IRpcChannelBuffer *This, DWORD *ctx, void **pv)
{
    (void)This;
    *ctx = MSHCTX_LOCAL;
    if (pv) *pv = NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_isconnected(IRpcChannelBuffer *This) { (void)This; return S_OK; }
static IRpcChannelBufferVtbl g_loop_vtbl = { c_qi, c_addref, c_release, c_getbuffer, c_sendreceive, c_freebuffer, c_getdestctx, c_isconnected };

/* ---- registration ---------------------------------------------------------- */
static int reg_value(const WCHAR *key, WCHAR *out, DWORD size)
{
    return RegGetValueW(HKEY_CLASSES_ROOT, key, NULL, RRF_RT_REG_SZ, NULL, out, &size) == ERROR_SUCCESS;
}

static void registration(HMODULE dll)
{
    HRESULT (STDAPICALLTYPE *reg)(void) = (void *)GetProcAddress(dll, "DllRegisterServer");
    CHECK("DllRegisterServer exported", reg != NULL);
    if (!reg) return;
    CHECK("DllRegisterServer (NdrDllRegisterProxy)", reg() == S_OK);
    WCHAR v[MAX_PATH], self[MAX_PATH];
    CHECK("Interface\\{INdrTest} names it", reg_value(L"Interface\\{6E0F3A10-4D2B-4C55-9A51-7C0DE0A1B002}", v, sizeof v) && !wcscmp(v, L"INdrTest"));
    CHECK("ProxyStubClsid32 of INdrTest", reg_value(L"Interface\\{6E0F3A10-4D2B-4C55-9A51-7C0DE0A1B002}\\ProxyStubClsid32", v, sizeof v)
          && !_wcsicmp(v, L"{6E0F3A10-4D2B-4C55-9A51-7C0DE0A1B0FF}"));
    CHECK("ProxyStubClsid32 of INdrBase", reg_value(L"Interface\\{6E0F3A10-4D2B-4C55-9A51-7C0DE0A1B001}\\ProxyStubClsid32", v, sizeof v)
          && !_wcsicmp(v, L"{6E0F3A10-4D2B-4C55-9A51-7C0DE0A1B0FF}"));
    GetModuleFileNameW(dll, self, MAX_PATH);
    CHECK("InProcServer32 is the DLL", reg_value(L"CLSID\\{6E0F3A10-4D2B-4C55-9A51-7C0DE0A1B0FF}\\InProcServer32", v, sizeof v) && !_wcsicmp(v, self));
    CLSID ps;
    CHECK("CoGetPSClsid(INdrTest)", CoGetPSClsid(&IID_INdrTest, &ps) == S_OK && IsEqualCLSID(&ps, &CLSID_NdrTestPS));
    static const IID unknown_iid = { 0x6e0f3a10, 0x4d2b, 0x4c55, { 0x9a, 0x51, 0x7c, 0x0d, 0xe0, 0xa1, 0xb0, 0xee } };
    CHECK("CoGetPSClsid of an unregistered interface", CoGetPSClsid(&unknown_iid, &ps) == REGDB_E_IIDNOTREG);
}

static void unregistration(HMODULE dll)
{
    HRESULT (STDAPICALLTYPE *unreg)(void) = (void *)GetProcAddress(dll, "DllUnregisterServer");
    CHECK("DllUnregisterServer (NdrDllUnregisterProxy)", unreg && unreg() == S_OK);
    WCHAR v[MAX_PATH];
    CHECK("Interface key gone", !reg_value(L"Interface\\{6E0F3A10-4D2B-4C55-9A51-7C0DE0A1B002}\\ProxyStubClsid32", v, sizeof v));
    CHECK("CLSID key gone", !reg_value(L"CLSID\\{6E0F3A10-4D2B-4C55-9A51-7C0DE0A1B0FF}\\InProcServer32", v, sizeof v));
}

/* ---- the calls ------------------------------------------------------------- */
static void calls(INdrTest *p, Server *s, Loop *chan)
{
    LONG l = 0;
    LONG before = s->calls;
    CHECK("Add (delegated to INdrBase's proxy and stub)", INdrTest_Add(p, 40, 2, &l) == S_OK && l == 42);
    l = 17;
    CHECK("Negate [in, out], S_FALSE comes back", INdrTest_Negate(p, &l) == S_FALSE && l == -17);

    WCHAR *out = NULL;
    HRESULT hr = INdrTest_Echo(p, L"h\u00e9llo, w\u00f6rld", &out);
    CHECK("Echo: [string] in, [string] out", hr == S_OK && out && !wcscmp(out, L"echo: h\u00e9llo, w\u00f6rld"));
    if (hr != S_OK || !out) printf("  Echo: hr %08lx, out %p\n", (unsigned long)hr, (void *)out);
    CoTaskMemFree(out);
    out = NULL;
    CHECK("Echo of an empty string", INdrTest_Echo(p, L"", &out) == S_OK && out && !wcscmp(out, L"echo: "));
    CoTaskMemFree(out);

    LONG v[300];
    hyper want = 0;
    for (int i = 0; i < 300; i++) { v[i] = i * 31 - 4000; want += v[i]; }
    hyper total = 0;
    CHECK("Sum: [size_is] array in, hyper out", INdrTest_Sum(p, 300, v, &total) == S_OK && total == want * 1000000007LL);
    CHECK("Sum of nothing", INdrTest_Sum(p, 0, v, &total) == S_OK && total == 0);

    BYTE buf[1001];
    memset(buf, 0, sizeof buf);
    buf[1000] = 0x5a;
    int ok = INdrTest_Fill(p, 1000, 3, buf) == S_OK;
    for (int i = 0; i < 1000; i++) ok &= buf[i] == (BYTE)(3 + i * 7);
    CHECK("Fill: [out, size_is] array into the caller's buffer", ok && buf[1000] == 0x5a);

    double r = 0;
    CHECK("Scale: doubles, a float, short, byte and hyper in registers and on the stack",
          INdrTest_Scale(p, 1.5, 2.25f, -3, 200, 0x123456789LL, 0.125, &r) == S_OK && r == 1.5 * 2.25 - 3 + 200 + (double)0x123456789LL + 0.125);

    NdrPoint by = { 3, -70000, 0.5 }, pt = { 10, 100000, 1.25 };
    CHECK("Move: a struct by value and one [in, out]", INdrTest_Move(p, by, &pt) == S_OK && pt.x == 13 && pt.y == 30000 && pt.z == 1.75);

    LONG got = 0, nine = 9;
    CHECK("Maybe: [unique] pointer given", INdrTest_Maybe(p, &nine, &got) == S_OK && got == 9);
    CHECK("Maybe: [unique] pointer NULL", INdrTest_Maybe(p, NULL, &got) == S_FALSE && got == -1);

    ULONG count = 0;
    LONG *items = NULL;
    ok = INdrTest_List(p, 50, &count, &items) == S_OK && count == 50 && items;
    for (ULONG i = 0; ok && i < 50; i++) ok &= items[i] == (LONG)(i * i);
    CHECK("List: [out] array the proxy allocates, sized by another [out]", ok);
    CoTaskMemFree(items);

    NdrNamed named = { 41, CoTaskMemAlloc(sizeof L"nova") };
    wcscpy(named.name, L"nova");
    hr = INdrTest_Rename(p, &named);
    CHECK("Rename: struct with an embedded [string] pointer, [in, out]",
          hr == S_OK && named.id == 42 && named.name && !wcscmp(named.name, L"~nova"));
    if (hr != S_OK || named.id != 42) printf("  Rename: hr %08lx, id %ld\n", (unsigned long)hr, (long)named.id);
    CoTaskMemFree(named.name);

    NdrVec *vec = CoTaskMemAlloc(sizeof(NdrVec) + 99 * sizeof(LONG));
    vec->n = 100;
    for (int i = 0; i < 100; i++) vec->items[i] = i;
    l = 0;
    CHECK("Total: conformant struct", INdrTest_Total(p, vec, &l) == S_OK && l == 4950);
    CoTaskMemFree(vec);

    CHECK("Fail: the server's HRESULT comes back", INdrTest_Fail(p, E_ACCESSDENIED) == E_ACCESSDENIED);
    CHECK("Fail: a crash in the server is RPC_E_SERVERFAULT", INdrTest_Fail(p, HR_CRASH) == RPC_E_SERVERFAULT);
    CHECK("each call reached the server once", s->calls - before == 16);
    CHECK("every buffer went back to the channel", chan->buffers == 0);
    CHECK("each call was one send", chan->sends == 16);
}

int main(void)
{
    CHECK("CoInitializeEx", SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
    HMODULE dll = LoadLibraryW(L"ndrtestps.dll");
    CHECK("LoadLibrary(ndrtestps.dll)", dll != NULL);
    if (!dll) goto out;
    registration(dll);

    CLSID ps = CLSID_NdrTestPS;
    IPSFactoryBuffer *factory = NULL;
    CHECK("CoGetClassObject(proxy/stub class) gives an IPSFactoryBuffer",
          CoGetClassObject(&ps, CLSCTX_INPROC_SERVER, NULL, &IID_IPSFactoryBuffer, (void **)&factory) == S_OK && factory);
    if (!factory) goto unreg;
    HRESULT (STDAPICALLTYPE *can_unload)(void) = (void *)GetProcAddress(dll, "DllCanUnloadNow");
    CHECK("DllCanUnloadNow while the factory is held", can_unload && can_unload() == S_FALSE);

    Server server = { { &g_server_vtbl }, 1, 0 };
    IRpcStubBuffer *stub = NULL;
    CHECK("CreateStub", factory->lpVtbl->CreateStub(factory, &IID_INdrTest, (IUnknown *)&server.iface, &stub) == S_OK && stub);
    CHECK("the stub holds the server", server.refs > 1);
    IRpcStubBuffer *same = stub ? stub->lpVtbl->IsIIDSupported(stub, &IID_INdrTest) : NULL;
    CHECK("IsIIDSupported", stub && same == stub);
    if (same) same->lpVtbl->Release(same);

    Outer outer = { { &g_outer_vtbl }, 1, NULL };
    IRpcProxyBuffer *proxy = NULL;
    INdrTest *p = NULL;
    CHECK("CreateProxy", factory->lpVtbl->CreateProxy(factory, &outer.iface, &IID_INdrTest, &proxy, (void **)&p) == S_OK && proxy && p);
    if (!stub || !proxy || !p) goto release;
    outer.proxy = p;

    LONG l = 0;
    CHECK("a call before Connect fails", INdrTest_Add(p, 1, 2, &l) == CO_E_OBJNOTCONNECTED);
    Loop chan = { { &g_loop_vtbl }, 1, stub, 0, 0 };
    CHECK("Connect the proxy", proxy->lpVtbl->Connect(proxy, &chan.iface) == S_OK);

    /* IUnknown through the proxy goes to the outer object */
    LONG refs = outer.refs;
    INdrTest_AddRef(p);
    CHECK("the proxy's AddRef is the outer object's", outer.refs == refs + 1);
    INdrTest_Release(p);
    INdrBase *base = NULL;
    CHECK("the proxy's QueryInterface is the outer object's",
          INdrTest_QueryInterface(p, &IID_INdrBase, (void **)&base) == S_OK && base == (INdrBase *)p);
    if (base) INdrBase_Release(base);

    calls(p, &server, &chan);

    proxy->lpVtbl->Disconnect(proxy);
    CHECK("a call after Disconnect fails", INdrTest_Add(p, 1, 2, &l) == CO_E_OBJNOTCONNECTED);
    CHECK("the channel was released", chan.refs == 1);
release:
    if (proxy) proxy->lpVtbl->Release(proxy);
    if (stub) {
        stub->lpVtbl->Disconnect(stub);
        CHECK("Disconnect releases the server", server.refs == 1);
        stub->lpVtbl->Release(stub);
    }
    factory->lpVtbl->Release(factory);
    CHECK("DllCanUnloadNow once everything is released", can_unload && can_unload() == S_OK);
unreg:
    unregistration(dll);
out:
    CoUninitialize();
#ifdef _WIN64
    printf("ndrtest: %d passed, %d failed (64-bit)\n", pass, fail);
#else
    printf("ndrtest: %d passed, %d failed (32-bit)\n", pass, fail);
#endif
    return fail != 0;
}
