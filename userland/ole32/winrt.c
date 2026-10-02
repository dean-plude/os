/*
 * Windows Runtime strings (HSTRING) and activation, the part of combase
 * (api-ms-win-core-winrt-*) Win32 programs touch: they make HSTRINGs to
 * name runtime classes, and ask for activation factories, which NovaOS
 * has none of (REGDB_E_CLASSNOTREG, as for a class Windows lacks).
 */
#define NOVA_BUILD_OLE32
#include <objbase.h>

typedef struct HSTRING_ *HSTRING;
/* An HSTRING is one of these; a reference string lives in the caller's
 * HSTRING_HEADER (24 bytes on x64, 20 on x86), a heap one is followed by
 * its characters */
typedef struct {
    UINT32 flags;                       /* 1: a reference (not owned) */
    UINT32 length;
    const WCHAR *chars;
    LONG refs;
} HStr;
typedef struct { BYTE b[sizeof(void *) == 8 ? 24 : 20]; } HSTRING_HEADER;
_Static_assert(sizeof(HStr) <= sizeof(HSTRING_HEADER), "HSTRING_HEADER holds a reference string");

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
    s->chars = src;
    s->refs = 0;
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

/* Activation: no runtime classes are installed */
WINOLEAPI_(HRESULT) RoInitialize(int type) { return CoInitializeEx(0, type == 0 ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED); }
WINOLEAPI_(void) RoUninitialize(void) { CoUninitialize(); }
WINOLEAPI_(HRESULT) RoGetActivationFactory(HSTRING cls, REFIID iid, void **f) { (void)cls; (void)iid; if (f) *f = 0; return REGDB_E_CLASSNOTREG; }
WINOLEAPI_(HRESULT) RoActivateInstance(HSTRING cls, void **inst) { (void)cls; if (inst) *inst = 0; return REGDB_E_CLASSNOTREG; }
WINOLEAPI_(BOOL) RoOriginateErrorW(HRESULT hr, UINT32 len, const WCHAR *msg) { (void)hr; (void)len; (void)msg; return FALSE; }
WINOLEAPI_(BOOL) RoOriginateError(HRESULT hr, HSTRING msg) { (void)hr; (void)msg; return FALSE; }
