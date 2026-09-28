/*
 * comserver.c — testdll.dll is also an in-process COM server for the
 * sample class Nova.Calc (novacalc.h): DllGetClassObject, DllCanUnloadNow,
 * and DllRegisterServer / DllUnregisterServer for regsvr32.
 */
#include <windows.h>
#include "novacalc.h"

int _fltused = 0x9875;      /* Invoke converts doubles */
static volatile LONG g_objects, g_locks;

/* ---- the Calc object ---- */
typedef struct Calc { ICalc iface; LONG refs, count; } Calc;

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
        HeapFree(GetProcessHeap(), 0, This);
        InterlockedDecrement(&g_objects);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE calc_tinfo_count(ICalc *This, UINT *n) { (void)This; *n = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE calc_tinfo(ICalc *This, UINT i, LCID l, ITypeInfo **out) { (void)This; (void)i; (void)l; *out = 0; return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE calc_add(ICalc *This, LONG a, LONG b, LONG *sum)
{
    if (!sum) return E_POINTER;
    InterlockedIncrement(&((Calc *)This)->count);
    *sum = a + b;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE calc_count(ICalc *This, LONG *n) { if (!n) return E_POINTER; *n = ((Calc *)This)->count; return S_OK; }

static HRESULT STDMETHODCALLTYPE calc_ids(ICalc *This, REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids)
{
    (void)This; (void)riid; (void)lcid;
    HRESULT hr = S_OK;
    for (UINT i = 0; i < n; i++) {
        if (!lstrcmpiW(names[i], L"Add")) ids[i] = 1;
        else if (!lstrcmpiW(names[i], L"Count")) ids[i] = 2;
        else { ids[i] = DISPID_UNKNOWN; hr = DISP_E_UNKNOWNNAME; }
    }
    return hr;
}

static HRESULT arg_long(DISPPARAMS *p, UINT pos, LONG *out)
{
    VARIANT *v = &p->rgvarg[p->cArgs - 1 - pos];          /* arguments come last to first */
    switch (v->vt) {
    case VT_I4: case VT_INT: *out = v->lVal; return S_OK;
    case VT_I2: *out = v->iVal; return S_OK;
    case VT_UI1: *out = v->bVal; return S_OK;
    case VT_R8: *out = (LONG)v->dblVal; return S_OK;
    default: return DISP_E_TYPEMISMATCH;
    }
}

static HRESULT STDMETHODCALLTYPE calc_invoke(ICalc *This, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *p,
                                             VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    (void)riid; (void)lcid; (void)ei; (void)argerr;
    LONG r;
    HRESULT hr;
    if (id == 1 && (flags & DISPATCH_METHOD)) {
        LONG a, b;
        if (p->cArgs != 2) return DISP_E_BADPARAMCOUNT;
        if (FAILED(hr = arg_long(p, 0, &a)) || FAILED(hr = arg_long(p, 1, &b))) return hr;
        hr = calc_add(This, a, b, &r);
    } else if (id == 2 && (flags & DISPATCH_PROPERTYGET)) {
        if (p && p->cArgs) return DISP_E_BADPARAMCOUNT;
        hr = calc_count(This, &r);
    } else return DISP_E_MEMBERNOTFOUND;
    if (SUCCEEDED(hr) && res) { res->vt = VT_I4; res->lVal = r; }
    return hr;
}

static const ICalcVtbl g_calc_vtbl = {
    calc_qi, calc_addref, calc_release, calc_tinfo_count, calc_tinfo, calc_ids, calc_invoke, calc_add, calc_count,
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
    HMODULE self;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)DllRegisterServer, &self) || !GetModuleFileNameW(self, path, MAX_PATH))
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
    return S_OK;
}

__declspec(dllexport) HRESULT STDAPICALLTYPE DllUnregisterServer(void)
{
    RegDeleteTreeW(HKEY_CLASSES_ROOT, CALC_KEY);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"Nova.Calc");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"Nova.Calc.1");
    return S_OK;
}
