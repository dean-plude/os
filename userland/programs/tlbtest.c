/* tlbtest.exe — COM type libraries: LoadTypeLib, ITypeLib/ITypeInfo, ITypeComp, registration,
   ITypeInfo::Invoke (DispInvoke), DispCallFunc and CreateStdDispatch, over testdll.dll's
   embedded library (userland/testdll/idl/novacalc.idl) and the built-in stdole2.tlb */
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../testdll/novacalc.h"

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static BOOL name_is(ITypeInfo *ti, MEMBERID id, const WCHAR *want)
{
    BSTR n = 0;
    BOOL ok = SUCCEEDED(ITypeInfo_GetDocumentation(ti, id, &n, 0, 0, 0)) && n && !wcscmp(n, want);
    SysFreeString(n);
    return ok;
}

static ITypeInfo *by_name(ITypeLib *tl, const WCHAR *name)
{
    UINT n = ITypeLib_GetTypeInfoCount(tl);
    for (UINT i = 0; i < n; i++) {
        ITypeInfo *ti;
        if (SUCCEEDED(ITypeLib_GetTypeInfo(tl, i, &ti))) {
            if (name_is(ti, MEMBERID_NIL, name)) return ti;
            ITypeInfo_Release(ti);
        }
    }
    return 0;
}

