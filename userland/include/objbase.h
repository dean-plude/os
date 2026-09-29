/* objbase.h — COM: interfaces, GUIDs and the ole32 API */
#pragma once
#include <windows.h>

_NOVA_BEGIN

#define STDMETHODCALLTYPE __stdcall
#define STDAPICALLTYPE    __stdcall
#define STDAPI            EXTERN_C HRESULT STDAPICALLTYPE
#define STDAPI_(t)        EXTERN_C t STDAPICALLTYPE
#define STDMETHODIMP      HRESULT STDMETHODCALLTYPE
#define STDMETHODIMP_(t)  t STDMETHODCALLTYPE
#ifndef EXTERN_C
#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif
#endif
#ifndef WINOLEAPI
#define WINOLEAPI WINOLEAPI_(HRESULT)
#endif

/* Interface declarations work the classic objbase.h way: in C++ an
 * interface is a struct of pure virtual methods, in C a struct holding
 * lpVtbl whose table lists every method (base methods first) with an
 * explicit This. */
#ifdef __cplusplus
#define interface                 struct
#define STDMETHOD(m)              virtual HRESULT STDMETHODCALLTYPE m
#define STDMETHOD_(t, m)          virtual t STDMETHODCALLTYPE m
#define PURE                      = 0
#define THIS_
#define THIS                      void
#define DECLARE_INTERFACE(i)      interface i
#define DECLARE_INTERFACE_(i, b)  interface i : public b
#define BEGIN_INTERFACE
#define END_INTERFACE
#else
#define interface                 struct
#define STDMETHOD(m)              HRESULT (STDMETHODCALLTYPE *m)
#define STDMETHOD_(t, m)          t (STDMETHODCALLTYPE *m)
#define PURE
#define THIS_                     INTERFACE *This,
#define THIS                      INTERFACE *This
#define DECLARE_INTERFACE(i)      typedef interface i { const struct i##Vtbl *lpVtbl; } i; \
                                  typedef struct i##Vtbl i##Vtbl; struct i##Vtbl
#define DECLARE_INTERFACE_(i, b)  DECLARE_INTERFACE(i)
#define BEGIN_INTERFACE
#define END_INTERFACE
#endif

#ifdef __cplusplus
#define DEFINE_GUID(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID n __attribute__((unused)) = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#else
#define DEFINE_GUID(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID n __attribute__((unused)) = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#endif
#define DEFINE_OLEGUID(n, l, w1, w2) DEFINE_GUID(n, l, w1, w2, 0xC0, 0, 0, 0, 0, 0, 0, 0x46)

#ifdef __cplusplus
static inline int IsEqualGUID(REFGUID a, REFGUID b) { return !__builtin_memcmp(&a, &b, sizeof(GUID)); }
static inline bool operator==(const GUID &a, const GUID &b) { return IsEqualGUID(a, b); }
static inline bool operator!=(const GUID &a, const GUID &b) { return !IsEqualGUID(a, b); }
#else
#define IsEqualGUID(a, b) (!__builtin_memcmp((a), (b), sizeof(GUID)))
#endif
#define IsEqualIID(a, b)   IsEqualGUID(a, b)
#define IsEqualCLSID(a, b) IsEqualGUID(a, b)

typedef WCHAR OLECHAR, *LPOLESTR;
typedef const WCHAR *LPCOLESTR;
#define OLESTR(s) L##s

