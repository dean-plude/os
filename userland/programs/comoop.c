/* comoop.exe — COM between processes.  CoCreateInstance with
 * CLSCTX_LOCAL_SERVER starts comserver.exe (its LocalServer32, with
 * -Embedding) and gets a proxy: calls go through ole32's standard
 * marshaler and channel to the server process and through
 * ndrtestps.dll's widl-made proxies and stubs (rpcrt4's NDR engine), the
 * dual interface's IDispatch part through oleaut32's, BSTRs and VARIANTs
 * through oleaut32's user-marshal routines.  This thread is a
 * single-threaded apartment, so the server's calls back into an object
 * of ours run here while we wait for its reply.  Then the server goes
 * away once everything is released, a second one starts for the class
 * object path, and the same runs against the other bitness's server
 * (a 32-bit client and a 64-bit server, and the other way round). */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#define COBJMACROS
#include <objbase.h>
#include <oleauto.h>
#include "../ndrtestps/ndrtest.h"
#include "../ndrtestps/ndrdual.h"

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static const CLSID CLSID_ComTest64 = { 0x6e0f3a10, 0x4d2b, 0x4c55, { 0x9a, 0x51, 0x7c, 0x0d, 0xe0, 0xa1, 0xc0, 0x64 } };
static const CLSID CLSID_ComTest32 = { 0x6e0f3a10, 0x4d2b, 0x4c55, { 0x9a, 0x51, 0x7c, 0x0d, 0xe0, 0xa1, 0xc0, 0x32 } };
static const CLSID CLSID_Nobody = { 0x6e0f3a10, 0x4d2b, 0x4c55, { 0x9a, 0x51, 0x7c, 0x0d, 0xe0, 0xa1, 0xc0, 0xee } };