static void library_tests(ITypeLib *tl)
{
    TLIBATTR *la = 0;
    CHECK("GetLibAttr", SUCCEEDED(ITypeLib_GetLibAttr(tl, &la)) && la);
    if (la) {
        CHECK("library guid", IsEqualGUID(&la->guid, &LIBID_NovaCalcLib));
        CHECK("library version", la->wMajorVerNum == 1 && la->wMinorVerNum == 2);
        CHECK("library lcid", la->lcid == 0);
        CHECK("library syskind", la->syskind == (sizeof(void *) == 8 ? SYS_WIN64 : SYS_WIN32));
        ITypeLib_ReleaseTLibAttr(tl, la);
    }
    BSTR name = 0, doc = 0;
    CHECK("library documentation", SUCCEEDED(ITypeLib_GetDocumentation(tl, -1, &name, &doc, 0, 0)) &&
                                   name && !wcscmp(name, L"NovaCalcLib") && doc && !wcscmp(doc, L"NovaOS sample type library"));
    SysFreeString(name); SysFreeString(doc);
    CHECK("GetTypeInfoCount", ITypeLib_GetTypeInfoCount(tl) == 5);
    TYPEKIND k = 0;
    CHECK("GetTypeInfoType", ITypeLib_GetTypeInfoType(tl, 4, &k) == S_OK && k == TKIND_COCLASS);
    CHECK("GetTypeInfoType out of range", ITypeLib_GetTypeInfoType(tl, 5, &k) == TYPE_E_ELEMENTNOTFOUND);

    BOOL found = FALSE;
    WCHAR nm[] = L"icalc";
    CHECK("IsName", tl->lpVtbl->IsName(tl, nm, 0, &found) == S_OK && found && !wcscmp(nm, L"ICalc"));
    WCHAR nm2[] = L"factor";
    CHECK("IsName (a parameter)", tl->lpVtbl->IsName(tl, nm2, 0, &found) == S_OK && found);
    WCHAR nm3[] = L"nothing";
    CHECK("IsName (absent)", tl->lpVtbl->IsName(tl, nm3, 0, &found) == S_OK && !found);
    ITypeInfo *tis[2] = { 0 };
    MEMBERID ids[2];
    USHORT nf = 2;
    WCHAR nm4[] = L"NovaPoint";
    CHECK("FindName", tl->lpVtbl->FindName(tl, nm4, 0, tis, ids, &nf) == S_OK && nf == 1 && tis[0] &&
                      ids[0] == MEMBERID_NIL);
    if (tis[0]) ITypeInfo_Release(tis[0]);

    /* the enum */
    ITypeInfo *ti = by_name(tl, L"ShapeKind");
    TYPEATTR *ta = 0;
    CHECK("enum", ti && SUCCEEDED(ITypeInfo_GetTypeAttr(ti, &ta)) && ta->typekind == TKIND_ENUM && ta->cVars == 3);
    if (ta) { ITypeInfo_ReleaseTypeAttr(ti, ta); ta = 0; }
    VARDESC *vd = 0;
    CHECK("enum constant", ti && SUCCEEDED(ITypeInfo_GetVarDesc(ti, 2, &vd)) && vd->varkind == VAR_CONST &&
                           vd->lpvarValue && vd->lpvarValue->vt == VT_I4 && vd->lpvarValue->lVal == 5 &&
                           name_is(ti, vd->memid, L"ShapeStar"));
    if (vd) { ITypeInfo_ReleaseVarDesc(ti, vd); vd = 0; }
    if (ti) ITypeInfo_Release(ti);

    /* the record */
    ti = by_name(tl, L"NovaPoint");
    CHECK("record", ti && SUCCEEDED(ITypeInfo_GetTypeAttr(ti, &ta)) && ta->typekind == TKIND_RECORD && ta->cVars == 3 &&
                    ta->cbSizeInstance == 16 && ta->cbAlignment == 8);
    if (ta) { ITypeInfo_ReleaseTypeAttr(ti, ta); ta = 0; }
    CHECK("record field", ti && SUCCEEDED(ITypeInfo_GetVarDesc(ti, 2, &vd)) && vd->varkind == VAR_PERINSTANCE &&
                          vd->oInst == 8 && vd->elemdescVar.tdesc.vt == VT_R8);
    if (vd) { ITypeInfo_ReleaseVarDesc(ti, vd); vd = 0; }
    if (ti) ITypeInfo_Release(ti);

    /* the coclass and its source interface */
    ti = 0;
    CHECK("GetTypeInfoOfGuid(coclass)", ITypeLib_GetTypeInfoOfGuid(tl, &CLSID_NovaCalc, &ti) == S_OK && ti);
    if (ti) {
        CHECK("coclass", SUCCEEDED(ITypeInfo_GetTypeAttr(ti, &ta)) && ta->typekind == TKIND_COCLASS && ta->cImplTypes == 2 &&
                         (ta->wTypeFlags & TYPEFLAG_FCANCREATE));
        if (ta) { ITypeInfo_ReleaseTypeAttr(ti, ta); ta = 0; }
        INT fl = 0;
        CHECK("impl type flags", ITypeInfo_GetImplTypeFlags(ti, 1, &fl) == S_OK &&
                                 fl == (IMPLTYPEFLAG_FDEFAULT | IMPLTYPEFLAG_FSOURCE));
        HREFTYPE h;
        ITypeInfo *ev = 0;
        CHECK("source interface", ITypeInfo_GetRefTypeOfImplType(ti, 1, &h) == S_OK &&
                                  ITypeInfo_GetRefTypeInfo(ti, h, &ev) == S_OK && name_is(ev, MEMBERID_NIL, L"DCalcEvents"));
        if (ev) {
            CHECK("dispinterface", SUCCEEDED(ITypeInfo_GetTypeAttr(ev, &ta)) && ta->typekind == TKIND_DISPATCH &&
                                   !(ta->wTypeFlags & TYPEFLAG_FDUAL) && ta->cVars == 1 && ta->cFuncs == 1 &&
                                   ta->cbSizeVft == 7 * sizeof(void *));
            if (ta) { ITypeInfo_ReleaseTypeAttr(ev, ta); ta = 0; }
            FUNCDESC *fd = 0;
            CHECK("dispinterface method", SUCCEEDED(ITypeInfo_GetFuncDesc(ev, 0, &fd)) && fd->funckind == FUNC_DISPATCH &&
                                          fd->memid == 2 && fd->cParams == 1 && fd->elemdescFunc.tdesc.vt == VT_VOID);
            if (fd) ITypeInfo_ReleaseFuncDesc(ev, fd);
            CHECK("dispinterface property", SUCCEEDED(ITypeInfo_GetVarDesc(ev, 0, &vd)) && vd->varkind == VAR_DISPATCH &&
                                            vd->memid == 1);
            if (vd) { ITypeInfo_ReleaseVarDesc(ev, vd); vd = 0; }
            ITypeInfo_Release(ev);
        }
        ITypeLib *owner = 0;
        UINT idx = 99;
        CHECK("GetContainingTypeLib", ITypeInfo_GetContainingTypeLib(ti, &owner, &idx) == S_OK && owner == tl && idx == 4);
        if (owner) ITypeLib_Release(owner);
        ITypeInfo_Release(ti);
    }
}

