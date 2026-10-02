/* novacalc.h — the sample COM class served by testdll.dll ("Nova.Calc") */
#pragma once
#include <oleauto.h>

/* {6E6F7661-4341-4C43-8001-000000000001} */
DEFINE_GUID(CLSID_NovaCalc, 0x6E6F7661, 0x4341, 0x4C43, 0x80, 0x01, 0, 0, 0, 0, 0, 0x01);
/* {6E6F7661-4341-4C43-8001-000000000002} */
DEFINE_GUID(IID_ICalc,      0x6E6F7661, 0x4341, 0x4C43, 0x80, 0x01, 0, 0, 0, 0, 0, 0x02);

/* {6E6F7661-4341-4C43-8001-000000000010}: the type library (idl/novacalc.idl), TYPELIB resource 1 */
DEFINE_GUID(LIBID_NovaCalcLib, 0x6E6F7661, 0x4341, 0x4C43, 0x80, 0x01, 0, 0, 0, 0, 0, 0x10);
/* {6E6F7661-4341-4C43-8001-000000000003}: its event dispinterface */
DEFINE_GUID(DIID_DCalcEvents, 0x6E6F7661, 0x4341, 0x4C43, 0x80, 0x01, 0, 0, 0, 0, 0, 0x03);

/* ICalc, a dual interface (DISPIDs as in idl/novacalc.idl): Add 1, Count 2, Scale 3,
   Greet 4, Name 5, Describe 6, Fail 7, Sum6 8, Swap 9 */
#undef INTERFACE
#define INTERFACE ICalc
DECLARE_INTERFACE_(ICalc, IDispatch)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(GetTypeInfoCount)(THIS_ UINT *n) PURE;
    STDMETHOD(GetTypeInfo)(THIS_ UINT i, LCID lcid, ITypeInfo **out) PURE;
    STDMETHOD(GetIDsOfNames)(THIS_ REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids) PURE;
    STDMETHOD(Invoke)(THIS_ DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params,
                      VARIANT *result, EXCEPINFO *ei, UINT *argerr) PURE;
#endif
    STDMETHOD(Add)(THIS_ LONG a, LONG b, LONG *sum) PURE;
    STDMETHOD(get_Count)(THIS_ LONG *n) PURE;
    STDMETHOD(Scale)(THIS_ double x, LONG factor, double *r) PURE;
    STDMETHOD(Greet)(THIS_ BSTR who, BSTR *s) PURE;
    STDMETHOD(get_Name)(THIS_ BSTR *s) PURE;
    STDMETHOD(put_Name)(THIS_ BSTR s) PURE;
    STDMETHOD(Describe)(THIS_ VARIANT v, BSTR *s) PURE;
    STDMETHOD(Fail)(THIS_ LONG code) PURE;
    STDMETHOD(Sum6)(THIS_ LONG a, double b, float c, LONG d, double e, SHORT f, double *r) PURE;
    STDMETHOD(Swap)(THIS_ LONG *a, BSTR *b) PURE;
    END_INTERFACE
};