DEFINE_OLEGUID(IID_IUnknown,             0x00000000, 0, 0);
DEFINE_OLEGUID(IID_IClassFactory,        0x00000001, 0, 0);
DEFINE_OLEGUID(IID_IMalloc,              0x00000002, 0, 0);
DEFINE_OLEGUID(IID_IMarshal,             0x00000003, 0, 0);
DEFINE_OLEGUID(IID_IStream,              0x0000000C, 0, 0);
DEFINE_OLEGUID(IID_ISequentialStream,    0x0C733A30, 0x2A1C, 0x11CE);
DEFINE_OLEGUID(IID_IPersist,             0x0000010C, 0, 0);
DEFINE_OLEGUID(IID_IEnumUnknown,         0x00000100, 0, 0);
DEFINE_OLEGUID(IID_IDispatch,            0x00020400, 0, 0);
DEFINE_OLEGUID(IID_IErrorInfo,           0x1CF2B120, 0x547D, 0x101B);
DEFINE_OLEGUID(IID_ICreateErrorInfo,     0x22F03340, 0x547D, 0x101B);
DEFINE_OLEGUID(IID_ISupportErrorInfo,    0xDF0B3D60, 0x548F, 0x101B);
DEFINE_GUID(IID_NULL,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
DEFINE_GUID(GUID_NULL,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
DEFINE_GUID(CLSID_NULL,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
DEFINE_GUID(IID_IAgileObject, 0x94EA2B94, 0xE9CC, 0x49E0, 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90);

/* ---- IUnknown ---- */
#undef INTERFACE
#define INTERFACE IUnknown
DECLARE_INTERFACE(IUnknown)
{
    BEGIN_INTERFACE
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    END_INTERFACE
};
typedef IUnknown *LPUNKNOWN;

/* ---- IClassFactory ---- */
#undef INTERFACE
#define INTERFACE IClassFactory
DECLARE_INTERFACE_(IClassFactory, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD(CreateInstance)(THIS_ IUnknown *outer, REFIID riid, void **ppv) PURE;
    STDMETHOD(LockServer)(THIS_ BOOL lock) PURE;
    END_INTERFACE
};
typedef IClassFactory *LPCLASSFACTORY;

/* ---- IMalloc ---- */
#undef INTERFACE
#define INTERFACE IMalloc
DECLARE_INTERFACE_(IMalloc, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD_(void *, Alloc)(THIS_ SIZE_T n) PURE;
    STDMETHOD_(void *, Realloc)(THIS_ void *p, SIZE_T n) PURE;
    STDMETHOD_(void, Free)(THIS_ void *p) PURE;
    STDMETHOD_(SIZE_T, GetSize)(THIS_ void *p) PURE;
    STDMETHOD_(int, DidAlloc)(THIS_ void *p) PURE;
    STDMETHOD_(void, HeapMinimize)(THIS) PURE;
    END_INTERFACE
};
typedef IMalloc *LPMALLOC;

/* ---- ISequentialStream / IStream ---- */
typedef struct tagSTATSTG {
    LPOLESTR pwcsName;
    DWORD type;
    ULARGE_INTEGER cbSize;
    FILETIME mtime, ctime, atime;
    DWORD grfMode, grfLocksSupported;
    CLSID clsid;
    DWORD grfStateBits, reserved;
} STATSTG;
#define STGTY_STORAGE 1
#define STGTY_STREAM  2
#define STREAM_SEEK_SET 0
#define STREAM_SEEK_CUR 1
#define STREAM_SEEK_END 2
#define STATFLAG_DEFAULT 0
#define STATFLAG_NONAME  1
#define STG_E_INVALIDFUNCTION ((HRESULT)0x80030001L)
#define STG_E_ACCESSDENIED    ((HRESULT)0x80030005L)
#define STG_E_MEDIUMFULL      ((HRESULT)0x80030070L)
#define STG_E_INVALIDPOINTER  ((HRESULT)0x80030009L)

#undef INTERFACE
#define INTERFACE ISequentialStream
DECLARE_INTERFACE_(ISequentialStream, IUnknown)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#endif
    STDMETHOD(Read)(THIS_ void *pv, ULONG cb, ULONG *read) PURE;
    STDMETHOD(Write)(THIS_ const void *pv, ULONG cb, ULONG *written) PURE;
    END_INTERFACE
};

#undef INTERFACE
#define INTERFACE IStream
DECLARE_INTERFACE_(IStream, ISequentialStream)
{
    BEGIN_INTERFACE
#ifndef __cplusplus
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(Read)(THIS_ void *pv, ULONG cb, ULONG *read) PURE;
    STDMETHOD(Write)(THIS_ const void *pv, ULONG cb, ULONG *written) PURE;
#endif
    STDMETHOD(Seek)(THIS_ LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *pos) PURE;
    STDMETHOD(SetSize)(THIS_ ULARGE_INTEGER size) PURE;
    STDMETHOD(CopyTo)(THIS_ IStream *to, ULARGE_INTEGER cb, ULARGE_INTEGER *read, ULARGE_INTEGER *written) PURE;
    STDMETHOD(Commit)(THIS_ DWORD flags) PURE;
    STDMETHOD(Revert)(THIS) PURE;
    STDMETHOD(LockRegion)(THIS_ ULARGE_INTEGER off, ULARGE_INTEGER cb, DWORD type) PURE;
    STDMETHOD(UnlockRegion)(THIS_ ULARGE_INTEGER off, ULARGE_INTEGER cb, DWORD type) PURE;
    STDMETHOD(Stat)(THIS_ STATSTG *st, DWORD flags) PURE;
    STDMETHOD(Clone)(THIS_ IStream **out) PURE;
    END_INTERFACE
};
typedef IStream *LPSTREAM;

/* ---- C call helpers ---- */
#ifndef __cplusplus
#define IUnknown_QueryInterface(p, r, v)   (p)->lpVtbl->QueryInterface(p, r, v)
#define IUnknown_AddRef(p)                 (p)->lpVtbl->AddRef(p)
#define IUnknown_Release(p)                (p)->lpVtbl->Release(p)
#define IClassFactory_QueryInterface(p, r, v) (p)->lpVtbl->QueryInterface(p, r, v)
#define IClassFactory_AddRef(p)            (p)->lpVtbl->AddRef(p)
#define IClassFactory_Release(p)           (p)->lpVtbl->Release(p)
#define IClassFactory_CreateInstance(p, o, r, v) (p)->lpVtbl->CreateInstance(p, o, r, v)
#define IClassFactory_LockServer(p, l)     (p)->lpVtbl->LockServer(p, l)
#define IMalloc_Alloc(p, n)                (p)->lpVtbl->Alloc(p, n)
#define IMalloc_Free(p, v)                 (p)->lpVtbl->Free(p, v)
#define IMalloc_Release(p)                 (p)->lpVtbl->Release(p)
#define IStream_QueryInterface(p, r, v)    (p)->lpVtbl->QueryInterface(p, r, v)
#define IStream_AddRef(p)                  (p)->lpVtbl->AddRef(p)
#define IStream_Release(p)                 (p)->lpVtbl->Release(p)
#define IStream_Read(p, v, n, r)           (p)->lpVtbl->Read(p, v, n, r)
#define IStream_Write(p, v, n, w)          (p)->lpVtbl->Write(p, v, n, w)
#define IStream_Seek(p, m, o, n)           (p)->lpVtbl->Seek(p, m, o, n)
#define IStream_SetSize(p, n)              (p)->lpVtbl->SetSize(p, n)
#define IStream_Stat(p, s, f)              (p)->lpVtbl->Stat(p, s, f)
#define IStream_Clone(p, o)                (p)->lpVtbl->Clone(p, o)
#endif

/* ---- ole32 API ---- */
typedef enum tagCOINIT {
    COINIT_APARTMENTTHREADED = 0x2,
    COINIT_MULTITHREADED     = 0x0,
    COINIT_DISABLE_OLE1DDE   = 0x4,
    COINIT_SPEED_OVER_MEMORY = 0x8,
} COINIT;
typedef enum tagCLSCTX {
    CLSCTX_INPROC_SERVER = 0x1, CLSCTX_INPROC_HANDLER = 0x2, CLSCTX_LOCAL_SERVER = 0x4,
    CLSCTX_REMOTE_SERVER = 0x10, CLSCTX_NO_CODE_DOWNLOAD = 0x400, CLSCTX_NO_FAILURE_LOG = 0x4000,
} CLSCTX;
#define CLSCTX_INPROC (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER)
#define CLSCTX_SERVER (CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER | CLSCTX_REMOTE_SERVER)
#define CLSCTX_ALL    (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER | CLSCTX_REMOTE_SERVER)
typedef enum tagREGCLS { REGCLS_SINGLEUSE = 0, REGCLS_MULTIPLEUSE = 1, REGCLS_MULTI_SEPARATE = 2, REGCLS_SUSPENDED = 4 } REGCLS;
typedef enum _APTTYPE { APTTYPE_CURRENT = -1, APTTYPE_STA = 0, APTTYPE_MTA = 1, APTTYPE_NA = 2, APTTYPE_MAINSTA = 3 } APTTYPE;
typedef enum _APTTYPEQUALIFIER { APTTYPEQUALIFIER_NONE = 0 } APTTYPEQUALIFIER;
typedef struct tagMULTI_QI { const IID *pIID; IUnknown *pItf; HRESULT hr; } MULTI_QI;
typedef struct _COSERVERINFO COSERVERINFO;
typedef void *CO_MTA_USAGE_COOKIE;

#define RPC_E_CHANGED_MODE        ((HRESULT)0x80010106L)
#define RPC_E_TOO_LATE            ((HRESULT)0x80010119L)
#define CO_E_NOTINITIALIZED       ((HRESULT)0x800401F0L)
#define CO_E_CLASSSTRING          ((HRESULT)0x800401F3L)
#define CO_E_DLLNOTFOUND          ((HRESULT)0x800401F8L)
#define CO_E_ERRORINDLL           ((HRESULT)0x800401F9L)
#define CO_E_OBJNOTREG            ((HRESULT)0x800401FBL)
#define CO_E_SERVER_EXEC_FAILURE  ((HRESULT)0x80080005L)
#define REGDB_E_CLASSNOTREG       ((HRESULT)0x80040154L)
#define REGDB_E_READREGDB         ((HRESULT)0x80040150L)
#define REGDB_E_IIDNOTREG         ((HRESULT)0x80040155L)
#define CLASS_E_NOAGGREGATION     ((HRESULT)0x80040110L)
#define CLASS_E_CLASSNOTAVAILABLE ((HRESULT)0x80040111L)
#define CO_E_CLASS_CREATE_FAILED  ((HRESULT)0x80080001L)
#define E_NOT_SUFFICIENT_BUFFER   ((HRESULT)0x8007007AL)

WINOLEAPI_(HRESULT) CoInitialize(LPVOID reserved);
WINOLEAPI_(HRESULT) CoInitializeEx(LPVOID reserved, DWORD coinit);
WINOLEAPI_(void)    CoUninitialize(void);
WINOLEAPI_(HRESULT) CoGetApartmentType(APTTYPE *type, APTTYPEQUALIFIER *q);
WINOLEAPI_(HRESULT) CoInitializeSecurity(PVOID sd, LONG n, PVOID auth, void *res, DWORD authn, DWORD imp, void *list, DWORD caps, void *res3);
WINOLEAPI_(HRESULT) CoCreateInstance(REFCLSID clsid, IUnknown *outer, DWORD ctx, REFIID riid, LPVOID *ppv);
WINOLEAPI_(HRESULT) CoCreateInstanceEx(REFCLSID clsid, IUnknown *outer, DWORD ctx, COSERVERINFO *server, DWORD n, MULTI_QI *res);
WINOLEAPI_(HRESULT) CoGetClassObject(REFCLSID clsid, DWORD ctx, LPVOID reserved, REFIID riid, LPVOID *ppv);
WINOLEAPI_(HRESULT) CoRegisterClassObject(REFCLSID clsid, IUnknown *obj, DWORD ctx, DWORD flags, LPDWORD cookie);
WINOLEAPI_(HRESULT) CoRevokeClassObject(DWORD cookie);
WINOLEAPI_(void)    CoFreeUnusedLibraries(void);
WINOLEAPI_(void)    CoFreeUnusedLibrariesEx(DWORD delay, DWORD reserved);
WINOLEAPI_(LPVOID)  CoTaskMemAlloc(SIZE_T n);
WINOLEAPI_(LPVOID)  CoTaskMemRealloc(LPVOID p, SIZE_T n);
WINOLEAPI_(void)    CoTaskMemFree(LPVOID p);
WINOLEAPI_(HRESULT) CoGetMalloc(DWORD ctx, LPMALLOC *out);
WINOLEAPI_(HRESULT) CoCreateGuid(GUID *g);
WINOLEAPI_(int)     StringFromGUID2(REFGUID g, LPOLESTR out, int cch);
WINOLEAPI_(HRESULT) StringFromCLSID(REFCLSID clsid, LPOLESTR *out);
WINOLEAPI_(HRESULT) StringFromIID(REFIID iid, LPOLESTR *out);
WINOLEAPI_(HRESULT) CLSIDFromString(LPCOLESTR s, LPCLSID out);
WINOLEAPI_(HRESULT) IIDFromString(LPCOLESTR s, LPIID out);
WINOLEAPI_(HRESULT) CLSIDFromProgID(LPCOLESTR progid, LPCLSID out);
WINOLEAPI_(HRESULT) CLSIDFromProgIDEx(LPCOLESTR progid, LPCLSID out);
WINOLEAPI_(HRESULT) ProgIDFromCLSID(REFCLSID clsid, LPOLESTR *out);
WINOLEAPI_(HRESULT) CreateStreamOnHGlobal(HGLOBAL h, BOOL del, LPSTREAM *out);
WINOLEAPI_(HRESULT) GetHGlobalFromStream(LPSTREAM s, HGLOBAL *out);
WINOLEAPI_(HRESULT) CoIncrementMTAUsage(CO_MTA_USAGE_COOKIE *cookie);
WINOLEAPI_(HRESULT) CoDecrementMTAUsage(CO_MTA_USAGE_COOKIE cookie);
WINOLEAPI_(DWORD)   CoGetCurrentProcess(void);
WINOLEAPI_(HRESULT) CoWaitForMultipleHandles(DWORD flags, DWORD ms, ULONG n, LPHANDLE h, LPDWORD index);

/* ---- data transfer: FORMATETC, STGMEDIUM, IDataObject; drag and drop ---- */
typedef WORD CLIPFORMAT;
#ifndef _NOVA_POINTL
#define _NOVA_POINTL
typedef struct tagPOINTL { LONG x, y; } POINTL;
#endif
#ifndef _NOVA_HENHMETAFILE
#define _NOVA_HENHMETAFILE
typedef HANDLE HENHMETAFILE;
#endif
typedef struct tagDVTARGETDEVICE { DWORD tdSize; WORD tdDriverNameOffset, tdDeviceNameOffset, tdPortNameOffset, tdExtDevmodeOffset; BYTE tdData[1]; } DVTARGETDEVICE;
typedef struct tagFORMATETC { CLIPFORMAT cfFormat; DVTARGETDEVICE *ptd; DWORD dwAspect; LONG lindex; DWORD tymed; } FORMATETC, *LPFORMATETC;
typedef struct tagSTGMEDIUM { DWORD tymed; union { HBITMAP hBitmap; void *hMetaFilePict; HENHMETAFILE hEnhMetaFile; HGLOBAL hGlobal; LPOLESTR lpszFileName; IStream *pstm; void *pstg; }; IUnknown *pUnkForRelease; } STGMEDIUM, *LPSTGMEDIUM;
typedef struct tagSTATDATA { FORMATETC formatetc; DWORD advf; struct IAdviseSink *pAdvSink; DWORD dwConnection; } STATDATA;
typedef struct IAdviseSink IAdviseSink;
typedef struct IEnumSTATDATA IEnumSTATDATA;
#define DVASPECT_CONTENT 1
#define DVASPECT_THUMBNAIL 2
#define DVASPECT_ICON 4
#define DVASPECT_DOCPRINT 8
#define TYMED_HGLOBAL 1
#define TYMED_FILE 2
#define TYMED_ISTREAM 4
#define TYMED_ISTORAGE 8
#define TYMED_GDI 16
#define TYMED_MFPICT 32
#define TYMED_ENHMF 64
#define TYMED_NULL 0
#define DATADIR_GET 1
#define DATADIR_SET 2
#define DV_E_FORMATETC ((HRESULT)0x80040064L)
#define DV_E_TYMED ((HRESULT)0x80040069L)
#define DV_E_DVASPECT ((HRESULT)0x8004006BL)
#define DV_E_LINDEX ((HRESULT)0x80040068L)
#define OLE_E_ADVISENOTSUPPORTED ((HRESULT)0x80040003L)
#define DATA_S_SAMEFORMATETC ((HRESULT)0x00040130L)
#define DROPEFFECT_NONE 0
#define DROPEFFECT_COPY 1
#define DROPEFFECT_MOVE 2
#define DROPEFFECT_LINK 4
#define DROPEFFECT_SCROLL 0x80000000
#define DRAGDROP_S_DROP ((HRESULT)0x00040100L)
#define DRAGDROP_S_CANCEL ((HRESULT)0x00040101L)
#define DRAGDROP_S_USEDEFAULTCURSORS ((HRESULT)0x00040102L)
#define DRAGDROP_E_NOTREGISTERED ((HRESULT)0x80040100L)
#define DRAGDROP_E_ALREADYREGISTERED ((HRESULT)0x80040101L)
#define DRAGDROP_E_INVALIDHWND ((HRESULT)0x80040102L)
#define CLIPBRD_E_CANT_OPEN ((HRESULT)0x800401D0L)
DEFINE_OLEGUID(IID_IDataObject,          0x0000010E, 0, 0);
DEFINE_OLEGUID(IID_IEnumFORMATETC,       0x00000103, 0, 0);
DEFINE_OLEGUID(IID_IDropSource,          0x00000121, 0, 0);
DEFINE_OLEGUID(IID_IDropTarget,          0x00000122, 0, 0);
DEFINE_OLEGUID(IID_IAdviseSink,          0x0000010F, 0, 0);

#undef INTERFACE
#define INTERFACE IEnumFORMATETC
DECLARE_INTERFACE_(IEnumFORMATETC, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **out) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(Next)(THIS_ ULONG n, FORMATETC *out, ULONG *fetched) PURE;
    STDMETHOD(Skip)(THIS_ ULONG n) PURE;
    STDMETHOD(Reset)(THIS) PURE;
    STDMETHOD(Clone)(THIS_ IEnumFORMATETC **out) PURE;
};
#undef INTERFACE
#define INTERFACE IDataObject
DECLARE_INTERFACE_(IDataObject, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **out) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(GetData)(THIS_ FORMATETC *fmt, STGMEDIUM *medium) PURE;
    STDMETHOD(GetDataHere)(THIS_ FORMATETC *fmt, STGMEDIUM *medium) PURE;
    STDMETHOD(QueryGetData)(THIS_ FORMATETC *fmt) PURE;
    STDMETHOD(GetCanonicalFormatEtc)(THIS_ FORMATETC *in, FORMATETC *out) PURE;
    STDMETHOD(SetData)(THIS_ FORMATETC *fmt, STGMEDIUM *medium, BOOL release) PURE;
    STDMETHOD(EnumFormatEtc)(THIS_ DWORD dir, IEnumFORMATETC **out) PURE;
    STDMETHOD(DAdvise)(THIS_ FORMATETC *fmt, DWORD advf, IAdviseSink *sink, DWORD *conn) PURE;
    STDMETHOD(DUnadvise)(THIS_ DWORD conn) PURE;
    STDMETHOD(EnumDAdvise)(THIS_ IEnumSTATDATA **out) PURE;
};
typedef IDataObject *LPDATAOBJECT;
#undef INTERFACE
#define INTERFACE IDropSource
DECLARE_INTERFACE_(IDropSource, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **out) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(QueryContinueDrag)(THIS_ BOOL escape, DWORD keys) PURE;
    STDMETHOD(GiveFeedback)(THIS_ DWORD effect) PURE;
};
typedef IDropSource *LPDROPSOURCE;
#undef INTERFACE
#define INTERFACE IDropTarget
DECLARE_INTERFACE_(IDropTarget, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **out) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(DragEnter)(THIS_ IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) PURE;
    STDMETHOD(DragOver)(THIS_ DWORD keys, POINTL pt, DWORD *effect) PURE;
    STDMETHOD(DragLeave)(THIS) PURE;
    STDMETHOD(Drop)(THIS_ IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) PURE;
};
typedef IDropTarget *LPDROPTARGET;
#undef INTERFACE

WINOLEAPI_(HRESULT) RegisterDragDrop(HWND w, IDropTarget *target);
WINOLEAPI_(HRESULT) RevokeDragDrop(HWND w);
WINOLEAPI_(HRESULT) DoDragDrop(IDataObject *data, IDropSource *source, DWORD ok, DWORD *effect);
WINOLEAPI_(void)    ReleaseStgMedium(STGMEDIUM *m);

WINOLEAPI_(HRESULT) OleInitialize(LPVOID reserved);
WINOLEAPI_(void)    OleUninitialize(void);

_NOVA_END