/* ICalc: a dual interface, seen as a dispinterface and (href -1) as a vtable interface */
static void interface_tests(ITypeLib *tl, ITypeInfo **disp_out, ITypeInfo **vt_out)
{
    ITypeInfo *ti = 0, *vt = 0, *base = 0;
    TYPEATTR *ta = 0;
    CHECK("GetTypeInfoOfGuid(ICalc)", ITypeLib_GetTypeInfoOfGuid(tl, &IID_ICalc, &ti) == S_OK && ti);
    if (!ti) return;
    CHECK("dual: dispatch view", SUCCEEDED(ITypeInfo_GetTypeAttr(ti, &ta)) && ta->typekind == TKIND_DISPATCH &&
                                 (ta->wTypeFlags & TYPEFLAG_FDUAL) && ta->cFuncs == 17 && ta->cImplTypes == 1 &&
                                 ta->cbSizeVft == 7 * sizeof(void *) && IsEqualGUID(&ta->guid, &IID_ICalc));
    if (ta) { ITypeInfo_ReleaseTypeAttr(ti, ta); ta = 0; }
    HREFTYPE h = 0;
    CHECK("dual: href -1", ITypeInfo_GetRefTypeOfImplType(ti, -1, &h) == S_OK && ITypeInfo_GetRefTypeInfo(ti, h, &vt) == S_OK);
    if (!vt) { ITypeInfo_Release(ti); return; }
    CHECK("dual: vtable view", SUCCEEDED(ITypeInfo_GetTypeAttr(vt, &ta)) && ta->typekind == TKIND_INTERFACE &&
                               (ta->wTypeFlags & TYPEFLAG_FDUAL) && ta->cFuncs == 10 &&
                               ta->cbSizeVft == 17 * sizeof(void *));
    if (ta) { ITypeInfo_ReleaseTypeAttr(vt, ta); ta = 0; }
    CHECK("dispatch base", ITypeInfo_GetRefTypeOfImplType(ti, 0, &h) == S_OK && ITypeInfo_GetRefTypeInfo(ti, h, &base) == S_OK &&
                           name_is(base, MEMBERID_NIL, L"IDispatch"));
    if (base) { ITypeInfo_Release(base); base = 0; }
    CHECK("vtable base (stdole2)", ITypeInfo_GetRefTypeOfImplType(vt, 0, &h) == S_OK &&
                                   ITypeInfo_GetRefTypeInfo(vt, h, &base) == S_OK &&
                                   SUCCEEDED(ITypeInfo_GetTypeAttr(base, &ta)) && IsEqualGUID(&ta->guid, &IID_IDispatch) &&
                                   ta->typekind == TKIND_INTERFACE && ta->cFuncs == 4);
    if (ta) { ITypeInfo_ReleaseTypeAttr(base, ta); ta = 0; }
    if (base) ITypeInfo_Release(base);

    /* Scale(double x, [optional, defaultvalue(2)] long factor, [out, retval] double *r) */
    FUNCDESC *fd = 0;
    CHECK("vtable method", SUCCEEDED(ITypeInfo_GetFuncDesc(vt, 2, &fd)) && fd->memid == 3 && fd->funckind == FUNC_PUREVIRTUAL &&
                           fd->cParams == 3 && fd->oVft == 9 * sizeof(void *) && fd->elemdescFunc.tdesc.vt == VT_HRESULT &&
                           fd->callconv == CC_STDCALL);
    if (fd) {
        PARAMDESC *pd = &fd->lprgelemdescParam[1].paramdesc;
        CHECK("optional parameter", (pd->wParamFlags & (PARAMFLAG_FOPT | PARAMFLAG_FHASDEFAULT)) ==
                                    (PARAMFLAG_FOPT | PARAMFLAG_FHASDEFAULT) && pd->pparamdescex &&
                                    pd->pparamdescex->varDefaultValue.vt == VT_I4 && pd->pparamdescex->varDefaultValue.lVal == 2);
        CHECK("retval parameter", (fd->lprgelemdescParam[2].paramdesc.wParamFlags & PARAMFLAG_FRETVAL) &&
                                  fd->lprgelemdescParam[2].tdesc.vt == VT_PTR &&
                                  fd->lprgelemdescParam[2].tdesc.lptdesc->vt == VT_R8);
        ITypeInfo_ReleaseFuncDesc(vt, fd);
        fd = 0;
    }
    /* the same method in the dispatch view: no retval parameter, returns double */
    CHECK("dispatch method", SUCCEEDED(ITypeInfo_GetFuncDesc(ti, 9, &fd)) && fd->memid == 3 && fd->funckind == FUNC_DISPATCH &&
                             fd->cParams == 2 && fd->elemdescFunc.tdesc.vt == VT_R8);
    if (fd) { ITypeInfo_ReleaseFuncDesc(ti, fd); fd = 0; }
    CHECK("dispatch view: IUnknown first", SUCCEEDED(ITypeInfo_GetFuncDesc(ti, 0, &fd)) && fd->memid == 0x60000000);
    if (fd) { ITypeInfo_ReleaseFuncDesc(ti, fd); fd = 0; }

    LPOLESTR names[2] = { L"scale", L"FACTOR" };
    MEMBERID ids[2] = { 0, 0 };
    CHECK("GetIDsOfNames (with a parameter)", ITypeInfo_GetIDsOfNames(ti, names, 2, ids) == S_OK && ids[0] == 3 && ids[1] == 1);
    LPOLESTR bad = L"Nope";
    CHECK("GetIDsOfNames (unknown)", ITypeInfo_GetIDsOfNames(ti, &bad, 1, ids) == DISP_E_UNKNOWNNAME);
    BSTR pn[4] = { 0 };
    UINT got = 0;
    CHECK("GetNames", ITypeInfo_GetNames(vt, 3, pn, 4, &got) == S_OK && got == 4 && !wcscmp(pn[0], L"Scale") &&
                      !wcscmp(pn[2], L"factor") && !wcscmp(pn[3], L"r"));
    for (UINT i = 0; i < got; i++) SysFreeString(pn[i]);
    BSTR doc = 0;
    CHECK("method helpstring", ITypeInfo_GetDocumentation(ti, 1, 0, &doc, 0, 0) == S_OK && doc && !wcscmp(doc, L"Adds two numbers"));
    SysFreeString(doc);

    /* ITypeComp */
    ITypeComp *tc = 0;
    CHECK("GetTypeComp", ti->lpVtbl->GetTypeComp(ti, &tc) == S_OK && tc);
    if (tc) {
        ITypeInfo *bti = 0;
        DESCKIND dk = DESCKIND_NONE;
        BINDPTR bp = { 0 };
        CHECK("ITypeComp::Bind", tc->lpVtbl->Bind(tc, L"Greet", LHashValOfNameSys(sizeof(void *) == 8 ? SYS_WIN64 : SYS_WIN32,
                                                  0, L"Greet"), INVOKE_FUNC, &bti, &dk, &bp) == S_OK &&
                                 dk == DESCKIND_FUNCDESC && bp.lpfuncdesc && bp.lpfuncdesc->memid == 4);
        if (bti) { ITypeInfo_ReleaseFuncDesc(bti, bp.lpfuncdesc); ITypeInfo_Release(bti); }
        tc->lpVtbl->Release(tc);
    }
    ITypeComp *ltc = 0;
    CHECK("library ITypeComp::BindType", tl->lpVtbl->GetTypeComp(tl, &ltc) == S_OK && ltc);
    if (ltc) {
        ITypeInfo *bti = 0;
        ITypeComp *dummy = 0;
        CHECK("library ITypeComp::BindType", ltc->lpVtbl->BindType(ltc, L"NovaPoint", 0, &bti, &dummy) == S_OK && bti &&
                                             name_is(bti, MEMBERID_NIL, L"NovaPoint"));
        if (bti) ITypeInfo_Release(bti);
        ltc->lpVtbl->Release(ltc);
    }
    *disp_out = ti;
    *vt_out = vt;
}

