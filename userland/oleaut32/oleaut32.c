/*
 * oleaut32.dll — OLE Automation: BSTR strings, VARIANTs and conversions,
 * SAFEARRAYs, variant dates and per-thread error info.  Type libraries are
 * in typelib.c and typeinfo.c, Invoke, DispCallFunc and the standard
 * IDispatch in invoke.c.
 */

#define NOVA_BUILD_OLEAUT32
#include <oleauto.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>

int _fltused = 0x9875;      /* floating point in use (the compiler references it) */

/* ---------------------------------------------------------------------------
 * BSTR: a length-prefixed (bytes, 32-bit) string with a terminating NUL
 * ------------------------------------------------------------------------- */
static BSTR bstr_alloc(UINT bytes)
{
    if (bytes > 0x7FFFFFF0u) return 0;
    BYTE *p = CoTaskMemAlloc((SIZE_T)bytes + sizeof(DWORD) + sizeof(WCHAR) + 1);
    if (!p) return 0;
    *(DWORD *)p = bytes;
    p[sizeof(DWORD) + bytes] = 0;
    p[sizeof(DWORD) + bytes + 1] = 0;
    p[sizeof(DWORD) + bytes + 2] = 0;
    return (BSTR)(p + sizeof(DWORD));
}

WINOLEAUTAPI_(BSTR) SysAllocStringLen(const OLECHAR *s, UINT n)
{
    if (n > 0x3FFFFFF0u) return 0;
    BSTR b = bstr_alloc(n * sizeof(WCHAR));
    if (b && s) CopyMemory(b, s, n * sizeof(WCHAR));
    else if (b) ZeroMemory(b, n * sizeof(WCHAR));
    return b;
}

WINOLEAUTAPI_(BSTR) SysAllocString(const OLECHAR *s)
{
    return s ? SysAllocStringLen(s, (UINT)wcslen(s)) : 0;
}

WINOLEAUTAPI_(BSTR) SysAllocStringByteLen(LPCSTR s, UINT n)
{
    BSTR b = bstr_alloc(n);
    if (b && s) CopyMemory(b, s, n);
    else if (b) ZeroMemory(b, n);
    return b;
}

WINOLEAUTAPI_(void) SysFreeString(BSTR b)
{
    if (b) CoTaskMemFree((BYTE *)b - sizeof(DWORD));
}

WINOLEAUTAPI_(UINT) SysStringByteLen(BSTR b) { return b ? *(DWORD *)((BYTE *)b - sizeof(DWORD)) : 0; }
WINOLEAUTAPI_(UINT) SysStringLen(BSTR b) { return SysStringByteLen(b) / sizeof(WCHAR); }

WINOLEAUTAPI_(INT) SysReAllocStringLen(BSTR *b, const OLECHAR *s, UINT n)
{
    if (!b) return FALSE;
    BSTR nb = SysAllocStringLen(s, n);           /* s may point into *b: copy first */
    if (!nb) return FALSE;
    SysFreeString(*b);
    *b = nb;
    return TRUE;
}

WINOLEAUTAPI_(INT) SysReAllocString(BSTR *b, const OLECHAR *s)
{
    return SysReAllocStringLen(b, s, s ? (UINT)wcslen(s) : 0);
}

/* ---------------------------------------------------------------------------
 * Dates: days since 1899-12-30, the fraction is the time of day
 * ------------------------------------------------------------------------- */
static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;       /* days since 1970-01-01 */
}

static void civil_from_days(long long z, int *y, int *m, int *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

#define DAYS_1899_TO_1970 25569

WINOLEAUTAPI_(INT) SystemTimeToVariantTime(SYSTEMTIME *st, double *out)
{
    if (!st || !out || st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > 31 || st->wYear > 9999) return FALSE;
    double days = (double)(days_from_civil(st->wYear, st->wMonth, st->wDay) + DAYS_1899_TO_1970);
    double frac = (st->wHour * 3600.0 + st->wMinute * 60.0 + st->wSecond) / 86400.0;
    *out = days >= 0 ? days + frac : days - frac;       /* negative dates count the time forwards */
    return TRUE;
}

WINOLEAUTAPI_(INT) VariantTimeToSystemTime(double t, SYSTEMTIME *st)
{
    if (!st || t < -657434.0 || t >= 2958466.0) return FALSE;
    /* the integer part is the day (toward zero); the time counts forwards from it */
    double whole = (double)(long long)t;
    double frac = t - whole;
    if (frac < 0) frac = -frac;
    long long secs = (long long)(frac * 86400.0 + 0.5);
    long long day = (long long)whole;
    if (secs >= 86400) { secs -= 86400; day++; }
    int y, m, d;
    civil_from_days(day - DAYS_1899_TO_1970, &y, &m, &d);
    st->wYear = (WORD)y;
    st->wMonth = (WORD)m;
    st->wDay = (WORD)d;
    st->wDayOfWeek = (WORD)(((day - DAYS_1899_TO_1970) % 7 + 7 + 4) % 7);   /* 1970-01-01 was a Thursday */
    st->wHour = (WORD)(secs / 3600);
    st->wMinute = (WORD)(secs / 60 % 60);
    st->wSecond = (WORD)(secs % 60);
    st->wMilliseconds = 0;
    return TRUE;
}

WINOLEAUTAPI_(INT) DosDateTimeToVariantTime(USHORT date, USHORT time, double *out)
{
    SYSTEMTIME st = { (WORD)(1980 + (date >> 9)), (WORD)(date >> 5 & 15), 0, (WORD)(date & 31),
                      (WORD)(time >> 11), (WORD)(time >> 5 & 63), (WORD)((time & 31) * 2), 0 };
    return SystemTimeToVariantTime(&st, out);
}

WINOLEAUTAPI_(INT) VariantTimeToDosDateTime(double t, USHORT *date, USHORT *time)
{
    SYSTEMTIME st;
    if (!VariantTimeToSystemTime(t, &st) || st.wYear < 1980 || st.wYear > 2107) return FALSE;
    *date = (USHORT)((st.wYear - 1980) << 9 | st.wMonth << 5 | st.wDay);
    *time = (USHORT)(st.wHour << 11 | st.wMinute << 5 | st.wSecond / 2);
    return TRUE;
}

/* en-US style "M/D/YYYY h:mm:ss AM", leaving out a zero date or time */
static BSTR date_to_bstr(DATE t)
{
    SYSTEMTIME st;
    if (!VariantTimeToSystemTime(t, &st)) return 0;
    WCHAR buf[64];
    int n = 0;
    BOOL has_date = (long long)t != 0, has_time = st.wHour || st.wMinute || st.wSecond;
    if (has_date || !has_time) n = _snwprintf(buf, 64, L"%u/%u/%u", st.wMonth, st.wDay, st.wYear);
    if (has_time) {
        unsigned h = st.wHour % 12 ? st.wHour % 12 : 12;
        n += _snwprintf(buf + n, (size_t)(64 - n), L"%s%u:%02u:%02u %s", n ? L" " : L"", h, st.wMinute, st.wSecond,
                        st.wHour < 12 ? L"AM" : L"PM");
    }
    return SysAllocStringLen(buf, (UINT)n);
}

/* accepts "YYYY-MM-DD[ HH:MM[:SS]]", "M/D/YYYY[ h:mm[:ss][ AM|PM]]" and "h:mm[:ss][ AM|PM]" */
static HRESULT date_from_str(const WCHAR *s, DATE *out)
{
    unsigned a[6] = { 0 };
    int n = 0, sep_date = 0;
    const WCHAR *p = s;
    while (*p == ' ') p++;
    SYSTEMTIME st = { 1899, 12, 0, 30, 0, 0, 0, 0 };
    while (*p && n < 6) {
        if (*p < '0' || *p > '9') break;
        unsigned v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned)(*p++ - '0');
        a[n++] = v;
        if (*p == '-' || *p == '/') { sep_date = sep_date ? sep_date : *p; p++; }
        else if (*p == ':' || *p == ' ' || *p == 'T') { if (n == 3 || *p == ':') ; p++; while (*p == ' ') p++; }
        else break;
    }
    int t0 = 0;
    if (sep_date == '-' && n >= 3) { st.wYear = (WORD)a[0]; st.wMonth = (WORD)a[1]; st.wDay = (WORD)a[2]; t0 = 3; }
    else if (sep_date == '/' && n >= 3) { st.wMonth = (WORD)a[0]; st.wDay = (WORD)a[1]; st.wYear = (WORD)a[2]; t0 = 3; }
    else if (sep_date) return DISP_E_TYPEMISMATCH;
    if (n > t0) {
        if (n - t0 < 2) return DISP_E_TYPEMISMATCH;
        st.wHour = (WORD)a[t0]; st.wMinute = (WORD)a[t0 + 1]; st.wSecond = (WORD)(n - t0 > 2 ? a[t0 + 2] : 0);
        while (*p == ' ') p++;
        if ((p[0] | 32) == 'p' && (p[1] | 32) == 'm' && st.wHour < 12) st.wHour += 12;
        else if ((p[0] | 32) == 'a' && (p[1] | 32) == 'm' && st.wHour == 12) st.wHour = 0;
        if (st.wHour > 23 || st.wMinute > 59 || st.wSecond > 59) return DISP_E_TYPEMISMATCH;
    }
    if (!n) return DISP_E_TYPEMISMATCH;
    if (st.wYear < 100 && t0) st.wYear += st.wYear < 30 ? 2000 : 1900;
    return SystemTimeToVariantTime(&st, out) ? S_OK : DISP_E_TYPEMISMATCH;
}

