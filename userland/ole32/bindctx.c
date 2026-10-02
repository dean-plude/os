/*
 * bindctx.c — bind contexts (CreateBindCtx) and persisting an object in a
 * stream: WriteClassStm/ReadClassStm and OleSaveToStream/OleLoadFromStream
 * (the CLSID, then the object's own IPersistStream data).
 */
#define NOVA_BUILD_OLE32
#include <objbase.h>

#define OLEAPI __declspec(dllexport)
#define MK_E_UNAVAILABLE_   ((HRESULT)0x800401E3L)
#define STG_E_READFAULT_    ((HRESULT)0x8003001EL)
#define E_OUTOFMEMORY_      ((HRESULT)0x8007000EL)

static const IID IID_IBindCtx_ = { 0x0000000E, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID IID_IPersistStream_ = { 0x00000109, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const CLSID CLSID_NULL_ = { 0 };

/* -----------------------------------------------------------------------
 * IBindCtx: bind options, the objects bound during the operation and the
 * named object parameters
 * ----------------------------------------------------------------------- */
typedef struct BindCtx BindCtx;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(BindCtx *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(BindCtx *);
    ULONG (STDMETHODCALLTYPE *Release)(BindCtx *);
    HRESULT (STDMETHODCALLTYPE *RegisterObjectBound)(BindCtx *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *RevokeObjectBound)(BindCtx *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *ReleaseBoundObjects)(BindCtx *);
    HRESULT (STDMETHODCALLTYPE *SetBindOptions)(BindCtx *, void *);
    HRESULT (STDMETHODCALLTYPE *GetBindOptions)(BindCtx *, void *);
    HRESULT (STDMETHODCALLTYPE *GetRunningObjectTable)(BindCtx *, void **);
    HRESULT (STDMETHODCALLTYPE *RegisterObjectParam)(BindCtx *, LPOLESTR, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *GetObjectParam)(BindCtx *, LPOLESTR, IUnknown **);
    HRESULT (STDMETHODCALLTYPE *EnumObjectParam)(BindCtx *, void **);
    HRESULT (STDMETHODCALLTYPE *RevokeObjectParam)(BindCtx *, LPOLESTR);
} BindCtxVtbl;

#define MAX_BOUND 32
#define MAX_PARAMS 16
#define OPTS_MAX 64                                     /* BIND_OPTS3 and a margin */

struct BindCtx {
    const BindCtxVtbl *lpVtbl;
    LONG refs;
    BYTE opts[OPTS_MAX];
    IUnknown *bound[MAX_BOUND];
    int nbound;
    struct { WCHAR *key; IUnknown *obj; } params[MAX_PARAMS];
    int nparams;
};

static void *bc_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void bc_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static HRESULT STDMETHODCALLTYPE bc_qi(BindCtx *b, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IBindCtx_)) {
        *ppv = b;
        b->lpVtbl->AddRef(b);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE bc_addref(BindCtx *b) { return (ULONG)InterlockedIncrement(&b->refs); }
static HRESULT STDMETHODCALLTYPE bc_release_bound(BindCtx *b)
{
    for (int i = 0; i < b->nbound; i++) b->bound[i]->lpVtbl->Release(b->bound[i]);
    b->nbound = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bc_revoke_param(BindCtx *b, LPOLESTR key);
static ULONG STDMETHODCALLTYPE bc_release(BindCtx *b)
{
    LONG r = InterlockedDecrement(&b->refs);
    if (!r) {
        bc_release_bound(b);
        while (b->nparams) bc_revoke_param(b, b->params[0].key);
        bc_free(b);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE bc_register_bound(BindCtx *b, IUnknown *o)
{
    if (!o) return E_INVALIDARG;
    if (b->nbound == MAX_BOUND) return E_OUTOFMEMORY_;
    o->lpVtbl->AddRef(o);
    b->bound[b->nbound++] = o;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bc_revoke_bound(BindCtx *b, IUnknown *o)
{
    for (int i = 0; i < b->nbound; i++)
        if (b->bound[i] == o) {
            o->lpVtbl->Release(o);
            b->bound[i] = b->bound[--b->nbound];
            return S_OK;
        }
    return MK_E_UNAVAILABLE_;
}
static HRESULT STDMETHODCALLTYPE bc_set_opts(BindCtx *b, void *opts)
{
    if (!opts) return E_INVALIDARG;
    DWORD cb = *(DWORD *)opts;
    if (cb < 16 || cb > OPTS_MAX) return E_INVALIDARG;
    CopyMemory(b->opts, opts, cb);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bc_get_opts(BindCtx *b, void *opts)
{
    if (!opts) return E_INVALIDARG;
    DWORD cb = *(DWORD *)opts;
    if (cb < 16) return E_INVALIDARG;
    if (cb > OPTS_MAX) cb = OPTS_MAX;
    CopyMemory((BYTE *)opts + 4, b->opts + 4, cb - 4);  /* keep the caller's cbStruct */
    return S_OK;
}
/* NovaOS keeps no running object table */
static HRESULT STDMETHODCALLTYPE bc_get_rot(BindCtx *b, void **rot)
{
    (void)b;
    if (rot) *rot = 0;
    return E_NOTIMPL;
}
static int find_param(BindCtx *b, LPOLESTR key)
{
    for (int i = 0; key && i < b->nparams; i++) if (!lstrcmpW(b->params[i].key, key)) return i;
    return -1;
}
static HRESULT STDMETHODCALLTYPE bc_register_param(BindCtx *b, LPOLESTR key, IUnknown *o)
{
    if (!key || !o) return E_INVALIDARG;
    int i = find_param(b, key);
    if (i >= 0) {
        o->lpVtbl->AddRef(o);
        b->params[i].obj->lpVtbl->Release(b->params[i].obj);
        b->params[i].obj = o;
        return S_OK;
    }
    if (b->nparams == MAX_PARAMS) return E_OUTOFMEMORY_;
    int n = lstrlenW(key) + 1;
    WCHAR *k = bc_alloc(n * sizeof(WCHAR));
    if (!k) return E_OUTOFMEMORY_;
    CopyMemory(k, key, n * sizeof(WCHAR));
    o->lpVtbl->AddRef(o);
    b->params[b->nparams].key = k;
    b->params[b->nparams].obj = o;
    b->nparams++;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bc_get_param(BindCtx *b, LPOLESTR key, IUnknown **o)
{
    if (!o) return E_POINTER;
    *o = 0;
    int i = find_param(b, key);
    if (i < 0) return E_FAIL;
    *o = b->params[i].obj;
    (*o)->lpVtbl->AddRef(*o);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bc_enum_param(BindCtx *b, void **e)
{
    (void)b;
    if (e) *e = 0;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE bc_revoke_param(BindCtx *b, LPOLESTR key)
{
    int i = find_param(b, key);
    if (i < 0) return S_FALSE;
    b->params[i].obj->lpVtbl->Release(b->params[i].obj);
    bc_free(b->params[i].key);
    b->params[i] = b->params[--b->nparams];
    return S_OK;
}

static const BindCtxVtbl g_bc_vtbl = {
    bc_qi, bc_addref, bc_release, bc_register_bound, bc_revoke_bound, bc_release_bound, bc_set_opts, bc_get_opts,
    bc_get_rot, bc_register_param, bc_get_param, bc_enum_param, bc_revoke_param,
};

OLEAPI HRESULT WINAPI CreateBindCtx(DWORD reserved, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (reserved) return E_INVALIDARG;
    BindCtx *b = bc_alloc(sizeof(*b));
    if (!b) return E_OUTOFMEMORY_;
    b->lpVtbl = &g_bc_vtbl;
    b->refs = 1;
    DWORD *o = (DWORD *)b->opts;
    o[0] = 32;                                          /* cbStruct: a BIND_OPTS2 */
    o[2] = 0x12;                                        /* grfMode: STGM_READWRITE | STGM_SHARE_EXCLUSIVE */
    *out = b;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * Objects in streams
 * ----------------------------------------------------------------------- */
typedef struct PersistStream PersistStream;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(PersistStream *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(PersistStream *);
    ULONG (STDMETHODCALLTYPE *Release)(PersistStream *);
    HRESULT (STDMETHODCALLTYPE *GetClassID)(PersistStream *, CLSID *);
    HRESULT (STDMETHODCALLTYPE *IsDirty)(PersistStream *);
    HRESULT (STDMETHODCALLTYPE *Load)(PersistStream *, IStream *);
    HRESULT (STDMETHODCALLTYPE *Save)(PersistStream *, IStream *, BOOL);
    HRESULT (STDMETHODCALLTYPE *GetSizeMax)(PersistStream *, ULARGE_INTEGER *);
} PersistStreamVtbl;
struct PersistStream { const PersistStreamVtbl *lpVtbl; };

OLEAPI HRESULT WINAPI WriteClassStm(IStream *stm, REFCLSID clsid)
{
    if (!stm || !clsid) return E_INVALIDARG;
    return stm->lpVtbl->Write(stm, clsid, sizeof(CLSID), 0);
}

OLEAPI HRESULT WINAPI ReadClassStm(IStream *stm, CLSID *clsid)
{
    ULONG n = 0;
    if (!stm || !clsid) return E_INVALIDARG;
    HRESULT hr = stm->lpVtbl->Read(stm, clsid, sizeof(CLSID), &n);
    if (FAILED(hr)) return hr;
    return n == sizeof(CLSID) ? S_OK : STG_E_READFAULT_;
}

OLEAPI HRESULT WINAPI OleSaveToStream(PersistStream *obj, IStream *stm)
{
    if (!stm) return E_INVALIDARG;
    if (!obj) return WriteClassStm(stm, &CLSID_NULL_);
    CLSID clsid;
    HRESULT hr = obj->lpVtbl->GetClassID(obj, &clsid);
    if (SUCCEEDED(hr)) hr = WriteClassStm(stm, &clsid);
    if (SUCCEEDED(hr)) hr = obj->lpVtbl->Save(obj, stm, TRUE);
    return hr;
}

OLEAPI HRESULT WINAPI OleLoadFromStream(IStream *stm, REFIID riid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    CLSID clsid;
    HRESULT hr = ReadClassStm(stm, &clsid);
    if (FAILED(hr)) return hr;
    PersistStream *ps = 0;
    hr = CoCreateInstance(&clsid, 0, CLSCTX_ALL, &IID_IPersistStream_, (void **)&ps);
    if (FAILED(hr)) return hr;
    hr = ps->lpVtbl->Load(ps, stm);
    if (SUCCEEDED(hr)) hr = ps->lpVtbl->QueryInterface(ps, riid, out);
    ps->lpVtbl->Release(ps);
    return hr;
}