static void stdole_tests(void)
{
    ITypeLib *so = 0;
    CHECK("LoadTypeLib(stdole2.tlb)", LoadTypeLib(L"stdole2.tlb", &so) == S_OK && so);
    if (!so) return;
    ITypeInfo *ti = 0;
    TYPEATTR *ta = 0;
    CHECK("stdole2 IDispatch", ITypeLib_GetTypeInfoOfGuid(so, &IID_IDispatch, &ti) == S_OK &&
                               SUCCEEDED(ITypeInfo_GetTypeAttr(ti, &ta)) && ta->cFuncs == 4 && ta->cImplTypes == 1);
    if (ta) ITypeInfo_ReleaseTypeAttr(ti, ta);
    if (ti) ITypeInfo_Release(ti);
    ITypeLib_Release(so);
    ITypeLib *x = 0;
    CHECK("LoadTypeLib (missing file)", FAILED(LoadTypeLib(L"C:\\nothere.tlb", &x)) && !x);
}

static void registry_tests(ITypeLib *tl, const WCHAR *path)
{
    SYSKIND sk = sizeof(void *) == 8 ? SYS_WIN64 : SYS_WIN32;
    UnRegisterTypeLib(&LIBID_NovaCalcLib, 1, 2, 0, sk);
    ITypeLib *r = 0;
    CHECK("LoadRegTypeLib (not registered)", LoadRegTypeLib(&LIBID_NovaCalcLib, 1, 0, 0, &r) == TYPE_E_LIBNOTREGISTERED);
    CHECK("RegisterTypeLib", RegisterTypeLib(tl, path, 0) == S_OK);
    BSTR p = 0;
    CHECK("QueryPathOfRegTypeLib", QueryPathOfRegTypeLib(&LIBID_NovaCalcLib, 1, 1, 0x409, &p) == S_OK && p &&
                                   !lstrcmpiW(p, path));
    SysFreeString(p);
    CHECK("QueryPathOfRegTypeLib (newer minor)", QueryPathOfRegTypeLib(&LIBID_NovaCalcLib, 1, 3, 0, &p) == TYPE_E_LIBNOTREGISTERED);
    CHECK("LoadRegTypeLib", LoadRegTypeLib(&LIBID_NovaCalcLib, 1, 0, 0, &r) == S_OK && r == tl);
    if (r) ITypeLib_Release(r);
    HKEY k;
    CHECK("Interface key", RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Interface\\{6E6F7661-4341-4C43-8001-000000000002}\\ProxyStubClsid32",
                                         0, KEY_READ, &k) == ERROR_SUCCESS);
    RegCloseKey(k);
    CHECK("UnRegisterTypeLib", UnRegisterTypeLib(&LIBID_NovaCalcLib, 1, 2, 0, sk) == S_OK &&
                               LoadRegTypeLib(&LIBID_NovaCalcLib, 1, 0, 0, &r) == TYPE_E_LIBNOTREGISTERED);
}