/* ---------------------------------------------------------------------------
 * SAFEARRAY: the descriptor is preceded by 16 bytes holding an IID (FADF_HAVEIID)
 * or, in its last 4 bytes, the VARTYPE (FADF_HAVEVARTYPE), as on Windows
 * ------------------------------------------------------------------------- */
static ULONG vt_size(VARTYPE vt)
{
    switch (vt) {
    case VT_I1: case VT_UI1: return 1;
    case VT_I2: case VT_UI2: case VT_BOOL: return 2;
    case VT_I4: case VT_UI4: case VT_R4: case VT_INT: case VT_UINT: case VT_ERROR: return 4;
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: return 8;
    case VT_BSTR: case VT_UNKNOWN: case VT_DISPATCH: case VT_INT_PTR: case VT_UINT_PTR: return sizeof(void *);
    case VT_VARIANT: return sizeof(VARIANT);
    case VT_DECIMAL: return sizeof(DECIMAL);
    default: return 0;
    }
}

static ULONG sa_count(SAFEARRAY *a)
{
    ULONG n = 1;
    for (USHORT i = 0; i < a->cDims; i++) n *= a->rgsabound[i].cElements;
    return n;
}

WINOLEAUTAPI_(HRESULT) SafeArrayAllocDescriptor(UINT dims, SAFEARRAY **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    if (!dims || dims > 65535) return E_INVALIDARG;
    SIZE_T size = sizeof(SAFEARRAY) + (dims - 1) * sizeof(SAFEARRAYBOUND);
    BYTE *p = CoTaskMemAlloc(16 + size);
    if (!p) return E_OUTOFMEMORY;
    ZeroMemory(p, 16 + size);
    *out = (SAFEARRAY *)(p + 16);
    (*out)->cDims = (USHORT)dims;
    return S_OK;
}

static void sa_set_vt(SAFEARRAY *a, VARTYPE vt)
{
    a->fFeatures &= ~(FADF_BSTR | FADF_UNKNOWN | FADF_DISPATCH | FADF_VARIANT | FADF_HAVEVARTYPE | FADF_HAVEIID);
    if (vt == VT_BSTR) a->fFeatures |= FADF_BSTR;
    else if (vt == VT_UNKNOWN) a->fFeatures |= FADF_UNKNOWN;
    else if (vt == VT_DISPATCH) a->fFeatures |= FADF_DISPATCH;
    else if (vt == VT_VARIANT) a->fFeatures |= FADF_VARIANT;
    if (vt == VT_UNKNOWN || vt == VT_DISPATCH) {
        a->fFeatures |= FADF_HAVEIID;
        ((GUID *)a)[-1] = vt == VT_UNKNOWN ? IID_IUnknown : IID_IDispatch;
    } else {
        a->fFeatures |= FADF_HAVEVARTYPE;
        ((DWORD *)a)[-1] = vt;
    }
}

