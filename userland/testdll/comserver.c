/*
 * comserver.c — testdll.dll is also an in-process COM server for the
 * sample class Nova.Calc (novacalc.h): DllGetClassObject, DllCanUnloadNow,
 * and DllRegisterServer / DllUnregisterServer for regsvr32 (which also
 * register its type library). The object's IDispatch is oleaut32's
 * DispGetIDsOfNames / DispInvoke over that type library (idl/novacalc.idl).
 */
#include <windows.h>
#include "novacalc.h"

int _fltused = 0x9875;      /* the methods take doubles */
static volatile LONG g_objects, g_locks;

/* ---- the type library: ICalc's IDispatch is oleaut32's DispInvoke over it ---- */
static HMODULE self_module(void)
{
    HMODULE self = 0;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)self_module, &self);
    return self;
}

static ITypeInfo *volatile g_tinfo;
static HRESULT calc_typeinfo(ITypeInfo **out)
{
    if (!g_tinfo) {
        WCHAR path[MAX_PATH];
        ITypeLib *tl;
        ITypeInfo *ti;
        if (!GetModuleFileNameW(self_module(), path, MAX_PATH)) return E_FAIL;
        HRESULT hr = LoadTypeLibEx(path, REGKIND_NONE, &tl);
        if (FAILED(hr)) return hr;
        hr = tl->lpVtbl->GetTypeInfoOfGuid(tl, &IID_ICalc, &ti);
        tl->lpVtbl->Release(tl);
        if (FAILED(hr)) return hr;
        if (InterlockedCompareExchangePointer((void *volatile *)&g_tinfo, ti, 0))
            ti->lpVtbl->Release(ti);                 /* another thread was first */
    }
    *out = g_tinfo;
    return S_OK;
}

/* ---- the Calc object ---- */
typedef struct Calc { ICalc iface; LONG refs, count; BSTR name; } Calc;