/* ---- calls through the type information ---- */
static HRESULT call(ITypeInfo *ti, void *obj, DISPID id, WORD flags, VARIANT *args, UINT n, VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    DISPID put = DISPID_PROPERTYPUT;
    DISPPARAMS dp = { args, 0, n, 0 };
    if (flags & DISPATCH_PROPERTYPUT) { dp.rgdispidNamedArgs = &put; dp.cNamedArgs = 1; }
    if (res) VariantInit(res);
    return DispInvoke(obj, ti, id, flags, &dp, res, ei, argerr);
}

static void invoke_tests(ITypeInfo *ti, ICalc *calc)
{
    VARIANT a[7], r;
    EXCEPINFO ei;
    UINT argerr = 99;
    memset(a, 0, sizeof a);

    a[1].vt = VT_I4; a[1].lVal = 40; a[0].vt = VT_BSTR; a[0].bstrVal = SysAllocString(L"2");   /* converted */
    CHECK("Invoke Add", call(ti, calc, 1, DISPATCH_METHOD, a, 2, &r, 0, 0) == S_OK && r.vt == VT_I4 && r.lVal == 42);
    SysFreeString(a[0].bstrVal);
    LONG count = 0;
    calc->lpVtbl->get_Count(calc, &count);
    CHECK("Invoke Count (property get)", call(ti, calc, 2, DISPATCH_PROPERTYGET, 0, 0, &r, 0, 0) == S_OK && r.vt == VT_I4 &&
                                         r.lVal == count && count > 0);
    CHECK("Invoke Count (as a method)", call(ti, calc, 2, DISPATCH_METHOD | DISPATCH_PROPERTYGET, 0, 0, &r, 0, 0) == S_OK &&
                                        r.lVal == count);

    a[0].vt = VT_R8; a[0].dblVal = 1.5;
    CHECK("Invoke Scale (default argument)", call(ti, calc, 3, DISPATCH_METHOD, a, 1, &r, 0, 0) == S_OK && r.vt == VT_R8 && r.dblVal == 3.0);
    a[1].vt = VT_R8; a[1].dblVal = 1.5; a[0].vt = VT_I2; a[0].iVal = 4;
    CHECK("Invoke Scale", call(ti, calc, 3, DISPATCH_METHOD, a, 2, &r, 0, 0) == S_OK && r.dblVal == 6.0);
    a[1].vt = VT_R8; a[1].dblVal = 1.5; a[0].vt = VT_ERROR; a[0].scode = DISP_E_PARAMNOTFOUND;
    CHECK("Invoke Scale (missing argument)", call(ti, calc, 3, DISPATCH_METHOD, a, 2, &r, 0, 0) == S_OK && r.dblVal == 3.0);
    {   /* named arguments: factor:=10, x:=0.5 */
        DISPID named[2] = { 1, 0 };
        a[0].vt = VT_I4; a[0].lVal = 10; a[1].vt = VT_R8; a[1].dblVal = 0.5;
        DISPPARAMS dp = { a, named, 2, 2 };
        CHECK("Invoke Scale (named arguments)", DispInvoke(calc, ti, 3, DISPATCH_METHOD, &dp, &r, 0, 0) == S_OK && r.dblVal == 5.0);
    }
    CHECK("Invoke (too few arguments)", call(ti, calc, 3, DISPATCH_METHOD, a, 0, &r, 0, 0) == DISP_E_BADPARAMCOUNT);
    CHECK("Invoke (too many arguments)", call(ti, calc, 2, DISPATCH_PROPERTYGET, a, 3, &r, 0, 0) == DISP_E_BADPARAMCOUNT);
    a[0].vt = VT_UNKNOWN; a[0].punkVal = 0;
    a[1].vt = VT_I4; a[1].lVal = 1;
    CHECK("Invoke (type mismatch)", call(ti, calc, 1, DISPATCH_METHOD, a, 2, &r, 0, &argerr) == DISP_E_TYPEMISMATCH && argerr == 0);
    CHECK("Invoke (unknown member)", call(ti, calc, 77, DISPATCH_METHOD, 0, 0, &r, 0, 0) == DISP_E_MEMBERNOTFOUND);

    a[0].vt = VT_BSTR; a[0].bstrVal = SysAllocString(L"Nova");
    CHECK("Invoke Greet (BSTR)", call(ti, calc, 4, DISPATCH_METHOD, a, 1, &r, 0, 0) == S_OK && r.vt == VT_BSTR &&
                                 !wcscmp(r.bstrVal, L"Hello, Nova"));
    VariantClear(&r);
    CHECK("Invoke Name (property put)", call(ti, calc, 5, DISPATCH_PROPERTYPUT, a, 1, 0, 0, 0) == S_OK);
    SysFreeString(a[0].bstrVal);
    CHECK("Invoke Name (property get)", call(ti, calc, 5, DISPATCH_PROPERTYGET, 0, 0, &r, 0, 0) == S_OK && r.vt == VT_BSTR &&
                                        !wcscmp(r.bstrVal, L"Nova"));
    VariantClear(&r);

    a[0].vt = VT_I4; a[0].lVal = 42;
    CHECK("Invoke Describe (VARIANT)", call(ti, calc, 6, DISPATCH_METHOD, a, 1, &r, 0, 0) == S_OK && r.vt == VT_BSTR &&
                                       !wcscmp(r.bstrVal, L"3:42"));
    VariantClear(&r);

    a[0].vt = VT_I4; a[0].lVal = 5;
    memset(&ei, 0, sizeof ei);
    CHECK("Invoke Fail (DISP_E_EXCEPTION)", call(ti, calc, 7, DISPATCH_METHOD, a, 1, &r, &ei, 0) == DISP_E_EXCEPTION &&
                                            ei.scode == (SCODE)0x80040205 && ei.bstrSource && !wcscmp(ei.bstrSource, L"Nova.Calc") &&
                                            ei.bstrDescription && !wcscmp(ei.bstrDescription, L"Calc failed on purpose"));
    SysFreeString(ei.bstrSource); SysFreeString(ei.bstrDescription); SysFreeString(ei.bstrHelpFile);

    /* Sum6(long, double, float, long, double, short): register and stack arguments of both kinds */
    a[5].vt = VT_I4; a[5].lVal = 1;
    a[4].vt = VT_R8; a[4].dblVal = 0.5;
    a[3].vt = VT_R4; a[3].fltVal = 0.25f;
    a[2].vt = VT_I4; a[2].lVal = 100;
    a[1].vt = VT_R8; a[1].dblVal = 1000.0;
    a[0].vt = VT_I2; a[0].iVal = -2;
    CHECK("Invoke Sum6", call(ti, calc, 8, DISPATCH_METHOD, a, 6, &r, 0, 0) == S_OK && r.vt == VT_R8 && r.dblVal == 1099.75);

    /* Swap([in, out] long *, [in, out] BSTR *) */
    LONG n = 41;
    BSTR s = SysAllocString(L"hi");
    a[1].vt = VT_I4 | VT_BYREF; a[1].plVal = &n;
    a[0].vt = VT_BSTR | VT_BYREF; a[0].pbstrVal = &s;
    CHECK("Invoke Swap (by reference)", call(ti, calc, 9, DISPATCH_METHOD, a, 2, &r, 0, 0) == S_OK && n == 42 && s &&
                                        !wcscmp(s, L"hi!"));
    SysFreeString(s);
    VARIANT vn, vs;
    vn.vt = VT_I4; vn.lVal = 1;
    vs.vt = VT_BSTR; vs.bstrVal = SysAllocString(L"x");
    a[1].vt = VT_VARIANT | VT_BYREF; a[1].pvarVal = &vn;
    a[0].vt = VT_VARIANT | VT_BYREF; a[0].pvarVal = &vs;
    CHECK("Invoke Swap (VARIANT by reference)", call(ti, calc, 9, DISPATCH_METHOD, a, 2, &r, 0, 0) == S_OK &&
                                                vn.vt == VT_I4 && vn.lVal == 2 && vs.vt == VT_BSTR && !wcscmp(vs.bstrVal, L"x!"));
    VariantClear(&vs);

    /* the object's own IDispatch (testdll's DispGetIDsOfNames / DispInvoke) */
    IDispatch *d = (IDispatch *)calc;
    LPOLESTR nm = L"Greet";
    DISPID id = 0;
    CHECK("object GetIDsOfNames", d->lpVtbl->GetIDsOfNames(d, &IID_NULL, &nm, 1, 0, &id) == S_OK && id == 4);
    UINT cnt = 0;
    ITypeInfo *oti = 0;
    CHECK("object GetTypeInfo", d->lpVtbl->GetTypeInfoCount(d, &cnt) == S_OK && cnt == 1 &&
                                d->lpVtbl->GetTypeInfo(d, 0, 0, &oti) == S_OK && oti);
    if (oti) ITypeInfo_Release(oti);
    a[0].vt = VT_I4; a[0].lVal = 1;
    memset(&ei, 0, sizeof ei);
    DISPPARAMS dp = { a, 0, 1, 0 };
    CHECK("object Invoke (exception)", d->lpVtbl->Invoke(d, 7, &IID_NULL, 0, DISPATCH_METHOD, &dp, &r, &ei, 0) == DISP_E_EXCEPTION &&
                                       ei.scode == (SCODE)0x80040201);
    SysFreeString(ei.bstrSource); SysFreeString(ei.bstrDescription); SysFreeString(ei.bstrHelpFile);
}

