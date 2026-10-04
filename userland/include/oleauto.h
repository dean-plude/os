/* oleauto.h — OLE Automation: BSTR, VARIANT, SAFEARRAY, IDispatch, type
 * libraries (ITypeLib/ITypeInfo), error info (oleaut32) */
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

/* ---- type libraries: ITypeLib, ITypeInfo and their descriptions ---- */
#define TYPE_E_BUFFERTOOSMALL   ((HRESULT)0x80028016L)
#define TYPE_E_INVDATAREAD      ((HRESULT)0x80028018L)
#define TYPE_E_UNSUPFORMAT      ((HRESULT)0x80028019L)
#define TYPE_E_REGISTRYACCESS   ((HRESULT)0x8002801CL)
#define TYPE_E_UNDEFINEDTYPE    ((HRESULT)0x80028027L)
#define TYPE_E_QUALIFIEDNAMEDISALLOWED ((HRESULT)0x80028028L)
#define TYPE_E_INVALIDSTATE     ((HRESULT)0x80028029L)
#define TYPE_E_WRONGTYPEKIND    ((HRESULT)0x8002802AL)
#define TYPE_E_AMBIGUOUSNAME    ((HRESULT)0x8002802CL)
#define TYPE_E_BADMODULEKIND    ((HRESULT)0x800288BDL)
#define TYPE_E_DLLFUNCTIONNOTFOUND ((HRESULT)0x8002802FL)
#define TYPE_E_IOERROR          ((HRESULT)0x80028CA2L)
#define TYPE_E_CANTCREATETMPFILE ((HRESULT)0x80028CA3L)

typedef DWORD HREFTYPE;
typedef LONG SCODE;
#define MEMBERID_NIL DISPID_UNKNOWN

