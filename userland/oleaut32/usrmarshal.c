/*
 * usrmarshal.c — BSTRs, VARIANTs and SAFEARRAYs between processes: the
 * user-marshal routines MIDL-made proxy/stub DLLs name for them
 * (BSTR_UserSize/Marshal/Unmarshal/Free and the VARIANT_ and LPSAFEARRAY_
 * ones, which rpcrt4's NDR engine calls), and the wire form of a VARIANT
 * the IDispatch proxy (psdispatch.c) uses too.
 *
 * Both ends are NovaOS's, so the wire form is NovaOS's own, laid out like
 * Windows' (a length-prefixed BSTR; a VARIANT as its type then its value;
 * a SAFEARRAY as its bounds then its elements; interface pointers as an
 * OBJREF from CoMarshalInterface for the destination the NDR engine
 * names) and the same for 32- and 64-bit callers.  A value that cannot
 * be marshaled makes the routine return NULL, which fails the call with
 * RPC_X_BAD_STUB_DATA.  Fields are aligned
 * from where each value starts, which the engine aligns to 8 for VARIANTs
 * and SAFEARRAYs and to 4 for BSTRs.  VT_RECORD (user-defined types) is not
 * marshaled.
 */

#define NOVA_BUILD_OLEAUT32
#include <oleauto.h>
#include "usrmarshal.h"

/* ---- a cursor that sizes (buf NULL), writes or reads ------------------- */
static ULONG_PTR wb_pos(const WireBuf *w) { return w->buf ? (ULONG_PTR)(w->buf - w->start) : w->size; }
void wb_align(WireBuf *w, unsigned n)
{
    ULONG_PTR pos = wb_pos(w), to = (pos + n - 1) & ~(ULONG_PTR)(n - 1);
    if (!w->buf) { w->size = (ULONG)to; return; }
    while (pos < to) { *w->buf++ = 0; pos++; }
}
void wb_put(WireBuf *w, const void *p, ULONG n)
{
    if (!w->buf) { w->size += n; return; }
    CopyMemory(w->buf, p, n);
    w->buf += n;
}
void wb_u32(WireBuf *w, ULONG v) { wb_align(w, 4); wb_put(w, &v, 4); }

void rb_align(WireBuf *r, unsigned n)
{
    ULONG_PTR pos = (ULONG_PTR)(r->buf - r->start), to = (pos + n - 1) & ~(ULONG_PTR)(n - 1);
    r->buf = r->start + to;
}
int rb_get(WireBuf *r, void *p, ULONG n)
{
    if (r->fail || (r->end && (ULONG_PTR)(r->end - r->buf) < n)) { r->fail = 1; return 0; }
    CopyMemory(p, r->buf, n);
    r->buf += n;
    return 1;
}
ULONG rb_u32(WireBuf *r) { ULONG v = 0; rb_align(r, 4); rb_get(r, &v, 4); return v; }

/* ---- BSTR: present flag, byte length, the bytes ------------------------- */
void wire_bstr(WireBuf *w, BSTR b)
{
    wb_u32(w, b ? 1 : 0);
    if (!b) return;
    ULONG n = SysStringByteLen(b);
    wb_u32(w, n);
    wb_put(w, b, n);
}

BSTR wire_get_bstr(WireBuf *r)
{
    if (!rb_u32(r)) return 0;
    ULONG n = rb_u32(r);
    if (r->fail || (r->end && (ULONG_PTR)(r->end - r->buf) < n)) { r->fail = 1; return 0; }
    BSTR b = SysAllocStringByteLen((LPCSTR)r->buf, n);
    r->buf += n;
    if (!b) r->fail = 1;
    return b;
}

/* ---- interface pointers: present flag, OBJREF length, the OBJREF -------- */
void wire_iface(WireBuf *w, IUnknown *p, REFIID iid)
{
    wb_u32(w, p ? 1 : 0);
    if (!p) return;
    if (!w->buf) {
        ULONG n = 0;
        if (FAILED(CoGetMarshalSizeMax(&n, iid, p, w->ctx, 0, MSHLFLAGS_NORMAL))) n = 1024;
        wb_u32(w, n);
        w->size += n;
        return;
    }
    IStream *s = 0;
    HRESULT hr = CreateStreamOnHGlobal(0, TRUE, &s);
    if (SUCCEEDED(hr)) hr = CoMarshalInterface(s, iid, p, w->ctx, 0, MSHLFLAGS_NORMAL);
    STATSTG st;
    if (SUCCEEDED(hr)) hr = s->lpVtbl->Stat(s, &st, 1);
    if (FAILED(hr)) { w->fail = 1; wb_u32(w, 0); if (s) s->lpVtbl->Release(s); return; }
    ULONG n = (ULONG)st.cbSize.QuadPart;
    HGLOBAL g = 0;
    GetHGlobalFromStream(s, &g);
    wb_u32(w, n);
    wb_put(w, GlobalLock(g), n);
    GlobalUnlock(g);
    s->lpVtbl->Release(s);
}