static double __stdcall mix(int a, double b, float c, LONGLONG d, short e)
{
    return a + b + c + (double)d + e;
}

static void dispcall_tests(ICalc *calc)
{
    VARIANT v[7], *pv[7], r;
    VARTYPE vt[7];
    for (int i = 0; i < 7; i++) { VariantInit(&v[i]); pv[i] = &v[i]; }
    v[0].vt = VT_I4; v[0].lVal = 3;
    v[1].vt = VT_R8; v[1].dblVal = 0.5;
    v[2].vt = VT_R4; v[2].fltVal = 0.25f;
    v[3].vt = VT_I8; v[3].llVal = 10000000000LL;
    v[4].vt = VT_I2; v[4].iVal = -1;
    for (int i = 0; i < 5; i++) vt[i] = v[i].vt;
    VariantInit(&r);
    CHECK("DispCallFunc (a function)", DispCallFunc(0, (ULONG_PTR)mix, CC_STDCALL, VT_R8, 5, vt, pv, &r) == S_OK &&
                                       r.vt == VT_R8 && r.dblVal == 10000000002.75);
    /* a vtable method: ICalc::Add(40, 2, &sum) at slot 7 */
    LONG sum = 0;
    v[0].vt = VT_I4; v[0].lVal = 40;
    v[1].vt = VT_I4; v[1].lVal = 2;
    v[2].vt = VT_BYREF | VT_I4; v[2].plVal = &sum;
    for (int i = 0; i < 3; i++) vt[i] = v[i].vt;
    CHECK("DispCallFunc (a vtable method)", DispCallFunc(calc, 7 * sizeof(void *), CC_STDCALL, VT_HRESULT, 3, vt, pv, &r) == S_OK &&
                                            r.vt == VT_HRESULT && r.scode == S_OK && sum == 42);
    CHECK("DispCallFunc (bad calling convention)", DispCallFunc(0, (ULONG_PTR)mix, CC_MAX, VT_R8, 0, 0, 0, &r) == E_INVALIDARG);
}

