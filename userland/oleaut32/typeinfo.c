/*
 * typeinfo.c — the ITypeLib, ITypeInfo and ITypeComp objects over the
 * decoded libraries of typelib.c.
 */
#define NOVA_BUILD_OLEAUT32
#include <oleauto.h>
#include "typelib.h"

#define TI(iface) ((TInfo *)(iface))
#define TI_FROM_COMP(iface) ((TInfo *)((char *)(iface) - __builtin_offsetof(TInfo, ITypeComp_iface)))
#define TL(iface) ((TLib *)(iface))
#define TL_FROM_COMP(iface) ((TLib *)((char *)(iface) - __builtin_offsetof(TLib, ITypeComp_iface)))

static const ITypeInfo2Vtbl g_tinfo_vtbl;
static const ITypeCompVtbl g_tinfo_comp_vtbl;

static int wieq(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b) return 0;
    for (;; a++, b++) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static BSTR copy(BSTR s) { return s ? SysAllocStringLen(s, SysStringLen(s)) : 0; }

TInfo *tinfo_new(TLib *lib, UINT index)
{
    TInfo *ti = tl_alloc(lib, sizeof(TInfo));
    if (!ti) return 0;
    ti->ITypeInfo2_iface.lpVtbl = &g_tinfo_vtbl;
    ti->ITypeComp_iface.lpVtbl = &g_tinfo_comp_vtbl;
    ti->lib = lib;
    ti->index = index;
    ti->href = index * 0x64;
    ti->base_href = (HREFTYPE)-1;
    return ti;
}

/* Dual interfaces get their TKIND_INTERFACE view; dispinterfaces show
 * IDispatch's vtable */
HRESULT tlib_finish(TLib *lib)
{
    for (UINT i = 0; i < lib->ntinfos; i++) {
        TInfo *ti = lib->tinfos[i];
        if (!ti || ti->attr.typekind != TKIND_DISPATCH) continue;
        if (ti->attr.wTypeFlags & TYPEFLAG_FDUAL) {
            TInfo *iv = tl_alloc(lib, sizeof(TInfo));
            if (!iv) return E_OUTOFMEMORY;
            *iv = *ti;
            iv->attr.typekind = TKIND_INTERFACE;
            iv->href = ti->href | 2;
            iv->impls = tl_alloc(lib, sizeof(TImpl));
            if (!iv->impls) return E_OUTOFMEMORY;
            iv->impls[0].href = ti->base_href;
            iv->nimpls = ti->base_href != (HREFTYPE)-1;
            iv->attr.cImplTypes = (WORD)iv->nimpls;
            iv->dual = ti;
            ti->dual = iv;
            ti->dispview = TRUE;
            ti->nown = ti->attr.cFuncs;
        }
        ti->attr.cbSizeVft = 7 * sizeof(void *);
    }
    return S_OK;
}

HRESULT tinfo_ref(TInfo *ti, HREFTYPE href, ITypeInfo **out)
{
    TLib *lib = ti->lib;
    *out = 0;
    if (href == (HREFTYPE)-1) return TYPE_E_ELEMENTNOTFOUND;
    if (href & 1) {                                     /* in another library */
        UINT k = (href & ~3u) / 12;
        if (k >= lib->nimpinfos) return TYPE_E_ELEMENTNOTFOUND;
        TImpInfo *ii = &lib->impinfos[k];
        ITypeLib *tl;
        HRESULT hr = tlib_import(lib, ii->lib, &tl);
        if (FAILED(hr)) return hr;
        hr = ii->by_guid ? tl->lpVtbl->GetTypeInfoOfGuid(tl, &ii->guid, out)
                         : tl->lpVtbl->GetTypeInfo(tl, (UINT)ii->index, out);
        tl->lpVtbl->Release(tl);
        return hr;
    }
    UINT idx = (href & ~3u) / 0x64;
    if ((href & ~3u) % 0x64 || idx >= lib->ntinfos || !lib->tinfos[idx]) return TYPE_E_ELEMENTNOTFOUND;
    TInfo *t = lib->tinfos[idx];
    if (href & 2) {
        if (!t->dual) return TYPE_E_ELEMENTNOTFOUND;
        t = t->dual;
    }
    tlib_addref(lib);
    *out = (ITypeInfo *)&t->ITypeInfo2_iface;
    return S_OK;
}

/* The interface view a method list is taken from */
static TInfo *iface_view(TInfo *t) { return t->dispview && t->dual ? t->dual : t; }

/* @base's methods, its own base's first */
static UINT collect(TInfo *t, TFunc **list, UINT n, int depth)
{
    t = iface_view(t);
    if (depth > 32) return n;
    if (t->nimpls && t->attr.typekind == TKIND_INTERFACE) {
        ITypeInfo *b;
        if (SUCCEEDED(tinfo_ref(t, t->impls[0].href, &b))) {
            n = collect(TI(b), list, n, depth + 1);
            b->lpVtbl->Release(b);
        }
    }
    for (UINT i = 0; i < t->attr.cFuncs; i++) {
        if (list) list[n] = &t->funcs[i];
        n++;
    }
    return n;
}

