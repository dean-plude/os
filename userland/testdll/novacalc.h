/* novacalc.h — the sample COM class served by testdll.dll ("Nova.Calc") */
#pragma once
#include <oleauto.h>

/* {6E6F7661-4341-4C43-8001-000000000001} */
DEFINE_GUID(CLSID_NovaCalc, 0x6E6F7661, 0x4341, 0x4C43, 0x80, 0x01, 0, 0, 0, 0, 0, 0x01);
/* {6E6F7661-4341-4C43-8001-000000000002} */
DEFINE_GUID(IID_ICalc,      0x6E6F7661, 0x4341, 0x4C43, 0x80, 0x01, 0, 0, 0, 0, 0, 0x02);

/* ICalc derives from IDispatch: Add is DISPID 1, Count (a property) DISPID 2 */
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
    END_INTERFACE
};