static void stddispatch_tests(ITypeInfo *vt, ICalc *calc)
{
    IUnknown *u = 0;
    CHECK("CreateStdDispatch", CreateStdDispatch(0, calc, vt, &u) == S_OK && u);
    if (!u) return;
    IDispatch *d = 0;
    CHECK("std dispatch QueryInterface", u->lpVtbl->QueryInterface(u, &IID_IDispatch, (void **)&d) == S_OK && d);
    if (d) {
        LPOLESTR nm = L"Add";
        DISPID id = 0;
        CHECK("std dispatch GetIDsOfNames", d->lpVtbl->GetIDsOfNames(d, &IID_NULL, &nm, 1, 0, &id) == S_OK && id == 1);
        VARIANT a[2], r;
        a[1].vt = VT_I4; a[1].lVal = 20;
        a[0].vt = VT_I4; a[0].lVal = 22;
        DISPPARAMS dp = { a, 0, 2, 0 };
        VariantInit(&r);
        CHECK("std dispatch Invoke", d->lpVtbl->Invoke(d, 1, &IID_NULL, 0, DISPATCH_METHOD, &dp, &r, 0, 0) == S_OK &&
                                     r.vt == VT_I4 && r.lVal == 42);
        d->lpVtbl->Release(d);
    }
    u->lpVtbl->Release(u);
}