IUnknown *wire_get_iface(WireBuf *r, REFIID iid)
{
    if (!rb_u32(r)) return 0;
    ULONG n = rb_u32(r);
    if (r->fail || (r->end && (ULONG_PTR)(r->end - r->buf) < n)) { r->fail = 1; return 0; }
    IStream *s = 0;
    IUnknown *p = 0;
    HRESULT hr = CreateStreamOnHGlobal(0, TRUE, &s);
    if (SUCCEEDED(hr)) {
        LARGE_INTEGER zero = { 0 };
        s->lpVtbl->Write(s, r->buf, n, 0);
        s->lpVtbl->Seek(s, zero, STREAM_SEEK_SET, 0);
        hr = CoUnmarshalInterface(s, iid, (void **)&p);
        s->lpVtbl->Release(s);
    }
    r->buf += n;
    if (FAILED(hr)) { r->fail = 1; r->hr = hr; return 0; }
    return p;
}

/* ---- SAFEARRAY: present flag, dims, features, element size, element
 * type, bounds, then the elements ------------------------------------------ */
static ULONG sa_elements(const SAFEARRAY *a)
{
    ULONG n = 1;
    for (USHORT i = 0; i < a->cDims; i++) n *= a->rgsabound[i].cElements;
    return a->cDims ? n : 0;
}

static VARTYPE sa_vt(SAFEARRAY *a)
{
    VARTYPE vt = VT_EMPTY;
    if (FAILED(SafeArrayGetVartype(a, &vt))) {
        if (a->cbElements == 1) vt = VT_UI1;
        else if (a->cbElements == 2) vt = VT_I2;
        else if (a->cbElements == 4) vt = VT_I4;
        else vt = VT_R8;
    }
    return vt;
}

static void wire_value(WireBuf *w, VARTYPE vt, const void *p);
static void get_value(WireBuf *r, VARTYPE vt, void *p);

void wire_safearray(WireBuf *w, SAFEARRAY *a)
{
    wb_u32(w, a ? 1 : 0);
    if (!a) return;
    VARTYPE vt = sa_vt(a);
    if (vt == VT_RECORD) { w->fail = 1; return; }
    wb_u32(w, a->cDims);
    wb_u32(w, a->fFeatures & ~(FADF_AUTO | FADF_STATIC | FADF_EMBEDDED | FADF_FIXEDSIZE));
    wb_u32(w, a->cbElements);
    wb_u32(w, vt);
    for (USHORT i = 0; i < a->cDims; i++) {
        wb_u32(w, a->rgsabound[i].cElements);
        wb_u32(w, (ULONG)a->rgsabound[i].lLbound);
    }
    ULONG n = sa_elements(a);
    BYTE *data = a->pvData;
    if (vt == VT_BSTR || vt == VT_UNKNOWN || vt == VT_DISPATCH || vt == VT_VARIANT) {
        for (ULONG i = 0; i < n; i++) wire_value(w, vt, data + (SIZE_T)i * a->cbElements);
    } else {
        wb_align(w, 8);
        wb_put(w, data, n * a->cbElements);
    }
}

SAFEARRAY *wire_get_safearray(WireBuf *r)
{
    if (!rb_u32(r)) return 0;
    ULONG dims = rb_u32(r), features = rb_u32(r), elsize = rb_u32(r), vt = rb_u32(r);
    if (r->fail || !dims || dims > 64 || !elsize || elsize > 64) { r->fail = 1; return 0; }
    SAFEARRAYBOUND b[64];
    for (ULONG i = 0; i < dims; i++) {
        b[i].cElements = rb_u32(r);
        b[i].lLbound = (LONG)rb_u32(r);
    }
    if (r->fail) return 0;
    SAFEARRAY *a = 0;
    if (FAILED(SafeArrayAllocDescriptorEx((VARTYPE)vt, dims, &a))) { r->fail = 1; return 0; }
    for (ULONG i = 0; i < dims; i++) a->rgsabound[i] = b[i];
    a->cbElements = elsize;
    a->fFeatures |= (USHORT)(features & (FADF_BSTR | FADF_UNKNOWN | FADF_DISPATCH | FADF_VARIANT | FADF_HAVEIID | FADF_HAVEVARTYPE));
    if (FAILED(SafeArrayAllocData(a))) { SafeArrayDestroyDescriptor(a); r->fail = 1; return 0; }
    ULONG n = sa_elements(a);
    BYTE *data = a->pvData;
    if (vt == VT_BSTR || vt == VT_UNKNOWN || vt == VT_DISPATCH || vt == VT_VARIANT) {
        for (ULONG i = 0; i < n && !r->fail; i++) get_value(r, (VARTYPE)vt, data + (SIZE_T)i * elsize);
    } else {
        rb_align(r, 8);
        rb_get(r, data, n * elsize);
    }
    if (r->fail) { SafeArrayDestroy(a); return 0; }
    return a;
}

