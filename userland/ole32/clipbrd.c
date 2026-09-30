/*
 * clipbrd.c — the OLE clipboard over the system clipboard
 *
 * OleSetClipboard copies every HGLOBAL format the data object offers onto
 * the clipboard (keeping the object until the clipboard changes, for
 * OleIsCurrentClipboard); OleGetClipboard returns a data object that reads
 * the clipboard, whoever put the data there.
 */
#define NOVA_BUILD_OLE32
#include <windows.h>
#include <objbase.h>

static IDataObject *g_current;          /* what OleSetClipboard was given */
static DWORD g_current_seq;

/* -----------------------------------------------------------------------
 * A list of formats as an IEnumFORMATETC
 * ----------------------------------------------------------------------- */
typedef struct { IEnumFORMATETC iface; LONG refs; ULONG pos, n; FORMATETC f[40]; } FmtEnum;

static HRESULT STDMETHODCALLTYPE en_qi(IEnumFORMATETC *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumFORMATETC)) { *out = This; This->lpVtbl->AddRef(This); return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE en_addref(IEnumFORMATETC *This) { return (ULONG)InterlockedIncrement(&((FmtEnum *)This)->refs); }
static ULONG STDMETHODCALLTYPE en_release(IEnumFORMATETC *This)
{
    LONG r = InterlockedDecrement(&((FmtEnum *)This)->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, This);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE en_next(IEnumFORMATETC *This, ULONG n, FORMATETC *out, ULONG *fetched)
{
    FmtEnum *e = (FmtEnum *)This;
    ULONG k = 0;
    while (k < n && e->pos < e->n) out[k++] = e->f[e->pos++];
    if (fetched) *fetched = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE en_skip(IEnumFORMATETC *This, ULONG n)
{
    FmtEnum *e = (FmtEnum *)This;
    e->pos += n;
    if (e->pos > e->n) { e->pos = e->n; return S_FALSE; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE en_reset(IEnumFORMATETC *This) { ((FmtEnum *)This)->pos = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE en_clone(IEnumFORMATETC *This, IEnumFORMATETC **out);
static const IEnumFORMATETCVtbl g_en_vtbl = { en_qi, en_addref, en_release, en_next, en_skip, en_reset, en_clone };
static HRESULT STDMETHODCALLTYPE en_clone(IEnumFORMATETC *This, IEnumFORMATETC **out)
{
    FmtEnum *c = HeapAlloc(GetProcessHeap(), 0, sizeof *c);
    if (!c) return E_OUTOFMEMORY;
    *c = *(FmtEnum *)This;
    c->refs = 1;
    *out = &c->iface;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * The clipboard as a data object
 * ----------------------------------------------------------------------- */
typedef struct { IDataObject iface; LONG refs; } ClipData;

static HRESULT STDMETHODCALLTYPE cd_qi(IDataObject *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) { *out = This; This->lpVtbl->AddRef(This); return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cd_addref(IDataObject *This) { return (ULONG)InterlockedIncrement(&((ClipData *)This)->refs); }
static ULONG STDMETHODCALLTYPE cd_release(IDataObject *This)
{
    LONG r = InterlockedDecrement(&((ClipData *)This)->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, This);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE cd_getdata(IDataObject *This, FORMATETC *fmt, STGMEDIUM *m)
{
    (void)This;
    if (!fmt || !m) return E_INVALIDARG;
    if (!(fmt->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
    if (!OpenClipboard(NULL)) return CLIPBRD_E_CANT_OPEN;
    HANDLE src = GetClipboardData(fmt->cfFormat);
    HRESULT hr = DV_E_FORMATETC;
    if (src) {
        SIZE_T n = GlobalSize(src);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1);
        if (!g) hr = E_OUTOFMEMORY;
        else {
            CopyMemory(GlobalLock(g), GlobalLock(src), n);
            GlobalUnlock(g);
            GlobalUnlock(src);
            m->tymed = TYMED_HGLOBAL;
            m->hGlobal = g;
            m->pUnkForRelease = NULL;
            hr = S_OK;
        }
    }
    CloseClipboard();
    return hr;
}
static HRESULT STDMETHODCALLTYPE cd_getdatahere(IDataObject *This, FORMATETC *fmt, STGMEDIUM *m) { (void)This; (void)fmt; (void)m; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE cd_querygetdata(IDataObject *This, FORMATETC *fmt)
{
    (void)This;
    if (!fmt) return E_INVALIDARG;
    if (!(fmt->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
    return IsClipboardFormatAvailable(fmt->cfFormat) ? S_OK : DV_E_FORMATETC;
}
static HRESULT STDMETHODCALLTYPE cd_canonical(IDataObject *This, FORMATETC *in, FORMATETC *out) { (void)This; if (out) { *out = *in; out->ptd = NULL; } return DATA_S_SAMEFORMATETC; }
static HRESULT STDMETHODCALLTYPE cd_setdata(IDataObject *This, FORMATETC *fmt, STGMEDIUM *m, BOOL rel) { (void)This; (void)fmt; (void)m; (void)rel; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE cd_enum(IDataObject *This, DWORD dir, IEnumFORMATETC **out)
{
    (void)This;
    if (!out) return E_POINTER;
    *out = NULL;
    if (dir != DATADIR_GET) return E_NOTIMPL;
    FmtEnum *e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *e);
    if (!e) return E_OUTOFMEMORY;
    e->iface.lpVtbl = &g_en_vtbl;
    e->refs = 1;
    for (UINT f = EnumClipboardFormats(0); f && e->n < 40; f = EnumClipboardFormats(f)) {
        FORMATETC *x = &e->f[e->n++];
        x->cfFormat = (CLIPFORMAT)f;
        x->ptd = NULL;
        x->dwAspect = DVASPECT_CONTENT;
        x->lindex = -1;
        x->tymed = f == CF_BITMAP ? TYMED_GDI : TYMED_HGLOBAL;
    }
    *out = &e->iface;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cd_dadvise(IDataObject *This, FORMATETC *f, DWORD a, IAdviseSink *s, DWORD *c) { (void)This; (void)f; (void)a; (void)s; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE cd_dunadvise(IDataObject *This, DWORD c) { (void)This; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE cd_enumdadvise(IDataObject *This, IEnumSTATDATA **out) { (void)This; if (out) *out = NULL; return OLE_E_ADVISENOTSUPPORTED; }
static const IDataObjectVtbl g_cd_vtbl = { cd_qi, cd_addref, cd_release, cd_getdata, cd_getdatahere, cd_querygetdata, cd_canonical, cd_setdata, cd_enum, cd_dadvise, cd_dunadvise, cd_enumdadvise };

/* -----------------------------------------------------------------------
 * The API
 * ----------------------------------------------------------------------- */
static void forget_current(void)
{
    if (g_current) g_current->lpVtbl->Release(g_current);
    g_current = NULL;
}

WINOLEAPI_(HRESULT) OleSetClipboard(LPDATAOBJECT obj)
{
    /* take the object's data first: it may be reading the clipboard itself */
    struct { CLIPFORMAT fmt; HANDLE h; } items[32];
    int n = 0;
    if (obj) {
        IEnumFORMATETC *en = NULL;
        if (SUCCEEDED(obj->lpVtbl->EnumFormatEtc(obj, DATADIR_GET, &en)) && en) {
            FORMATETC f;
            ULONG got;
            while (n < 32 && en->lpVtbl->Next(en, 1, &f, &got) == S_OK && got) {
                if (!(f.tymed & (TYMED_HGLOBAL | TYMED_GDI))) continue;
                FORMATETC want = f;
                want.tymed = f.tymed & TYMED_HGLOBAL ? TYMED_HGLOBAL : TYMED_GDI;
                STGMEDIUM m = { 0 };
                if (FAILED(obj->lpVtbl->GetData(obj, &want, &m))) continue;
                if (m.tymed == TYMED_HGLOBAL && m.hGlobal) {
                    SIZE_T k = GlobalSize(m.hGlobal);
                    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, k ? k : 1);
                    if (g) {
                        CopyMemory(GlobalLock(g), GlobalLock(m.hGlobal), k);
                        GlobalUnlock(g);
                        GlobalUnlock(m.hGlobal);
                        items[n].fmt = f.cfFormat;
                        items[n++].h = g;
                    }
                } else if (m.tymed == TYMED_GDI && m.hBitmap && f.cfFormat == CF_BITMAP) {
                    items[n].fmt = CF_BITMAP;            /* (sent as a DIB) */
                    items[n++].h = m.hBitmap;
                    m.hBitmap = NULL;
                }
                ReleaseStgMedium(&m);
            }
            en->lpVtbl->Release(en);
        }
    }
    if (!OpenClipboard(NULL)) {
        for (int i = 0; i < n; i++) { if (items[i].fmt != CF_BITMAP) GlobalFree(items[i].h); }
        return CLIPBRD_E_CANT_OPEN;
    }
    EmptyClipboard();
    forget_current();
    for (int i = 0; i < n; i++)
        if (!SetClipboardData(items[i].fmt, items[i].h)) { if (items[i].fmt != CF_BITMAP) GlobalFree(items[i].h); }
    if (obj) {
        obj->lpVtbl->AddRef(obj);
        g_current = obj;
    }
    CloseClipboard();
    g_current_seq = GetClipboardSequenceNumber();
    return S_OK;
}

WINOLEAPI_(HRESULT) OleGetClipboard(LPDATAOBJECT *out)
{
    if (!out) return E_INVALIDARG;
    ClipData *d = HeapAlloc(GetProcessHeap(), 0, sizeof *d);
    if (!d) { *out = NULL; return E_OUTOFMEMORY; }
    d->iface.lpVtbl = &g_cd_vtbl;
    d->refs = 1;
    *out = &d->iface;
    return S_OK;
}

WINOLEAPI_(HRESULT) OleFlushClipboard(void)
{
    forget_current();                   /* the data is on the clipboard already */
    return S_OK;
}

WINOLEAPI_(HRESULT) OleIsCurrentClipboard(LPDATAOBJECT obj)
{
    if (g_current && g_current_seq != GetClipboardSequenceNumber()) forget_current();
    return obj && obj == g_current ? S_OK : S_FALSE;
}