typedef enum tagTYPEKIND {
    TKIND_ENUM, TKIND_RECORD, TKIND_MODULE, TKIND_INTERFACE, TKIND_DISPATCH, TKIND_COCLASS, TKIND_ALIAS,
    TKIND_UNION, TKIND_MAX
} TYPEKIND;
typedef enum tagSYSKIND { SYS_WIN16, SYS_WIN32, SYS_MAC, SYS_WIN64 } SYSKIND;
typedef enum tagREGKIND { REGKIND_DEFAULT, REGKIND_REGISTER, REGKIND_NONE } REGKIND;
typedef enum tagFUNCKIND { FUNC_VIRTUAL, FUNC_PUREVIRTUAL, FUNC_NONVIRTUAL, FUNC_STATIC, FUNC_DISPATCH } FUNCKIND;
typedef enum tagINVOKEKIND {
    INVOKE_FUNC = 1, INVOKE_PROPERTYGET = 2, INVOKE_PROPERTYPUT = 4, INVOKE_PROPERTYPUTREF = 8
} INVOKEKIND;
typedef enum tagCALLCONV {
    CC_FASTCALL, CC_CDECL, CC_MSCPASCAL, CC_PASCAL = CC_MSCPASCAL, CC_MACPASCAL, CC_STDCALL,
    CC_FPFASTCALL, CC_SYSCALL, CC_MPWCDECL, CC_MPWPASCAL, CC_MAX
} CALLCONV;
typedef enum tagVARKIND { VAR_PERINSTANCE, VAR_STATIC, VAR_CONST, VAR_DISPATCH } VARKIND;
typedef enum tagDESCKIND {
    DESCKIND_NONE, DESCKIND_FUNCDESC, DESCKIND_VARDESC, DESCKIND_TYPECOMP, DESCKIND_IMPLICITAPPOBJ, DESCKIND_MAX
} DESCKIND;
typedef enum tagLIBFLAGS { LIBFLAG_FRESTRICTED = 1, LIBFLAG_FCONTROL = 2, LIBFLAG_FHIDDEN = 4, LIBFLAG_FHASDISKIMAGE = 8 } LIBFLAGS;
enum {
    TYPEFLAG_FAPPOBJECT = 0x1, TYPEFLAG_FCANCREATE = 0x2, TYPEFLAG_FLICENSED = 0x4, TYPEFLAG_FPREDECLID = 0x8,
    TYPEFLAG_FHIDDEN = 0x10, TYPEFLAG_FCONTROL = 0x20, TYPEFLAG_FDUAL = 0x40, TYPEFLAG_FNONEXTENSIBLE = 0x80,
    TYPEFLAG_FOLEAUTOMATION = 0x100, TYPEFLAG_FRESTRICTED = 0x200, TYPEFLAG_FAGGREGATABLE = 0x400,
    TYPEFLAG_FREPLACEABLE = 0x800, TYPEFLAG_FDISPATCHABLE = 0x1000, TYPEFLAG_FREVERSEBIND = 0x2000,
    TYPEFLAG_FPROXY = 0x4000,
};
enum {
    FUNCFLAG_FRESTRICTED = 0x1, FUNCFLAG_FSOURCE = 0x2, FUNCFLAG_FBINDABLE = 0x4, FUNCFLAG_FREQUESTEDIT = 0x8,
    FUNCFLAG_FDISPLAYBIND = 0x10, FUNCFLAG_FDEFAULTBIND = 0x20, FUNCFLAG_FHIDDEN = 0x40,
    FUNCFLAG_FUSESGETLASTERROR = 0x80, FUNCFLAG_FDEFAULTCOLLELEM = 0x100, FUNCFLAG_FUIDEFAULT = 0x200,
    FUNCFLAG_FNONBROWSABLE = 0x400, FUNCFLAG_FREPLACEABLE = 0x800, FUNCFLAG_FIMMEDIATEBIND = 0x1000,
};
#define IMPLTYPEFLAG_FDEFAULT       0x1
#define IMPLTYPEFLAG_FSOURCE        0x2
#define IMPLTYPEFLAG_FRESTRICTED    0x4
#define IMPLTYPEFLAG_FDEFAULTVTABLE 0x8
#define PARAMFLAG_NONE         0x00
#define PARAMFLAG_FIN          0x01
#define PARAMFLAG_FOUT         0x02
#define PARAMFLAG_FLCID        0x04
#define PARAMFLAG_FRETVAL      0x08
#define PARAMFLAG_FOPT         0x10
#define PARAMFLAG_FHASDEFAULT  0x20
#define PARAMFLAG_FHASCUSTDATA 0x40

