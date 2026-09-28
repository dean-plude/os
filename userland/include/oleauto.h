/* oleauto.h — OLE Automation: BSTR, VARIANT, SAFEARRAY, IDispatch, error info (oleaut32) */
#pragma once
#include <objbase.h>

_NOVA_BEGIN

typedef OLECHAR *BSTR;
typedef BSTR *LPBSTR;
typedef unsigned short VARTYPE;
typedef short VARIANT_BOOL;
typedef double DATE;
typedef LONG DISPID;
typedef DISPID MEMBERID;
#define VARIANT_TRUE  ((VARIANT_BOOL)-1)
#define VARIANT_FALSE ((VARIANT_BOOL)0)

typedef union tagCY { struct { ULONG Lo; LONG Hi; }; LONGLONG int64; } CY;
typedef struct tagDEC {
    USHORT wReserved;
    union { struct { BYTE scale, sign; }; USHORT signscale; };
    ULONG Hi32;
    union { struct { ULONG Lo32, Mid32; }; ULONGLONG Lo64; };
} DECIMAL;

enum VARENUM {
    VT_EMPTY = 0, VT_NULL = 1, VT_I2 = 2, VT_I4 = 3, VT_R4 = 4, VT_R8 = 5, VT_CY = 6, VT_DATE = 7,
    VT_BSTR = 8, VT_DISPATCH = 9, VT_ERROR = 10, VT_BOOL = 11, VT_VARIANT = 12, VT_UNKNOWN = 13,
    VT_DECIMAL = 14, VT_I1 = 16, VT_UI1 = 17, VT_UI2 = 18, VT_UI4 = 19, VT_I8 = 20, VT_UI8 = 21,
    VT_INT = 22, VT_UINT = 23, VT_VOID = 24, VT_HRESULT = 25, VT_PTR = 26, VT_SAFEARRAY = 27,
    VT_CARRAY = 28, VT_USERDEFINED = 29, VT_LPSTR = 30, VT_LPWSTR = 31, VT_RECORD = 36,
    VT_INT_PTR = 37, VT_UINT_PTR = 38, VT_FILETIME = 64, VT_BLOB = 65, VT_CLSID = 72,
    VT_VECTOR = 0x1000, VT_ARRAY = 0x2000, VT_BYREF = 0x4000, VT_RESERVED = 0x8000,
    VT_ILLEGAL = 0xFFFF, VT_ILLEGALMASKED = 0x0FFF, VT_TYPEMASK = 0x0FFF,
};

typedef struct tagSAFEARRAYBOUND { ULONG cElements; LONG lLbound; } SAFEARRAYBOUND, *LPSAFEARRAYBOUND;
typedef struct tagSAFEARRAY {
    USHORT cDims;
    USHORT fFeatures;
    ULONG cbElements;
    ULONG cLocks;
    PVOID pvData;
    SAFEARRAYBOUND rgsabound[1];
} SAFEARRAY, *LPSAFEARRAY;
#define FADF_AUTO        0x0001
#define FADF_STATIC      0x0002
#define FADF_EMBEDDED    0x0004
#define FADF_FIXEDSIZE   0x0010
#define FADF_RECORD      0x0020
#define FADF_HAVEIID     0x0040
#define FADF_HAVEVARTYPE 0x0080
#define FADF_BSTR        0x0100
#define FADF_UNKNOWN     0x0200
#define FADF_DISPATCH    0x0400
#define FADF_VARIANT     0x0800

typedef interface IDispatch IDispatch;
typedef interface IRecordInfo IRecordInfo;
typedef interface ITypeInfo ITypeInfo;
typedef interface ITypeLib ITypeLib;
typedef struct tagVARIANT VARIANT, *LPVARIANT, VARIANTARG, *LPVARIANTARG;

struct tagVARIANT {
    union {
        struct {
            VARTYPE vt;
            WORD wReserved1, wReserved2, wReserved3;
            union {
                LONGLONG llVal; LONG lVal; BYTE bVal; SHORT iVal; float fltVal; double dblVal;
                VARIANT_BOOL boolVal; HRESULT scode; CY cyVal; DATE date; BSTR bstrVal;
                IUnknown *punkVal; IDispatch *pdispVal; SAFEARRAY *parray;
                BYTE *pbVal; SHORT *piVal; LONG *plVal; LONGLONG *pllVal; float *pfltVal; double *pdblVal;
                VARIANT_BOOL *pboolVal; HRESULT *pscode; CY *pcyVal; DATE *pdate; BSTR *pbstrVal;
                IUnknown **ppunkVal; IDispatch **ppdispVal; SAFEARRAY **pparray; VARIANT *pvarVal;
                PVOID byref; CHAR cVal; USHORT uiVal; ULONG ulVal; ULONGLONG ullVal; INT intVal; UINT uintVal;
                DECIMAL *pdecVal; CHAR *pcVal; USHORT *puiVal; ULONG *pulVal; ULONGLONG *pullVal;
                INT *pintVal; UINT *puintVal;
                struct { PVOID pvRecord; IRecordInfo *pRecInfo; };
            };
        };
        DECIMAL decVal;
    };
};

