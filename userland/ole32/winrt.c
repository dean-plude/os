/*
 * Windows Runtime strings (HSTRING) and activation, the part of combase
 * (api-ms-win-core-winrt-*) Win32 programs touch: they make HSTRINGs to
 * name runtime classes, and ask for activation factories, which NovaOS
 * has few of (winrt_classes.c, uisettings.c; others give
 * REGDB_E_CLASSNOTREG, as for a class Windows lacks).
 */
#define NOVA_BUILD_OLE32
#include <objbase.h>

typedef struct HSTRING_ *HSTRING;
/* An HSTRING is one of these, laid out as Windows lays them out: C++/WinRT
 * builds HSTRINGs itself (reference ones, and heap ones with the count
 * after the header and the characters after that, from the process heap)
 * and hands them to these functions.  A reference string lives in the
 * caller's HSTRING_HEADER (24 bytes on x64, 20 on x86), without a count. */
typedef struct {
    UINT32 flags;                       /* 1: a reference (not owned) */
    UINT32 length;
    UINT32 pad1, pad2;
    const WCHAR *chars;
    LONG refs;                          /* heap strings only */
} HStr;
typedef struct { BYTE b[sizeof(void *) == 8 ? 24 : 20]; } HSTRING_HEADER;
_Static_assert(__builtin_offsetof(HStr, refs) == sizeof(HSTRING_HEADER), "HSTRING_HEADER holds a reference string");

#define HSTR_REF 1
#define E_STRING_NOT_NULL_TERMINATED ((HRESULT)0x80000017L)

WINOLEAPI_(HRESULT) WindowsCreateString(const WCHAR *src, UINT32 len, HSTRING *out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (!src && len) return E_POINTER;
    if (!len) return S_OK;                              /* the empty string is NULL */
    HStr *s = HeapAlloc(GetProcessHeap(), 0, sizeof(HStr) + (len + 1) * sizeof(WCHAR));
    if (!s) return E_OUTOFMEMORY;
    WCHAR *c = (WCHAR *)(s + 1);
    for (UINT32 i = 0; i < len; i++) c[i] = src[i];
    c[len] = 0;
    s->flags = 0;
    s->length = len;
    s->pad1 = s->pad2 = 0;
    s->chars = c;
    s->refs = 1;
    *out = (HSTRING)s;
    return S_OK;
}

WINOLEAPI_(HRESULT) WindowsCreateStringReference(const WCHAR *src, UINT32 len, HSTRING_HEADER *hdr, HSTRING *out)
{
    if (!out || !hdr) return E_INVALIDARG;
    *out = 0;
    if (!src && len) return E_POINTER;
    if (len && src[len]) return E_STRING_NOT_NULL_TERMINATED;
    if (!len) return S_OK;
    HStr *s = (HStr *)hdr;
    s->flags = HSTR_REF;
    s->length = len;
    s->pad1 = s->pad2 = 0;
    s->chars = src;
    *out = (HSTRING)s;
    return S_OK;
}

WINOLEAPI_(HRESULT) WindowsDeleteString(HSTRING h)
{
    HStr *s = (HStr *)h;
    if (s && !(s->flags & HSTR_REF) && !InterlockedDecrement(&s->refs)) HeapFree(GetProcessHeap(), 0, s);
    return S_OK;
}

WINOLEAPI_(HRESULT) WindowsDuplicateString(HSTRING h, HSTRING *out)
{
    HStr *s = (HStr *)h;
    if (!out) return E_INVALIDARG;
    if (!s) { *out = 0; return S_OK; }
    if (s->flags & HSTR_REF) return WindowsCreateString(s->chars, s->length, out);
    InterlockedIncrement(&s->refs);
    *out = h;
    return S_OK;
}