/* ---- VARIANT: type, then the value (or what a VT_BYREF points to) -------- */
static ULONG scalar_size(VARTYPE vt)
{
    switch (vt) {
    case VT_I1: case VT_UI1: return 1;
    case VT_I2: case VT_UI2: case VT_BOOL: return 2;
    case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4: case VT_ERROR: case VT_HRESULT: return 4;
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: return 8;
    case VT_DECIMAL: return 16;
    default: return 0;
    }
}

/* one value of type vt at p (a BSTR*, IUnknown**, SAFEARRAY**, VARIANT* or
 * the scalar itself) */
static void wire_value(WireBuf *w, VARTYPE vt, const void *p)
{
    if (vt & VT_ARRAY) { wire_safearray(w, *(SAFEARRAY *const *)p); return; }
    switch (vt) {
    case VT_EMPTY: case VT_NULL: return;
    case VT_BSTR: wire_bstr(w, *(BSTR const *)p); return;
    case VT_UNKNOWN: wire_iface(w, *(IUnknown *const *)p, &IID_IUnknown); return;
    case VT_DISPATCH: wire_iface(w, *(IUnknown *const *)p, &IID_IDispatch); return;
    case VT_VARIANT: wire_variant(w, (const VARIANT *)p); return;
    }
    ULONG n = scalar_size(vt);
    if (!n) { w->fail = 1; return; }
    wb_align(w, 8);
    ULONGLONG slot[2] = { 0, 0 };
    CopyMemory(slot, p, n);
    wb_put(w, slot, n > 8 ? 16 : 8);
}

static void get_value(WireBuf *r, VARTYPE vt, void *p)
{
    if (vt & VT_ARRAY) { *(SAFEARRAY **)p = wire_get_safearray(r); return; }
    switch (vt) {
    case VT_EMPTY: case VT_NULL: return;
    case VT_BSTR: *(BSTR *)p = wire_get_bstr(r); return;
    case VT_UNKNOWN: *(IUnknown **)p = wire_get_iface(r, &IID_IUnknown); return;
    case VT_DISPATCH: *(IUnknown **)p = wire_get_iface(r, &IID_IDispatch); return;
    case VT_VARIANT: wire_get_variant(r, (VARIANT *)p); return;
    }
    ULONG n = scalar_size(vt);
    if (!n) { r->fail = 1; return; }
    rb_align(r, 8);
    ULONGLONG slot[2] = { 0, 0 };
    if (rb_get(r, slot, n > 8 ? 16 : 8)) CopyMemory(p, slot, n);
}

static ULONG value_size(VARTYPE vt)
{
    if (vt & VT_ARRAY) return sizeof(SAFEARRAY *);
    switch (vt) {
    case VT_BSTR: return sizeof(BSTR);
    case VT_UNKNOWN: case VT_DISPATCH: return sizeof(void *);
    case VT_VARIANT: return sizeof(VARIANT);
    }
    return scalar_size(vt);
}

void wire_variant(WireBuf *w, const VARIANT *v)
{
    VARTYPE vt = V_VT(v);
    wb_align(w, 8);
    wb_u32(w, vt);
    wb_u32(w, 0);
    if ((vt & VT_TYPEMASK) == VT_RECORD) { w->fail = 1; return; }
    if (vt & VT_BYREF) {
        wb_u32(w, v->byref ? 1 : 0);
        if (!v->byref) return;
        if (vt == (VT_BYREF | VT_DECIMAL)) { wire_value(w, VT_DECIMAL, v->pdecVal); return; }
        wire_value(w, vt & ~VT_BYREF, v->byref);
        return;
    }
    if (vt == VT_DECIMAL) { wire_value(w, VT_DECIMAL, &v->decVal); return; }
    if (vt == VT_VARIANT) { w->fail = 1; return; }               /* only by reference */
    wire_value(w, vt, &v->llVal);
}