static HRESULT STDMETHODCALLTYPE calc_qi(ICalc *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_ICalc)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE calc_addref(ICalc *This) { return (ULONG)InterlockedIncrement(&((Calc *)This)->refs); }
static ULONG STDMETHODCALLTYPE calc_release(ICalc *This)
{
    LONG r = InterlockedDecrement(&((Calc *)This)->refs);
    if (!r) {
        SysFreeString(((Calc *)This)->name);
        HeapFree(GetProcessHeap(), 0, This);
        InterlockedDecrement(&g_objects);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE calc_tinfo_count(ICalc *This, UINT *n) { (void)This; *n = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE calc_tinfo(ICalc *This, UINT i, LCID l, ITypeInfo **out)
{
    (void)This; (void)l;
    *out = 0;
    if (i) return DISP_E_BADINDEX;
    HRESULT hr = calc_typeinfo(out);
    if (SUCCEEDED(hr)) (*out)->lpVtbl->AddRef(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE calc_ids(ICalc *This, REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids)
{
    (void)This; (void)riid; (void)lcid;
    ITypeInfo *ti;
    HRESULT hr = calc_typeinfo(&ti);
    return FAILED(hr) ? hr : DispGetIDsOfNames(ti, names, n, ids);
}
static HRESULT STDMETHODCALLTYPE calc_invoke(ICalc *This, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *p,
                                             VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    (void)riid; (void)lcid;
    ITypeInfo *ti;
    HRESULT hr = calc_typeinfo(&ti);
    return FAILED(hr) ? hr : DispInvoke(This, ti, id, flags, p, res, ei, argerr);
}

/* a + b as a new BSTR */
static BSTR concat(const WCHAR *a, const WCHAR *b)
{
    UINT na = a ? (UINT)lstrlenW(a) : 0, nb = b ? (UINT)lstrlenW(b) : 0;
    BSTR s = SysAllocStringLen(0, na + nb);
    if (!s) return 0;
    for (UINT i = 0; i < na; i++) s[i] = a[i];
    for (UINT i = 0; i < nb; i++) s[na + i] = b[i];
    s[na + nb] = 0;
    return s;
}

static HRESULT STDMETHODCALLTYPE calc_add(ICalc *This, LONG a, LONG b, LONG *sum)
{
    if (!sum) return E_POINTER;
    InterlockedIncrement(&((Calc *)This)->count);
    *sum = a + b;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE calc_count(ICalc *This, LONG *n) { if (!n) return E_POINTER; *n = ((Calc *)This)->count; return S_OK; }
static HRESULT STDMETHODCALLTYPE calc_scale(ICalc *This, double x, LONG factor, double *r)
{
    (void)This;
    if (!r) return E_POINTER;
    *r = x * factor;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE calc_greet(ICalc *This, BSTR who, BSTR *s)
{
    (void)This;
    if (!s) return E_POINTER;
    return (*s = concat(L"Hello, ", who)) ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE calc_get_name(ICalc *This, BSTR *s)
{
    if (!s) return E_POINTER;
    Calc *c = (Calc *)This;
    return (*s = SysAllocString(c->name ? c->name : L"calc")) ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE calc_put_name(ICalc *This, BSTR s)
{
    Calc *c = (Calc *)This;
    SysFreeString(c->name);
    c->name = SysAllocString(s);
    return S_OK;
}
/* "<vt>:<value as text>" */
static HRESULT STDMETHODCALLTYPE calc_describe(ICalc *This, VARIANT v, BSTR *s)
{
    (void)This;
    if (!s) return E_POINTER;
    VARIANT t;
    VariantInit(&t);
    HRESULT hr = VariantChangeType(&t, &v, 0, VT_BSTR);
    if (FAILED(hr)) return hr;
    WCHAR vt[8];
    int n = 0, x = v.vt;
    WCHAR d[8];
    do d[n++] = (WCHAR)(L'0' + x % 10); while ((x /= 10));
    for (int i = 0; i < n; i++) vt[i] = d[n - 1 - i];
    vt[n] = L':'; vt[n + 1] = 0;
    *s = concat(vt, t.bstrVal);
    VariantClear(&t);
    return *s ? S_OK : E_OUTOFMEMORY;
}
/* fails with error info (which IDispatch::Invoke turns into DISP_E_EXCEPTION) */
static HRESULT STDMETHODCALLTYPE calc_fail(ICalc *This, LONG code)
{
    (void)This;
    ICreateErrorInfo *cei;
    if (SUCCEEDED(CreateErrorInfo(&cei))) {
        IErrorInfo *e;
        ICreateErrorInfo_SetSource(cei, L"Nova.Calc");
        ICreateErrorInfo_SetDescription(cei, L"Calc failed on purpose");
        if (SUCCEEDED(ICreateErrorInfo_QueryInterface(cei, &IID_IErrorInfo, (void **)&e))) {
            SetErrorInfo(0, e);
            e->lpVtbl->Release(e);
        }
        ICreateErrorInfo_Release(cei);
    }
    return (HRESULT)(0x80040200 | (code & 0xFF));
}
static HRESULT STDMETHODCALLTYPE calc_sum6(ICalc *This, LONG a, double b, float c, LONG d, double e, SHORT f, double *r)
{
    (void)This;
    if (!r) return E_POINTER;
    *r = a + b + c + d + e + f;
    return S_OK;
}
/* *a += 1, *b gets "!" appended */
static HRESULT STDMETHODCALLTYPE calc_swap(ICalc *This, LONG *a, BSTR *b)
{
    (void)This;
    if (!a || !b) return E_POINTER;
    *a += 1;
    BSTR s = concat(*b, L"!");
    if (!s) return E_OUTOFMEMORY;
    SysFreeString(*b);
    *b = s;
    return S_OK;
}

static const ICalcVtbl g_calc_vtbl = {
    calc_qi, calc_addref, calc_release, calc_tinfo_count, calc_tinfo, calc_ids, calc_invoke, calc_add, calc_count,
    calc_scale, calc_greet, calc_get_name, calc_put_name, calc_describe, calc_fail, calc_sum6, calc_swap,
};

/* ---- its class factory (a static object) ---- */
static HRESULT STDMETHODCALLTYPE cf_qi(IClassFactory *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) { *ppv = This; return S_OK; }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_addref(IClassFactory *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE cf_release(IClassFactory *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE cf_create(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv)
{
    (void)This;
    *ppv = 0;
    if (outer) return CLASS_E_NOAGGREGATION;
    Calc *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *c);
    if (!c) return E_OUTOFMEMORY;
    c->iface.lpVtbl = &g_calc_vtbl;
    c->refs = 1;
    InterlockedIncrement(&g_objects);
    HRESULT hr = c->iface.lpVtbl->QueryInterface(&c->iface, riid, ppv);
    c->iface.lpVtbl->Release(&c->iface);
    return hr;
}
static HRESULT STDMETHODCALLTYPE cf_lock(IClassFactory *This, BOOL lock)
{
    (void)This;
    if (lock) InterlockedIncrement(&g_locks); else InterlockedDecrement(&g_locks);
    return S_OK;
}
static const IClassFactoryVtbl g_cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static IClassFactory g_factory = { &g_cf_vtbl };

__declspec(dllexport) HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!IsEqualCLSID(clsid, &CLSID_NovaCalc)) { *ppv = 0; return CLASS_E_CLASSNOTAVAILABLE; }
    return g_factory.lpVtbl->QueryInterface(&g_factory, riid, ppv);
}

__declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow(void) { return g_objects || g_locks ? S_FALSE : S_OK; }

/* ---- self-registration ---- */
#define CALC_KEY L"CLSID\\{6E6F7661-4341-4C43-8001-000000000001}"

static LSTATUS set_sz(const WCHAR *key, const WCHAR *name, const WCHAR *val)
{
    return RegSetKeyValueW(HKEY_CLASSES_ROOT, key, name, REG_SZ, val, (DWORD)(lstrlenW(val) + 1) * sizeof(WCHAR));
}

__declspec(dllexport) HRESULT STDAPICALLTYPE DllRegisterServer(void)
{
    WCHAR path[MAX_PATH];
    if (!GetModuleFileNameW(self_module(), path, MAX_PATH))
        return E_FAIL;
    LSTATUS e;
    if ((e = set_sz(CALC_KEY, 0, L"Nova Calculator")) ||
        (e = set_sz(CALC_KEY L"\\InprocServer32", 0, path)) ||
        (e = set_sz(CALC_KEY L"\\InprocServer32", L"ThreadingModel", L"Both")) ||
        (e = set_sz(CALC_KEY L"\\ProgID", 0, L"Nova.Calc.1")) ||
        (e = set_sz(CALC_KEY L"\\VersionIndependentProgID", 0, L"Nova.Calc")) ||
        (e = set_sz(L"Nova.Calc", 0, L"Nova Calculator")) ||
        (e = set_sz(L"Nova.Calc\\CLSID", 0, L"{6E6F7661-4341-4C43-8001-000000000001}")) ||
        (e = set_sz(L"Nova.Calc\\CurVer", 0, L"Nova.Calc.1")) ||
        (e = set_sz(L"Nova.Calc.1\\CLSID", 0, L"{6E6F7661-4341-4C43-8001-000000000001}")))
        return HRESULT_FROM_WIN32(e);
    /* and the type library (TypeLib\{LIBID}, ICalc's Interface key) */
    ITypeLib *tl;
    HRESULT hr = LoadTypeLibEx(path, REGKIND_NONE, &tl);
    if (SUCCEEDED(hr)) {
        hr = RegisterTypeLib(tl, path, 0);
        tl->lpVtbl->Release(tl);
    }
    return hr;
}

__declspec(dllexport) HRESULT STDAPICALLTYPE DllUnregisterServer(void)
{
    RegDeleteTreeW(HKEY_CLASSES_ROOT, CALC_KEY);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"Nova.Calc");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"Nova.Calc.1");
    UnRegisterTypeLib(&LIBID_NovaCalcLib, 1, 2, 0, sizeof(void *) == 8 ? SYS_WIN64 : SYS_WIN32);
    return S_OK;
}