WINOLEAPI_(const WCHAR *) WindowsGetStringRawBuffer(HSTRING h, UINT32 *len)
{
    HStr *s = (HStr *)h;
    if (len) *len = s ? s->length : 0;
    return s ? s->chars : L"";
}
WINOLEAPI_(UINT32) WindowsGetStringLen(HSTRING h) { return h ? ((HStr *)h)->length : 0; }
WINOLEAPI_(BOOL) WindowsIsStringEmpty(HSTRING h) { return !h || !((HStr *)h)->length; }
WINOLEAPI_(HRESULT) WindowsStringHasEmbeddedNull(HSTRING h, BOOL *has)
{
    HStr *s = (HStr *)h;
    if (!has) return E_INVALIDARG;
    *has = FALSE;
    for (UINT32 i = 0; s && i < s->length; i++) if (!s->chars[i]) *has = TRUE;
    return S_OK;
}
WINOLEAPI_(HRESULT) WindowsCompareStringOrdinal(HSTRING a, HSTRING b, INT32 *r)
{
    if (!r) return E_INVALIDARG;
    UINT32 la, lb;
    const WCHAR *x = WindowsGetStringRawBuffer(a, &la), *y = WindowsGetStringRawBuffer(b, &lb);
    *r = 0;
    for (UINT32 i = 0; i < la && i < lb && !*r; i++) if (x[i] != y[i]) *r = x[i] < y[i] ? -1 : 1;
    if (!*r && la != lb) *r = la < lb ? -1 : 1;
    return S_OK;
}
WINOLEAPI_(HRESULT) WindowsConcatString(HSTRING a, HSTRING b, HSTRING *out)
{
    if (!out) return E_INVALIDARG;
    UINT32 la, lb;
    const WCHAR *x = WindowsGetStringRawBuffer(a, &la), *y = WindowsGetStringRawBuffer(b, &lb);
    *out = 0;
    if (!la && !lb) return S_OK;
    WCHAR *t = HeapAlloc(GetProcessHeap(), 0, (la + lb + 1) * sizeof(WCHAR));
    if (!t) return E_OUTOFMEMORY;
    for (UINT32 i = 0; i < la; i++) t[i] = x[i];
    for (UINT32 i = 0; i < lb; i++) t[la + i] = y[i];
    t[la + lb] = 0;
    HRESULT hr = WindowsCreateString(t, la + lb, out);
    HeapFree(GetProcessHeap(), 0, t);
    return hr;
}

