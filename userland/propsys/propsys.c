/*
 * propsys.dll — the property system, the parts programs use without the
 * shell's property schema: in-memory property stores and PROPVARIANT
 * conversions.
 */
#include <objbase.h>

#define PSAPI_ __declspec(dllexport)
int _fltused = 0x9875;      /* VariantCompare compares doubles */
#define TYPE_E_TYPEMISMATCH ((HRESULT)0x80028CA0L)
#define STRSAFE_E_INSUFFICIENT_BUFFER ((HRESULT)0x8007007AL)

typedef struct { GUID fmtid; DWORD pid; } PROPERTYKEY;
/* PROPVARIANT (24 bytes on x64): the type, then a 16-byte value */
typedef struct {
    WORD vt, r1, r2, r3;
    union { LPWSTR pwszVal; LPSTR pszVal; LONG lVal; ULONG ulVal; SHORT iVal; USHORT uiVal; LONGLONG hVal; ULONGLONG uhVal;
            double dblVal; float fltVal; BOOL boolVal; struct { void *a, *b; } pad; };
} PROPVARIANT;
enum { VT_EMPTY = 0, VT_NULL = 1, VT_I2 = 2, VT_I4 = 3, VT_R4 = 4, VT_R8 = 5, VT_BSTR = 8, VT_BOOL = 11,
       VT_I1 = 16, VT_UI1 = 17, VT_UI2 = 18, VT_UI4 = 19, VT_I8 = 20, VT_UI8 = 21, VT_INT = 22, VT_UINT = 23,
       VT_LPSTR = 30, VT_LPWSTR = 31 };

WINOLEAPI_(HRESULT) PropVariantCopy(PROPVARIANT *dst, const PROPVARIANT *src);
WINOLEAPI_(HRESULT) PropVariantClear(PROPVARIANT *pv);

/* ---------------------------------------------------------------------------
 * PropVariantToString: strings as they are, numbers in decimal
 * ------------------------------------------------------------------------- */
static int put_dec(WCHAR *t, LONGLONG v, int neg)
{
    WCHAR r[24];
    int n = 0, k = 0;
    ULONGLONG u = neg && v < 0 ? (ULONGLONG)0 - (ULONGLONG)v : (ULONGLONG)v;
    do { r[n++] = (WCHAR)('0' + u % 10); u /= 10; } while (u);
    if (neg && v < 0) t[k++] = '-';
    while (n) t[k++] = r[--n];
    t[k] = 0;
    return k;
}

PSAPI_ HRESULT WINAPI PropVariantToString(const PROPVARIANT *pv, PWSTR out, UINT cch)
{
    if (!out || !cch) return E_INVALIDARG;
    out[0] = 0;
    WCHAR num[32];
    const WCHAR *w = 0;
    const char *a = 0;
    switch (pv ? pv->vt : VT_EMPTY) {
    case VT_EMPTY: case VT_NULL: w = L""; break;
    case VT_LPWSTR: case VT_BSTR: w = pv->pwszVal ? pv->pwszVal : L""; break;
    case VT_LPSTR: a = pv->pszVal ? pv->pszVal : ""; break;
    case VT_BOOL: w = pv->iVal ? L"1" : L"0"; break;
    case VT_I1: put_dec(num, (signed char)pv->iVal, 1); w = num; break;
    case VT_UI1: put_dec(num, (BYTE)pv->uiVal, 0); w = num; break;
    case VT_I2: put_dec(num, pv->iVal, 1); w = num; break;
    case VT_UI2: put_dec(num, pv->uiVal, 0); w = num; break;
    case VT_I4: case VT_INT: put_dec(num, pv->lVal, 1); w = num; break;
    case VT_UI4: case VT_UINT: put_dec(num, pv->ulVal, 0); w = num; break;
    case VT_I8: put_dec(num, pv->hVal, 1); w = num; break;
    case VT_UI8: put_dec(num, (LONGLONG)pv->uhVal, 0); w = num; break;
    default: return TYPE_E_TYPEMISMATCH;
    }
    UINT i = 0;
    if (w) for (; w[i] && i + 1 < cch; i++) out[i] = w[i];
    else i = (UINT)MultiByteToWideChar(CP_UTF8, 0, a, -1, out, (int)cch) ? (UINT)lstrlenW(out) : 0;
    out[i] = 0;
    if ((w && w[i]) || (a && !i && *a)) return STRSAFE_E_INSUFFICIENT_BUFFER;
    return S_OK;
}