#define V_VT(v)        ((v)->vt)
#define V_ISBYREF(v)   (V_VT(v) & VT_BYREF)
#define V_ISARRAY(v)   (V_VT(v) & VT_ARRAY)
#define V_UNION(v, m)  ((v)->m)
#define V_I1(v)        ((v)->cVal)
#define V_UI1(v)       ((v)->bVal)
#define V_I2(v)        ((v)->iVal)
#define V_UI2(v)       ((v)->uiVal)
#define V_I4(v)        ((v)->lVal)
#define V_UI4(v)       ((v)->ulVal)
#define V_I8(v)        ((v)->llVal)
#define V_UI8(v)       ((v)->ullVal)
#define V_INT(v)       ((v)->intVal)
#define V_UINT(v)      ((v)->uintVal)
#define V_R4(v)        ((v)->fltVal)
#define V_R8(v)        ((v)->dblVal)
#define V_CY(v)        ((v)->cyVal)
#define V_DATE(v)      ((v)->date)
#define V_BSTR(v)      ((v)->bstrVal)
#define V_BOOL(v)      ((v)->boolVal)
#define V_ERROR(v)     ((v)->scode)
#define V_UNKNOWN(v)   ((v)->punkVal)
#define V_DISPATCH(v)  ((v)->pdispVal)
#define V_ARRAY(v)     ((v)->parray)
#define V_BYREF(v)     ((v)->byref)
#define V_VARIANTREF(v) ((v)->pvarVal)
#define V_DECIMAL(v)   ((v)->decVal)

typedef struct tagDISPPARAMS { VARIANTARG *rgvarg; DISPID *rgdispidNamedArgs; UINT cArgs, cNamedArgs; } DISPPARAMS;
typedef struct tagEXCEPINFO {
    WORD wCode, wReserved;
    BSTR bstrSource, bstrDescription, bstrHelpFile;
    DWORD dwHelpContext;
    PVOID pvReserved;
    HRESULT (__stdcall *pfnDeferredFillIn)(struct tagEXCEPINFO *);
    HRESULT scode;
} EXCEPINFO, *LPEXCEPINFO;
#define DISPATCH_METHOD         0x1
#define DISPATCH_PROPERTYGET    0x2
#define DISPATCH_PROPERTYPUT    0x4
#define DISPATCH_PROPERTYPUTREF 0x8
#define DISPID_UNKNOWN     (-1)
#define DISPID_VALUE       0
#define DISPID_PROPERTYPUT (-3)
#define DISPID_NEWENUM     (-4)

#define DISP_E_UNKNOWNINTERFACE ((HRESULT)0x80020001L)
#define DISP_E_MEMBERNOTFOUND   ((HRESULT)0x80020003L)
#define DISP_E_PARAMNOTFOUND    ((HRESULT)0x80020004L)
#define DISP_E_TYPEMISMATCH     ((HRESULT)0x80020005L)
#define DISP_E_UNKNOWNNAME      ((HRESULT)0x80020006L)
#define DISP_E_NONAMEDARGS      ((HRESULT)0x80020007L)
#define DISP_E_BADVARTYPE       ((HRESULT)0x80020008L)
#define DISP_E_EXCEPTION        ((HRESULT)0x80020009L)
#define DISP_E_OVERFLOW         ((HRESULT)0x8002000AL)
#define DISP_E_BADINDEX         ((HRESULT)0x8002000BL)
#define DISP_E_ARRAYISLOCKED    ((HRESULT)0x8002000DL)
#define DISP_E_BADPARAMCOUNT    ((HRESULT)0x8002000EL)
#define DISP_E_PARAMNOTOPTIONAL ((HRESULT)0x8002000FL)
#define DISP_E_BADCALLEE        ((HRESULT)0x80020010L)
#define DISP_E_DIVBYZERO        ((HRESULT)0x80020012L)
#define TYPE_E_CANTLOADLIBRARY  ((HRESULT)0x80029C4AL)
#define TYPE_E_ELEMENTNOTFOUND  ((HRESULT)0x8002802BL)
#define TYPE_E_LIBNOTREGISTERED ((HRESULT)0x8002801DL)

/* ---- IDispatch ---- */
#undef INTERFACE
#define INTERFACE IDispatch
DECLARE_INTERFACE_(IDispatch, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD(GetTypeInfoCount)(THIS_ UINT *n) PURE;
    STDMETHOD(GetTypeInfo)(THIS_ UINT i, LCID lcid, ITypeInfo **out) PURE;
    STDMETHOD(GetIDsOfNames)(THIS_ REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids) PURE;
    STDMETHOD(Invoke)(THIS_ DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params,
                      VARIANT *result, EXCEPINFO *ei, UINT *argerr) PURE;
    END_INTERFACE
};
typedef IDispatch *LPDISPATCH;