typedef struct tagTYPEDESC {
    union { struct tagTYPEDESC *lptdesc; struct tagARRAYDESC *lpadesc; HREFTYPE hreftype; };
    VARTYPE vt;
} TYPEDESC;
typedef struct tagARRAYDESC { TYPEDESC tdescElem; USHORT cDims; SAFEARRAYBOUND rgbounds[1]; } ARRAYDESC;
typedef struct tagPARAMDESCEX { ULONG cBytes; VARIANTARG varDefaultValue; } PARAMDESCEX, *LPPARAMDESCEX;
typedef struct tagPARAMDESC { LPPARAMDESCEX pparamdescex; USHORT wParamFlags; } PARAMDESC, *LPPARAMDESC;
typedef struct tagIDLDESC { ULONG_PTR dwReserved; USHORT wIDLFlags; } IDLDESC, *LPIDLDESC;
typedef struct tagELEMDESC { TYPEDESC tdesc; union { IDLDESC idldesc; PARAMDESC paramdesc; }; } ELEMDESC, *LPELEMDESC;
typedef struct tagTYPEATTR {
    GUID guid; LCID lcid; DWORD dwReserved; MEMBERID memidConstructor, memidDestructor;
    LPOLESTR lpstrSchema; ULONG cbSizeInstance; TYPEKIND typekind; WORD cFuncs, cVars, cImplTypes, cbSizeVft,
    cbAlignment, wTypeFlags, wMajorVerNum, wMinorVerNum; TYPEDESC tdescAlias; IDLDESC idldescType;
} TYPEATTR, *LPTYPEATTR;
typedef struct tagFUNCDESC {
    MEMBERID memid; SCODE *lprgscode; ELEMDESC *lprgelemdescParam; FUNCKIND funckind; INVOKEKIND invkind;
    CALLCONV callconv; SHORT cParams, cParamsOpt, oVft, cScodes; ELEMDESC elemdescFunc; WORD wFuncFlags;
} FUNCDESC, *LPFUNCDESC;
typedef struct tagVARDESC {
    MEMBERID memid; LPOLESTR lpstrSchema; union { ULONG oInst; VARIANT *lpvarValue; };
    ELEMDESC elemdescVar; WORD wVarFlags; VARKIND varkind;
} VARDESC, *LPVARDESC;
typedef struct tagTLIBATTR { GUID guid; LCID lcid; SYSKIND syskind; WORD wMajorVerNum, wMinorVerNum, wLibFlags; } TLIBATTR, *LPTLIBATTR;
typedef struct tagCUSTDATAITEM { GUID guid; VARIANTARG varValue; } CUSTDATAITEM;
typedef struct tagCUSTDATA { DWORD cCustData; CUSTDATAITEM *prgCustData; } CUSTDATA;
typedef interface ITypeComp ITypeComp;
typedef union tagBINDPTR { FUNCDESC *lpfuncdesc; VARDESC *lpvardesc; ITypeComp *lptcomp; } BINDPTR;

#undef INTERFACE
#define INTERFACE ITypeComp
DECLARE_INTERFACE_(ITypeComp, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD(Bind)(THIS_ LPOLESTR name, ULONG hash, WORD flags, ITypeInfo **ti, DESCKIND *kind, BINDPTR *bind) PURE;
    STDMETHOD(BindType)(THIS_ LPOLESTR name, ULONG hash, ITypeInfo **ti, ITypeComp **tc) PURE;
    END_INTERFACE
};

#undef INTERFACE
#define INTERFACE ITypeInfo
DECLARE_INTERFACE_(ITypeInfo, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD(GetTypeAttr)(THIS_ TYPEATTR **attr) PURE;
    STDMETHOD(GetTypeComp)(THIS_ ITypeComp **tc) PURE;
    STDMETHOD(GetFuncDesc)(THIS_ UINT index, FUNCDESC **fd) PURE;
    STDMETHOD(GetVarDesc)(THIS_ UINT index, VARDESC **vd) PURE;
    STDMETHOD(GetNames)(THIS_ MEMBERID memid, BSTR *names, UINT max, UINT *n) PURE;
    STDMETHOD(GetRefTypeOfImplType)(THIS_ UINT index, HREFTYPE *href) PURE;
    STDMETHOD(GetImplTypeFlags)(THIS_ UINT index, INT *flags) PURE;
    STDMETHOD(GetIDsOfNames)(THIS_ LPOLESTR *names, UINT n, MEMBERID *ids) PURE;
    STDMETHOD(Invoke)(THIS_ PVOID obj, MEMBERID memid, WORD flags, DISPPARAMS *params, VARIANT *result,
                      EXCEPINFO *ei, UINT *argerr) PURE;
    STDMETHOD(GetDocumentation)(THIS_ MEMBERID memid, BSTR *name, BSTR *doc, DWORD *helpctx, BSTR *helpfile) PURE;
    STDMETHOD(GetDllEntry)(THIS_ MEMBERID memid, INVOKEKIND kind, BSTR *dll, BSTR *name, WORD *ordinal) PURE;
    STDMETHOD(GetRefTypeInfo)(THIS_ HREFTYPE href, ITypeInfo **ti) PURE;
    STDMETHOD(AddressOfMember)(THIS_ MEMBERID memid, INVOKEKIND kind, PVOID *addr) PURE;
    STDMETHOD(CreateInstance)(THIS_ IUnknown *outer, REFIID riid, PVOID *obj) PURE;
    STDMETHOD(GetMops)(THIS_ MEMBERID memid, BSTR *mops) PURE;
    STDMETHOD(GetContainingTypeLib)(THIS_ ITypeLib **tl, UINT *index) PURE;
    STDMETHOD_(void, ReleaseTypeAttr)(THIS_ TYPEATTR *attr) PURE;
    STDMETHOD_(void, ReleaseFuncDesc)(THIS_ FUNCDESC *fd) PURE;
    STDMETHOD_(void, ReleaseVarDesc)(THIS_ VARDESC *vd) PURE;
    END_INTERFACE
};
typedef ITypeInfo *LPTYPEINFO;

