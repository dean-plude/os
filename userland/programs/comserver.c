/* comserver.exe — the local server comoop starts: ole32 runs it with
 * -Embedding when comoop's CoCreateInstance(CLSCTX_LOCAL_SERVER) finds
 * nobody serving the class (HKCR\CLSID\{...}\LocalServer32).  Like an ATL
 * server it registers its class objects suspended, resumes them, takes
 * calls in its single-threaded apartment's message loop and quits when
 * its last object and lock are gone.  The object is INdrTest and INdrDual
 * (a dual interface: its proxies are ndrtestps.dll's and oleaut32's) and
 * answers IDispatch by name. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#define COBJMACROS
#include <objbase.h>
#include <oleauto.h>
#include "../ndrtestps/ndrtest.h"
#include "../ndrtestps/ndrdual.h"

static const CLSID CLSID_ComTest64 = { 0x6e0f3a10, 0x4d2b, 0x4c55, { 0x9a, 0x51, 0x7c, 0x0d, 0xe0, 0xa1, 0xc0, 0x64 } };
static const CLSID CLSID_ComTest32 = { 0x6e0f3a10, 0x4d2b, 0x4c55, { 0x9a, 0x51, 0x7c, 0x0d, 0xe0, 0xa1, 0xc0, 0x32 } };

static DWORD g_main_tid;
static LONG g_locks;                       /* objects alive + LockServer(TRUE) */

static void lock_server(void) { InterlockedIncrement(&g_locks); }
static void unlock_server(void)
{
    if (!InterlockedDecrement(&g_locks)) PostThreadMessageW(g_main_tid, WM_QUIT, 0, 0);
}

/* ---- the object ------------------------------------------------------------ */
typedef struct { INdrTest test; INdrDual dual; LONG refs; } Obj;
#define OBJ_T(p) ((Obj *)(p))
#define OBJ_D(p) ((Obj *)((char *)(p) - offsetof(Obj, dual)))