WINOLEAUTAPI_(HRESULT) SafeArrayAllocDescriptorEx(VARTYPE vt, UINT dims, SAFEARRAY **out)
{
    HRESULT hr = SafeArrayAllocDescriptor(dims, out);
    if (FAILED(hr)) return hr;
    sa_set_vt(*out, vt);
    (*out)->cbElements = vt_size(vt);
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayAllocData(SAFEARRAY *a)
{
    if (!a) return E_INVALIDARG;
    SIZE_T n = (SIZE_T)sa_count(a) * a->cbElements;
    a->pvData = CoTaskMemAlloc(n ? n : 1);
    if (!a->pvData) return E_OUTOFMEMORY;
    ZeroMemory(a->pvData, n);
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayGetVartype(SAFEARRAY *a, VARTYPE *vt)
{
    if (!a || !vt) return E_INVALIDARG;
    if (a->fFeatures & FADF_HAVEVARTYPE) *vt = (VARTYPE)((DWORD *)a)[-1];
    else if (a->fFeatures & FADF_BSTR) *vt = VT_BSTR;
    else if (a->fFeatures & FADF_UNKNOWN) *vt = VT_UNKNOWN;
    else if (a->fFeatures & FADF_DISPATCH) *vt = VT_DISPATCH;
    else if (a->fFeatures & FADF_VARIANT) *vt = VT_VARIANT;
    else if (a->fFeatures & FADF_RECORD) *vt = VT_RECORD;
    else return E_INVALIDARG;
    return S_OK;
}

WINOLEAUTAPI_(SAFEARRAY *) SafeArrayCreate(VARTYPE vt, UINT dims, SAFEARRAYBOUND *bounds)
{
    if (!bounds || !vt_size(vt)) return 0;
    SAFEARRAY *a;
    if (FAILED(SafeArrayAllocDescriptorEx(vt, dims, &a))) return 0;
    for (UINT i = 0; i < dims; i++) a->rgsabound[i] = bounds[dims - 1 - i];     /* stored right to left */
    if (FAILED(SafeArrayAllocData(a))) { CoTaskMemFree((BYTE *)a - 16); return 0; }
    return a;
}

WINOLEAUTAPI_(SAFEARRAY *) SafeArrayCreateVector(VARTYPE vt, LONG lbound, ULONG n)
{
    SAFEARRAYBOUND b = { n, lbound };
    return SafeArrayCreate(vt, 1, &b);
}

WINOLEAUTAPI_(SAFEARRAY *) SafeArrayCreateEx(VARTYPE vt, UINT dims, SAFEARRAYBOUND *bounds, PVOID extra)
{
    (void)extra;
    return SafeArrayCreate(vt, dims, bounds);
}

static void release_elems(SAFEARRAY *a, void *data, ULONG n)
{
    if (a->fFeatures & FADF_BSTR) for (ULONG i = 0; i < n; i++) { SysFreeString(((BSTR *)data)[i]); ((BSTR *)data)[i] = 0; }
    else if (a->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH))
        for (ULONG i = 0; i < n; i++) {
            IUnknown *u = ((IUnknown **)data)[i];
            if (u) u->lpVtbl->Release(u);
            ((IUnknown **)data)[i] = 0;
        }
    else if (a->fFeatures & FADF_VARIANT) for (ULONG i = 0; i < n; i++) VariantClear(&((VARIANT *)data)[i]);
}

WINOLEAUTAPI_(HRESULT) SafeArrayDestroyData(SAFEARRAY *a)
{
    if (!a) return E_INVALIDARG;
    if (a->cLocks) return DISP_E_ARRAYISLOCKED;
    if (a->pvData) {
        release_elems(a, a->pvData, sa_count(a));
        if (!(a->fFeatures & (FADF_STATIC | FADF_AUTO | FADF_EMBEDDED))) {
            CoTaskMemFree(a->pvData);
            a->pvData = 0;
        }
    }
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayDestroyDescriptor(SAFEARRAY *a)
{
    if (!a) return S_OK;
    if (a->cLocks) return DISP_E_ARRAYISLOCKED;
    CoTaskMemFree((BYTE *)a - 16);
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayDestroy(SAFEARRAY *a)
{
    if (!a) return S_OK;
    if (a->cLocks) return DISP_E_ARRAYISLOCKED;
    HRESULT hr = SafeArrayDestroyData(a);
    if (FAILED(hr)) return hr;
    return SafeArrayDestroyDescriptor(a);
}

WINOLEAUTAPI_(UINT) SafeArrayGetDim(SAFEARRAY *a) { return a ? a->cDims : 0; }
WINOLEAUTAPI_(UINT) SafeArrayGetElemsize(SAFEARRAY *a) { return a ? a->cbElements : 0; }

WINOLEAUTAPI_(HRESULT) SafeArrayGetLBound(SAFEARRAY *a, UINT dim, LONG *out)
{
    if (!a || !out) return E_INVALIDARG;
    if (!dim || dim > a->cDims) return DISP_E_BADINDEX;
    *out = a->rgsabound[a->cDims - dim].lLbound;
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayGetUBound(SAFEARRAY *a, UINT dim, LONG *out)
{
    if (!a || !out) return E_INVALIDARG;
    if (!dim || dim > a->cDims) return DISP_E_BADINDEX;
    SAFEARRAYBOUND *b = &a->rgsabound[a->cDims - dim];
    *out = b->lLbound + (LONG)b->cElements - 1;
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayLock(SAFEARRAY *a)
{
    if (!a) return E_INVALIDARG;
    if (InterlockedIncrement((LONG *)&a->cLocks) > 0xFFFF) { InterlockedDecrement((LONG *)&a->cLocks); return E_UNEXPECTED; }
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayUnlock(SAFEARRAY *a)
{
    if (!a) return E_INVALIDARG;
    if (!a->cLocks) return E_UNEXPECTED;
    InterlockedDecrement((LONG *)&a->cLocks);
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayAccessData(SAFEARRAY *a, void **data)
{
    if (!a || !data) return E_INVALIDARG;
    HRESULT hr = SafeArrayLock(a);
    *data = SUCCEEDED(hr) ? a->pvData : 0;
    return hr;
}

WINOLEAUTAPI_(HRESULT) SafeArrayUnaccessData(SAFEARRAY *a) { return SafeArrayUnlock(a); }

/* idx[0] is the leftmost (first) dimension, which is stored last */
WINOLEAUTAPI_(HRESULT) SafeArrayPtrOfIndex(SAFEARRAY *a, LONG *idx, void **out)
{
    if (!a || !idx || !out) return E_INVALIDARG;
    ULONG off = 0, mult = 1;
    for (USHORT d = 0; d < a->cDims; d++) {
        SAFEARRAYBOUND *b = &a->rgsabound[a->cDims - 1 - d];
        LONG i = idx[d] - b->lLbound;
        if (i < 0 || (ULONG)i >= b->cElements) return DISP_E_BADINDEX;
        off += (ULONG)i * mult;
        mult *= b->cElements;
    }
    *out = (BYTE *)a->pvData + (SIZE_T)off * a->cbElements;
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayGetElement(SAFEARRAY *a, LONG *idx, void *out)
{
    void *p;
    HRESULT hr = SafeArrayPtrOfIndex(a, idx, &p);
    if (FAILED(hr)) return hr;
    if (!out) return E_INVALIDARG;
    if (a->fFeatures & FADF_BSTR) {
        BSTR s = *(BSTR *)p;
        *(BSTR *)out = s ? SysAllocStringByteLen((LPCSTR)s, SysStringByteLen(s)) : 0;
        if (s && !*(BSTR *)out) return E_OUTOFMEMORY;
    } else if (a->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
        IUnknown *u = *(IUnknown **)p;
        if (u) u->lpVtbl->AddRef(u);
        *(IUnknown **)out = u;
    } else if (a->fFeatures & FADF_VARIANT) {
        VariantInit(out);
        return VariantCopy(out, p);
    } else CopyMemory(out, p, a->cbElements);
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayPutElement(SAFEARRAY *a, LONG *idx, void *in)
{
    void *p;
    HRESULT hr = SafeArrayPtrOfIndex(a, idx, &p);
    if (FAILED(hr)) return hr;
    if (a->fFeatures & FADF_BSTR) {
        BSTR s = in, c = s ? SysAllocStringByteLen((LPCSTR)s, SysStringByteLen(s)) : 0;
        if (s && !c) return E_OUTOFMEMORY;
        SysFreeString(*(BSTR *)p);
        *(BSTR *)p = c;
    } else if (a->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
        IUnknown *u = in, *old = *(IUnknown **)p;
        if (u) u->lpVtbl->AddRef(u);
        if (old) old->lpVtbl->Release(old);
        *(IUnknown **)p = u;
    } else if (a->fFeatures & FADF_VARIANT) {
        return VariantCopy(p, in);
    } else CopyMemory(p, in, a->cbElements);
    return S_OK;
}

static HRESULT copy_elems(SAFEARRAY *src, void *dst)
{
    ULONG n = sa_count(src);
    if (src->fFeatures & FADF_BSTR) {
        for (ULONG i = 0; i < n; i++) {
            BSTR s = ((BSTR *)src->pvData)[i];
            ((BSTR *)dst)[i] = s ? SysAllocStringByteLen((LPCSTR)s, SysStringByteLen(s)) : 0;
            if (s && !((BSTR *)dst)[i]) return E_OUTOFMEMORY;
        }
    } else if (src->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
        for (ULONG i = 0; i < n; i++) {
            IUnknown *u = ((IUnknown **)src->pvData)[i];
            if (u) u->lpVtbl->AddRef(u);
            ((IUnknown **)dst)[i] = u;
        }
    } else if (src->fFeatures & FADF_VARIANT) {
        for (ULONG i = 0; i < n; i++) {
            VariantInit(&((VARIANT *)dst)[i]);
            HRESULT hr = VariantCopy(&((VARIANT *)dst)[i], &((VARIANT *)src->pvData)[i]);
            if (FAILED(hr)) return hr;
        }
    } else CopyMemory(dst, src->pvData, (SIZE_T)n * src->cbElements);
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayCopy(SAFEARRAY *a, SAFEARRAY **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (!a) return S_OK;
    SAFEARRAY *c;
    HRESULT hr = SafeArrayAllocDescriptor(a->cDims, &c);
    if (FAILED(hr)) return hr;
    CopyMemory((BYTE *)c - 16, (BYTE *)a - 16, 16 + sizeof(SAFEARRAY) + (a->cDims - 1) * sizeof(SAFEARRAYBOUND));
    c->cLocks = 0;
    c->fFeatures &= ~(FADF_STATIC | FADF_AUTO | FADF_EMBEDDED);
    c->pvData = 0;
    if (a->pvData) {
        if (FAILED(hr = SafeArrayAllocData(c)) || FAILED(hr = copy_elems(a, c->pvData))) {
            SafeArrayDestroy(c);
            return hr;
        }
    }
    *out = c;
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayCopyData(SAFEARRAY *src, SAFEARRAY *dst)
{
    if (!src || !dst || src->cDims != dst->cDims || src->cbElements != dst->cbElements) return E_INVALIDARG;
    for (USHORT i = 0; i < src->cDims; i++)
        if (src->rgsabound[i].cElements != dst->rgsabound[i].cElements) return E_INVALIDARG;
    release_elems(dst, dst->pvData, sa_count(dst));
    return copy_elems(src, dst->pvData);
}

/* changes the last (rightmost) dimension, keeping the elements that remain */
WINOLEAUTAPI_(HRESULT) SafeArrayRedim(SAFEARRAY *a, SAFEARRAYBOUND *bound)
{
    if (!a || !bound) return E_INVALIDARG;
    if (a->cLocks) return DISP_E_ARRAYISLOCKED;
    if (a->fFeatures & FADF_FIXEDSIZE) return E_INVALIDARG;
    ULONG old_n = sa_count(a), per = old_n / (a->rgsabound[0].cElements ? a->rgsabound[0].cElements : 1);
    if (!a->rgsabound[0].cElements) per = 1;
    ULONG new_n = per * bound->cElements;
    if (new_n < old_n) release_elems(a, (BYTE *)a->pvData + (SIZE_T)new_n * a->cbElements, old_n - new_n);
    void *p = CoTaskMemRealloc(a->pvData, (SIZE_T)(new_n ? new_n : 1) * a->cbElements);
    if (!p) return E_OUTOFMEMORY;
    if (new_n > old_n) ZeroMemory((BYTE *)p + (SIZE_T)old_n * a->cbElements, (SIZE_T)(new_n - old_n) * a->cbElements);
    a->pvData = p;
    a->rgsabound[0] = *bound;
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArrayGetIID(SAFEARRAY *a, GUID *g)
{
    if (!a || !g || !(a->fFeatures & FADF_HAVEIID)) return E_INVALIDARG;
    *g = ((GUID *)a)[-1];
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SafeArraySetIID(SAFEARRAY *a, REFGUID g)
{
    if (!a || !g || !(a->fFeatures & FADF_HAVEIID)) return E_INVALIDARG;
    ((GUID *)a)[-1] = *g;
    return S_OK;
}

/* ---------------------------------------------------------------------------
 * VARIANT
 * ------------------------------------------------------------------------- */
WINOLEAUTAPI_(void) VariantInit(VARIANTARG *v)
{
    if (v) { v->vt = VT_EMPTY; v->wReserved1 = v->wReserved2 = v->wReserved3 = 0; }
}

static BOOL vt_valid(VARTYPE vt)
{
    VARTYPE base = vt & VT_TYPEMASK;
    if (vt & VT_VECTOR) return FALSE;
    if (base == VT_EMPTY || base == VT_NULL) return !(vt & (VT_BYREF | VT_ARRAY)) || base == VT_EMPTY;
    return base <= VT_UINT || base == VT_RECORD || base == VT_INT_PTR || base == VT_UINT_PTR;
}

WINOLEAUTAPI_(HRESULT) VariantClear(VARIANTARG *v)
{
    if (!v) return E_INVALIDARG;
    if (!vt_valid(v->vt) && v->vt != VT_EMPTY) return DISP_E_BADVARTYPE;
    if (!(v->vt & VT_BYREF)) {
        if (v->vt & VT_ARRAY) SafeArrayDestroy(v->parray);
        else switch (v->vt) {
            case VT_BSTR: SysFreeString(v->bstrVal); break;
            case VT_UNKNOWN: case VT_DISPATCH: if (v->punkVal) v->punkVal->lpVtbl->Release(v->punkVal); break;
            default: break;
            }
    }
    v->vt = VT_EMPTY;
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) VariantCopy(VARIANTARG *dst, const VARIANTARG *src)
{
    if (!dst || !src) return E_INVALIDARG;
    if (!vt_valid(src->vt) && src->vt != VT_EMPTY) return DISP_E_BADVARTYPE;
    if (dst == src) return S_OK;
    HRESULT hr = VariantClear(dst);
    if (FAILED(hr)) return hr;
    VARIANT t = *src;
    if (!(src->vt & VT_BYREF)) {
        if (src->vt & VT_ARRAY) {
            if (FAILED(hr = SafeArrayCopy(src->parray, &t.parray))) return hr;
        } else if (src->vt == VT_BSTR) {
            if (src->bstrVal && !(t.bstrVal = SysAllocStringByteLen((LPCSTR)src->bstrVal, SysStringByteLen(src->bstrVal))))
                return E_OUTOFMEMORY;
        } else if ((src->vt == VT_UNKNOWN || src->vt == VT_DISPATCH) && src->punkVal) {
            src->punkVal->lpVtbl->AddRef(src->punkVal);
        }
    }
    *dst = t;
    return S_OK;
}

/* the value behind a VT_BYREF, as a by-value variant (no copy of owned data) */
static void deref(const VARIANT *src, VARIANT *out)
{
    VariantInit(out);
    VARTYPE vt = src->vt & ~VT_BYREF;
    out->vt = vt;
    if (vt & VT_ARRAY) { out->parray = *src->pparray; return; }
    switch (vt) {
    case VT_VARIANT: *out = *src->pvarVal; break;
    case VT_DECIMAL: out->decVal = *src->pdecVal; out->vt = VT_DECIMAL; break;
    default: {
        ULONG n = vt_size(vt);
        if (n) CopyMemory(&out->llVal, src->byref, n);
    }
    }
}

WINOLEAUTAPI_(HRESULT) VariantCopyInd(VARIANT *dst, const VARIANTARG *src)
{
    if (!dst || !src) return E_INVALIDARG;
    if (!(src->vt & VT_BYREF)) return VariantCopy(dst, src);
    VARIANT tmp;
    deref(src, &tmp);
    if (dst == src) {                               /* the copy must not free what it reads */
        VARIANT c;
        VariantInit(&c);
        HRESULT hr = VariantCopy(&c, &tmp);
        if (SUCCEEDED(hr)) *dst = c;
        return hr;
    }
    return VariantCopy(dst, &tmp);
}

/* numbers go through a double or a 64-bit integer, whichever is exact */
typedef struct { int kind; long long i; unsigned long long u; double d; } Num;   /* kind: 0 int, 1 uint, 2 real */

static HRESULT parse_num(const WCHAR *s, Num *n)
{
    if (!s) return DISP_E_TYPEMISMATCH;
    while (*s == ' ' || *s == '\t') s++;
    WCHAR *end;
    if (s[0] == '&' && (s[1] | 32) == 'h') {                 /* Visual Basic hex */
        n->kind = 0;
        n->i = (long long)wcstoull(s + 2, &end, 16);
    } else {
        long long iv = wcstoll(s, &end, 10);
        if (*end == '.' || *end == 'e' || *end == 'E') {
            n->kind = 2;
            n->d = wcstod(s, &end);
        } else {
            n->kind = 0;
            n->i = iv;
        }
    }
    while (*end == ' ' || *end == '\t') end++;
    if (end == s || *end) return DISP_E_TYPEMISMATCH;
    return S_OK;
}

static HRESULT to_num(const VARIANT *v, Num *n)
{
    n->kind = 0;
    switch (v->vt) {
    case VT_EMPTY: n->i = 0; return S_OK;
    case VT_I1: n->i = v->cVal; return S_OK;
    case VT_UI1: n->i = v->bVal; return S_OK;
    case VT_I2: n->i = v->iVal; return S_OK;
    case VT_UI2: n->i = v->uiVal; return S_OK;
    case VT_I4: case VT_INT: case VT_ERROR: n->i = v->lVal; return S_OK;
    case VT_UI4: case VT_UINT: n->i = v->ulVal; return S_OK;
    case VT_I8: case VT_INT_PTR: n->i = v->llVal; return S_OK;
    case VT_UI8: case VT_UINT_PTR: n->kind = 1; n->u = v->ullVal; return S_OK;
    case VT_BOOL: n->i = v->boolVal ? -1 : 0; return S_OK;
    case VT_R4: n->kind = 2; n->d = v->fltVal; return S_OK;
    case VT_R8: case VT_DATE: n->kind = 2; n->d = v->dblVal; return S_OK;
    case VT_CY: n->kind = 2; n->d = (double)v->cyVal.int64 / 10000.0; return S_OK;
    case VT_DECIMAL: {
        double d = (double)v->decVal.Hi32 * 18446744073709551616.0 + (double)v->decVal.Lo64;
        for (int i = 0; i < v->decVal.scale; i++) d /= 10;
        n->kind = 2;
        n->d = v->decVal.sign & 0x80 ? -d : d;
        return S_OK;
    }
    case VT_BSTR: {
        const WCHAR *s = v->bstrVal ? v->bstrVal : L"";
        if (!_wcsicmp(s, L"true") || !_wcsicmp(s, L"#TRUE#")) { n->i = -1; return S_OK; }
        if (!_wcsicmp(s, L"false") || !_wcsicmp(s, L"#FALSE#")) { n->i = 0; return S_OK; }
        HRESULT hr = parse_num(s, n);
        if (FAILED(hr)) {                                    /* a date string counts as its number */
            DATE d;
            if (SUCCEEDED(date_from_str(s, &d))) { n->kind = 2; n->d = d; return S_OK; }
        }
        return hr;
    }
    default: return DISP_E_TYPEMISMATCH;
    }
}

/* rounds half to even, as VariantChangeType does */
static double round_even(double d)
{
    double f = (double)(long long)d;                 /* toward zero */
    double r = d - f;
    if (r > 0.5 || (r == 0.5 && ((long long)f & 1))) f += 1;
    else if (r < -0.5 || (r == -0.5 && ((long long)f & 1))) f -= 1;
    return f;
}

static HRESULT num_to_i64(const Num *n, long long lo, long long hi, long long *out)
{
    if (n->kind == 2) {
        if (n->d != n->d || n->d < (double)lo - 0.5 || n->d >= (double)hi + 0.5) return DISP_E_OVERFLOW;
        *out = (long long)round_even(n->d);
    } else if (n->kind == 1) {
        if (n->u > (unsigned long long)hi) return DISP_E_OVERFLOW;
        *out = (long long)n->u;
    } else {
        if (n->i < lo || n->i > hi) return DISP_E_OVERFLOW;
        *out = n->i;
    }
    return S_OK;
}

static double num_to_double(const Num *n) { return n->kind == 2 ? n->d : n->kind == 1 ? (double)n->u : (double)n->i; }

static BSTR num_to_bstr(const Num *n, VARTYPE from)
{
    WCHAR buf[64];
    if (n->kind == 0) _snwprintf(buf, 64, L"%lld", n->i);
    else if (n->kind == 1) _snwprintf(buf, 64, L"%llu", n->u);
    else _snwprintf(buf, 64, from == VT_R4 ? L"%.7G" : L"%.15G", n->d);
    return SysAllocString(buf);
}

WINOLEAUTAPI_(HRESULT) VariantChangeTypeEx(VARIANTARG *dst, const VARIANTARG *src, LCID lcid, USHORT flags, VARTYPE vt)
{
    (void)lcid;
    if (!dst || !src) return E_INVALIDARG;
    if (!vt_valid(vt) && vt != VT_EMPTY) return DISP_E_BADVARTYPE;
    VARIANT in;
    VariantInit(&in);
    HRESULT hr;
    if (src->vt & VT_BYREF) { VARIANT t; deref(src, &t); hr = VariantCopy(&in, &t); }
    else hr = VariantCopy(&in, src);
    if (FAILED(hr)) return hr;

    VARIANT out;
    VariantInit(&out);
    out.vt = vt;
    if (in.vt == vt) {
        out = in;
        in.vt = VT_EMPTY;
        goto done;
    }
    if ((in.vt & VT_ARRAY) || (vt & VT_ARRAY)) { hr = DISP_E_TYPEMISMATCH; goto fail; }
    if (in.vt == VT_NULL) { hr = vt == VT_EMPTY ? S_OK : DISP_E_TYPEMISMATCH; if (!hr) out.vt = VT_EMPTY; goto check; }

    /* objects: their default value is not reachable without IDispatch::Invoke */
    if (in.vt == VT_UNKNOWN || in.vt == VT_DISPATCH) {
        if (vt == VT_UNKNOWN || vt == VT_DISPATCH) {
            if (!in.punkVal) { out.punkVal = 0; goto done; }
            hr = in.punkVal->lpVtbl->QueryInterface(in.punkVal, vt == VT_UNKNOWN ? &IID_IUnknown : &IID_IDispatch,
                                                    (void **)&out.punkVal);
            if (FAILED(hr)) hr = DISP_E_TYPEMISMATCH;
            goto check;
        }
        hr = DISP_E_TYPEMISMATCH;
        goto fail;
    }

    switch (vt) {
    case VT_EMPTY: goto done;
    case VT_NULL: goto done;
    case VT_BSTR: {
        if (in.vt == VT_BOOL) {
            out.bstrVal = SysAllocString(flags & VARIANT_ALPHABOOL ? (in.boolVal ? L"True" : L"False") : (in.boolVal ? L"-1" : L"0"));
        } else if (in.vt == VT_DATE) {
            out.bstrVal = date_to_bstr(in.date);
            if (!out.bstrVal) { hr = E_INVALIDARG; goto fail; }
        } else if (in.vt == VT_EMPTY) {
            out.bstrVal = SysAllocString(L"");
        } else {
            Num n;
            if (FAILED(hr = to_num(&in, &n))) goto fail;
            out.bstrVal = num_to_bstr(&n, in.vt);
        }
        if (!out.bstrVal) { hr = E_OUTOFMEMORY; goto fail; }
        goto done;
    }
    case VT_DATE:
        if (in.vt == VT_BSTR) {
            if (FAILED(hr = date_from_str(in.bstrVal ? in.bstrVal : L"", &out.date))) {
                Num n;
                if (FAILED(to_num(&in, &n))) goto fail;
                out.date = num_to_double(&n);
                hr = S_OK;
            }
            goto done;
        }
        /* fall through: dates are doubles */
    case VT_R8: case VT_R4: case VT_CY: case VT_DECIMAL: {
        Num n;
        if (FAILED(hr = to_num(&in, &n))) goto fail;
        double d = num_to_double(&n);
        if (vt == VT_R4) out.fltVal = (float)d;
        else if (vt == VT_CY) {
            if (d >= 922337203685477.5807 || d <= -922337203685477.5808) { hr = DISP_E_OVERFLOW; goto fail; }
            out.cyVal.int64 = (long long)round_even(d * 10000.0);
        } else if (vt == VT_DECIMAL) {
            DECIMAL dec = { 0 };
            double a = d < 0 ? -d : d;
            if (a >= 18446744073709551616.0) { hr = DISP_E_OVERFLOW; goto fail; }
            BYTE scale = 0;
            while (scale < 12 && a != (double)(unsigned long long)a && a * 10 < 1e18) { a *= 10; scale++; }
            dec.Lo64 = (ULONGLONG)(a + 0.5);
            dec.scale = scale;
            dec.sign = d < 0 ? 0x80 : 0;
            out.decVal = dec;
            out.vt = VT_DECIMAL;
        } else out.dblVal = d;
        goto done;
    }
    case VT_BOOL: {
        Num n;
        if (FAILED(hr = to_num(&in, &n))) goto fail;
        out.boolVal = (n.kind == 2 ? n.d != 0 : n.kind == 1 ? n.u != 0 : n.i != 0) ? VARIANT_TRUE : VARIANT_FALSE;
        goto done;
    }
    default: {
        static const struct { VARTYPE vt; long long lo, hi; } lim[] = {
            { VT_I1, -128, 127 }, { VT_UI1, 0, 255 }, { VT_I2, -32768, 32767 }, { VT_UI2, 0, 65535 },
            { VT_I4, -2147483647LL - 1, 2147483647 }, { VT_INT, -2147483647LL - 1, 2147483647 },
            { VT_UI4, 0, 4294967295LL }, { VT_UINT, 0, 4294967295LL }, { VT_ERROR, -2147483647LL - 1, 4294967295LL },
            { VT_I8, -9223372036854775807LL - 1, 9223372036854775807LL }, { VT_INT_PTR, -9223372036854775807LL - 1, 9223372036854775807LL },
        };
        Num n;
        if (FAILED(hr = to_num(&in, &n))) goto fail;
        if (vt == VT_UI8 || vt == VT_UINT_PTR) {
            if ((n.kind == 0 && n.i < 0) || (n.kind == 2 && (n.d < -0.5 || n.d >= 18446744073709551615.0))) { hr = DISP_E_OVERFLOW; goto fail; }
            out.ullVal = n.kind == 1 ? n.u : n.kind == 0 ? (unsigned long long)n.i : (unsigned long long)round_even(n.d);
            goto done;
        }
        size_t i;
        for (i = 0; i < sizeof lim / sizeof *lim; i++) if (lim[i].vt == vt) break;
        if (i == sizeof lim / sizeof *lim) { hr = DISP_E_TYPEMISMATCH; goto fail; }
        long long v;
        if (FAILED(hr = num_to_i64(&n, lim[i].lo, lim[i].hi, &v))) goto fail;
        out.llVal = 0;
        switch (vt_size(vt)) {
        case 1: out.bVal = (BYTE)v; break;
        case 2: out.iVal = (SHORT)v; break;
        case 4: out.lVal = (LONG)v; break;
        default: out.llVal = v;
        }
        goto done;
    }
    }
check:
    if (FAILED(hr)) goto fail;
done:
    VariantClear(&in);
    if (dst == src || !(dst->vt & VT_BYREF)) VariantClear(dst);
    out.wReserved1 = out.wReserved2 = out.wReserved3 = 0;
    *dst = out;
    return S_OK;
fail:
    VariantClear(&in);
    return hr;
}

WINOLEAUTAPI_(HRESULT) VariantChangeType(VARIANTARG *dst, const VARIANTARG *src, USHORT flags, VARTYPE vt)
{
    return VariantChangeTypeEx(dst, src, 0x409, flags, vt);
}

/* ---- the VarXxxFromYyy helpers that programs call directly ---- */
static HRESULT conv(VARIANT *in, VARTYPE vt, VARIANT *out, ULONG flags)
{
    VariantInit(out);
    HRESULT hr = VariantChangeTypeEx(out, in, 0x409, (USHORT)flags, vt);
    return hr;
}

#define VAR_FROM(name, invt, intype, field, outvt, outtype, ofield)                       \
    WINOLEAUTAPI_(HRESULT) name(intype v, outtype *out)                                   \
    {                                                                                     \
        VARIANT a, b; VariantInit(&a); a.vt = invt; a.field = v;                          \
        HRESULT hr = conv(&a, outvt, &b, 0);                                              \
        if (SUCCEEDED(hr)) *out = b.ofield;                                               \
        return hr;                                                                        \
    }
VAR_FROM(VarI4FromR8, VT_R8, double, dblVal, VT_I4, LONG, lVal)
VAR_FROM(VarI4FromI8, VT_I8, LONG64, llVal, VT_I4, LONG, lVal)
VAR_FROM(VarR8FromI4, VT_I4, LONG, lVal, VT_R8, double, dblVal)
VAR_FROM(VarR8FromI8, VT_I8, LONG64, llVal, VT_R8, double, dblVal)
VAR_FROM(VarI8FromR8, VT_R8, double, dblVal, VT_I8, LONG64, llVal)
VAR_FROM(VarUI4FromR8, VT_R8, double, dblVal, VT_UI4, ULONG, ulVal)
VAR_FROM(VarI2FromI4, VT_I4, LONG, lVal, VT_I2, SHORT, iVal)
VAR_FROM(VarUI1FromI4, VT_I4, LONG, lVal, VT_UI1, BYTE, bVal)
VAR_FROM(VarR4FromR8, VT_R8, double, dblVal, VT_R4, float, fltVal)
VAR_FROM(VarCyFromR8, VT_R8, double, dblVal, VT_CY, CY, cyVal)
VAR_FROM(VarR8FromCy, VT_CY, CY, cyVal, VT_R8, double, dblVal)
VAR_FROM(VarBoolFromI4, VT_I4, LONG, lVal, VT_BOOL, VARIANT_BOOL, boolVal)

#define VAR_FROM_STR(name, outvt, outtype, ofield)                                                     \
    WINOLEAUTAPI_(HRESULT) name(const OLECHAR *s, LCID lcid, ULONG flags, outtype *out)                \
    {                                                                                                  \
        (void)lcid;                                                                                    \
        VARIANT a, b; VariantInit(&a); a.vt = VT_BSTR; a.bstrVal = (BSTR)s;                            \
        VariantInit(&b);                                                                               \
        HRESULT hr = s ? VariantChangeTypeEx(&b, &a, lcid, (USHORT)flags, outvt) : E_INVALIDARG;       \
        if (SUCCEEDED(hr)) *out = b.ofield;                                                            \
        return hr;                                                                                     \
    }
/* (the BSTR in @a is borrowed: VariantChangeTypeEx copies its source first) */
VAR_FROM_STR(VarI4FromStr, VT_I4, LONG, lVal)
VAR_FROM_STR(VarUI4FromStr, VT_UI4, ULONG, ulVal)
VAR_FROM_STR(VarI8FromStr, VT_I8, LONG64, llVal)
VAR_FROM_STR(VarI2FromStr, VT_I2, SHORT, iVal)
VAR_FROM_STR(VarR8FromStr, VT_R8, double, dblVal)
VAR_FROM_STR(VarR4FromStr, VT_R4, float, fltVal)
VAR_FROM_STR(VarBoolFromStr, VT_BOOL, VARIANT_BOOL, boolVal)
VAR_FROM_STR(VarDateFromStr, VT_DATE, DATE, date)
VAR_FROM_STR(VarCyFromStr, VT_CY, CY, cyVal)

#define VAR_TO_STR(name, invt, intype, field)                                          \
    WINOLEAUTAPI_(HRESULT) name(intype v, LCID lcid, ULONG flags, BSTR *out)           \
    {                                                                                  \
        (void)lcid;                                                                    \
        if (!out) return E_INVALIDARG;                                                 \
        VARIANT a, b; VariantInit(&a); a.vt = invt; a.field = v;                       \
        HRESULT hr = conv(&a, VT_BSTR, &b, flags);                                     \
        *out = SUCCEEDED(hr) ? b.bstrVal : 0;                                          \
        return hr;                                                                     \
    }
VAR_TO_STR(VarBstrFromI4, VT_I4, LONG, lVal)
VAR_TO_STR(VarBstrFromUI4, VT_UI4, ULONG, ulVal)
VAR_TO_STR(VarBstrFromI8, VT_I8, LONG64, llVal)
VAR_TO_STR(VarBstrFromUI8, VT_UI8, ULONG64, ullVal)
VAR_TO_STR(VarBstrFromI2, VT_I2, SHORT, iVal)
VAR_TO_STR(VarBstrFromR8, VT_R8, double, dblVal)
VAR_TO_STR(VarBstrFromR4, VT_R4, float, fltVal)
VAR_TO_STR(VarBstrFromBool, VT_BOOL, VARIANT_BOOL, boolVal)
VAR_TO_STR(VarBstrFromDate, VT_DATE, DATE, date)
VAR_TO_STR(VarBstrFromCy, VT_CY, CY, cyVal)

WINOLEAUTAPI_(HRESULT) VarBstrCat(BSTR a, BSTR b, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    UINT la = SysStringByteLen(a), lb = SysStringByteLen(b);
    BSTR r = bstr_alloc(la + lb);
    if (!r) return E_OUTOFMEMORY;
    if (la) CopyMemory(r, a, la);
    if (lb) CopyMemory((BYTE *)r + la, b, lb);
    *out = r;
    return S_OK;
}

/* flags: NORM_IGNORECASE (1) */
WINOLEAUTAPI_(HRESULT) VarBstrCmp(BSTR a, BSTR b, LCID lcid, ULONG flags)
{
    (void)lcid;
    UINT la = SysStringLen(a), lb = SysStringLen(b);
    if (!la && !lb) return VARCMP_EQ;                        /* NULL and "" are equal */
    int r = CompareStringW(0x409, flags & 0xFFFF, a ? a : L"", (int)la, b ? b : L"", (int)lb);
    return r == CSTR_LESS_THAN ? VARCMP_LT : r == CSTR_EQUAL ? VARCMP_EQ : r == CSTR_GREATER_THAN ? VARCMP_GT : E_INVALIDARG;
}

WINOLEAUTAPI_(HRESULT) VarFormat(VARIANT *v, LPOLESTR fmt, int fdow, int fwoy, ULONG flags, BSTR *out)
{
    (void)fmt; (void)fdow; (void)fwoy;
    VARIANT b;
    VariantInit(&b);
    HRESULT hr = VariantChangeTypeEx(&b, v, 0x409, (USHORT)(flags | VARIANT_ALPHABOOL), VT_BSTR);
    *out = SUCCEEDED(hr) ? b.bstrVal : 0;
    return hr;
}

/* ---------------------------------------------------------------------------
 * Error info: one IErrorInfo per thread
 * ------------------------------------------------------------------------- */
static __declspec(thread) IErrorInfo *t_error;

typedef struct ErrorInfo {
    IErrorInfo err;
    ICreateErrorInfo create;
    LONG refs;
    GUID guid;
    BSTR source, desc, helpfile;
    DWORD helpctx;
} ErrorInfo;

#define EI_FROM_ERR(p)    ((ErrorInfo *)(p))
#define EI_FROM_CREATE(p) ((ErrorInfo *)((BYTE *)(p) - offsetof(ErrorInfo, create)))

static HRESULT ei_qi(ErrorInfo *e, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IErrorInfo)) *ppv = &e->err;
    else if (IsEqualIID(riid, &IID_ICreateErrorInfo)) *ppv = &e->create;
    else { *ppv = 0; return E_NOINTERFACE; }
    InterlockedIncrement(&e->refs);
    return S_OK;
}
static ULONG ei_release(ErrorInfo *e)
{
    LONG r = InterlockedDecrement(&e->refs);
    if (!r) {
        SysFreeString(e->source);
        SysFreeString(e->desc);
        SysFreeString(e->helpfile);
        HeapFree(GetProcessHeap(), 0, e);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE err_qi(IErrorInfo *This, REFIID riid, void **ppv) { return ei_qi(EI_FROM_ERR(This), riid, ppv); }
static ULONG STDMETHODCALLTYPE err_addref(IErrorInfo *This) { return (ULONG)InterlockedIncrement(&EI_FROM_ERR(This)->refs); }
static ULONG STDMETHODCALLTYPE err_release(IErrorInfo *This) { return ei_release(EI_FROM_ERR(This)); }
static HRESULT copy_bstr(BSTR s, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    *out = s ? SysAllocStringLen(s, SysStringLen(s)) : 0;
    return s && !*out ? E_OUTOFMEMORY : S_OK;
}
static HRESULT STDMETHODCALLTYPE err_guid(IErrorInfo *This, GUID *g) { if (!g) return E_INVALIDARG; *g = EI_FROM_ERR(This)->guid; return S_OK; }
static HRESULT STDMETHODCALLTYPE err_source(IErrorInfo *This, BSTR *s) { return copy_bstr(EI_FROM_ERR(This)->source, s); }
static HRESULT STDMETHODCALLTYPE err_desc(IErrorInfo *This, BSTR *s) { return copy_bstr(EI_FROM_ERR(This)->desc, s); }
static HRESULT STDMETHODCALLTYPE err_helpfile(IErrorInfo *This, BSTR *s) { return copy_bstr(EI_FROM_ERR(This)->helpfile, s); }
static HRESULT STDMETHODCALLTYPE err_helpctx(IErrorInfo *This, DWORD *c) { if (!c) return E_INVALIDARG; *c = EI_FROM_ERR(This)->helpctx; return S_OK; }

static const IErrorInfoVtbl g_err_vtbl = { err_qi, err_addref, err_release, err_guid, err_source, err_desc, err_helpfile, err_helpctx };

static HRESULT STDMETHODCALLTYPE cei_qi(ICreateErrorInfo *This, REFIID riid, void **ppv) { return ei_qi(EI_FROM_CREATE(This), riid, ppv); }
static ULONG STDMETHODCALLTYPE cei_addref(ICreateErrorInfo *This) { return (ULONG)InterlockedIncrement(&EI_FROM_CREATE(This)->refs); }
static ULONG STDMETHODCALLTYPE cei_release(ICreateErrorInfo *This) { return ei_release(EI_FROM_CREATE(This)); }
static HRESULT set_bstr(BSTR *field, LPOLESTR s)
{
    BSTR n = s ? SysAllocString(s) : 0;
    if (s && !n) return E_OUTOFMEMORY;
    SysFreeString(*field);
    *field = n;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cei_guid(ICreateErrorInfo *This, REFGUID g) { EI_FROM_CREATE(This)->guid = *g; return S_OK; }
static HRESULT STDMETHODCALLTYPE cei_source(ICreateErrorInfo *This, LPOLESTR s) { return set_bstr(&EI_FROM_CREATE(This)->source, s); }
static HRESULT STDMETHODCALLTYPE cei_desc(ICreateErrorInfo *This, LPOLESTR s) { return set_bstr(&EI_FROM_CREATE(This)->desc, s); }
static HRESULT STDMETHODCALLTYPE cei_helpfile(ICreateErrorInfo *This, LPOLESTR s) { return set_bstr(&EI_FROM_CREATE(This)->helpfile, s); }
static HRESULT STDMETHODCALLTYPE cei_helpctx(ICreateErrorInfo *This, DWORD c) { EI_FROM_CREATE(This)->helpctx = c; return S_OK; }

static const ICreateErrorInfoVtbl g_cei_vtbl = { cei_qi, cei_addref, cei_release, cei_guid, cei_source, cei_desc, cei_helpfile, cei_helpctx };

WINOLEAUTAPI_(HRESULT) CreateErrorInfo(ICreateErrorInfo **out)
{
    if (!out) return E_INVALIDARG;
    ErrorInfo *e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *e);
    if (!e) return E_OUTOFMEMORY;
    e->err.lpVtbl = &g_err_vtbl;
    e->create.lpVtbl = &g_cei_vtbl;
    e->refs = 1;
    *out = &e->create;
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) SetErrorInfo(ULONG reserved, IErrorInfo *e)
{
    (void)reserved;
    if (e) e->lpVtbl->AddRef(e);
    IErrorInfo *old = t_error;
    t_error = e;
    if (old) old->lpVtbl->Release(old);
    return S_OK;
}

/* hands the thread's error object to the caller and clears it */
WINOLEAUTAPI_(HRESULT) GetErrorInfo(ULONG reserved, IErrorInfo **out)
{
    (void)reserved;
    if (!out) return E_INVALIDARG;
    *out = t_error;
    t_error = 0;
    return *out ? S_OK : S_FALSE;
}

/* DispGetParam: argument @pos (0 = first), converted to @vt */
WINOLEAUTAPI_(HRESULT) DispGetParam(DISPPARAMS *p, UINT pos, VARTYPE vt, VARIANT *out, UINT *argerr)
{
    if (!p || !out) return E_INVALIDARG;
    if (pos >= p->cArgs) return DISP_E_PARAMNOTFOUND;
    UINT i = p->cArgs - 1 - pos;                  /* arguments are stored last to first */
    VariantInit(out);
    HRESULT hr = VariantChangeType(out, &p->rgvarg[i], 0, vt);
    if (FAILED(hr) && argerr) *argerr = i;
    return hr;
}

WINOLEAUTAPI_(ULONG) OaBuildVersion(void) { return 0x000A0000 | 22000; }
WINOLEAUTAPI_(void) OaEnablePerUserTLibRegistration(void) { }

WINOLEAUTAPI_(HRESULT) GetActiveObject(REFCLSID clsid, PVOID reserved, IUnknown **out)
{
    (void)clsid; (void)reserved;
    if (out) *out = 0;
    return (HRESULT)0x800401E3L;                  /* MK_E_UNAVAILABLE */
}
WINOLEAUTAPI_(HRESULT) RegisterActiveObject(IUnknown *p, REFCLSID clsid, DWORD flags, DWORD *reg)
{
    (void)p; (void)clsid; (void)flags;
    if (reg) *reg = 0;
    return E_NOTIMPL;
}
WINOLEAUTAPI_(HRESULT) RevokeActiveObject(DWORD reg, PVOID reserved) { (void)reg; (void)reserved; return E_NOTIMPL; }

/* ---- variant arithmetic and comparison ---------------------------------
 * VarAdd, VarSub, VarMul, VarDiv, VarIdiv, VarMod, VarAnd, VarOr, VarXor,
 * VarNeg, VarNot, VarAbs, VarFix, VarInt, VarCat and VarCmp, as Visual
 * Basic's and Delphi's variant operators use them (Delphi's runtime looks
 * them up by name; Inno Setup's installers do).  Integers are worked in
 * 64 bits; the result has the wider operand's type (Empty and Boolean count
 * as Integer, I2), growing I2 -> I4 -> R8 when it does not fit, as Windows
 * does; reals give R8 (R4 when both sides are R4 or narrow integers);
 * Currency stays Currency; a Date plus or minus a number stays a Date.
 * Null in gives Null out.  Decimal is worked as a double. */

static int int_rank(VARTYPE vt)                      /* 0: not an integer type */
{
    switch (vt) {
    case VT_EMPTY: case VT_BOOL: case VT_I1: case VT_I2: return 2;
    case VT_UI1: return 1;
    case VT_UI2: case VT_I4: case VT_INT: case VT_ERROR: return 4;
    case VT_UI4: case VT_UINT: case VT_I8: case VT_UI8: case VT_INT_PTR: case VT_UINT_PTR: return 8;
    default: return 0;
    }
}

static VARTYPE rank_vt(int r) { return r == 1 ? VT_UI1 : r == 2 ? VT_I2 : r == 4 ? VT_I4 : VT_I8; }

/* @v (deref'd) as a Num; Empty is 0, strings their number */
static HRESULT arg_num(const VARIANT *v, Num *n) { return to_num(v, n); }

static HRESULT put_int(VARIANT *out, long long v, int rank)
{
    VARIANT t;
    VariantInit(&t);
    t.vt = VT_I8;
    t.llVal = v;
    for (; rank <= 8; rank = rank == 1 ? 2 : rank * 2) {
        VARIANT r;
        VariantInit(&r);
        if (SUCCEEDED(VariantChangeType(&r, &t, 0, rank_vt(rank)))) { *out = r; return S_OK; }
        if (rank == 4) {                                 /* past I4: R8, as Windows widens */
            out->vt = VT_R8;
            out->dblVal = (double)v;
            return S_OK;
        }
    }
    return DISP_E_OVERFLOW;
}

static HRESULT put_real(VARIANT *out, double d, VARTYPE vt)
{
    VARIANT t;
    VariantInit(&t);
    t.vt = VT_R8;
    t.dblVal = d;
    if (vt == VT_R8 || vt == VT_DATE) { out->vt = vt; out->dblVal = d; return S_OK; }
    VARIANT r;
    VariantInit(&r);
    HRESULT hr = VariantChangeType(&r, &t, 0, vt);
    if (FAILED(hr)) return hr;
    *out = r;
    return S_OK;
}

/* The operands by value; S_FALSE when either is Null (@out set to Null) */
static HRESULT prep2(const VARIANT *l, const VARIANT *r, VARIANT *a, VARIANT *b, VARIANT *out)
{
    if (!l || !r || !out) return E_INVALIDARG;
    if (l->vt & VT_BYREF) deref(l, a); else *a = *l;
    if (r->vt & VT_BYREF) deref(r, b); else *b = *r;
    if ((a->vt & VT_ARRAY) || (b->vt & VT_ARRAY) || a->vt == VT_DISPATCH || b->vt == VT_DISPATCH ||
        a->vt == VT_UNKNOWN || b->vt == VT_UNKNOWN)
        return DISP_E_TYPEMISMATCH;
    if (a->vt == VT_NULL || b->vt == VT_NULL) {
        VariantInit(out);
        out->vt = VT_NULL;
        return S_FALSE;
    }
    return S_OK;
}

static int is_real(VARTYPE vt) { return vt == VT_R4 || vt == VT_R8 || vt == VT_DATE || vt == VT_CY || vt == VT_DECIMAL || vt == VT_BSTR; }

/* The result type of + - * on @a, @b (0: integers, see int_rank) */
static VARTYPE real_type(VARTYPE a, VARTYPE b)
{
    if (!is_real(a) && !is_real(b)) return 0;
    if (a == VT_R8 || b == VT_R8 || a == VT_BSTR || b == VT_BSTR || a == VT_DECIMAL || b == VT_DECIMAL) return VT_R8;
    if (a == VT_DATE || b == VT_DATE) return VT_DATE;
    if (a == VT_CY || b == VT_CY) return VT_CY;
    /* R4 with R4 or a narrow integer stays R4; with I4 or wider, R8 */
    VARTYPE o = a == VT_R4 ? b : a;
    return o == VT_R4 || (int_rank(o) && int_rank(o) <= 2) ? VT_R4 : VT_R8;
}

enum { OP_ADD, OP_SUB, OP_MUL };

static HRESULT arith(const VARIANT *l, const VARIANT *r, VARIANT *out, int op)
{
    VARIANT a, b;
    HRESULT hr = prep2(l, r, &a, &b, out);
    if (hr != S_OK) return hr == S_FALSE ? S_OK : hr;
    if (op == OP_ADD && a.vt == VT_BSTR && b.vt == VT_BSTR) {
        BSTR s;
        hr = VarBstrCat(a.bstrVal, b.bstrVal, &s);
        if (FAILED(hr)) return hr;
        VariantInit(out);
        out->vt = VT_BSTR;
        out->bstrVal = s;
        return S_OK;
    }
    Num x, y;
    if (FAILED(hr = arg_num(&a, &x)) || FAILED(hr = arg_num(&b, &y))) return hr;
    VARTYPE rt = real_type(a.vt, b.vt);
    if (rt == VT_DATE && op == OP_SUB && a.vt == VT_DATE && b.vt == VT_DATE) rt = VT_R8;   /* date - date: days */
    if (rt == VT_DATE && op == OP_MUL) rt = VT_R8;
    VariantInit(out);
    if (rt) {
        double p = num_to_double(&x), q = num_to_double(&y);
        return put_real(out, op == OP_ADD ? p + q : op == OP_SUB ? p - q : p * q, rt);
    }
    if (x.kind == 1 || y.kind == 1) {                    /* a UI8 operand: as doubles, back to I8 */
        double p = num_to_double(&x), q = num_to_double(&y), d = op == OP_ADD ? p + q : op == OP_SUB ? p - q : p * q;
        if (d < -9.2233720368547758e18 || d >= 9.2233720368547758e18) return DISP_E_OVERFLOW;
        out->vt = VT_I8;
        out->llVal = (long long)d;
        return S_OK;
    }
    long long v;
    int ovf = op == OP_ADD ? __builtin_add_overflow(x.i, y.i, &v) : op == OP_SUB ? __builtin_sub_overflow(x.i, y.i, &v)
                                                                               : __builtin_mul_overflow(x.i, y.i, &v);
    int rank = int_rank(a.vt) > int_rank(b.vt) ? int_rank(a.vt) : int_rank(b.vt);
    if (rank == 1 && (int_rank(a.vt) != 1 || int_rank(b.vt) != 1)) rank = 2;
    if (ovf) {
        if (rank == 8) return DISP_E_OVERFLOW;
        double p = (double)x.i, q = (double)y.i;
        out->vt = VT_R8;
        out->dblVal = op == OP_ADD ? p + q : op == OP_SUB ? p - q : p * q;
        return S_OK;
    }
    return put_int(out, v, rank);
}

WINOLEAUTAPI_(HRESULT) VarAdd(LPVARIANT l, LPVARIANT r, LPVARIANT out) { return arith(l, r, out, OP_ADD); }
WINOLEAUTAPI_(HRESULT) VarSub(LPVARIANT l, LPVARIANT r, LPVARIANT out) { return arith(l, r, out, OP_SUB); }
WINOLEAUTAPI_(HRESULT) VarMul(LPVARIANT l, LPVARIANT r, LPVARIANT out) { return arith(l, r, out, OP_MUL); }

WINOLEAUTAPI_(HRESULT) VarDiv(LPVARIANT l, LPVARIANT r, LPVARIANT out)
{
    VARIANT a, b;
    HRESULT hr = prep2(l, r, &a, &b, out);
    if (hr != S_OK) return hr == S_FALSE ? S_OK : hr;
    Num x, y;
    if (FAILED(hr = arg_num(&a, &x)) || FAILED(hr = arg_num(&b, &y))) return hr;
    double q = num_to_double(&y);
    if (q == 0) return DISP_E_DIVBYZERO;
    VARTYPE rt = a.vt == VT_CY && !is_real(b.vt) ? VT_CY : real_type(a.vt, b.vt) == VT_R4 ? VT_R4 : VT_R8;
    VariantInit(out);
    return put_real(out, num_to_double(&x) / q, rt);
}

/* Integer operands: reals rounded half to even, the result as wide as the
 * wider operand (at least I2; I4 for reals and strings) */
static HRESULT int_args(const VARIANT *a, const VARIANT *b, long long *x, long long *y, int *rank)
{
    Num n;
    HRESULT hr;
    int ra = int_rank(a->vt) ? int_rank(a->vt) : 4, rb = b ? (int_rank(b->vt) ? int_rank(b->vt) : 4) : ra;
    *rank = ra > rb ? ra : rb;
    if (*rank == 1 && (ra != 1 || rb != 1)) *rank = 2;
    if (FAILED(hr = arg_num(a, &n)) || FAILED(hr = num_to_i64(&n, -0x7FFFFFFFFFFFFFFFLL - 1, 0x7FFFFFFFFFFFFFFFLL, x))) return hr;
    if (b && (FAILED(hr = arg_num(b, &n)) || FAILED(hr = num_to_i64(&n, -0x7FFFFFFFFFFFFFFFLL - 1, 0x7FFFFFFFFFFFFFFFLL, y)))) return hr;
    return S_OK;
}

static HRESULT idiv_mod(const VARIANT *l, const VARIANT *r, VARIANT *out, int mod)
{
    VARIANT a, b;
    HRESULT hr = prep2(l, r, &a, &b, out);
    if (hr != S_OK) return hr == S_FALSE ? S_OK : hr;
    long long x, y;
    int rank;
    if (FAILED(hr = int_args(&a, &b, &x, &y, &rank))) return hr;
    if (!y) return DISP_E_DIVBYZERO;
    if (x == -0x7FFFFFFFFFFFFFFFLL - 1 && y == -1) return mod ? (VariantInit(out), put_int(out, 0, rank)) : DISP_E_OVERFLOW;
    VariantInit(out);
    return put_int(out, mod ? x % y : x / y, rank);
}

WINOLEAUTAPI_(HRESULT) VarIdiv(LPVARIANT l, LPVARIANT r, LPVARIANT out) { return idiv_mod(l, r, out, 0); }
WINOLEAUTAPI_(HRESULT) VarMod(LPVARIANT l, LPVARIANT r, LPVARIANT out) { return idiv_mod(l, r, out, 1); }

enum { OP_AND, OP_OR, OP_XOR };

static HRESULT bitwise(const VARIANT *l, const VARIANT *r, VARIANT *out, int op)
{
    if (!l || !r || !out) return E_INVALIDARG;
    VARIANT a, b;
    if (l->vt & VT_BYREF) deref(l, &a); else a = *l;
    if (r->vt & VT_BYREF) deref(r, &b); else b = *r;
    /* Null with False is False (And), Null with True is True (Or); else Null */
    if (a.vt == VT_NULL || b.vt == VT_NULL) {
        const VARIANT *o = a.vt == VT_NULL ? &b : &a;
        Num n;
        long long v = 0;
        int known = o->vt != VT_NULL && SUCCEEDED(arg_num(o, &n)) &&
                    SUCCEEDED(num_to_i64(&n, -0x7FFFFFFFFFFFFFFFLL - 1, 0x7FFFFFFFFFFFFFFFLL, &v));
        VariantInit(out);
        if (known && ((op == OP_AND && v == 0) || (op == OP_OR && v == -1))) {
            if (o->vt == VT_BOOL) { out->vt = VT_BOOL; out->boolVal = o->boolVal; }
            else put_int(out, v, int_rank(o->vt) ? int_rank(o->vt) : 4);
        } else out->vt = VT_NULL;
        return S_OK;
    }
    long long x, y;
    int rank;
    HRESULT hr = int_args(&a, &b, &x, &y, &rank);
    if (FAILED(hr)) return hr;
    long long v = op == OP_AND ? x & y : op == OP_OR ? x | y : x ^ y;
    VariantInit(out);
    if (a.vt == VT_BOOL && b.vt == VT_BOOL) {
        out->vt = VT_BOOL;
        out->boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }
    return put_int(out, v, rank);
}

WINOLEAUTAPI_(HRESULT) VarAnd(LPVARIANT l, LPVARIANT r, LPVARIANT out) { return bitwise(l, r, out, OP_AND); }
WINOLEAUTAPI_(HRESULT) VarOr(LPVARIANT l, LPVARIANT r, LPVARIANT out) { return bitwise(l, r, out, OP_OR); }
WINOLEAUTAPI_(HRESULT) VarXor(LPVARIANT l, LPVARIANT r, LPVARIANT out) { return bitwise(l, r, out, OP_XOR); }

/* The operand by value; S_FALSE when it is Null (@out set to Null) */
static HRESULT prep1(const VARIANT *in, VARIANT *a, VARIANT *out)
{
    if (!in || !out) return E_INVALIDARG;
    if (in->vt & VT_BYREF) deref(in, a); else *a = *in;
    if (a->vt & VT_ARRAY) return DISP_E_TYPEMISMATCH;
    if (a->vt == VT_NULL) { VariantInit(out); out->vt = VT_NULL; return S_FALSE; }
    return S_OK;
}

WINOLEAUTAPI_(HRESULT) VarNeg(LPVARIANT in, LPVARIANT out)
{
    VARIANT a;
    HRESULT hr = prep1(in, &a, out);
    if (hr != S_OK) return hr == S_FALSE ? S_OK : hr;
    Num n;
    if (FAILED(hr = arg_num(&a, &n))) return hr;
    VariantInit(out);
    if (is_real(a.vt)) return put_real(out, -num_to_double(&n), a.vt == VT_BSTR || a.vt == VT_DECIMAL ? VT_R8 : a.vt);
    if (n.kind == 1) { if (n.u > 0x8000000000000000ull) return DISP_E_OVERFLOW; out->vt = VT_I8; out->llVal = -(long long)n.u; return S_OK; }
    int rank = int_rank(a.vt) < 2 ? 2 : int_rank(a.vt);
    if (n.i == -0x7FFFFFFFFFFFFFFFLL - 1) return DISP_E_OVERFLOW;
    return put_int(out, -n.i, rank);
}

WINOLEAUTAPI_(HRESULT) VarNot(LPVARIANT in, LPVARIANT out)
{
    VARIANT a;
    HRESULT hr = prep1(in, &a, out);
    if (hr != S_OK) return hr == S_FALSE ? S_OK : hr;
    long long x;
    int rank;
    if (FAILED(hr = int_args(&a, NULL, &x, NULL, &rank))) return hr;
    VariantInit(out);
    if (a.vt == VT_BOOL) { out->vt = VT_BOOL; out->boolVal = a.boolVal ? VARIANT_FALSE : VARIANT_TRUE; return S_OK; }
    if (a.vt == VT_UI1) { out->vt = VT_UI1; out->bVal = (BYTE)~a.bVal; return S_OK; }
    return put_int(out, ~x, rank < 2 ? 2 : rank);
}

/* Abs, Fix (toward zero) and Int (down) */
static HRESULT unary_real(const VARIANT *in, VARIANT *out, int how)
{
    VARIANT a;
    HRESULT hr = prep1(in, &a, out);
    if (hr != S_OK) return hr == S_FALSE ? S_OK : hr;
    Num n;
    if (FAILED(hr = arg_num(&a, &n))) return hr;
    VariantInit(out);
    if (!is_real(a.vt)) {
        if (a.vt == VT_BOOL && how) { *out = a; return S_OK; }
        if (how) return VariantCopy(out, &a);
        if (n.kind == 1) return VariantCopy(out, &a);
        if (n.i == -0x7FFFFFFFFFFFFFFFLL - 1) return DISP_E_OVERFLOW;
        return put_int(out, n.i < 0 ? -n.i : n.i, int_rank(a.vt) < 2 ? 2 : int_rank(a.vt));
    }
    double d = num_to_double(&n);
    if (how == 0) d = d < 0 ? -d : d;
    else {
        double t = (double)(long long)d;              /* toward zero */
        if (how == 2 && t > d) t -= 1;
        d = (d > 9.2e18 || d < -9.2e18) ? d : t;
    }
    return put_real(out, d, a.vt == VT_BSTR || a.vt == VT_DECIMAL ? VT_R8 : a.vt);
}

WINOLEAUTAPI_(HRESULT) VarAbs(LPVARIANT in, LPVARIANT out) { return unary_real(in, out, 0); }
WINOLEAUTAPI_(HRESULT) VarFix(LPVARIANT in, LPVARIANT out) { return unary_real(in, out, 1); }
WINOLEAUTAPI_(HRESULT) VarInt(LPVARIANT in, LPVARIANT out) { return unary_real(in, out, 2); }

/* Both as strings, joined (Null joins as an empty string; Null & Null is Null) */
WINOLEAUTAPI_(HRESULT) VarCat(LPVARIANT l, LPVARIANT r, LPVARIANT out)
{
    if (!l || !r || !out) return E_INVALIDARG;
    VARIANT a, b, sa, sb;
    if (l->vt & VT_BYREF) deref(l, &a); else a = *l;
    if (r->vt & VT_BYREF) deref(r, &b); else b = *r;
    if (a.vt == VT_NULL && b.vt == VT_NULL) { VariantInit(out); out->vt = VT_NULL; return S_OK; }
    VariantInit(&sa);
    VariantInit(&sb);
    HRESULT hr = S_OK;
    if (a.vt != VT_NULL && a.vt != VT_EMPTY) hr = VariantChangeType(&sa, &a, 0, VT_BSTR);
    if (SUCCEEDED(hr) && b.vt != VT_NULL && b.vt != VT_EMPTY) hr = VariantChangeType(&sb, &b, 0, VT_BSTR);
    if (SUCCEEDED(hr)) {
        BSTR s;
        hr = VarBstrCat(sa.vt == VT_BSTR ? sa.bstrVal : NULL, sb.vt == VT_BSTR ? sb.bstrVal : NULL, &s);
        if (SUCCEEDED(hr)) { VariantInit(out); out->vt = VT_BSTR; out->bstrVal = s; }
    }
    VariantClear(&sa);
    VariantClear(&sb);
    return hr;
}

/* VARCMP_LT, _EQ, _GT, or _NULL; strings with each other by
 * CompareString (@flags: NORM_IGNORECASE ...), a string is greater than a
 * number, Empty is "" against a string and 0 against a number */
WINOLEAUTAPI_(HRESULT) VarCmp(LPVARIANT l, LPVARIANT r, LCID lcid, ULONG flags)
{
    if (!l || !r) return E_INVALIDARG;
    VARIANT a, b;
    if (l->vt & VT_BYREF) deref(l, &a); else a = *l;
    if (r->vt & VT_BYREF) deref(r, &b); else b = *r;
    if (a.vt == VT_NULL || b.vt == VT_NULL) return VARCMP_NULL;
    if ((a.vt & VT_ARRAY) || (b.vt & VT_ARRAY)) return DISP_E_TYPEMISMATCH;
    int sa = a.vt == VT_BSTR, sb = b.vt == VT_BSTR;
    if (sa || sb) {
        if (sa && sb) return VarBstrCmp(a.bstrVal, b.bstrVal, lcid, flags);
        if (sa && b.vt == VT_EMPTY) return VarBstrCmp(a.bstrVal, NULL, lcid, flags);
        if (sb && a.vt == VT_EMPTY) return VarBstrCmp(NULL, b.bstrVal, lcid, flags);
        return sa ? VARCMP_GT : VARCMP_LT;
    }
    Num x, y;
    HRESULT hr;
    if (FAILED(hr = arg_num(&a, &x)) || FAILED(hr = arg_num(&b, &y))) return hr;
    if (x.kind == 2 || y.kind == 2 || x.kind != y.kind) {
        double p = num_to_double(&x), q = num_to_double(&y);
        return p < q ? VARCMP_LT : p > q ? VARCMP_GT : VARCMP_EQ;
    }
    if (x.kind == 1) return x.u < y.u ? VARCMP_LT : x.u > y.u ? VARCMP_GT : VARCMP_EQ;
    return x.i < y.i ? VARCMP_LT : x.i > y.i ? VARCMP_GT : VARCMP_EQ;
}