/* into a VARIANT already empty (VT_EMPTY); a VT_BYREF one gets storage of
 * its own (CoTaskMemAlloc), which wire_free_variant releases */
void wire_get_variant(WireBuf *r, VARIANT *v)
{
    rb_align(r, 8);
    VARTYPE vt = (VARTYPE)rb_u32(r);
    rb_u32(r);
    if (r->fail) return;
    if ((vt & VT_TYPEMASK) == VT_RECORD) { r->fail = 1; return; }
    if (vt & VT_BYREF) {
        if (!rb_u32(r)) { V_VT(v) = vt; v->byref = 0; return; }
        VARTYPE base = vt & ~VT_BYREF;
        ULONG n = value_size(base);
        void *p = n ? CoTaskMemAlloc(n) : 0;
        if (!p) { r->fail = 1; return; }
        ZeroMemory(p, n);
        get_value(r, base, p);
        V_VT(v) = vt;
        v->byref = p;
        return;
    }
    V_VT(v) = vt;
    if (vt == VT_DECIMAL) { DECIMAL d; get_value(r, VT_DECIMAL, &d); v->decVal = d; V_VT(v) = VT_DECIMAL; return; }
    get_value(r, vt, &v->llVal);
}

/* what wire_get_variant made: the value, and a VT_BYREF's own storage */
void wire_free_variant(VARIANT *v)
{
    VARTYPE vt = V_VT(v);
    if (!(vt & VT_BYREF)) { VariantClear(v); return; }
    void *p = v->byref;
    if (p) {
        VARTYPE base = vt & ~VT_BYREF;
        if (base & VT_ARRAY) { if (*(SAFEARRAY **)p) SafeArrayDestroy(*(SAFEARRAY **)p); }
        else if (base == VT_BSTR) SysFreeString(*(BSTR *)p);
        else if (base == VT_UNKNOWN || base == VT_DISPATCH) { if (*(IUnknown **)p) (*(IUnknown **)p)->lpVtbl->Release(*(IUnknown **)p); }
        else if (base == VT_VARIANT) VariantClear((VARIANT *)p);
        CoTaskMemFree(p);
    }
    V_VT(v) = VT_EMPTY;
}

/* ---- the user-marshal entry points ---------------------------------------
 * flags: the low word is the destination context (MSHCTX_*) */
static WireBuf sizer(ULONG *flags, ULONG start) { WireBuf w = { 0 }; w.size = start; w.ctx = *flags & 0xffff; return w; }
static WireBuf writer(ULONG *flags, unsigned char *buf) { WireBuf w = { 0 }; w.buf = w.start = buf; w.ctx = *flags & 0xffff; return w; }

WINOLEAUTAPI_(ULONG) BSTR_UserSize(ULONG *flags, ULONG start, BSTR *b)
{
    WireBuf w = sizer(flags, start);
    wb_align(&w, 4);
    WireBuf rel = w;                    /* (positions count from the aligned start, as when writing) */
    rel.size = 0;
    wire_bstr(&rel, *b);
    return w.size + rel.size;
}
WINOLEAUTAPI_(unsigned char *) BSTR_UserMarshal(ULONG *flags, unsigned char *buf, BSTR *b)
{
    buf = (unsigned char *)(((ULONG_PTR)buf + 3) & ~(ULONG_PTR)3);
    WireBuf w = writer(flags, buf);
    wire_bstr(&w, *b);
    return w.buf;
}
WINOLEAUTAPI_(unsigned char *) BSTR_UserUnmarshal(ULONG *flags, unsigned char *buf, BSTR *b)
{
    (void)flags;
    buf = (unsigned char *)(((ULONG_PTR)buf + 3) & ~(ULONG_PTR)3);
    WireBuf r = { 0 };
    r.buf = r.start = buf;
    BSTR n = wire_get_bstr(&r);
    if (r.fail) return NULL;                       /* (the engine fails the call: RPC_X_BAD_STUB_DATA) */
    SysFreeString(*b);
    *b = n;
    return r.buf;
}
WINOLEAUTAPI_(void) BSTR_UserFree(ULONG *flags, BSTR *b) { (void)flags; SysFreeString(*b); *b = 0; }