/* VariantCompare: VARIANTs of the same kind, by value (strings ordinally) */
typedef struct { WORD vt, r1, r2, r3; union { LONGLONG hVal; LPWSTR bstrVal; double dblVal; }; } VARIANT_;
PSAPI_ int WINAPI VariantCompare(const VARIANT_ *a, const VARIANT_ *b)
{
    if (a->vt != b->vt) return a->vt < b->vt ? -1 : 1;
    switch (a->vt) {
    case VT_EMPTY: case VT_NULL: return 0;
    case VT_BSTR: {
        const WCHAR *x = a->bstrVal ? a->bstrVal : L"", *y = b->bstrVal ? b->bstrVal : L"";
        while (*x && *x == *y) x++, y++;
        return *x == *y ? 0 : *x < *y ? -1 : 1;
    }
    case VT_R4: case VT_R8: {
        double x = a->vt == VT_R4 ? *(const float *)&a->dblVal : a->dblVal, y = b->vt == VT_R4 ? *(const float *)&b->dblVal : b->dblVal;
        return x < y ? -1 : x > y;
    }
    case VT_UI4: case VT_UINT: case VT_UI8: return (ULONGLONG)a->hVal < (ULONGLONG)b->hVal ? -1 : a->hVal != b->hVal;
    default: return a->hVal < b->hVal ? -1 : a->hVal > b->hVal;
    }
}

/* ---------------------------------------------------------------------------
 * The in-memory property store (IPropertyStore, IPropertyStoreCache)
 * ------------------------------------------------------------------------- */
typedef struct Store Store;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Store *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Store *);
    ULONG   (STDMETHODCALLTYPE *Release)(Store *);
    HRESULT (STDMETHODCALLTYPE *GetCount)(Store *, DWORD *);
    HRESULT (STDMETHODCALLTYPE *GetAt)(Store *, DWORD, PROPERTYKEY *);
    HRESULT (STDMETHODCALLTYPE *GetValue)(Store *, const PROPERTYKEY *, PROPVARIANT *);
    HRESULT (STDMETHODCALLTYPE *SetValue)(Store *, const PROPERTYKEY *, const PROPVARIANT *);
    HRESULT (STDMETHODCALLTYPE *Commit)(Store *);
    /* IPropertyStoreCache */
    HRESULT (STDMETHODCALLTYPE *GetState)(Store *, const PROPERTYKEY *, int *);
    HRESULT (STDMETHODCALLTYPE *GetValueAndState)(Store *, const PROPERTYKEY *, PROPVARIANT *, int *);
    HRESULT (STDMETHODCALLTYPE *SetState)(Store *, const PROPERTYKEY *, int);
    HRESULT (STDMETHODCALLTYPE *SetValueAndState)(Store *, const PROPERTYKEY *, const PROPVARIANT *, int);
} StoreVtbl;
typedef struct { PROPERTYKEY key; PROPVARIANT val; int state; } Prop;
struct Store {
    const StoreVtbl *lpVtbl;
    LONG refs;
    CRITICAL_SECTION lock;
    Prop *props;
    DWORD n, cap;
};

static const GUID IID_IUnknown_ = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IPropertyStore_ = { 0x886d8eeb, 0x8cf2, 0x4446, { 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99 } };
static const GUID IID_IPropertyStoreCache_ = { 0x3017056d, 0x9a91, 0x4e90, { 0x93, 0x7d, 0x74, 0x6c, 0x72, 0xab, 0xbf, 0x4f } };

static Prop *find(Store *s, const PROPERTYKEY *k)
{
    for (DWORD i = 0; i < s->n; i++)
        if (s->props[i].key.pid == k->pid && IsEqualGUID(&s->props[i].key.fmtid, &k->fmtid)) return &s->props[i];
    return 0;
}