/* ---- IErrorInfo / ICreateErrorInfo / ISupportErrorInfo ---- */
#undef INTERFACE
#define INTERFACE IErrorInfo
DECLARE_INTERFACE_(IErrorInfo, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD(GetGUID)(THIS_ GUID *g) PURE;
    STDMETHOD(GetSource)(THIS_ BSTR *s) PURE;
    STDMETHOD(GetDescription)(THIS_ BSTR *s) PURE;
    STDMETHOD(GetHelpFile)(THIS_ BSTR *s) PURE;
    STDMETHOD(GetHelpContext)(THIS_ DWORD *c) PURE;
    END_INTERFACE
};

#undef INTERFACE
#define INTERFACE ICreateErrorInfo
DECLARE_INTERFACE_(ICreateErrorInfo, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD(SetGUID)(THIS_ REFGUID g) PURE;
    STDMETHOD(SetSource)(THIS_ LPOLESTR s) PURE;
    STDMETHOD(SetDescription)(THIS_ LPOLESTR s) PURE;
    STDMETHOD(SetHelpFile)(THIS_ LPOLESTR s) PURE;
    STDMETHOD(SetHelpContext)(THIS_ DWORD c) PURE;
    END_INTERFACE
};

#undef INTERFACE
#define INTERFACE ISupportErrorInfo
DECLARE_INTERFACE_(ISupportErrorInfo, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD(InterfaceSupportsErrorInfo)(THIS_ REFIID riid) PURE;
    END_INTERFACE
};

#ifndef __cplusplus
#define IDispatch_QueryInterface(p, r, v)   (p)->lpVtbl->QueryInterface(p, r, v)
#define IDispatch_AddRef(p)                 (p)->lpVtbl->AddRef(p)
#define IDispatch_Release(p)                (p)->lpVtbl->Release(p)
#define IDispatch_GetIDsOfNames(p, r, n, c, l, d) (p)->lpVtbl->GetIDsOfNames(p, r, n, c, l, d)
#define IDispatch_Invoke(p, d, r, l, f, a, v, e, x) (p)->lpVtbl->Invoke(p, d, r, l, f, a, v, e, x)
#define IErrorInfo_GetDescription(p, s)     (p)->lpVtbl->GetDescription(p, s)
#define IErrorInfo_GetSource(p, s)          (p)->lpVtbl->GetSource(p, s)
#define IErrorInfo_Release(p)               (p)->lpVtbl->Release(p)
#define ICreateErrorInfo_SetDescription(p, s) (p)->lpVtbl->SetDescription(p, s)
#define ICreateErrorInfo_SetSource(p, s)    (p)->lpVtbl->SetSource(p, s)
#define ICreateErrorInfo_QueryInterface(p, r, v) (p)->lpVtbl->QueryInterface(p, r, v)
#define ICreateErrorInfo_Release(p)         (p)->lpVtbl->Release(p)
#endif

/* ---- BSTR ---- */
WINOLEAUTAPI_(BSTR)    SysAllocString(const OLECHAR *s);
WINOLEAUTAPI_(BSTR)    SysAllocStringLen(const OLECHAR *s, UINT n);
WINOLEAUTAPI_(BSTR)    SysAllocStringByteLen(LPCSTR s, UINT n);
WINOLEAUTAPI_(INT)     SysReAllocString(BSTR *b, const OLECHAR *s);
WINOLEAUTAPI_(INT)     SysReAllocStringLen(BSTR *b, const OLECHAR *s, UINT n);
WINOLEAUTAPI_(void)    SysFreeString(BSTR b);
WINOLEAUTAPI_(UINT)    SysStringLen(BSTR b);
WINOLEAUTAPI_(UINT)    SysStringByteLen(BSTR b);