static void build_dispview(TInfo *ti)
{
    if (ti->dfuncs || !ti->dispview) return;
    UINT n = collect(ti, 0, 0, 0);
    TFunc **list = tl_alloc(ti->lib, sizeof(TFunc *) * (n ? n : 1));
    if (!list) return;
    collect(ti, list, 0, 0);
    /* keep a base in another library alive while we point into it */
    TInfo *iv = iface_view(ti);
    ITypeInfo *b;
    if (iv->nimpls && (iv->impls[0].href & 1) && SUCCEEDED(tinfo_ref(iv, iv->impls[0].href, &b))) {
        if (InterlockedCompareExchangePointer((void **)&ti->dfuncs_hold, b, 0)) b->lpVtbl->Release(b);
    }
    ti->ndfuncs = n;
    InterlockedCompareExchangePointer((void **)&ti->dfuncs, list, 0);
}

UINT tinfo_nfuncs(TInfo *ti)
{
    if (ti->dispview) { build_dispview(ti); return ti->dfuncs ? ti->ndfuncs : ti->nown; }
    return ti->attr.cFuncs;
}

TFunc *tinfo_func(TInfo *ti, UINT i, const FUNCDESC **fd)
{
    if (ti->dispview) {
        build_dispview(ti);
        TFunc *f = ti->dfuncs ? (i < ti->ndfuncs ? ti->dfuncs[i] : 0) : (i < ti->nown ? &ti->funcs[i] : 0);
        if (f && fd) *fd = &f->disp;
        return f;
    }
    if (i >= ti->attr.cFuncs) return 0;
    TFunc *f = &ti->funcs[i];
    if (fd) *fd = ti->attr.typekind == TKIND_DISPATCH ? &f->disp : &f->fd;
    return f;
}

static BOOL invkind_ok(const FUNCDESC *fd, WORD flags) { return !flags || (fd->invkind & flags); }

/* A method by MEMBERID (and invoke kind): this view's, then its bases' */
HRESULT tinfo_find_func(TInfo *ti, MEMBERID id, WORD flags, TFunc **out, TInfo **owner)
{
    UINT n = tinfo_nfuncs(ti);
    for (UINT i = 0; i < n; i++) {
        const FUNCDESC *fd;
        TFunc *f = tinfo_func(ti, i, &fd);
        if (f && fd->memid == id && invkind_ok(fd, flags)) { *out = f; if (owner) *owner = ti; return S_OK; }
    }
    if (!ti->dispview && ti->attr.typekind == TKIND_INTERFACE && ti->nimpls) {
        ITypeInfo *b;
        if (SUCCEEDED(tinfo_ref(ti, ti->impls[0].href, &b))) {
            HRESULT hr = tinfo_find_func(TI(b), id, flags, out, owner);
            b->lpVtbl->Release(b);         /* (the library keeps it alive: imports hold their libraries) */
            return hr;
        }
    }
    return TYPE_E_ELEMENTNOTFOUND;
}

static TVar *find_var(TInfo *ti, MEMBERID id)
{
    for (UINT i = 0; i < ti->attr.cVars; i++)
        if (ti->vars[i].vd.memid == id) return &ti->vars[i];
    return 0;
}