int main(void)
{
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    HMODULE srv = LoadLibraryA("testdll.dll");
    WCHAR path[MAX_PATH] = { 0 };
    CHECK("load testdll.dll", srv && GetModuleFileNameW(srv, path, MAX_PATH));

    ITypeLib *tl = 0;
    CHECK("LoadTypeLibEx (a DLL's TYPELIB resource)", LoadTypeLibEx(path, REGKIND_NONE, &tl) == S_OK && tl);
    if (tl) {
        ITypeLib *again = 0;
        WCHAR res1[MAX_PATH + 4];
        swprintf(res1, MAX_PATH + 4, L"%ls\\1", path);
        CHECK("LoadTypeLibEx (path\\1, cached)", LoadTypeLibEx(res1, REGKIND_NONE, &again) == S_OK && again == tl);
        if (again) ITypeLib_Release(again);
        library_tests(tl);
        stdole_tests();
        registry_tests(tl, path);

        ITypeInfo *disp = 0, *vt = 0;
        interface_tests(tl, &disp, &vt);
        HRESULT (__stdcall *gco)(REFCLSID, REFIID, void **) =
            (HRESULT (__stdcall *)(REFCLSID, REFIID, void **))GetProcAddress(srv, "DllGetClassObject");
        IClassFactory *cf = 0;
        ICalc *calc = 0;
        CHECK("create Nova.Calc", gco && gco(&CLSID_NovaCalc, &IID_IClassFactory, (void **)&cf) == S_OK &&
                                  cf->lpVtbl->CreateInstance(cf, 0, &IID_ICalc, (void **)&calc) == S_OK && calc);
        if (calc && disp && vt) {
            invoke_tests(disp, calc);
            invoke_tests(vt, calc);         /* the vtable view invokes the same way */
            dispcall_tests(calc);
            stddispatch_tests(vt, calc);
        }
        if (calc) calc->lpVtbl->Release(calc);
        if (disp) ITypeInfo_Release(disp);
        if (vt) ITypeInfo_Release(vt);
        ITypeLib_Release(tl);
    }
    CHECK("LHashValOfNameSys ignores case", LHashValOfNameSys(SYS_WIN32, 0x409, L"Calc") == LHashValOfNameSys(SYS_WIN32, 0x409, L"CALC"));
    CoUninitialize();
    printf("tlbtest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