/* ---- an object of ours the server calls back -------------------------------- */
typedef struct { INdrDual iface; LONG refs; LONG calls; DWORD tid_ok; } Cb;
static HRESULT STDMETHODCALLTYPE cb_qi(INdrDual *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_INdrDual)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cb_addref(INdrDual *This) { return (ULONG)InterlockedIncrement(&((Cb *)This)->refs); }
static ULONG STDMETHODCALLTYPE cb_release(INdrDual *This) { return (ULONG)InterlockedDecrement(&((Cb *)This)->refs); }
static DWORD g_main_tid;
static void cb_called(Cb *c) { c->calls++; if (GetCurrentThreadId() != g_main_tid) c->tid_ok = 0; }
static HRESULT STDMETHODCALLTYPE cb_count(INdrDual *This, UINT *n) { (void)This; *n = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE cb_typeinfo(INdrDual *This, UINT i, LCID l, IUnknown **t) { (void)This; (void)i; (void)l; *t = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE cb_getids(INdrDual *This, REFIID r, LPOLESTR *n, UINT c, LCID l, DISPID *ids)
{ (void)This; (void)r; (void)n; (void)l; for (UINT i = 0; i < c; i++) ids[i] = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE cb_invoke(INdrDual *This, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *dp,
                                           VARIANT *result, EXCEPINFO *ei, UINT *argerr)
{
    (void)riid; (void)lcid; (void)flags; (void)ei; (void)argerr;
    cb_called((Cb *)This);
    if (id != 1 || dp->cArgs != 2 || V_VT(&dp->rgvarg[0]) != VT_BSTR || V_VT(&dp->rgvarg[1]) != VT_BSTR) return DISP_E_MEMBERNOTFOUND;
    WCHAR buf[200];
    swprintf(buf, 200, L"client got %ls%ls", V_BSTR(&dp->rgvarg[1]), V_BSTR(&dp->rgvarg[0]));
    if (result) { V_VT(result) = VT_BSTR; V_BSTR(result) = SysAllocString(buf); }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cb_concat(INdrDual *This, BSTR a, BSTR b, BSTR *r)
{
    cb_called((Cb *)This);
    WCHAR buf[200];
    swprintf(buf, 200, L"%ls%ls", a ? a : L"", b ? b : L"");
    *r = SysAllocString(buf);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cb_describe(INdrDual *This, VARIANT v, VARIANT *r) { (void)This; (void)v; VariantInit(r); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE cb_twice(INdrDual *This, VARIANT *v) { (void)This; (void)v; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE cb_pid(INdrDual *This, LONG *pid) { cb_called((Cb *)This); *pid = (LONG)GetCurrentProcessId(); return S_OK; }
static HRESULT STDMETHODCALLTYPE cb_call(INdrDual *This, INdrDual *cb, BSTR t, BSTR *r) { (void)This; (void)cb; (void)t; *r = NULL; return E_NOTIMPL; }
static INdrDualVtbl g_cb_vtbl = { cb_qi, cb_addref, cb_release, cb_count, cb_typeinfo, cb_getids, cb_invoke,
                                  cb_concat, cb_describe, cb_twice, cb_pid, cb_call };

/* ---- registration ------------------------------------------------------------ */
static void register_server(const CLSID *clsid, const WCHAR *exe)
{
    WCHAR key[100], g[40];
    StringFromGUID2(clsid, g, 40);
    swprintf(key, 100, L"CLSID\\%ls\\LocalServer32", g);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE *)exe, (DWORD)((wcslen(exe) + 1) * sizeof(WCHAR)));
        RegCloseKey(k);
    }
}
static void unregister_server(const CLSID *clsid)
{
    WCHAR key[100], g[40];
    StringFromGUID2(clsid, g, 40);
    swprintf(key, 100, L"CLSID\\%ls", g);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
}

static BOOL wait_exit(DWORD pid, DWORD ms)
{
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return TRUE;                            /* gone already */
    BOOL ok = WaitForSingleObject(h, ms) == WAIT_OBJECT_0;
    CloseHandle(h);
    return ok;
}

static BOOL is(BSTR b, const WCHAR *s) { return b && !wcscmp(b, s); }

/* ---- the calls ------------------------------------------------------------------ */
static void dual_calls(INdrDual *d, LONG server_pid)
{
    BSTR r = NULL;
    BSTR a = SysAllocString(L"h\u00e9llo, "), b = SysAllocString(L"w\u00f6rld");
    CHECK("Concat: BSTRs in, BSTR out (BSTR_UserMarshal)", INdrDual_Concat(d, a, b, &r) == S_OK && is(r, L"h\u00e9llo, w\u00f6rld"));
    SysFreeString(r);
    r = NULL;
    CHECK("Concat: a NULL BSTR", INdrDual_Concat(d, NULL, b, &r) == S_OK && is(r, L"w\u00f6rld"));
    SysFreeString(r);
    SysFreeString(a);
    SysFreeString(b);

    VARIANT v, out;
    VariantInit(&v);
    VariantInit(&out);
    V_VT(&v) = VT_I4;
    V_I4(&v) = 42;
    CHECK("Describe: a VARIANT by value (VARIANT_UserMarshal)", INdrDual_Describe(d, v, &out) == S_OK && V_VT(&out) == VT_BSTR && is(V_BSTR(&out), L"3:42"));
    VariantClear(&out);
    V_VT(&v) = VT_BSTR;
    V_BSTR(&v) = SysAllocString(L"text");
    CHECK("Describe: a BSTR VARIANT", INdrDual_Describe(d, v, &out) == S_OK && is(V_BSTR(&out), L"8:text"));
    VariantClear(&out);
    VariantClear(&v);
    V_VT(&v) = VT_R8;
    V_R8(&v) = 2.5;
    CHECK("Describe: a double VARIANT", INdrDual_Describe(d, v, &out) == S_OK && is(V_BSTR(&out), L"5:2.5"));
    VariantClear(&out);

    V_VT(&v) = VT_I4;
    V_I4(&v) = 21;
    CHECK("Twice: [in, out] VARIANT", INdrDual_Twice(d, &v) == S_OK && V_VT(&v) == VT_I4 && V_I4(&v) == 42);
    V_VT(&v) = VT_BSTR;
    V_BSTR(&v) = SysAllocString(L"ab");
    CHECK("Twice: [in, out] BSTR VARIANT", INdrDual_Twice(d, &v) == S_OK && V_VT(&v) == VT_BSTR && is(V_BSTR(&v), L"abab"));
    VariantClear(&v);

    LONG pid = 0;
    CHECK("Pid", INdrDual_Pid(d, &pid) == S_OK && pid == server_pid && pid != (LONG)GetCurrentProcessId());

    Cb cb = { { &g_cb_vtbl }, 1, 0, 1 };
    r = NULL;
    BSTR t = SysAllocString(L"call me");
    HRESULT hr = INdrDual_Call(d, &cb.iface, t, &r);
    WCHAR want[100];
    swprintf(want, 100, L"call me! %lu", (unsigned long)GetCurrentProcessId());
    CHECK("Call: an interface pointer in; the server calls it back", hr == S_OK && is(r, want));
    if (hr != S_OK || !is(r, want)) printf("  Call: hr %08lx, %ls\n", (unsigned long)hr, r ? r : L"(null)");
    CHECK("the callbacks ran on this thread (our apartment)", cb.calls == 2 && cb.tid_ok);
    SysFreeString(r);
    SysFreeString(t);
    CHECK("the server let go of our object", cb.refs == 1);
}

static void dispatch_calls(IDispatch *disp, Cb *cb)
{
    LPOLESTR names[2] = { L"Concat", L"Bump" };
    DISPID ids[2] = { 0, 0 };
    CHECK("GetIDsOfNames", disp->lpVtbl->GetIDsOfNames(disp, &IID_NULL, names, 1, 0, ids) == S_OK && ids[0] == 1);
    LPOLESTR bad = L"Nothing";
    DISPID bid = 0;
    CHECK("GetIDsOfNames of an unknown name", disp->lpVtbl->GetIDsOfNames(disp, &IID_NULL, &bad, 1, 0, &bid) == DISP_E_UNKNOWNNAME && bid == DISPID_UNKNOWN);
    UINT n = 9;
    CHECK("GetTypeInfoCount", disp->lpVtbl->GetTypeInfoCount(disp, &n) == S_OK && n == 0);

    VARIANT args[2], r;
    V_VT(&args[1]) = VT_BSTR; V_BSTR(&args[1]) = SysAllocString(L"Auto");
    V_VT(&args[0]) = VT_BSTR; V_BSTR(&args[0]) = SysAllocString(L"mation");
    DISPPARAMS dp = { args, NULL, 2, 0 };
    VariantInit(&r);
    HRESULT hr = disp->lpVtbl->Invoke(disp, 1, &IID_NULL, 0, DISPATCH_METHOD, &dp, &r, NULL, NULL);
    CHECK("Invoke with BSTR arguments", hr == S_OK && V_VT(&r) == VT_BSTR && is(V_BSTR(&r), L"Automation"));
    VariantClear(&r);
    VariantClear(&args[0]);
    VariantClear(&args[1]);

    LONG num = 41;
    BSTR text = SysAllocString(L"value");
    V_VT(&args[1]) = VT_BYREF | VT_I4; args[1].plVal = &num;
    V_VT(&args[0]) = VT_BYREF | VT_BSTR; args[0].pbstrVal = &text;
    hr = disp->lpVtbl->Invoke(disp, 2, &IID_NULL, 0, DISPATCH_METHOD, &dp, NULL, NULL, NULL);
    CHECK("Invoke with VT_BYREF arguments: the new values come back", hr == S_OK && num == 42 && is(text, L"bumped value"));
    SysFreeString(text);

    struct { VARTYPE vt; const WCHAR *want; } kinds[] = { { VT_I2, L"2:-7" }, { VT_BOOL, L"11:-1" }, { VT_UI1, L"17:200" },
                                                          { VT_I8, L"20:1234567890123" }, { VT_EMPTY, L"0:" }, { VT_NULL, L"1:?" } };
    int ok = 1;
    for (int i = 0; i < (int)(sizeof kinds / sizeof kinds[0]); i++) {
        VARIANT a;
        VariantInit(&a);
        V_VT(&a) = kinds[i].vt;
        if (kinds[i].vt == VT_I2) V_I2(&a) = -7;
        if (kinds[i].vt == VT_BOOL) V_BOOL(&a) = VARIANT_TRUE;
        if (kinds[i].vt == VT_UI1) V_UI1(&a) = 200;
        if (kinds[i].vt == VT_I8) V_I8(&a) = 1234567890123LL;
        DISPPARAMS p = { &a, NULL, 1, 0 };
        VariantInit(&r);
        hr = disp->lpVtbl->Invoke(disp, 3, &IID_NULL, 0, DISPATCH_METHOD, &p, &r, NULL, NULL);
        if (hr != S_OK || V_VT(&r) != VT_BSTR || !is(V_BSTR(&r), kinds[i].want)) {
            ok = 0;
            printf("  Kind %u: hr %08lx, %ls\n", kinds[i].vt, (unsigned long)hr, V_VT(&r) == VT_BSTR ? V_BSTR(&r) : L"(not a BSTR)");
        }
        VariantClear(&r);
    }
    CHECK("Invoke with VARIANTs of many types", ok);

    SAFEARRAY *sa = SafeArrayCreateVector(VT_I4, 0, 100);
    for (LONG i = 0; i < 100; i++) SafeArrayPutElement(sa, &i, &i);
    VARIANT arr;
    V_VT(&arr) = VT_ARRAY | VT_I4;
    V_ARRAY(&arr) = sa;
    DISPPARAMS ap = { &arr, NULL, 1, 0 };
    VariantInit(&r);
    hr = disp->lpVtbl->Invoke(disp, 7, &IID_NULL, 0, DISPATCH_METHOD, &ap, &r, NULL, NULL);
    CHECK("Invoke with a SAFEARRAY", hr == S_OK && V_VT(&r) == VT_I4 && V_I4(&r) == 4950);
    SafeArrayDestroy(sa);

    EXCEPINFO ei;
    UINT argerr = 0;
    DISPPARAMS none = { NULL, NULL, 0, 0 };
    hr = disp->lpVtbl->Invoke(disp, 4, &IID_NULL, 0, DISPATCH_METHOD, &none, NULL, &ei, &argerr);
    CHECK("Invoke: DISP_E_EXCEPTION and its EXCEPINFO", hr == DISP_E_EXCEPTION && ei.wCode == 1001 && ei.scode == E_ABORT &&
          is(ei.bstrSource, L"comserver") && is(ei.bstrDescription, L"failed on purpose"));
    SysFreeString(ei.bstrSource);
    SysFreeString(ei.bstrDescription);
    SysFreeString(ei.bstrHelpFile);
    CHECK("Invoke of an unknown member", disp->lpVtbl->Invoke(disp, 99, &IID_NULL, 0, DISPATCH_METHOD, &none, NULL, NULL, NULL) == DISP_E_MEMBERNOTFOUND);

    LONG calls = cb->calls;
    VARIANT cbv;
    V_VT(&cbv) = VT_DISPATCH;
    V_DISPATCH(&cbv) = (IDispatch *)&cb->iface;
    DISPPARAMS cp = { &cbv, NULL, 1, 0 };
    VariantInit(&r);
    hr = disp->lpVtbl->Invoke(disp, 5, &IID_NULL, 0, DISPATCH_METHOD, &cp, &r, NULL, NULL);
    CHECK("Invoke with a VT_DISPATCH argument; the server Invokes it back",
          hr == S_OK && V_VT(&r) == VT_BSTR && is(V_BSTR(&r), L"pong: client got ping from the server") && cb->calls == calls + 1);
    if (hr != S_OK) printf("  Callback: hr %08lx\n", (unsigned long)hr);
    VariantClear(&r);

    VariantInit(&r);
    CHECK("Invoke DISPATCH_PROPERTYGET", disp->lpVtbl->Invoke(disp, 6, &IID_NULL, 0, DISPATCH_PROPERTYGET, &none, &r, NULL, NULL) == S_OK &&
          V_VT(&r) == VT_I4 && V_I4(&r) != (LONG)GetCurrentProcessId());
}

static void suite(const CLSID *clsid, const char *which)
{
    printf("-- %s server\n", which);
    INdrTest *t = NULL;
    HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_LOCAL_SERVER, &IID_INdrTest, (void **)&t);
    CHECK("CoCreateInstance(CLSCTX_LOCAL_SERVER) starts the server", hr == S_OK && t);
    if (!t) { printf("  hr %08lx\n", (unsigned long)hr); return; }

    LONG sum = 0;
    CHECK("Add (a delegated base interface) in another process", INdrTest_Add(t, 40, 2, &sum) == S_OK && sum == 42);
    WCHAR *s = NULL;
    CHECK("Echo: strings in and out", INdrTest_Echo(t, L"across", &s) == S_OK && s && !wcscmp(s, L"server: across"));
    CoTaskMemFree(s);
    LONG v[200];
    hyper total = 0, want = 0;
    for (int i = 0; i < 200; i++) { v[i] = i * 7 - 300; want += v[i]; }
    CHECK("Sum: an array", INdrTest_Sum(t, 200, v, &total) == S_OK && total == want);
    double r8 = 0;
    CHECK("Scale: floating point and hyper", INdrTest_Scale(t, 1.5, 2.0f, -3, 4, 5, 0.25, &r8) == S_OK && r8 == 1.5 * 2 - 3 + 4 + 5 + 0.25);
    CHECK("the server's HRESULT comes back", INdrTest_Fail(t, E_ACCESSDENIED) == E_ACCESSDENIED);
    CHECK("a crash in the server is RPC_E_SERVERFAULT", INdrTest_Fail(t, (HRESULT)0x8badf00dL) == RPC_E_SERVERFAULT);

    IUnknown *u1 = NULL, *u2 = NULL;
    INdrDual *d = NULL;
    IDispatch *disp = NULL;
    CHECK("QueryInterface across: the dual interface", INdrTest_QueryInterface(t, &IID_INdrDual, (void **)&d) == S_OK && d);
    CHECK("QueryInterface across: IDispatch", INdrTest_QueryInterface(t, &IID_IDispatch, (void **)&disp) == S_OK && disp);
    INdrTest_QueryInterface(t, &IID_IUnknown, (void **)&u1);
    if (d) INdrDual_QueryInterface(d, &IID_IUnknown, (void **)&u2);
    CHECK("one identity (IUnknown) for every interface", u1 && u1 == u2);
    IStream *st = NULL;
    CHECK("an interface the object lacks", INdrTest_QueryInterface(t, &IID_IStream, (void **)&st) == E_NOINTERFACE && !st);
    if (u1) IUnknown_Release(u1);
    if (u2) IUnknown_Release(u2);

    LONG server_pid = 0;
    if (d) INdrDual_Pid(d, &server_pid);
    Cb cb = { { &g_cb_vtbl }, 1, 0, 1 };
    if (d) dual_calls(d, server_pid);
    if (disp) dispatch_calls(disp, &cb);

    if (d) INdrDual_Release(d);
    if (disp) IDispatch_Release(disp);
    INdrTest_Release(t);
    CHECK("the server exits once everything is released", server_pid && wait_exit((DWORD)server_pid, 30000));
    CHECK("the server let go of our callback object", cb.refs == 1);

    /* the class object path, and a new server */
    IClassFactory *cf = NULL;
    hr = CoGetClassObject(clsid, CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&cf);
    CHECK("CoGetClassObject(CLSCTX_LOCAL_SERVER) starts it again", hr == S_OK && cf);
    if (!cf) { printf("  hr %08lx\n", (unsigned long)hr); return; }
    CHECK("IClassFactory::LockServer", IClassFactory_LockServer(cf, TRUE) == S_OK);
    d = NULL;
    CHECK("IClassFactory::CreateInstance", IClassFactory_CreateInstance(cf, NULL, &IID_INdrDual, (void **)&d) == S_OK && d);
    LONG pid2 = 0;
    if (d) INdrDual_Pid(d, &pid2);
    CHECK("a new server process", pid2 && pid2 != server_pid);
    IClassFactory_LockServer(cf, FALSE);
    if (d) INdrDual_Release(d);
    IClassFactory_Release(cf);
    CHECK("and it exits too", pid2 && wait_exit((DWORD)pid2, 30000));
}

int main(void)
{
    g_main_tid = GetCurrentThreadId();
    CHECK("CoInitializeEx (single-threaded apartment)", SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)));
    HMODULE dll = LoadLibraryW(L"ndrtestps.dll");
    HRESULT (STDAPICALLTYPE *reg)(void) = dll ? (void *)GetProcAddress(dll, "DllRegisterServer") : NULL;
    HRESULT (STDAPICALLTYPE *unreg)(void) = dll ? (void *)GetProcAddress(dll, "DllUnregisterServer") : NULL;
    CHECK("register ndrtestps.dll's proxies", reg && reg() == S_OK);
#ifndef _WIN64
    /* NovaOS has one registry view for both bitnesses until the
     * WOW6432Node view lands, so the SysWOW64 path a 32-bit
     * DllRegisterServer writes would reach the 64-bit server too; the
     * System32 path loads the right copy in each (32-bit programs are
     * redirected to SysWOW64) */
    {
        static const WCHAR sys[] = L"C:\\Windows\\System32\\ndrtestps.dll";
        HKEY k;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"CLSID\\{6E0F3A10-4D2B-4C55-9A51-7C0DE0A1B0FF}\\InprocServer32", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE *)sys, sizeof sys);
            RegCloseKey(k);
        }
    }
#endif
    register_server(&CLSID_ComTest64, L"C:\\Programs\\comserver.exe");
    register_server(&CLSID_ComTest32, L"C:\\Programs\\x86\\comserver.exe");

    void *p = NULL;
    CHECK("a class nobody registered", CoCreateInstance(&CLSID_Nobody, NULL, CLSCTX_LOCAL_SERVER, &IID_IUnknown, &p) == REGDB_E_CLASSNOTREG && !p);
#ifdef _WIN64
    suite(&CLSID_ComTest64, "64-bit");
    suite(&CLSID_ComTest32, "32-bit");
#else
    suite(&CLSID_ComTest32, "32-bit");
    suite(&CLSID_ComTest64, "64-bit");
#endif

    unregister_server(&CLSID_ComTest64);
    unregister_server(&CLSID_ComTest32);
    if (unreg) unreg();
    CoUninitialize();
#ifdef _WIN64
    printf("comoop: %d passed, %d failed (64-bit)\n", pass, fail);
#else
    printf("comoop: %d passed, %d failed (32-bit)\n", pass, fail);
#endif
    return fail != 0;
}