/* ---------------------------------------------------------------------------
 * ITypeInfo2
 * ------------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE ti_QueryInterface(ITypeInfo2 *iface, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ITypeInfo) || IsEqualIID(riid, &IID_ITypeInfo2)) {
        *ppv = iface;
        tlib_addref(TI(iface)->lib);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ti_AddRef(ITypeInfo2 *iface) { return tlib_addref(TI(iface)->lib); }
static ULONG STDMETHODCALLTYPE ti_Release(ITypeInfo2 *iface) { return tlib_release(TI(iface)->lib); }

static HRESULT STDMETHODCALLTYPE ti_GetTypeAttr(ITypeInfo2 *iface, TYPEATTR **attr)
{
    TInfo *ti = TI(iface);
    if (!attr) return E_INVALIDARG;
    if (ti->dispview) ti->attr.cFuncs = (WORD)tinfo_nfuncs(ti);
    *attr = &ti->attr;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ti_GetTypeComp(ITypeInfo2 *iface, ITypeComp **tc)
{
    if (!tc) return E_INVALIDARG;
    *tc = &TI(iface)->ITypeComp_iface;
    tlib_addref(TI(iface)->lib);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ti_GetFuncDesc(ITypeInfo2 *iface, UINT index, FUNCDESC **out)
{
    if (!out) return E_INVALIDARG;
    const FUNCDESC *fd;
    if (!tinfo_func(TI(iface), index, &fd)) return TYPE_E_ELEMENTNOTFOUND;
    *out = (FUNCDESC *)fd;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ti_GetVarDesc(ITypeInfo2 *iface, UINT index, VARDESC **out)
{
    TInfo *ti = TI(iface);
    if (!out) return E_INVALIDARG;
    if (index >= ti->attr.cVars) return TYPE_E_ELEMENTNOTFOUND;
    *out = &ti->vars[index].vd;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ti_GetNames(ITypeInfo2 *iface, MEMBERID id, BSTR *names, UINT max, UINT *n)
{
    TInfo *ti = TI(iface);
    if (!names || !n) return E_INVALIDARG;
    *n = 0;
    TFunc *f;
    TInfo *owner;
    if (SUCCEEDED(tinfo_find_func(ti, id, 0, &f, &owner))) {
        const FUNCDESC *fd = ti->dispview || owner->attr.typekind == TKIND_DISPATCH ? &f->disp : &f->fd;
        if (max) names[(*n)++] = copy(f->name);
        for (int i = 0; i < fd->cParams && *n < max && f->pnames && f->pnames[i]; i++)
            names[(*n)++] = copy(f->pnames[i]);
        return S_OK;
    }
    TVar *v = find_var(ti, id);
    if (v) {
        if (max) names[(*n)++] = copy(v->name);
        return S_OK;
    }
    return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT STDMETHODCALLTYPE ti_GetRefTypeOfImplType(ITypeInfo2 *iface, UINT index, HREFTYPE *href)
{
    TInfo *ti = TI(iface);
    if (!href) return E_INVALIDARG;
    if (index == (UINT)-1) {
        if (!ti->dual) return TYPE_E_ELEMENTNOTFOUND;
        *href = ti->dual->href;
        return S_OK;
    }
    if (index >= ti->nimpls || ti->impls[index].href == (HREFTYPE)-1) return TYPE_E_ELEMENTNOTFOUND;
    *href = ti->impls[index].href;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ti_GetImplTypeFlags(ITypeInfo2 *iface, UINT index, INT *flags)
{
    TInfo *ti = TI(iface);
    if (!flags) return E_INVALIDARG;
    if (index >= ti->nimpls) return TYPE_E_ELEMENTNOTFOUND;
    *flags = ti->attr.typekind == TKIND_COCLASS ? ti->impls[index].flags : 0;
    return S_OK;
}

/* name -> MEMBERID: this view's methods and variables, then the bases' */
static HRESULT find_name(TInfo *ti, const WCHAR *name, MEMBERID *id, TFunc **func, BOOL *disp_form, int depth)
{
    UINT n = tinfo_nfuncs(ti);
    for (UINT i = 0; i < n; i++) {
        const FUNCDESC *fd;
        TFunc *f = tinfo_func(ti, i, &fd);
        if (f && wieq(f->name, name)) { *id = fd->memid; *func = f; *disp_form = fd == &f->disp; return S_OK; }
    }
    for (UINT i = 0; i < ti->attr.cVars; i++)
        if (wieq(ti->vars[i].name, name)) { *id = ti->vars[i].vd.memid; *func = 0; return S_OK; }
    if (!ti->dispview && ti->attr.typekind == TKIND_INTERFACE && ti->nimpls && depth < 32) {
        ITypeInfo *b;
        if (SUCCEEDED(tinfo_ref(ti, ti->impls[0].href, &b))) {
            HRESULT hr = find_name(TI(b), name, id, func, disp_form, depth + 1);
            b->lpVtbl->Release(b);
            return hr;
        }
    }
    return DISP_E_UNKNOWNNAME;
}