/* Activation: only the classes in winrt_classes.c */
HRESULT ole32_winrt_factory(const WCHAR *name, UINT32 len, REFIID iid, void **out);
WINOLEAPI_(HRESULT) RoInitialize(int type) { return CoInitializeEx(0, type == 0 ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED); }
WINOLEAPI_(void) RoUninitialize(void) { CoUninitialize(); }
WINOLEAPI_(HRESULT) RoGetActivationFactory(HSTRING cls, REFIID iid, void **f)
{
    if (!f) return E_POINTER;
    UINT32 len;
    const WCHAR *name = WindowsGetStringRawBuffer(cls, &len);
    return ole32_winrt_factory(name, len, iid, f);
}
static const GUID IID_IActivationFactory__ = { 0x00000035, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
/* The class's factory's IActivationFactory::ActivateInstance, the slot
 * after IInspectable's six */
WINOLEAPI_(HRESULT) RoActivateInstance(HSTRING cls, void **inst)
{
    if (!inst) return E_POINTER;
    *inst = 0;
    IUnknown *f;
    HRESULT hr = RoGetActivationFactory(cls, &IID_IActivationFactory__, (void **)&f);
    if (FAILED(hr)) return hr;
    typedef HRESULT (STDMETHODCALLTYPE *Activate)(IUnknown *, void **);
    hr = ((Activate)((void **)f->lpVtbl)[6])(f, inst);
    f->lpVtbl->Release(f);
    return hr;
}
WINOLEAPI_(BOOL) RoOriginateErrorW(HRESULT hr, UINT32 len, const WCHAR *msg) { (void)hr; (void)len; (void)msg; return FALSE; }
WINOLEAPI_(BOOL) RoOriginateError(HRESULT hr, HSTRING msg) { (void)hr; (void)msg; return FALSE; }
/* C++/WinRT's hresult_error reports through this before it throws; there is
 * no debugger to tell, so the error is only thrown (FALSE: not reported) */
WINOLEAPI_(BOOL) RoOriginateLanguageException(HRESULT hr, HSTRING msg, IUnknown *e) { (void)hr; (void)msg; (void)e; return FALSE; }

/* Agile references: every object here can be used from any thread, so
 * the reference holds the object and Resolve is a QueryInterface */
typedef struct AgileRef AgileRef;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(AgileRef *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(AgileRef *);
    ULONG   (STDMETHODCALLTYPE *Release)(AgileRef *);
    HRESULT (STDMETHODCALLTYPE *Resolve)(AgileRef *, REFIID, void **);
} AgileRefVtbl;
struct AgileRef { const AgileRefVtbl *lpVtbl; LONG refs; IUnknown *obj; };
static const GUID IID_IAgileReference_ = { 0xc03f6a43, 0x65a4, 0x9818, { 0x98, 0x7e, 0xe0, 0xb8, 0x10, 0xd2, 0xa6, 0xf2 } };
static const GUID IID_IUnknown__ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static HRESULT STDMETHODCALLTYPE ar_qi(AgileRef *a, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown__) || IsEqualGUID(iid, &IID_IAgileReference_)) {
        *out = a;
        InterlockedIncrement(&a->refs);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ar_addref(AgileRef *a) { return (ULONG)InterlockedIncrement(&a->refs); }
static ULONG STDMETHODCALLTYPE ar_release(AgileRef *a)
{
    LONG r = InterlockedDecrement(&a->refs);
    if (!r) { a->obj->lpVtbl->Release(a->obj); HeapFree(GetProcessHeap(), 0, a); }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE ar_resolve(AgileRef *a, REFIID iid, void **out) { return a->obj->lpVtbl->QueryInterface(a->obj, iid, out); }
static const AgileRefVtbl g_agile_vtbl = { ar_qi, ar_addref, ar_release, ar_resolve };

WINOLEAPI_(HRESULT) RoGetAgileReference(int options, REFIID iid, IUnknown *obj, void **out)
{
    (void)options;
    if (!obj || !out) return E_INVALIDARG;
    *out = 0;
    void *check;
    HRESULT hr = obj->lpVtbl->QueryInterface(obj, iid, &check);    /* the object must have @iid */
    if (FAILED(hr)) return hr;
    ((IUnknown *)check)->lpVtbl->Release((IUnknown *)check);
    AgileRef *a = HeapAlloc(GetProcessHeap(), 0, sizeof(AgileRef));
    if (!a) return E_OUTOFMEMORY;
    a->lpVtbl = &g_agile_vtbl;
    a->refs = 1;
    a->obj = obj;
    obj->lpVtbl->AddRef(obj);
    *out = a;
    return S_OK;
}

/* Channel hooks see cross-apartment calls; there are none */
WINOLEAPI_(HRESULT) CoRegisterChannelHook(REFGUID ext, void *hook) { (void)ext; (void)hook; return S_OK; }

/* OleDuplicateData: a copy of clipboard data; HGLOBAL-backed formats
 * (all but bitmaps, metafiles and palettes) by copying the memory */
WINOLEAPI_(HANDLE) OleDuplicateData(HANDLE src, WORD fmt, UINT flags)
{
    if (!src || fmt == 2 /* CF_BITMAP */ || fmt == 3 /* CF_METAFILEPICT */ || fmt == 9 /* CF_PALETTE */ ||
        fmt == 14 /* CF_ENHMETAFILE */) return 0;
    SIZE_T n = GlobalSize(src);
    HGLOBAL dst = GlobalAlloc(flags ? flags : GMEM_MOVEABLE, n);
    if (!dst) return 0;
    void *d = GlobalLock(dst), *s = GlobalLock(src);
    if (d && s) CopyMemory(d, s, n);
    if (s) GlobalUnlock(src);
    if (d) GlobalUnlock(dst);
    return dst;
}

/* No data object carries an OLE link */
WINOLEAPI_(HRESULT) OleQueryLinkFromData(IUnknown *data) { (void)data; return S_FALSE; }

/* Compound files (structured storage) cannot be created */
WINOLEAPI_(HRESULT) StgCreateStorageEx(const WCHAR *name, DWORD mode, DWORD fmt, DWORD attrs, void *opts, void *sd,
                                      REFIID iid, void **out)
{
    (void)name; (void)mode; (void)fmt; (void)attrs; (void)opts; (void)sd; (void)iid;
    if (out) *out = 0;
    return (HRESULT)0x80030001L;                    /* STG_E_INVALIDFUNCTION */
}