#undef INTERFACE
#define INTERFACE ITypeInfo2
DECLARE_INTERFACE_(ITypeInfo2, ITypeInfo)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(GetTypeAttr)(THIS_ TYPEATTR **attr) PURE;
    STDMETHOD(GetTypeComp)(THIS_ ITypeComp **tc) PURE;
    STDMETHOD(GetFuncDesc)(THIS_ UINT index, FUNCDESC **fd) PURE;
    STDMETHOD(GetVarDesc)(THIS_ UINT index, VARDESC **vd) PURE;
    STDMETHOD(GetNames)(THIS_ MEMBERID memid, BSTR *names, UINT max, UINT *n) PURE;
    STDMETHOD(GetRefTypeOfImplType)(THIS_ UINT index, HREFTYPE *href) PURE;
    STDMETHOD(GetImplTypeFlags)(THIS_ UINT index, INT *flags) PURE;
    STDMETHOD(GetIDsOfNames)(THIS_ LPOLESTR *names, UINT n, MEMBERID *ids) PURE;
    STDMETHOD(Invoke)(THIS_ PVOID obj, MEMBERID memid, WORD flags, DISPPARAMS *params, VARIANT *result,
                      EXCEPINFO *ei, UINT *argerr) PURE;
    STDMETHOD(GetDocumentation)(THIS_ MEMBERID memid, BSTR *name, BSTR *doc, DWORD *helpctx, BSTR *helpfile) PURE;
    STDMETHOD(GetDllEntry)(THIS_ MEMBERID memid, INVOKEKIND kind, BSTR *dll, BSTR *name, WORD *ordinal) PURE;
    STDMETHOD(GetRefTypeInfo)(THIS_ HREFTYPE href, ITypeInfo **ti) PURE;
    STDMETHOD(AddressOfMember)(THIS_ MEMBERID memid, INVOKEKIND kind, PVOID *addr) PURE;
    STDMETHOD(CreateInstance)(THIS_ IUnknown *outer, REFIID riid, PVOID *obj) PURE;
    STDMETHOD(GetMops)(THIS_ MEMBERID memid, BSTR *mops) PURE;
    STDMETHOD(GetContainingTypeLib)(THIS_ ITypeLib **tl, UINT *index) PURE;
    STDMETHOD_(void, ReleaseTypeAttr)(THIS_ TYPEATTR *attr) PURE;
    STDMETHOD_(void, ReleaseFuncDesc)(THIS_ FUNCDESC *fd) PURE;
    STDMETHOD_(void, ReleaseVarDesc)(THIS_ VARDESC *vd) PURE;