static HRESULT STDMETHODCALLTYPE ti_GetIDsOfNames(ITypeInfo2 *iface, LPOLESTR *names, UINT n, MEMBERID *ids)
{
    TInfo *ti = TI(iface);
    if (!names || !ids || !n) return n ? E_INVALIDARG : S_OK;
    for (UINT i = 0; i < n; i++) ids[i] = MEMBERID_NIL;
    TFunc *f = 0;
    BOOL disp_form = FALSE;
    HRESULT hr = find_name(ti, names[0], &ids[0], &f, &disp_form, 0);
    if (FAILED(hr)) return hr;
    for (UINT i = 1; i < n; i++) {
        int np = f ? (disp_form ? f->disp.cParams : f->fd.cParams) : 0;
        for (int k = 0; k < np && f->pnames; k++)
            if (wieq(f->pnames[k], names[i])) { ids[i] = k; break; }
        if (ids[i] == MEMBERID_NIL) hr = DISP_E_UNKNOWNNAME;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ti_Invoke(ITypeInfo2 *iface, PVOID obj, MEMBERID id, WORD flags, DISPPARAMS *dp,
                                           VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    return tinfo_invoke(TI(iface), obj, id, flags, dp, res, ei, argerr);
}

static HRESULT STDMETHODCALLTYPE ti_GetDocumentation(ITypeInfo2 *iface, MEMBERID id, BSTR *name, BSTR *doc,
                                                     DWORD *helpctx, BSTR *helpfile)
{
    TInfo *ti = TI(iface);
    BSTR n = 0, d = 0;
    DWORD c = 0;
    if (id == MEMBERID_NIL) { n = ti->name; d = ti->doc; c = ti->helpctx; }
    else {
        TFunc *f;
        TVar *v;
        if (SUCCEEDED(tinfo_find_func(ti, id, 0, &f, 0))) { n = f->name; d = f->doc; c = f->helpctx; }
        else if ((v = find_var(ti, id))) { n = v->name; d = v->doc; c = v->helpctx; }
        else return TYPE_E_ELEMENTNOTFOUND;
    }
    if (name) *name = copy(n);
    if (doc) *doc = copy(d);
    if (helpctx) *helpctx = c;
    if (helpfile) *helpfile = copy(ti->lib->helpfile);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ti_GetDllEntry(ITypeInfo2 *iface, MEMBERID id, INVOKEKIND kind, BSTR *dll,
                                                BSTR *name, WORD *ordinal)
{
    TInfo *ti = TI(iface);
    if (dll) *dll = 0;
    if (name) *name = 0;
    if (ordinal) *ordinal = 0;
    if (ti->attr.typekind != TKIND_MODULE) return TYPE_E_BADMODULEKIND;
    for (UINT i = 0; i < ti->attr.cFuncs; i++) {
        TFunc *f = &ti->funcs[i];
        if (f->fd.memid != id || !(f->fd.invkind & kind)) continue;
        if (dll) *dll = copy(ti->dllname);
        if (f->entry) { if (name) *name = copy(f->entry); }
        else if (ordinal) *ordinal = f->ordinal;
        return S_OK;
    }
    return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT STDMETHODCALLTYPE ti_GetRefTypeInfo(ITypeInfo2 *iface, HREFTYPE href, ITypeInfo **out)
{
    if (!out) return E_INVALIDARG;
    return tinfo_ref(TI(iface), href, out);
}

static HRESULT STDMETHODCALLTYPE ti_AddressOfMember(ITypeInfo2 *iface, MEMBERID id, INVOKEKIND kind, PVOID *addr)
{
    BSTR dll, name;
    WORD ord;
    if (!addr) return E_INVALIDARG;
    *addr = 0;
    HRESULT hr = ti_GetDllEntry(iface, id, kind, &dll, &name, &ord);
    if (FAILED(hr)) return hr;
    HMODULE m = dll ? LoadLibraryW(dll) : 0;
    if (m) {
        char a[256];
        if (name) WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof(a), 0, 0);
        *addr = (PVOID)GetProcAddress(m, name ? a : (LPCSTR)(ULONG_PTR)ord);
    }
    SysFreeString(dll);
    SysFreeString(name);
    return *addr ? S_OK : TYPE_E_DLLFUNCTIONNOTFOUND;
}

static HRESULT STDMETHODCALLTYPE ti_CreateInstance(ITypeInfo2 *iface, IUnknown *outer, REFIID riid, PVOID *obj)
{
    TInfo *ti = TI(iface);
    if (!obj) return E_INVALIDARG;
    *obj = 0;
    if (ti->attr.typekind != TKIND_COCLASS) return TYPE_E_WRONGTYPEKIND;
    return CoCreateInstance(&ti->attr.guid, outer, CLSCTX_ALL, riid, obj);
}

static HRESULT STDMETHODCALLTYPE ti_GetMops(ITypeInfo2 *iface, MEMBERID id, BSTR *mops)
{
    (void)iface; (void)id;
    if (mops) *mops = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ti_GetContainingTypeLib(ITypeInfo2 *iface, ITypeLib **tl, UINT *index)
{
    TInfo *ti = TI(iface);
    if (tl) { *tl = (ITypeLib *)&ti->lib->ITypeLib2_iface; tlib_addref(ti->lib); }
    if (index) *index = ti->index;
    return S_OK;
}

static void STDMETHODCALLTYPE ti_ReleaseTypeAttr(ITypeInfo2 *iface, TYPEATTR *a) { (void)iface; (void)a; }
static void STDMETHODCALLTYPE ti_ReleaseFuncDesc(ITypeInfo2 *iface, FUNCDESC *f) { (void)iface; (void)f; }
static void STDMETHODCALLTYPE ti_ReleaseVarDesc(ITypeInfo2 *iface, VARDESC *v) { (void)iface; (void)v; }

static HRESULT STDMETHODCALLTYPE ti_GetTypeKind(ITypeInfo2 *iface, TYPEKIND *kind)
{
    if (!kind) return E_INVALIDARG;
    *kind = TI(iface)->attr.typekind;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ti_GetTypeFlags(ITypeInfo2 *iface, ULONG *flags)
{
    if (!flags) return E_INVALIDARG;
    *flags = TI(iface)->attr.wTypeFlags;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ti_GetFuncIndexOfMemId(ITypeInfo2 *iface, MEMBERID id, INVOKEKIND kind, UINT *index)
{
    TInfo *ti = TI(iface);
    if (!index) return E_INVALIDARG;
    UINT n = tinfo_nfuncs(ti);
    for (UINT i = 0; i < n; i++) {
        const FUNCDESC *fd;
        if (tinfo_func(ti, i, &fd) && fd->memid == id && (fd->invkind & kind)) { *index = i; return S_OK; }
    }
    return TYPE_E_ELEMENTNOTFOUND;
}
static HRESULT STDMETHODCALLTYPE ti_GetVarIndexOfMemId(ITypeInfo2 *iface, MEMBERID id, UINT *index)
{
    TInfo *ti = TI(iface);
    if (!index) return E_INVALIDARG;
    for (UINT i = 0; i < ti->attr.cVars; i++)
        if (ti->vars[i].vd.memid == id) { *index = i; return S_OK; }
    return TYPE_E_ELEMENTNOTFOUND;
}
/* custom data ([custom(guid, value)] attributes) is not read: none found */
static HRESULT no_custdata(VARIANT *v) { if (!v) return E_INVALIDARG; VariantInit(v); return S_OK; }
static HRESULT STDMETHODCALLTYPE ti_GetCustData(ITypeInfo2 *iface, REFGUID g, VARIANT *v) { (void)iface; (void)g; return no_custdata(v); }
static HRESULT STDMETHODCALLTYPE ti_GetFuncCustData(ITypeInfo2 *iface, UINT i, REFGUID g, VARIANT *v) { (void)iface; (void)i; (void)g; return no_custdata(v); }
static HRESULT STDMETHODCALLTYPE ti_GetParamCustData(ITypeInfo2 *iface, UINT f, UINT p, REFGUID g, VARIANT *v) { (void)iface; (void)f; (void)p; (void)g; return no_custdata(v); }
static HRESULT STDMETHODCALLTYPE ti_GetVarCustData(ITypeInfo2 *iface, UINT i, REFGUID g, VARIANT *v) { (void)iface; (void)i; (void)g; return no_custdata(v); }
static HRESULT STDMETHODCALLTYPE ti_GetImplTypeCustData(ITypeInfo2 *iface, UINT i, REFGUID g, VARIANT *v) { (void)iface; (void)i; (void)g; return no_custdata(v); }
static HRESULT STDMETHODCALLTYPE ti_GetDocumentation2(ITypeInfo2 *iface, MEMBERID id, LCID lcid, BSTR *help, DWORD *ctx, BSTR *dll)
{
    (void)lcid;
    TInfo *ti = TI(iface);
    BSTR doc = 0;
    DWORD c = 0;
    HRESULT hr = ti_GetDocumentation(iface, id, 0, &doc, &c, 0);
    if (FAILED(hr)) return hr;
    if (ctx) {
        TFunc *f;
        *ctx = id == MEMBERID_NIL ? ti->helpstrctx : SUCCEEDED(tinfo_find_func(ti, id, 0, &f, 0)) ? f->helpstrctx : 0;
    }
    if (help) *help = doc; else SysFreeString(doc);
    if (dll) *dll = 0;
    return S_OK;
}
static HRESULT no_allcust(CUSTDATA *cd) { if (!cd) return E_INVALIDARG; cd->cCustData = 0; cd->prgCustData = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE ti_GetAllCustData(ITypeInfo2 *iface, CUSTDATA *cd) { (void)iface; return no_allcust(cd); }
static HRESULT STDMETHODCALLTYPE ti_GetAllFuncCustData(ITypeInfo2 *iface, UINT i, CUSTDATA *cd) { (void)iface; (void)i; return no_allcust(cd); }
static HRESULT STDMETHODCALLTYPE ti_GetAllParamCustData(ITypeInfo2 *iface, UINT f, UINT p, CUSTDATA *cd) { (void)iface; (void)f; (void)p; return no_allcust(cd); }
static HRESULT STDMETHODCALLTYPE ti_GetAllVarCustData(ITypeInfo2 *iface, UINT i, CUSTDATA *cd) { (void)iface; (void)i; return no_allcust(cd); }
static HRESULT STDMETHODCALLTYPE ti_GetAllImplTypeCustData(ITypeInfo2 *iface, UINT i, CUSTDATA *cd) { (void)iface; (void)i; return no_allcust(cd); }

static const ITypeInfo2Vtbl g_tinfo_vtbl = {
    ti_QueryInterface, ti_AddRef, ti_Release,
    ti_GetTypeAttr, ti_GetTypeComp, ti_GetFuncDesc, ti_GetVarDesc, ti_GetNames, ti_GetRefTypeOfImplType,
    ti_GetImplTypeFlags, ti_GetIDsOfNames, ti_Invoke, ti_GetDocumentation, ti_GetDllEntry, ti_GetRefTypeInfo,
    ti_AddressOfMember, ti_CreateInstance, ti_GetMops, ti_GetContainingTypeLib, ti_ReleaseTypeAttr,
    ti_ReleaseFuncDesc, ti_ReleaseVarDesc,
    ti_GetTypeKind, ti_GetTypeFlags, ti_GetFuncIndexOfMemId, ti_GetVarIndexOfMemId, ti_GetCustData,
    ti_GetFuncCustData, ti_GetParamCustData, ti_GetVarCustData, ti_GetImplTypeCustData, ti_GetDocumentation2,
    ti_GetAllCustData, ti_GetAllFuncCustData, ti_GetAllParamCustData, ti_GetAllVarCustData,
    ti_GetAllImplTypeCustData,
};

/* ---- ITypeComp of a typeinfo: its members by name ---- */
static HRESULT STDMETHODCALLTYPE tic_QueryInterface(ITypeComp *iface, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ITypeComp)) {
        *ppv = iface;
        tlib_addref(TI_FROM_COMP(iface)->lib);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE tic_AddRef(ITypeComp *iface) { return tlib_addref(TI_FROM_COMP(iface)->lib); }
static ULONG STDMETHODCALLTYPE tic_Release(ITypeComp *iface) { return tlib_release(TI_FROM_COMP(iface)->lib); }

static HRESULT bind_member(TInfo *ti, const WCHAR *name, WORD flags, ITypeInfo **out, DESCKIND *kind, BINDPTR *bind, int depth)
{
    UINT n = tinfo_nfuncs(ti);
    for (UINT i = 0; i < n; i++) {
        const FUNCDESC *fd;
        TFunc *f = tinfo_func(ti, i, &fd);
        if (f && wieq(f->name, name) && invkind_ok(fd, flags)) {
            *kind = DESCKIND_FUNCDESC;
            bind->lpfuncdesc = (FUNCDESC *)fd;
            *out = (ITypeInfo *)&ti->ITypeInfo2_iface;
            tlib_addref(ti->lib);
            return S_OK;
        }
    }
    for (UINT i = 0; i < ti->attr.cVars; i++)
        if (wieq(ti->vars[i].name, name)) {
            *kind = DESCKIND_VARDESC;
            bind->lpvardesc = &ti->vars[i].vd;
            *out = (ITypeInfo *)&ti->ITypeInfo2_iface;
            tlib_addref(ti->lib);
            return S_OK;
        }
    if (!ti->dispview && ti->attr.typekind == TKIND_INTERFACE && ti->nimpls && depth < 32) {
        ITypeInfo *b;
        if (SUCCEEDED(tinfo_ref(ti, ti->impls[0].href, &b))) {
            HRESULT hr = bind_member(TI(b), name, flags, out, kind, bind, depth + 1);
            b->lpVtbl->Release(b);
            return hr;
        }
    }
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE tic_Bind(ITypeComp *iface, LPOLESTR name, ULONG hash, WORD flags, ITypeInfo **out,
                                          DESCKIND *kind, BINDPTR *bind)
{
    (void)hash;
    if (!name || !out || !kind || !bind) return E_INVALIDARG;
    *out = 0;
    *kind = DESCKIND_NONE;
    bind->lpfuncdesc = 0;
    bind_member(TI_FROM_COMP(iface), name, flags, out, kind, bind, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tic_BindType(ITypeComp *iface, LPOLESTR name, ULONG hash, ITypeInfo **ti, ITypeComp **tc)
{
    (void)iface; (void)name; (void)hash;
    if (ti) *ti = 0;
    if (tc) *tc = 0;
    return S_OK;
}
static const ITypeCompVtbl g_tinfo_comp_vtbl = { tic_QueryInterface, tic_AddRef, tic_Release, tic_Bind, tic_BindType };

/* ---------------------------------------------------------------------------
 * ITypeLib2
 * ------------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE tl_QueryInterface(ITypeLib2 *iface, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ITypeLib) || IsEqualIID(riid, &IID_ITypeLib2)) {
        *ppv = iface;
        tlib_addref(TL(iface));
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE tl_AddRef(ITypeLib2 *iface) { return tlib_addref(TL(iface)); }
static ULONG STDMETHODCALLTYPE tl_Release(ITypeLib2 *iface) { return tlib_release(TL(iface)); }
static UINT STDMETHODCALLTYPE tl_GetTypeInfoCount(ITypeLib2 *iface) { return TL(iface)->ntinfos; }

static HRESULT STDMETHODCALLTYPE tl_GetTypeInfo(ITypeLib2 *iface, UINT index, ITypeInfo **out)
{
    TLib *lib = TL(iface);
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (index >= lib->ntinfos) return TYPE_E_ELEMENTNOTFOUND;
    *out = (ITypeInfo *)&lib->tinfos[index]->ITypeInfo2_iface;
    tlib_addref(lib);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tl_GetTypeInfoType(ITypeLib2 *iface, UINT index, TYPEKIND *kind)
{
    TLib *lib = TL(iface);
    if (!kind) return E_INVALIDARG;
    if (index >= lib->ntinfos) return TYPE_E_ELEMENTNOTFOUND;
    *kind = lib->tinfos[index]->attr.typekind;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tl_GetTypeInfoOfGuid(ITypeLib2 *iface, REFGUID guid, ITypeInfo **out)
{
    TLib *lib = TL(iface);
    if (!out || !guid) return E_INVALIDARG;
    *out = 0;
    for (UINT i = 0; i < lib->ntinfos; i++)
        if (IsEqualGUID(&lib->tinfos[i]->attr.guid, guid)) {
            *out = (ITypeInfo *)&lib->tinfos[i]->ITypeInfo2_iface;
            tlib_addref(lib);
            return S_OK;
        }
    return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT STDMETHODCALLTYPE tl_GetLibAttr(ITypeLib2 *iface, TLIBATTR **attr)
{
    if (!attr) return E_INVALIDARG;
    *attr = &TL(iface)->attr;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tl_GetTypeComp(ITypeLib2 *iface, ITypeComp **tc)
{
    if (!tc) return E_INVALIDARG;
    *tc = &TL(iface)->ITypeComp_iface;
    tlib_addref(TL(iface));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tl_GetDocumentation(ITypeLib2 *iface, INT index, BSTR *name, BSTR *doc,
                                                     DWORD *helpctx, BSTR *helpfile)
{
    TLib *lib = TL(iface);
    if (index == -1) {
        if (name) *name = copy(lib->name);
        if (doc) *doc = copy(lib->doc);
        if (helpctx) *helpctx = lib->helpctx;
        if (helpfile) *helpfile = copy(lib->helpfile);
        return S_OK;
    }
    if (index < 0 || (UINT)index >= lib->ntinfos) return TYPE_E_ELEMENTNOTFOUND;
    return ti_GetDocumentation(&lib->tinfos[index]->ITypeInfo2_iface, MEMBERID_NIL, name, doc, helpctx, helpfile);
}

/* each name the library defines: types, members, parameters */
static BSTR match_name(TLib *lib, const WCHAR *name, UINT *tinfo, MEMBERID *id)
{
    for (UINT i = 0; i < lib->ntinfos; i++) {
        TInfo *ti = lib->tinfos[i];
        if (wieq(ti->name, name)) { *tinfo = i; *id = MEMBERID_NIL; return ti->name; }
        UINT nf = ti->dispview ? ti->nown : ti->attr.cFuncs;
        for (UINT k = 0; k < nf; k++) {
            TFunc *f = &ti->funcs[k];
            if (wieq(f->name, name)) { *tinfo = i; *id = f->fd.memid; return f->name; }
            for (int p = 0; p < f->fd.cParams && f->pnames; p++)
                if (wieq(f->pnames[p], name)) { *tinfo = i; *id = f->fd.memid; return f->pnames[p]; }
        }
        for (UINT k = 0; k < ti->attr.cVars; k++)
            if (wieq(ti->vars[k].name, name)) { *tinfo = i; *id = ti->vars[k].vd.memid; return ti->vars[k].name; }
    }
    return 0;
}

static HRESULT STDMETHODCALLTYPE tl_IsName(ITypeLib2 *iface, LPOLESTR name, ULONG hash, BOOL *found)
{
    (void)hash;
    if (!name || !found) return E_INVALIDARG;
    UINT t;
    MEMBERID id;
    BSTR s = match_name(TL(iface), name, &t, &id);
    *found = s != 0;
    if (s) for (UINT i = 0; s[i]; i++) name[i] = s[i];          /* the library's spelling */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tl_FindName(ITypeLib2 *iface, LPOLESTR name, ULONG hash, ITypeInfo **tis,
                                             MEMBERID *ids, USHORT *n)
{
    (void)hash;
    TLib *lib = TL(iface);
    if (!name || !tis || !ids || !n) return E_INVALIDARG;
    USHORT max = *n, got = 0;
    for (UINT i = 0; i < lib->ntinfos && got < max; i++) {
        TInfo *ti = lib->tinfos[i];
        MEMBERID id = MEMBERID_NIL + 1;
        BOOL hit = FALSE;
        if (wieq(ti->name, name)) { hit = TRUE; id = MEMBERID_NIL; }
        UINT nf = ti->dispview ? ti->nown : ti->attr.cFuncs;
        for (UINT k = 0; k < nf && !hit; k++)
            if (wieq(ti->funcs[k].name, name)) { hit = TRUE; id = ti->funcs[k].fd.memid; }
        for (UINT k = 0; k < ti->attr.cVars && !hit; k++)
            if (wieq(ti->vars[k].name, name)) { hit = TRUE; id = ti->vars[k].vd.memid; }
        if (!hit) continue;
        tis[got] = (ITypeInfo *)&ti->ITypeInfo2_iface;
        tlib_addref(lib);
        ids[got++] = id;
    }
    *n = got;
    return S_OK;
}

static void STDMETHODCALLTYPE tl_ReleaseTLibAttr(ITypeLib2 *iface, TLIBATTR *a) { (void)iface; (void)a; }
static HRESULT STDMETHODCALLTYPE tl_GetCustData(ITypeLib2 *iface, REFGUID g, VARIANT *v) { (void)iface; (void)g; return no_custdata(v); }
static HRESULT STDMETHODCALLTYPE tl_GetLibStatistics(ITypeLib2 *iface, ULONG *unique, ULONG *chars)
{
    TLib *lib = TL(iface);
    ULONG n = 0, c = 0;
    for (UINT i = 0; i < lib->ntinfos; i++) {
        TInfo *ti = lib->tinfos[i];
        n++; c += SysStringLen(ti->name);
        UINT nf = ti->dispview ? ti->nown : ti->attr.cFuncs;
        for (UINT k = 0; k < nf; k++) { n++; c += SysStringLen(ti->funcs[k].name); }
        for (UINT k = 0; k < ti->attr.cVars; k++) { n++; c += SysStringLen(ti->vars[k].name); }
    }
    if (unique) *unique = n;
    if (chars) *chars = c;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_GetDocumentation2(ITypeLib2 *iface, INT index, LCID lcid, BSTR *help, DWORD *ctx, BSTR *dll)
{
    (void)lcid;
    if (dll) *dll = 0;
    if (index == -1) {
        if (help) *help = copy(TL(iface)->doc);
        if (ctx) *ctx = 0;
        return S_OK;
    }
    if (index < 0 || (UINT)index >= TL(iface)->ntinfos) return TYPE_E_ELEMENTNOTFOUND;
    return ti_GetDocumentation2(&TL(iface)->tinfos[index]->ITypeInfo2_iface, MEMBERID_NIL, lcid, help, ctx, 0);
}
static HRESULT STDMETHODCALLTYPE tl_GetAllCustData(ITypeLib2 *iface, CUSTDATA *cd) { (void)iface; return no_allcust(cd); }

const ITypeLib2Vtbl g_tlib_vtbl = {
    tl_QueryInterface, tl_AddRef, tl_Release,
    tl_GetTypeInfoCount, tl_GetTypeInfo, tl_GetTypeInfoType, tl_GetTypeInfoOfGuid, tl_GetLibAttr, tl_GetTypeComp,
    tl_GetDocumentation, tl_IsName, tl_FindName, tl_ReleaseTLibAttr,
    tl_GetCustData, tl_GetLibStatistics, tl_GetDocumentation2, tl_GetAllCustData,
};

/* ---- ITypeComp of a library: enum constants, module members, type names ---- */
static HRESULT STDMETHODCALLTYPE tlc_QueryInterface(ITypeComp *iface, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ITypeComp)) {
        *ppv = iface;
        tlib_addref(TL_FROM_COMP(iface));
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE tlc_AddRef(ITypeComp *iface) { return tlib_addref(TL_FROM_COMP(iface)); }
static ULONG STDMETHODCALLTYPE tlc_Release(ITypeComp *iface) { return tlib_release(TL_FROM_COMP(iface)); }

static HRESULT STDMETHODCALLTYPE tlc_Bind(ITypeComp *iface, LPOLESTR name, ULONG hash, WORD flags, ITypeInfo **out,
                                          DESCKIND *kind, BINDPTR *bind)
{
    (void)hash;
    TLib *lib = TL_FROM_COMP(iface);
    if (!name || !out || !kind || !bind) return E_INVALIDARG;
    *out = 0;
    *kind = DESCKIND_NONE;
    bind->lpfuncdesc = 0;
    for (UINT i = 0; i < lib->ntinfos; i++) {
        TInfo *ti = lib->tinfos[i];
        if (ti->attr.typekind == TKIND_ENUM || ti->attr.typekind == TKIND_MODULE) {
            if (bind_member(ti, name, flags, out, kind, bind, 0) == S_OK) return S_OK;
        } else if (ti->attr.typekind == TKIND_COCLASS && (ti->attr.wTypeFlags & TYPEFLAG_FAPPOBJECT) &&
                   wieq(ti->name, name)) {
            *kind = DESCKIND_IMPLICITAPPOBJ;
            *out = (ITypeInfo *)&ti->ITypeInfo2_iface;
            tlib_addref(lib);
            return S_OK;
        }
    }
    for (UINT i = 0; i < lib->ntinfos; i++) {
        TInfo *ti = lib->tinfos[i];
        if ((ti->attr.typekind == TKIND_ENUM || ti->attr.typekind == TKIND_MODULE) && wieq(ti->name, name)) {
            *kind = DESCKIND_TYPECOMP;
            bind->lptcomp = &ti->ITypeComp_iface;
            tlib_addref(lib);
            return S_OK;
        }
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tlc_BindType(ITypeComp *iface, LPOLESTR name, ULONG hash, ITypeInfo **out, ITypeComp **tc)
{
    (void)hash;
    TLib *lib = TL_FROM_COMP(iface);
    if (!name || !out || !tc) return E_INVALIDARG;
    *out = 0;
    *tc = 0;
    for (UINT i = 0; i < lib->ntinfos; i++)
        if (wieq(lib->tinfos[i]->name, name)) {
            *out = (ITypeInfo *)&lib->tinfos[i]->ITypeInfo2_iface;
            tlib_addref(lib);
            break;
        }
    return S_OK;
}

const ITypeCompVtbl g_tlib_comp_vtbl = { tlc_QueryInterface, tlc_AddRef, tlc_Release, tlc_Bind, tlc_BindType };