static HRESULT STDMETHODCALLTYPE st_qi(Store *s, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown_) || IsEqualGUID(iid, &IID_IPropertyStore_) || IsEqualGUID(iid, &IID_IPropertyStoreCache_)) {
        *out = s;
        InterlockedIncrement(&s->refs);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE st_addref(Store *s) { return (ULONG)InterlockedIncrement(&s->refs); }
static ULONG STDMETHODCALLTYPE st_release(Store *s)
{
    LONG r = InterlockedDecrement(&s->refs);
    if (!r) {
        for (DWORD i = 0; i < s->n; i++) PropVariantClear(&s->props[i].val);
        HeapFree(GetProcessHeap(), 0, s->props);
        DeleteCriticalSection(&s->lock);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE st_count(Store *s, DWORD *n) { if (!n) return E_POINTER; *n = s->n; return S_OK; }
static HRESULT STDMETHODCALLTYPE st_getat(Store *s, DWORD i, PROPERTYKEY *k)
{
    if (!k) return E_POINTER;
    EnterCriticalSection(&s->lock);
    HRESULT hr = i < s->n ? (*k = s->props[i].key, S_OK) : E_INVALIDARG;
    LeaveCriticalSection(&s->lock);
    return hr;
}
static HRESULT STDMETHODCALLTYPE st_getvs(Store *s, const PROPERTYKEY *k, PROPVARIANT *v, int *state)
{
    if (!k || !v) return E_POINTER;
    EnterCriticalSection(&s->lock);
    Prop *p = find(s, k);
    HRESULT hr = S_OK;
    if (p) hr = PropVariantCopy(v, &p->val);
    else ZeroMemory(v, sizeof(*v));                 /* absent: VT_EMPTY */
    if (state) *state = p ? p->state : 0;
    LeaveCriticalSection(&s->lock);
    return hr;
}
static HRESULT STDMETHODCALLTYPE st_get(Store *s, const PROPERTYKEY *k, PROPVARIANT *v) { return st_getvs(s, k, v, 0); }
static HRESULT STDMETHODCALLTYPE st_setvs(Store *s, const PROPERTYKEY *k, const PROPVARIANT *v, int state)
{
    if (!k || !v) return E_POINTER;
    EnterCriticalSection(&s->lock);
    Prop *p = find(s, k);
    HRESULT hr = S_OK;
    if (!p) {
        if (s->n == s->cap) {
            DWORD cap = s->cap ? s->cap * 2 : 8;
            Prop *np = s->props ? HeapReAlloc(GetProcessHeap(), 0, s->props, cap * sizeof(Prop))
                                : HeapAlloc(GetProcessHeap(), 0, cap * sizeof(Prop));
            if (!np) hr = E_OUTOFMEMORY;
            else s->props = np, s->cap = cap;
        }
        if (SUCCEEDED(hr)) {
            p = &s->props[s->n++];
            p->key = *k;
            ZeroMemory(&p->val, sizeof(p->val));
        }
    }
    if (p) {
        PROPVARIANT copy;
        ZeroMemory(&copy, sizeof(copy));
        hr = PropVariantCopy(&copy, v);
        if (SUCCEEDED(hr)) {
            PropVariantClear(&p->val);
            p->val = copy;
            p->state = state;
        }
    }
    LeaveCriticalSection(&s->lock);
    return hr;
}
static HRESULT STDMETHODCALLTYPE st_set(Store *s, const PROPERTYKEY *k, const PROPVARIANT *v) { return st_setvs(s, k, v, 0); }
static HRESULT STDMETHODCALLTYPE st_commit(Store *s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE st_getstate(Store *s, const PROPERTYKEY *k, int *state)
{
    if (!k || !state) return E_POINTER;
    EnterCriticalSection(&s->lock);
    Prop *p = find(s, k);
    *state = p ? p->state : 0;
    LeaveCriticalSection(&s->lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE st_setstate(Store *s, const PROPERTYKEY *k, int state)
{
    if (!k) return E_POINTER;
    EnterCriticalSection(&s->lock);
    Prop *p = find(s, k);
    if (p) p->state = state;
    LeaveCriticalSection(&s->lock);
    return p ? S_OK : E_INVALIDARG;
}

static const StoreVtbl g_store_vtbl = {
    st_qi, st_addref, st_release, st_count, st_getat, st_get, st_set, st_commit,
    st_getstate, st_getvs, st_setstate, st_setvs,
};

PSAPI_ HRESULT WINAPI PSCreateMemoryPropertyStore(REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Store *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Store));
    if (!s) return E_OUTOFMEMORY;
    s->lpVtbl = &g_store_vtbl;
    s->refs = 1;
    InitializeCriticalSection(&s->lock);
    HRESULT hr = st_qi(s, iid, out);
    st_release(s);
    return hr;
}

/* PROPERTYKEY <-> "{fmtid} pid" text */
PSAPI_ HRESULT WINAPI PSStringFromPropertyKey(const PROPERTYKEY *k, LPWSTR out, UINT cch)
{
    if (!k || !out) return E_POINTER;
    WCHAR t[64];
    int n = StringFromGUID2(&k->fmtid, t, 40) - 1;
    t[n++] = ' ';
    n += put_dec(t + n, k->pid, 0);
    if ((UINT)n + 1 > cch) return E_NOT_SUFFICIENT_BUFFER;
    for (int i = 0; i <= n; i++) out[i] = t[i];
    return S_OK;
}