#endif
    STDMETHOD(GetTypeKind)(THIS_ TYPEKIND *kind) PURE;
    STDMETHOD(GetTypeFlags)(THIS_ ULONG *flags) PURE;
    STDMETHOD(GetFuncIndexOfMemId)(THIS_ MEMBERID memid, INVOKEKIND kind, UINT *index) PURE;
    STDMETHOD(GetVarIndexOfMemId)(THIS_ MEMBERID memid, UINT *index) PURE;
    STDMETHOD(GetCustData)(THIS_ REFGUID guid, VARIANT *v) PURE;
    STDMETHOD(GetFuncCustData)(THIS_ UINT index, REFGUID guid, VARIANT *v) PURE;
    STDMETHOD(GetParamCustData)(THIS_ UINT func, UINT param, REFGUID guid, VARIANT *v) PURE;
    STDMETHOD(GetVarCustData)(THIS_ UINT index, REFGUID guid, VARIANT *v) PURE;
    STDMETHOD(GetImplTypeCustData)(THIS_ UINT index, REFGUID guid, VARIANT *v) PURE;
    STDMETHOD(GetDocumentation2)(THIS_ MEMBERID memid, LCID lcid, BSTR *help, DWORD *ctx, BSTR *dll) PURE;
    STDMETHOD(GetAllCustData)(THIS_ CUSTDATA *cd) PURE;
    STDMETHOD(GetAllFuncCustData)(THIS_ UINT index, CUSTDATA *cd) PURE;
    STDMETHOD(GetAllParamCustData)(THIS_ UINT func, UINT param, CUSTDATA *cd) PURE;
    STDMETHOD(GetAllVarCustData)(THIS_ UINT index, CUSTDATA *cd) PURE;
    STDMETHOD(GetAllImplTypeCustData)(THIS_ UINT index, CUSTDATA *cd) PURE;
    END_INTERFACE
};

#undef INTERFACE
#define INTERFACE ITypeLib
DECLARE_INTERFACE_(ITypeLib, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD_(UINT, GetTypeInfoCount)(THIS) PURE;
    STDMETHOD(GetTypeInfo)(THIS_ UINT index, ITypeInfo **ti) PURE;
    STDMETHOD(GetTypeInfoType)(THIS_ UINT index, TYPEKIND *kind) PURE;
    STDMETHOD(GetTypeInfoOfGuid)(THIS_ REFGUID guid, ITypeInfo **ti) PURE;
    STDMETHOD(GetLibAttr)(THIS_ TLIBATTR **attr) PURE;
    STDMETHOD(GetTypeComp)(THIS_ ITypeComp **tc) PURE;
    STDMETHOD(GetDocumentation)(THIS_ INT index, BSTR *name, BSTR *doc, DWORD *helpctx, BSTR *helpfile) PURE;
    STDMETHOD(IsName)(THIS_ LPOLESTR name, ULONG hash, BOOL *found) PURE;
    STDMETHOD(FindName)(THIS_ LPOLESTR name, ULONG hash, ITypeInfo **ti, MEMBERID *ids, USHORT *n) PURE;
    STDMETHOD_(void, ReleaseTLibAttr)(THIS_ TLIBATTR *attr) PURE;
    END_INTERFACE
};
typedef ITypeLib *LPTYPELIB;

#undef INTERFACE
#define INTERFACE ITypeLib2
DECLARE_INTERFACE_(ITypeLib2, ITypeLib)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD_(UINT, GetTypeInfoCount)(THIS) PURE;
    STDMETHOD(GetTypeInfo)(THIS_ UINT index, ITypeInfo **ti) PURE;
    STDMETHOD(GetTypeInfoType)(THIS_ UINT index, TYPEKIND *kind) PURE;
    STDMETHOD(GetTypeInfoOfGuid)(THIS_ REFGUID guid, ITypeInfo **ti) PURE;
    STDMETHOD(GetLibAttr)(THIS_ TLIBATTR **attr) PURE;
    STDMETHOD(GetTypeComp)(THIS_ ITypeComp **tc) PURE;
    STDMETHOD(GetDocumentation)(THIS_ INT index, BSTR *name, BSTR *doc, DWORD *helpctx, BSTR *helpfile) PURE;
    STDMETHOD(IsName)(THIS_ LPOLESTR name, ULONG hash, BOOL *found) PURE;
    STDMETHOD(FindName)(THIS_ LPOLESTR name, ULONG hash, ITypeInfo **ti, MEMBERID *ids, USHORT *n) PURE;
    STDMETHOD_(void, ReleaseTLibAttr)(THIS_ TLIBATTR *attr) PURE;
