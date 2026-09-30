/* comtest.exe — COM and OLE Automation: activation through the registry, IDispatch, BSTR, VARIANT, SAFEARRAY */
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../testdll/novacalc.h"

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

/* a class registered at run time with CoRegisterClassObject */
DEFINE_GUID(CLSID_Local, 0x11111111, 0x2222, 0x3333, 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55);
static HRESULT STDMETHODCALLTYPE lf_qi(IClassFactory *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) { *ppv = This; return S_OK; }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE lf_addref(IClassFactory *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE lf_release(IClassFactory *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE lf_create(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv)
{
    (void)outer;
    return lf_qi(This, riid, ppv);          /* the factory stands in for the object */
}
static HRESULT STDMETHODCALLTYPE lf_lock(IClassFactory *This, BOOL l) { (void)This; (void)l; return S_OK; }
static const IClassFactoryVtbl lf_vtbl = { lf_qi, lf_addref, lf_release, lf_create, lf_lock };
static IClassFactory g_local = { &lf_vtbl };

/* ---- shortcuts: shell32's ShellLink (IShellLinkW + IPersistFile), by hand-made vtables ---- */
DEFINE_GUID(CLSID_ShellLink_T, 0x00021401, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
DEFINE_GUID(IID_IShellLinkW_T, 0x000214F9, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
DEFINE_GUID(IID_IShellLinkA_T, 0x000214EE, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
DEFINE_GUID(IID_IPersistFile_T, 0x0000010B, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
typedef HRESULT (STDMETHODCALLTYPE *QiFn)(void *, REFIID, void **);
typedef ULONG (STDMETHODCALLTYPE *RelFn)(void *);
typedef HRESULT (STDMETHODCALLTYPE *GetStrFn)(void *, LPWSTR, int);
typedef HRESULT (STDMETHODCALLTYPE *SetStrFn)(void *, LPCWSTR);
typedef HRESULT (STDMETHODCALLTYPE *GetPathFn)(void *, LPWSTR, int, void *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *GetIconFn)(void *, LPWSTR, int, int *);
typedef HRESULT (STDMETHODCALLTYPE *SetIconFn)(void *, LPCWSTR, int);
typedef HRESULT (STDMETHODCALLTYPE *GetPathAFn)(void *, LPSTR, int, void *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PfFileFn)(void *, LPCWSTR, DWORD);
#define VT(obj) (*(void ***)(obj))
enum { SL_GETPATH = 3, SL_GETDESC = 6, SL_SETDESC, SL_GETDIR, SL_SETDIR, SL_GETARGS, SL_SETARGS,
       SL_GETICON = 16, SL_SETICON, SL_SETPATH = 20, PF_LOAD = 5, PF_SAVE = 6 };

static void shortcut_tests(void)
{
    void *sl = 0, *pf = 0;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink_T, 0, CLSCTX_INPROC_SERVER, &IID_IShellLinkW_T, &sl);
    CHECK("CoCreateInstance(ShellLink)", hr == S_OK && sl);
    if (!sl) return;
    ((SetStrFn)VT(sl)[SL_SETPATH])(sl, L"C:\\Programs\\hello.exe");
    ((SetStrFn)VT(sl)[SL_SETARGS])(sl, L"one \"two words\"");
    ((SetStrFn)VT(sl)[SL_SETDIR])(sl, L"C:\\Documents");
    ((SetStrFn)VT(sl)[SL_SETDESC])(sl, L"Says hello");
    ((SetIconFn)VT(sl)[SL_SETICON])(sl, L"C:\\Programs\\hello.exe", 2);
    CHECK("QueryInterface(IPersistFile)", ((QiFn)VT(sl)[0])(sl, &IID_IPersistFile_T, &pf) == S_OK && pf);
    CreateDirectoryA("C:\\Temp", 0);
    CHECK("IPersistFile::Save", pf && ((PfFileFn)VT(pf)[PF_SAVE])(pf, L"C:\\Temp\\hello.lnk", TRUE) == S_OK);
    if (pf) ((RelFn)VT(pf)[2])(pf);
    ((RelFn)VT(sl)[2])(sl);

    /* read it back into a new object */
    sl = pf = 0;
    CoCreateInstance(&CLSID_ShellLink_T, 0, CLSCTX_INPROC_SERVER, &IID_IShellLinkW_T, &sl);
    if (sl) ((QiFn)VT(sl)[0])(sl, &IID_IPersistFile_T, &pf);
    CHECK("IPersistFile::Load", pf && ((PfFileFn)VT(pf)[PF_LOAD])(pf, L"C:\\Temp\\hello.lnk", 0) == S_OK);
    WCHAR b[260];
    int idx = -1;
    b[0] = 0; ((GetPathFn)VT(sl)[SL_GETPATH])(sl, b, 260, 0, 0);
    CHECK("shortcut target", !wcscmp(b, L"C:\\Programs\\hello.exe"));
    b[0] = 0; ((GetStrFn)VT(sl)[SL_GETARGS])(sl, b, 260);
    CHECK("shortcut arguments", !wcscmp(b, L"one \"two words\""));
    b[0] = 0; ((GetStrFn)VT(sl)[SL_GETDIR])(sl, b, 260);
    CHECK("shortcut working folder", !wcscmp(b, L"C:\\Documents"));
    b[0] = 0; ((GetStrFn)VT(sl)[SL_GETDESC])(sl, b, 260);
    CHECK("shortcut description", !wcscmp(b, L"Says hello"));
    b[0] = 0; ((GetIconFn)VT(sl)[SL_GETICON])(sl, b, 260, &idx);
    CHECK("shortcut icon", !wcscmp(b, L"C:\\Programs\\hello.exe") && idx == 2);
    void *sa = 0;
    char a[260] = "";
    CHECK("IShellLinkA", ((QiFn)VT(sl)[0])(sl, &IID_IShellLinkA_T, &sa) == S_OK && sa &&
                         ((GetPathAFn)VT(sa)[SL_GETPATH])(sa, a, 260, 0, 0) == S_OK && !strcmp(a, "C:\\Programs\\hello.exe"));
    if (sa) ((RelFn)VT(sa)[2])(sa);
    if (pf) ((RelFn)VT(pf)[2])(pf);
    if (sl) ((RelFn)VT(sl)[2])(sl);
    DeleteFileA("C:\\Temp\\hello.lnk");
}

int main(void)
{
    HRESULT hr = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    CHECK("CoInitializeEx", hr == S_OK);
    CHECK("CoInitializeEx again", CoInitializeEx(0, COINIT_APARTMENTTHREADED) == S_FALSE);
    CHECK("changed mode", CoInitializeEx(0, COINIT_MULTITHREADED) == RPC_E_CHANGED_MODE);
    CoUninitialize();

    /* GUID strings */
    WCHAR s[40];
    GUID g;
    CHECK("StringFromGUID2", StringFromGUID2(&CLSID_NovaCalc, s, 40) == 39 &&
                             !wcscmp(s, L"{6E6F7661-4341-4C43-8001-000000000001}"));
    CHECK("CLSIDFromString", CLSIDFromString(s, &g) == S_OK && IsEqualGUID(&g, &CLSID_NovaCalc));
    CHECK("bad GUID string", IIDFromString(L"{nope}", &g) == E_INVALIDARG);
    GUID g1, g2;
    CHECK("CoCreateGuid", CoCreateGuid(&g1) == S_OK && CoCreateGuid(&g2) == S_OK && !IsEqualGUID(&g1, &g2) &&
                          (g1.Data3 >> 12) == 4);
    void *m = CoTaskMemAlloc(100);
    m = CoTaskMemRealloc(m, 1000);
    CHECK("CoTaskMem", m != 0);
    CoTaskMemFree(m);

    /* activation through the registry: regsvr32 testdll.dll registers Nova.Calc */
    HMODULE srv = LoadLibraryA("testdll.dll");
    HRESULT (__stdcall *reg)(void) = (HRESULT (__stdcall *)(void))GetProcAddress(srv, "DllRegisterServer");
    CHECK("DllRegisterServer", reg && reg() == S_OK);
    CLSID c;
    CHECK("CLSIDFromProgID", CLSIDFromProgID(L"Nova.Calc", &c) == S_OK && IsEqualCLSID(&c, &CLSID_NovaCalc));
    LPOLESTR pid = 0;
    CHECK("ProgIDFromCLSID", ProgIDFromCLSID(&CLSID_NovaCalc, &pid) == S_OK && !wcscmp(pid, L"Nova.Calc.1"));
    CoTaskMemFree(pid);

    ICalc *calc = 0;
    hr = CoCreateInstance(&CLSID_NovaCalc, 0, CLSCTX_INPROC_SERVER, &IID_ICalc, (void **)&calc);
    CHECK("CoCreateInstance", hr == S_OK && calc);
    LONG sum = 0, count = 0;
    if (calc) {
        CHECK("ICalc::Add", calc->lpVtbl->Add(calc, 40, 2, &sum) == S_OK && sum == 42);
        IDispatch *disp = 0;
        CHECK("QueryInterface(IDispatch)", calc->lpVtbl->QueryInterface(calc, &IID_IDispatch, (void **)&disp) == S_OK);
        LPOLESTR name = L"Add";
        DISPID id = 0;
        CHECK("GetIDsOfNames", disp && disp->lpVtbl->GetIDsOfNames(disp, &IID_NULL, &name, 1, 0, &id) == S_OK && id == 1);
        VARIANT args[2], res;
        VariantInit(&args[0]); VariantInit(&args[1]); VariantInit(&res);
        args[1].vt = VT_I4; args[1].lVal = 7;                  /* first argument is last */
        args[0].vt = VT_R8; args[0].dblVal = 5.0;
        DISPPARAMS dp = { args, 0, 2, 0 };
        CHECK("IDispatch::Invoke", disp && disp->lpVtbl->Invoke(disp, 1, &IID_NULL, 0, DISPATCH_METHOD, &dp, &res, 0, 0) == S_OK &&
                                   res.vt == VT_I4 && res.lVal == 12);
        DISPPARAMS none = { 0, 0, 0, 0 };
        CHECK("property get", disp && disp->lpVtbl->Invoke(disp, 2, &IID_NULL, 0, DISPATCH_PROPERTYGET, &none, &res, 0, 0) == S_OK &&
                              res.lVal == 2);
        if (disp) disp->lpVtbl->Release(disp);
        calc->lpVtbl->get_Count(calc, &count);
        CHECK("get_Count", count == 2);
        calc->lpVtbl->Release(calc);
    }
    MULTI_QI qi[2] = { { &IID_IDispatch, 0, 0 }, { &IID_IStream, 0, 0 } };
    CHECK("CoCreateInstanceEx", CoCreateInstanceEx(&CLSID_NovaCalc, 0, CLSCTX_ALL, 0, 2, qi) == 0x00040012L &&
                                qi[0].hr == S_OK && qi[1].hr == E_NOINTERFACE);
    if (qi[0].pItf) qi[0].pItf->lpVtbl->Release(qi[0].pItf);
    CHECK("unregistered class", CoCreateInstance(&CLSID_Local, 0, CLSCTX_ALL, &IID_IUnknown, (void **)&calc) == REGDB_E_CLASSNOTREG);

    DWORD cookie;
    IUnknown *u = 0;
    CHECK("CoRegisterClassObject", CoRegisterClassObject(&CLSID_Local, (IUnknown *)&g_local, CLSCTX_INPROC_SERVER,
                                                         REGCLS_MULTIPLEUSE, &cookie) == S_OK);
    CHECK("create registered class", CoCreateInstance(&CLSID_Local, 0, CLSCTX_ALL, &IID_IUnknown, (void **)&u) == S_OK &&
                                     u == (IUnknown *)&g_local);
    CHECK("CoRevokeClassObject", CoRevokeClassObject(cookie) == S_OK && CoRevokeClassObject(cookie) == CO_E_OBJNOTREG);

    HRESULT (__stdcall *unreg)(void) = (HRESULT (__stdcall *)(void))GetProcAddress(srv, "DllUnregisterServer");
    CHECK("DllUnregisterServer", unreg && unreg() == S_OK && CLSIDFromProgID(L"Nova.Calc", &c) == CO_E_CLASSSTRING);

    /* IStream on memory */
    IStream *st;
    ULONG n = 0;
    char buf[16] = { 0 };
    LARGE_INTEGER zero = { 0 };
    CHECK("CreateStreamOnHGlobal", CreateStreamOnHGlobal(0, TRUE, &st) == S_OK);
    CHECK("IStream Write/Seek/Read", st->lpVtbl->Write(st, "stream!", 8, &n) == S_OK && n == 8 &&
                                     st->lpVtbl->Seek(st, zero, STREAM_SEEK_SET, 0) == S_OK &&
                                     st->lpVtbl->Read(st, buf, 16, &n) == S_OK && n == 8 && !strcmp(buf, "stream!"));
    st->lpVtbl->Release(st);

    /* BSTR */
    BSTR b = SysAllocString(L"hello");
    CHECK("SysAllocString", b && SysStringLen(b) == 5 && SysStringByteLen(b) == 10 && b[5] == 0);
    BSTR cat = 0;
    BSTR w = SysAllocString(L" world");
    CHECK("VarBstrCat", VarBstrCat(b, w, &cat) == S_OK && !wcscmp(cat, L"hello world"));
    CHECK("VarBstrCmp", VarBstrCmp(b, cat, 0, 0) == VARCMP_LT && VarBstrCmp(0, 0, 0, 0) == VARCMP_EQ);
    CHECK("SysReAllocString", SysReAllocString(&b, L"bye") && SysStringLen(b) == 3);
    SysFreeString(b); SysFreeString(w); SysFreeString(cat);

    /* VARIANT conversions */
    VARIANT v, o;
    VariantInit(&v); VariantInit(&o);
    v.vt = VT_BSTR; v.bstrVal = SysAllocString(L" 1234 ");
    CHECK("BSTR -> I4", VariantChangeType(&o, &v, 0, VT_I4) == S_OK && o.vt == VT_I4 && o.lVal == 1234);
    VariantClear(&v);
    v.vt = VT_R8; v.dblVal = 2.5;
    CHECK("R8 -> I4 (banker's rounding)", VariantChangeType(&o, &v, 0, VT_I4) == S_OK && o.lVal == 2);
    CHECK("R8 -> BSTR", VariantChangeType(&o, &v, 0, VT_BSTR) == S_OK && !wcscmp(o.bstrVal, L"2.5"));
    VariantClear(&o);
    v.vt = VT_I4; v.lVal = 300;
    CHECK("I4 -> UI1 overflow", VariantChangeType(&o, &v, 0, VT_UI1) == DISP_E_OVERFLOW);
    v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
    CHECK("BOOL -> BSTR", VariantChangeType(&o, &v, VARIANT_ALPHABOOL, VT_BSTR) == S_OK && !wcscmp(o.bstrVal, L"True"));
    VariantClear(&o);
    SYSTEMTIME stime = { 2026, 9, 0, 28, 19, 6, 30, 0 }, back;
    DATE d;
    CHECK("SystemTimeToVariantTime", SystemTimeToVariantTime(&stime, &d) && (long)d == 46293);
    CHECK("VariantTimeToSystemTime", VariantTimeToSystemTime(d, &back) && back.wYear == 2026 && back.wMonth == 9 &&
                                     back.wDay == 28 && back.wHour == 19 && back.wMinute == 6 && back.wSecond == 30 &&
                                     back.wDayOfWeek == 1);
    v.vt = VT_DATE; v.date = d;
    CHECK("DATE -> BSTR", VariantChangeType(&o, &v, 0, VT_BSTR) == S_OK && !wcscmp(o.bstrVal, L"9/28/2026 7:06:30 PM"));
    VARIANT o2;
    VariantInit(&o2);
    CHECK("BSTR -> DATE", VariantChangeType(&o2, &o, 0, VT_DATE) == S_OK && o2.date > d - 1e-6 && o2.date < d + 1e-6);
    VariantClear(&o);

    /* SAFEARRAY */
    SAFEARRAYBOUND bounds[2] = { { 3, 0 }, { 4, 1 } };
    SAFEARRAY *sa = SafeArrayCreate(VT_I4, 2, bounds);
    LONG lb = -1, ub = -1, idx[2] = { 2, 4 }, val = 99, got = 0;
    CHECK("SafeArrayCreate", sa && SafeArrayGetDim(sa) == 2 && SafeArrayGetElemsize(sa) == 4);
    CHECK("bounds", SafeArrayGetLBound(sa, 2, &lb) == S_OK && lb == 1 && SafeArrayGetUBound(sa, 2, &ub) == S_OK && ub == 4 &&
                    SafeArrayGetUBound(sa, 1, &ub) == S_OK && ub == 2);
    CHECK("Put/GetElement", SafeArrayPutElement(sa, idx, &val) == S_OK && SafeArrayGetElement(sa, idx, &got) == S_OK && got == 99);
    LONG *data;
    CHECK("AccessData layout", SafeArrayAccessData(sa, (void **)&data) == S_OK && data[3 * 3 + 2] == 99 &&
                               SafeArrayDestroy(sa) == DISP_E_ARRAYISLOCKED && SafeArrayUnaccessData(sa) == S_OK);
    idx[0] = 3;
    CHECK("bad index", SafeArrayGetElement(sa, idx, &got) == DISP_E_BADINDEX);
    VARTYPE vt;
    CHECK("GetVartype", SafeArrayGetVartype(sa, &vt) == S_OK && vt == VT_I4);
    SafeArrayDestroy(sa);
    SAFEARRAY *bs = SafeArrayCreateVector(VT_BSTR, 0, 2), *copy = 0;
    LONG i0 = 1;
    BSTR e = SysAllocString(L"elem");
    CHECK("BSTR array", SafeArrayPutElement(bs, &i0, e) == S_OK && SafeArrayCopy(bs, &copy) == S_OK &&
                        !wcscmp(((BSTR *)copy->pvData)[1], L"elem") && ((BSTR *)copy->pvData)[1] != e);
    SysFreeString(e);
    SAFEARRAYBOUND nb = { 5, 0 };
    CHECK("SafeArrayRedim", SafeArrayRedim(copy, &nb) == S_OK && SafeArrayGetUBound(copy, 1, &ub) == S_OK && ub == 4);
    SafeArrayDestroy(bs);
    SafeArrayDestroy(copy);

    /* error info */
    ICreateErrorInfo *cei;
    IErrorInfo *ei = 0;
    BSTR desc = 0;
    CHECK("CreateErrorInfo", CreateErrorInfo(&cei) == S_OK && cei->lpVtbl->SetDescription(cei, L"it broke") == S_OK &&
                             cei->lpVtbl->QueryInterface(cei, &IID_IErrorInfo, (void **)&ei) == S_OK);
    SetErrorInfo(0, ei);
    ei->lpVtbl->Release(ei);
    cei->lpVtbl->Release(cei);
    ei = 0;
    CHECK("GetErrorInfo", GetErrorInfo(0, &ei) == S_OK && ei->lpVtbl->GetDescription(ei, &desc) == S_OK &&
                          !wcscmp(desc, L"it broke") && GetErrorInfo(0, &ei) == S_FALSE);
    SysFreeString(desc);

    shortcut_tests();
    CoUninitialize();
    printf("comtest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