/* ---- VARIANT ---- */
#define VARIANT_NOVALUEPROP      0x01
#define VARIANT_ALPHABOOL        0x02
#define VARIANT_NOUSEROVERRIDE   0x04
#define VARIANT_LOCALBOOL        0x10
WINOLEAUTAPI_(void)    VariantInit(VARIANTARG *v);
WINOLEAUTAPI_(HRESULT) VariantClear(VARIANTARG *v);
WINOLEAUTAPI_(HRESULT) VariantCopy(VARIANTARG *dst, const VARIANTARG *src);
WINOLEAUTAPI_(HRESULT) VariantCopyInd(VARIANT *dst, const VARIANTARG *src);
WINOLEAUTAPI_(HRESULT) VariantChangeType(VARIANTARG *dst, const VARIANTARG *src, USHORT flags, VARTYPE vt);
WINOLEAUTAPI_(HRESULT) VariantChangeTypeEx(VARIANTARG *dst, const VARIANTARG *src, LCID lcid, USHORT flags, VARTYPE vt);
WINOLEAUTAPI_(HRESULT) VarBstrCat(BSTR a, BSTR b, BSTR *out);
WINOLEAUTAPI_(HRESULT) VarBstrCmp(BSTR a, BSTR b, LCID lcid, ULONG flags);
WINOLEAUTAPI_(HRESULT) VarBstrFromI4(LONG v, LCID lcid, ULONG flags, BSTR *out);
WINOLEAUTAPI_(HRESULT) VarBstrFromR8(double v, LCID lcid, ULONG flags, BSTR *out);
WINOLEAUTAPI_(HRESULT) VarI4FromStr(const OLECHAR *s, LCID lcid, ULONG flags, LONG *out);
WINOLEAUTAPI_(HRESULT) VarR8FromStr(const OLECHAR *s, LCID lcid, ULONG flags, double *out);
WINOLEAUTAPI_(INT)     SystemTimeToVariantTime(SYSTEMTIME *st, double *out);
WINOLEAUTAPI_(INT)     VariantTimeToSystemTime(double t, SYSTEMTIME *st);
#define VARCMP_LT   0
#define VARCMP_EQ   1
#define VARCMP_GT   2
#define VARCMP_NULL 3

/* ---- SAFEARRAY ---- */
WINOLEAUTAPI_(SAFEARRAY *) SafeArrayCreate(VARTYPE vt, UINT dims, SAFEARRAYBOUND *bounds);
WINOLEAUTAPI_(SAFEARRAY *) SafeArrayCreateVector(VARTYPE vt, LONG lbound, ULONG n);
WINOLEAUTAPI_(HRESULT) SafeArrayDestroy(SAFEARRAY *a);
WINOLEAUTAPI_(HRESULT) SafeArrayAllocDescriptor(UINT dims, SAFEARRAY **out);
WINOLEAUTAPI_(HRESULT) SafeArrayAllocDescriptorEx(VARTYPE vt, UINT dims, SAFEARRAY **out);
WINOLEAUTAPI_(HRESULT) SafeArrayAllocData(SAFEARRAY *a);
WINOLEAUTAPI_(HRESULT) SafeArrayDestroyData(SAFEARRAY *a);
WINOLEAUTAPI_(HRESULT) SafeArrayDestroyDescriptor(SAFEARRAY *a);
WINOLEAUTAPI_(UINT)    SafeArrayGetDim(SAFEARRAY *a);
WINOLEAUTAPI_(UINT)    SafeArrayGetElemsize(SAFEARRAY *a);
WINOLEAUTAPI_(HRESULT) SafeArrayGetLBound(SAFEARRAY *a, UINT dim, LONG *out);
WINOLEAUTAPI_(HRESULT) SafeArrayGetUBound(SAFEARRAY *a, UINT dim, LONG *out);
WINOLEAUTAPI_(HRESULT) SafeArrayGetVartype(SAFEARRAY *a, VARTYPE *vt);
WINOLEAUTAPI_(HRESULT) SafeArrayLock(SAFEARRAY *a);
WINOLEAUTAPI_(HRESULT) SafeArrayUnlock(SAFEARRAY *a);
WINOLEAUTAPI_(HRESULT) SafeArrayAccessData(SAFEARRAY *a, void **data);
WINOLEAUTAPI_(HRESULT) SafeArrayUnaccessData(SAFEARRAY *a);
WINOLEAUTAPI_(HRESULT) SafeArrayPtrOfIndex(SAFEARRAY *a, LONG *idx, void **out);
WINOLEAUTAPI_(HRESULT) SafeArrayGetElement(SAFEARRAY *a, LONG *idx, void *out);
WINOLEAUTAPI_(HRESULT) SafeArrayPutElement(SAFEARRAY *a, LONG *idx, void *in);
WINOLEAUTAPI_(HRESULT) SafeArrayCopy(SAFEARRAY *a, SAFEARRAY **out);
WINOLEAUTAPI_(HRESULT) SafeArrayCopyData(SAFEARRAY *src, SAFEARRAY *dst);
WINOLEAUTAPI_(HRESULT) SafeArrayRedim(SAFEARRAY *a, SAFEARRAYBOUND *bound);

/* ---- error info ---- */
WINOLEAUTAPI_(HRESULT) SetErrorInfo(ULONG reserved, IErrorInfo *e);
WINOLEAUTAPI_(HRESULT) GetErrorInfo(ULONG reserved, IErrorInfo **out);
WINOLEAUTAPI_(HRESULT) CreateErrorInfo(ICreateErrorInfo **out);

_NOVA_END