#endif
    STDMETHOD(GetCustData)(THIS_ REFGUID guid, VARIANT *v) PURE;
    STDMETHOD(GetLibStatistics)(THIS_ ULONG *unique_names, ULONG *chars) PURE;
    STDMETHOD(GetDocumentation2)(THIS_ INT index, LCID lcid, BSTR *help, DWORD *ctx, BSTR *dll) PURE;
    STDMETHOD(GetAllCustData)(THIS_ CUSTDATA *cd) PURE;
    END_INTERFACE
};

DEFINE_OLEGUID(IID_ITypeInfo,  0x00020401, 0, 0);
DEFINE_OLEGUID(IID_ITypeLib,   0x00020402, 0, 0);
DEFINE_OLEGUID(IID_ITypeComp,  0x00020403, 0, 0);
DEFINE_OLEGUID(IID_ITypeInfo2, 0x00020412, 0, 0);
DEFINE_OLEGUID(IID_ITypeLib2,  0x00020411, 0, 0);

#ifndef __cplusplus
#define ITypeLib_AddRef(p)                      (p)->lpVtbl->AddRef(p)
#define ITypeLib_Release(p)                     (p)->lpVtbl->Release(p)
#define ITypeLib_GetTypeInfoCount(p)            (p)->lpVtbl->GetTypeInfoCount(p)
#define ITypeLib_GetTypeInfo(p, i, t)           (p)->lpVtbl->GetTypeInfo(p, i, t)
#define ITypeLib_GetTypeInfoType(p, i, k)       (p)->lpVtbl->GetTypeInfoType(p, i, k)
#define ITypeLib_GetTypeInfoOfGuid(p, g, t)     (p)->lpVtbl->GetTypeInfoOfGuid(p, g, t)
#define ITypeLib_GetLibAttr(p, a)               (p)->lpVtbl->GetLibAttr(p, a)
#define ITypeLib_GetDocumentation(p, i, n, d, c, f) (p)->lpVtbl->GetDocumentation(p, i, n, d, c, f)
#define ITypeLib_ReleaseTLibAttr(p, a)          (p)->lpVtbl->ReleaseTLibAttr(p, a)
#define ITypeInfo_AddRef(p)                     (p)->lpVtbl->AddRef(p)
#define ITypeInfo_Release(p)                    (p)->lpVtbl->Release(p)
#define ITypeInfo_GetTypeAttr(p, a)             (p)->lpVtbl->GetTypeAttr(p, a)
#define ITypeInfo_GetFuncDesc(p, i, f)          (p)->lpVtbl->GetFuncDesc(p, i, f)
#define ITypeInfo_GetVarDesc(p, i, v)           (p)->lpVtbl->GetVarDesc(p, i, v)
#define ITypeInfo_GetNames(p, m, n, x, c)       (p)->lpVtbl->GetNames(p, m, n, x, c)
#define ITypeInfo_GetRefTypeOfImplType(p, i, h) (p)->lpVtbl->GetRefTypeOfImplType(p, i, h)
#define ITypeInfo_GetImplTypeFlags(p, i, f)     (p)->lpVtbl->GetImplTypeFlags(p, i, f)
#define ITypeInfo_GetIDsOfNames(p, n, c, i)     (p)->lpVtbl->GetIDsOfNames(p, n, c, i)
#define ITypeInfo_Invoke(p, o, m, f, d, r, e, a) (p)->lpVtbl->Invoke(p, o, m, f, d, r, e, a)
#define ITypeInfo_GetDocumentation(p, m, n, d, c, f) (p)->lpVtbl->GetDocumentation(p, m, n, d, c, f)
#define ITypeInfo_GetRefTypeInfo(p, h, t)       (p)->lpVtbl->GetRefTypeInfo(p, h, t)
#define ITypeInfo_GetContainingTypeLib(p, l, i) (p)->lpVtbl->GetContainingTypeLib(p, l, i)
#define ITypeInfo_ReleaseTypeAttr(p, a)         (p)->lpVtbl->ReleaseTypeAttr(p, a)
#define ITypeInfo_ReleaseFuncDesc(p, f)         (p)->lpVtbl->ReleaseFuncDesc(p, f)
#define ITypeInfo_ReleaseVarDesc(p, v)          (p)->lpVtbl->ReleaseVarDesc(p, v)
#endif