static HRESULT obj_qi(Obj *o, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_INdrBase) || IsEqualIID(riid, &IID_INdrTest)) *ppv = &o->test;
    else if (IsEqualIID(riid, &IID_INdrDual) || IsEqualIID(riid, &IID_IDispatch)) *ppv = &o->dual;
    else { *ppv = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&o->refs);
    return S_OK;
}
static ULONG obj_release(Obj *o)
{
    LONG r = InterlockedDecrement(&o->refs);
    if (!r) { HeapFree(GetProcessHeap(), 0, o); unlock_server(); }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE t_qi(INdrTest *This, REFIID riid, void **ppv) { return obj_qi(OBJ_T(This), riid, ppv); }
static ULONG STDMETHODCALLTYPE t_addref(INdrTest *This) { return (ULONG)InterlockedIncrement(&OBJ_T(This)->refs); }
static ULONG STDMETHODCALLTYPE t_release(INdrTest *This) { return obj_release(OBJ_T(This)); }
static HRESULT STDMETHODCALLTYPE t_add(INdrTest *This, LONG a, LONG b, LONG *sum) { (void)This; *sum = a + b; return S_OK; }
static HRESULT STDMETHODCALLTYPE t_negate(INdrTest *This, LONG *v) { (void)This; *v = -*v; return S_OK; }
static HRESULT STDMETHODCALLTYPE t_echo(INdrTest *This, const WCHAR *in, WCHAR **out)
{
    (void)This;
    *out = CoTaskMemAlloc((wcslen(in) + 16) * sizeof(WCHAR));
    if (!*out) return E_OUTOFMEMORY;
    wcscpy(*out, L"server: ");
    wcscat(*out, in);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_sum(INdrTest *This, ULONG n, const LONG *v, hyper *total)
{
    (void)This;
    hyper t = 0;
    for (ULONG i = 0; i < n; i++) t += v[i];
    *total = t;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_fill(INdrTest *This, ULONG n, BYTE seed, BYTE *buf)
{
    (void)This;
    for (ULONG i = 0; i < n; i++) buf[i] = (BYTE)(seed + i);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_scale(INdrTest *This, double d, float f, short s, BYTE c, hyper h, double e, double *r)
{ (void)This; *r = d * f + s + c + (double)h + e; return S_OK; }
static HRESULT STDMETHODCALLTYPE t_move(INdrTest *This, NdrPoint by, NdrPoint *p) { (void)This; p->x += by.x; p->y += by.y; p->z += by.z; return S_OK; }
static HRESULT STDMETHODCALLTYPE t_maybe(INdrTest *This, LONG *v, LONG *got) { (void)This; *got = v ? *v : -1; return S_OK; }
static HRESULT STDMETHODCALLTYPE t_list(INdrTest *This, ULONG n, ULONG *count, LONG **items) { (void)This; (void)n; *count = 0; *items = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE t_rename(INdrTest *This, NdrNamed *n) { (void)This; (void)n; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE t_total(INdrTest *This, NdrVec *v, LONG *total) { (void)This; (void)v; *total = 0; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE t_fail(INdrTest *This, HRESULT hr)
{
    (void)This;
    if (hr == (HRESULT)0x8badf00dL) *(volatile int *)0 = 1;    /* the stub catches it: RPC_E_SERVERFAULT */
    return hr;
}
static INdrTestVtbl g_test_vtbl = {
    t_qi, t_addref, t_release, t_add, t_negate,
    t_echo, t_sum, t_fill, t_scale, t_move, t_maybe, t_list, t_rename, t_total, t_fail,
};

static BSTR cat(const WCHAR *a, const WCHAR *b)
{
    size_t na = a ? wcslen(a) : 0, nb = b ? wcslen(b) : 0;
    BSTR r = SysAllocStringLen(NULL, (UINT)(na + nb));
    if (!r) return NULL;
    if (na) memcpy(r, a, na * sizeof(WCHAR));
    if (nb) memcpy(r + na, b, nb * sizeof(WCHAR));
    r[na + nb] = 0;
    return r;
}

static HRESULT STDMETHODCALLTYPE d_qi(INdrDual *This, REFIID riid, void **ppv) { return obj_qi(OBJ_D(This), riid, ppv); }
static ULONG STDMETHODCALLTYPE d_addref(INdrDual *This) { return (ULONG)InterlockedIncrement(&OBJ_D(This)->refs); }
static ULONG STDMETHODCALLTYPE d_release(INdrDual *This) { return obj_release(OBJ_D(This)); }
static HRESULT STDMETHODCALLTYPE d_count(INdrDual *This, UINT *n) { (void)This; *n = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE d_typeinfo(INdrDual *This, UINT i, LCID lcid, IUnknown **ti) { (void)This; (void)i; (void)lcid; *ti = NULL; return E_NOTIMPL; }

static const WCHAR *const g_names[] = { NULL, L"Concat", L"Bump", L"Kind", L"Fail", L"Callback", L"Pid", L"Sum" };
static HRESULT STDMETHODCALLTYPE d_getids(INdrDual *This, REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids)
{
    (void)This; (void)riid; (void)lcid;
    HRESULT hr = S_OK;
    for (UINT i = 0; i < n; i++) {
        ids[i] = DISPID_UNKNOWN;
        for (int k = 1; k < 8; k++) if (!_wcsicmp(names[i], g_names[k])) ids[i] = k;
        if (ids[i] == DISPID_UNKNOWN) hr = DISP_E_UNKNOWNNAME;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE d_invoke(INdrDual *This, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *dp,
                                          VARIANT *result, EXCEPINFO *ei, UINT *argerr)
{
    (void)This; (void)riid; (void)lcid; (void)flags;
    VARIANT *a = dp->rgvarg;                        /* (the last argument first) */
    switch (id) {
    case 1:                                         /* Concat(a, b) */
        if (dp->cArgs != 2 || V_VT(&a[0]) != VT_BSTR || V_VT(&a[1]) != VT_BSTR) { if (argerr) *argerr = 0; return DISP_E_TYPEMISMATCH; }
        if (result) { V_VT(result) = VT_BSTR; V_BSTR(result) = cat(V_BSTR(&a[1]), V_BSTR(&a[0])); }
        return S_OK;
    case 2:                                         /* Bump(&long, &bstr) */
        if (dp->cArgs != 2 || V_VT(&a[1]) != (VT_BYREF | VT_I4) || V_VT(&a[0]) != (VT_BYREF | VT_BSTR)) return DISP_E_TYPEMISMATCH;
        *a[1].plVal += 1;
        {
            BSTR n = cat(L"bumped ", *a[0].pbstrVal);
            SysFreeString(*a[0].pbstrVal);
            *a[0].pbstrVal = n;
        }
        return S_OK;
    case 3:                                         /* Kind(v): its type, and a text form */
        if (dp->cArgs != 1) return DISP_E_BADPARAMCOUNT;
        if (result) {
            VARIANT t;
            VariantInit(&t);
            WCHAR buf[160];
            HRESULT hr = V_VT(&a[0]) & VT_ARRAY ? E_FAIL : VariantChangeType(&t, &a[0], 0, VT_BSTR);
            swprintf(buf, 160, L"%u:%ls", (unsigned)V_VT(&a[0]), SUCCEEDED(hr) && V_BSTR(&t) ? V_BSTR(&t) : L"?");
            VariantClear(&t);
            V_VT(result) = VT_BSTR;
            V_BSTR(result) = SysAllocString(buf);
        }
        return S_OK;
    case 4:                                         /* Fail: an exception */
        if (ei) {
            memset(ei, 0, sizeof *ei);
            ei->wCode = 1001;
            ei->bstrSource = SysAllocString(L"comserver");
            ei->bstrDescription = SysAllocString(L"failed on purpose");
            ei->scode = E_ABORT;
        }
        return DISP_E_EXCEPTION;
    case 5: {                                       /* Callback(disp): calls it back */
        if (dp->cArgs != 1 || V_VT(&a[0]) != VT_DISPATCH || !V_DISPATCH(&a[0])) return DISP_E_TYPEMISMATCH;
        IDispatch *cb = V_DISPATCH(&a[0]);
        VARIANT args[2], r;
        V_VT(&args[1]) = VT_BSTR; V_BSTR(&args[1]) = SysAllocString(L"ping");
        V_VT(&args[0]) = VT_BSTR; V_BSTR(&args[0]) = SysAllocString(L" from the server");
        DISPPARAMS p = { args, NULL, 2, 0 };
        VariantInit(&r);
        HRESULT hr = cb->lpVtbl->Invoke(cb, 1, &IID_NULL, 0, DISPATCH_METHOD, &p, &r, NULL, NULL);
        VariantClear(&args[0]);
        VariantClear(&args[1]);
        if (FAILED(hr)) return hr;
        if (result) { V_VT(result) = VT_BSTR; V_BSTR(result) = cat(L"pong: ", V_VT(&r) == VT_BSTR ? V_BSTR(&r) : L"?"); }
        VariantClear(&r);
        return S_OK;
    }
    case 6:                                         /* Pid */
        if (result) { V_VT(result) = VT_I4; V_I4(result) = (LONG)GetCurrentProcessId(); }
        return S_OK;
    case 7: {                                       /* Sum(SAFEARRAY of long) */
        if (dp->cArgs != 1 || V_VT(&a[0]) != (VT_ARRAY | VT_I4)) return DISP_E_TYPEMISMATCH;
        SAFEARRAY *sa = V_ARRAY(&a[0]);
        LONG lo = 0, hi = -1, total = 0, v;
        SafeArrayGetLBound(sa, 1, &lo);
        SafeArrayGetUBound(sa, 1, &hi);
        for (LONG i = lo; i <= hi; i++) { SafeArrayGetElement(sa, &i, &v); total += v; }
        if (result) { V_VT(result) = VT_I4; V_I4(result) = total; }
        return S_OK;
    }
    }
    return DISP_E_MEMBERNOTFOUND;
}

static HRESULT STDMETHODCALLTYPE d_concat(INdrDual *This, BSTR a, BSTR b, BSTR *r) { (void)This; *r = cat(a, b); return *r ? S_OK : E_OUTOFMEMORY; }
static HRESULT STDMETHODCALLTYPE d_describe(INdrDual *This, VARIANT v, VARIANT *r)
{
    (void)This;
    VARIANT t;
    VariantInit(&t);
    WCHAR buf[160];
    HRESULT hr = VariantChangeType(&t, &v, 0, VT_BSTR);
    swprintf(buf, 160, L"%u:%ls", (unsigned)V_VT(&v), SUCCEEDED(hr) && V_BSTR(&t) ? V_BSTR(&t) : L"?");
    VariantClear(&t);
    V_VT(r) = VT_BSTR;
    V_BSTR(r) = SysAllocString(buf);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_twice(INdrDual *This, VARIANT *v)
{
    (void)This;
    if (V_VT(v) == VT_I4) { V_I4(v) *= 2; return S_OK; }
    if (V_VT(v) == VT_BSTR) {
        BSTR n = cat(V_BSTR(v), V_BSTR(v));
        SysFreeString(V_BSTR(v));
        V_BSTR(v) = n;
        return S_OK;
    }
    return DISP_E_TYPEMISMATCH;
}
static HRESULT STDMETHODCALLTYPE d_pid(INdrDual *This, LONG *pid) { (void)This; *pid = (LONG)GetCurrentProcessId(); return S_OK; }
static HRESULT STDMETHODCALLTYPE d_call(INdrDual *This, INdrDual *cb, BSTR text, BSTR *r)
{
    (void)This;
    BSTR got = NULL;
    LONG pid = 0;
    *r = NULL;
    if (!cb) return E_POINTER;
    HRESULT hr = INdrDual_Concat(cb, text, L"!", &got);           /* calls back into the client */
    if (SUCCEEDED(hr)) hr = INdrDual_Pid(cb, &pid);
    if (SUCCEEDED(hr)) {
        WCHAR buf[200];
        swprintf(buf, 200, L"%ls %ld", got ? got : L"", (long)pid);
        *r = SysAllocString(buf);
    }
    SysFreeString(got);
    return hr;
}
static INdrDualVtbl g_dual_vtbl = {
    d_qi, d_addref, d_release, d_count, d_typeinfo, d_getids, d_invoke,
    d_concat, d_describe, d_twice, d_pid, d_call,
};

/* ---- the class object ------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE cf_qi(IClassFactory *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) { *ppv = This; return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_addref(IClassFactory *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE cf_release(IClassFactory *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE cf_create(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv)
{
    (void)This;
    *ppv = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    Obj *o = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *o);
    if (!o) return E_OUTOFMEMORY;
    o->test.lpVtbl = &g_test_vtbl;
    o->dual.lpVtbl = &g_dual_vtbl;
    o->refs = 1;
    lock_server();
    HRESULT hr = obj_qi(o, riid, ppv);
    obj_release(o);
    return hr;
}
static HRESULT STDMETHODCALLTYPE cf_lock(IClassFactory *This, BOOL lock)
{
    (void)This;
    if (lock) lock_server();
    else unlock_server();
    return S_OK;
}
static IClassFactoryVtbl g_cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static IClassFactory g_cf = { &g_cf_vtbl };

int main(int argc, char **argv)
{
    int embedding = 0;
    for (int i = 1; i < argc; i++)
        if (!_stricmp(argv[i], "-Embedding") || !_stricmp(argv[i], "/Embedding")) embedding = 1;
    if (!embedding) {
        printf("comserver: a COM local server for comoop; ole32 starts it with -Embedding\n");
        return 1;
    }
    g_main_tid = GetCurrentThreadId();
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 2;
    DWORD c64 = 0, c32 = 0;
    CoRegisterClassObject(&CLSID_ComTest64, (IUnknown *)&g_cf, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED, &c64);
    CoRegisterClassObject(&CLSID_ComTest32, (IUnknown *)&g_cf, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED, &c32);
    CoResumeClassObjects();
    SetTimer(NULL, 0, 180000, NULL);               /* nobody came: give up */
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_TIMER && !m.hwnd) break;
        DispatchMessageW(&m);
    }
    CoRevokeClassObject(c64);
    CoRevokeClassObject(c32);
    CoUninitialize();
    return 0;
}