WINOLEAUTAPI_(ULONG) VARIANT_UserSize(ULONG *flags, ULONG start, VARIANT *v)
{
    WireBuf w = sizer(flags, start);
    wb_align(&w, 8);
    WireBuf rel = w;
    rel.size = 0;
    wire_variant(&rel, v);
    return w.size + rel.size;
}
WINOLEAUTAPI_(unsigned char *) VARIANT_UserMarshal(ULONG *flags, unsigned char *buf, VARIANT *v)
{
    WireBuf w = writer(flags, buf);
    wire_variant(&w, v);
    return w.fail ? NULL : w.buf;
}
WINOLEAUTAPI_(unsigned char *) VARIANT_UserUnmarshal(ULONG *flags, unsigned char *buf, VARIANT *v)
{
    (void)flags;
    VARIANT n;
    WireBuf r = { 0 };
    r.buf = r.start = buf;
    ZeroMemory(&n, sizeof n);
    wire_get_variant(&r, &n);
    if (r.fail) { wire_free_variant(&n); return NULL; }
    VariantClear(v);
    *v = n;
    return r.buf;
}
WINOLEAUTAPI_(void) VARIANT_UserFree(ULONG *flags, VARIANT *v) { (void)flags; wire_free_variant(v); }

WINOLEAUTAPI_(ULONG) LPSAFEARRAY_UserSize(ULONG *flags, ULONG start, LPSAFEARRAY *a)
{
    WireBuf w = sizer(flags, start);
    wb_align(&w, 8);
    WireBuf rel = w;
    rel.size = 0;
    wire_safearray(&rel, *a);
    return w.size + rel.size;
}
WINOLEAUTAPI_(unsigned char *) LPSAFEARRAY_UserMarshal(ULONG *flags, unsigned char *buf, LPSAFEARRAY *a)
{
    WireBuf w = writer(flags, buf);
    wire_safearray(&w, *a);
    return w.fail ? NULL : w.buf;
}
WINOLEAUTAPI_(unsigned char *) LPSAFEARRAY_UserUnmarshal(ULONG *flags, unsigned char *buf, LPSAFEARRAY *a)
{
    (void)flags;
    WireBuf r = { 0 };
    r.buf = r.start = buf;
    SAFEARRAY *n = wire_get_safearray(&r);
    if (r.fail) return NULL;
    if (*a) SafeArrayDestroy(*a);
    *a = n;
    return r.buf;
}
WINOLEAUTAPI_(void) LPSAFEARRAY_UserFree(ULONG *flags, LPSAFEARRAY *a) { (void)flags; if (*a) SafeArrayDestroy(*a); *a = 0; }

/* the 64-bit names (NDR64 proxies) are the same routines */
WINOLEAUTAPI_(ULONG) BSTR_UserSize64(ULONG *f, ULONG s, BSTR *b) { return BSTR_UserSize(f, s, b); }
WINOLEAUTAPI_(unsigned char *) BSTR_UserMarshal64(ULONG *f, unsigned char *p, BSTR *b) { return BSTR_UserMarshal(f, p, b); }
WINOLEAUTAPI_(unsigned char *) BSTR_UserUnmarshal64(ULONG *f, unsigned char *p, BSTR *b) { return BSTR_UserUnmarshal(f, p, b); }
WINOLEAUTAPI_(void) BSTR_UserFree64(ULONG *f, BSTR *b) { BSTR_UserFree(f, b); }
WINOLEAUTAPI_(ULONG) VARIANT_UserSize64(ULONG *f, ULONG s, VARIANT *v) { return VARIANT_UserSize(f, s, v); }
WINOLEAUTAPI_(unsigned char *) VARIANT_UserMarshal64(ULONG *f, unsigned char *p, VARIANT *v) { return VARIANT_UserMarshal(f, p, v); }
WINOLEAUTAPI_(unsigned char *) VARIANT_UserUnmarshal64(ULONG *f, unsigned char *p, VARIANT *v) { return VARIANT_UserUnmarshal(f, p, v); }
WINOLEAUTAPI_(void) VARIANT_UserFree64(ULONG *f, VARIANT *v) { VARIANT_UserFree(f, v); }
WINOLEAUTAPI_(ULONG) LPSAFEARRAY_UserSize64(ULONG *f, ULONG s, LPSAFEARRAY *a) { return LPSAFEARRAY_UserSize(f, s, a); }
WINOLEAUTAPI_(unsigned char *) LPSAFEARRAY_UserMarshal64(ULONG *f, unsigned char *p, LPSAFEARRAY *a) { return LPSAFEARRAY_UserMarshal(f, p, a); }
WINOLEAUTAPI_(unsigned char *) LPSAFEARRAY_UserUnmarshal64(ULONG *f, unsigned char *p, LPSAFEARRAY *a) { return LPSAFEARRAY_UserUnmarshal(f, p, a); }
WINOLEAUTAPI_(void) LPSAFEARRAY_UserFree64(ULONG *f, LPSAFEARRAY *a) { LPSAFEARRAY_UserFree(f, a); }