WINOLEAUTAPI_(HRESULT) LoadTypeLib(LPCOLESTR file, ITypeLib **out);
WINOLEAUTAPI_(HRESULT) LoadTypeLibEx(LPCOLESTR file, REGKIND kind, ITypeLib **out);
WINOLEAUTAPI_(HRESULT) LoadRegTypeLib(REFGUID guid, WORD maj, WORD min, LCID lcid, ITypeLib **out);
WINOLEAUTAPI_(HRESULT) QueryPathOfRegTypeLib(REFGUID guid, USHORT maj, USHORT min, LCID lcid, BSTR *path);
WINOLEAUTAPI_(HRESULT) RegisterTypeLib(ITypeLib *tl, LPCOLESTR path, LPCOLESTR helpdir);
WINOLEAUTAPI_(HRESULT) UnRegisterTypeLib(REFGUID guid, WORD maj, WORD min, LCID lcid, SYSKIND kind);
WINOLEAUTAPI_(HRESULT) RegisterTypeLibForUser(ITypeLib *tl, LPOLESTR path, LPOLESTR helpdir);
WINOLEAUTAPI_(HRESULT) UnRegisterTypeLibForUser(REFGUID guid, WORD maj, WORD min, LCID lcid, SYSKIND kind);
WINOLEAUTAPI_(ULONG)   LHashValOfNameSys(SYSKIND kind, LCID lcid, LPCOLESTR name);
WINOLEAUTAPI_(ULONG)   LHashValOfNameSysA(SYSKIND kind, LCID lcid, LPCSTR name);
#define LHashValOfName(lcid, name) LHashValOfNameSys(SYS_WIN32, lcid, name)
WINOLEAUTAPI_(HRESULT) DispCallFunc(void *obj, ULONG_PTR ovft, CALLCONV cc, VARTYPE ret, UINT n,
                                    VARTYPE *types, VARIANTARG **args, VARIANT *result);
WINOLEAUTAPI_(HRESULT) DispGetIDsOfNames(ITypeInfo *ti, LPOLESTR *names, UINT n, DISPID *ids);
WINOLEAUTAPI_(HRESULT) DispInvoke(void *obj, ITypeInfo *ti, DISPID id, WORD flags, DISPPARAMS *p,
                                  VARIANT *res, EXCEPINFO *ei, UINT *argerr);
WINOLEAUTAPI_(HRESULT) DispGetParam(DISPPARAMS *p, UINT pos, VARTYPE vt, VARIANT *out, UINT *argerr);
WINOLEAUTAPI_(HRESULT) CreateStdDispatch(IUnknown *outer, void *obj, ITypeInfo *ti, IUnknown **out);

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
/* variant arithmetic and comparison (VarCmp returns VARCMP_*) */
WINOLEAUTAPI_(HRESULT) VarAdd(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarSub(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarMul(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarDiv(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarIdiv(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarMod(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarAnd(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarOr(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarXor(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarCat(LPVARIANT l, LPVARIANT r, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarNeg(LPVARIANT in, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarNot(LPVARIANT in, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarAbs(LPVARIANT in, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarFix(LPVARIANT in, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarInt(LPVARIANT in, LPVARIANT out);
WINOLEAUTAPI_(HRESULT) VarCmp(LPVARIANT l, LPVARIANT r, LCID lcid, ULONG flags);
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
