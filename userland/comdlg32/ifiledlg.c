/*
 * ifiledlg.c — the Vista-style file dialogs as COM objects:
 * CLSID_FileOpenDialog (IFileOpenDialog) and CLSID_FileSaveDialog
 * (IFileSaveDialog), which most current programs create instead of
 * calling GetOpenFileName.  Both show the dialog of filedlg.c, take file
 * types, options, folders and names, hand the choice back as shell items
 * (shell32's IShellItem), and call the program's IFileDialogEvents.
 * IFileDialogCustomize is accepted and remembers what is set, but adds no
 * controls to the dialog.
 */

#include <windows.h>
#include <objbase.h>
#include "filedlg.h"

#define CDAPI __declspec(dllexport)

#define S_OK_          ((HRESULT)0)
#define S_FALSE_       ((HRESULT)1)
#define E_FAIL_        ((HRESULT)0x80004005L)
#define E_INVALIDARG_  ((HRESULT)0x80070057L)
#define E_NOINTERFACE_ ((HRESULT)0x80004002L)
#define E_OUTOFMEMORY_ ((HRESULT)0x8007000EL)
#define E_NOTIMPL_     ((HRESULT)0x80004001L)
#define E_UNEXPECTED_  ((HRESULT)0x8000FFFFL)
#define HR_CANCELLED   ((HRESULT)0x800704C7L)       /* HRESULT_FROM_WIN32(ERROR_CANCELLED) */
#define CLASS_E_NOAGGREGATION_      ((HRESULT)0x80040110L)
#define CLASS_E_CLASSNOTAVAILABLE_  ((HRESULT)0x80040111L)

typedef struct { WORD cb; BYTE abID[1]; } SHITEMID;
typedef struct { SHITEMID mkid; } ITEMIDLIST, *LPITEMIDLIST;
__declspec(dllimport) LPITEMIDLIST WINAPI ILCreateFromPathW(LPCWSTR path);
__declspec(dllimport) void WINAPI ILFree(LPITEMIDLIST pidl);
__declspec(dllimport) HRESULT WINAPI SHCreateItemFromIDList(const ITEMIDLIST *pidl, REFIID riid, void **out);
__declspec(dllimport) HRESULT WINAPI SHCreateShellItemArrayFromIDLists(UINT n, const ITEMIDLIST **pidls, void **out);

static const GUID IID_IUnknown_         = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IClassFactory_    = { 0x00000001, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IModalWindow_     = { 0xB4DB1657, 0x70D7, 0x485E, { 0x8E, 0x3E, 0x6F, 0xCB, 0x5A, 0x5C, 0x18, 0x02 } };
static const GUID IID_IFileDialog_      = { 0x42F85136, 0xDB7E, 0x439C, { 0x85, 0xF1, 0xE4, 0x07, 0x5D, 0x13, 0x5F, 0xC8 } };
static const GUID IID_IFileOpenDialog_  = { 0xD57C7288, 0xD4AD, 0x4768, { 0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60 } };
static const GUID IID_IFileSaveDialog_  = { 0x84BCCD23, 0x5FDE, 0x4CDB, { 0xAE, 0xA4, 0xAF, 0x64, 0xB8, 0x3D, 0x78, 0xAB } };
static const GUID IID_IFileDialogCustomize_ = { 0xE6FDD21A, 0x163F, 0x4975, { 0x9C, 0x8C, 0xA6, 0x9F, 0x1B, 0xA3, 0x70, 0x34 } };
static const GUID IID_IShellItem_       = { 0x43826D1E, 0xE718, 0x42EE, { 0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE } };
static const GUID CLSID_FileOpenDialog_ = { 0xDC1C5A9C, 0xE88A, 0x4DDE, { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
static const GUID CLSID_FileSaveDialog_ = { 0xC0B4E2F3, 0xBA21, 0x4773, { 0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B } };

static int same_guid(const GUID *a, const GUID *b)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void wcopy(WCHAR *d, const WCHAR *s, int cap)
{
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

/* task memory (CoTaskMemAlloc is the process heap in NovaOS) */
static HRESULT dup_task(const WCHAR *s, LPWSTR *out)
{
    int n = wlen(s);
    WCHAR *d = HeapAlloc(GetProcessHeap(), 0, 2 * (SIZE_T)(n + 1));
    if (!d) { *out = 0; return E_OUTOFMEMORY_; }
    for (int i = 0; i <= n; i++) d[i] = s[i];
    *out = d;
    return S_OK_;
}

/* a shell item for a path (which need not exist: a name to save under) */
static HRESULT item_for(const WCHAR *path, void **out)
{
    *out = 0;
    LPITEMIDLIST p = ILCreateFromPathW(path);
    if (!p) return E_OUTOFMEMORY_;
    HRESULT hr = SHCreateItemFromIDList(p, &IID_IShellItem_, out);
    ILFree(p);
    return hr;
}

/* the file-system path of a program's IShellItem: GetDisplayName(SIGDN_FILESYSPATH) */
typedef HRESULT (STDMETHODCALLTYPE *GetName_)(void *, DWORD, LPWSTR *);
static BOOL item_path(void *si, WCHAR *out)
{
    if (!si) return FALSE;
    LPWSTR s = 0;
    GetName_ fn = (GetName_)(*(void ***)si)[5];
    if (fn(si, 0x80058000 /* SIGDN_FILESYSPATH */, &s) || !s) return FALSE;
    wcopy(out, s, MAX_PATH);
    HeapFree(GetProcessHeap(), 0, s);
    return TRUE;
}

static void unk_addref(void *p) { if (p) ((ULONG (STDMETHODCALLTYPE *)(void *))(*(void ***)p)[1])(p); }
static void unk_release(void *p) { if (p) ((ULONG (STDMETHODCALLTYPE *)(void *))(*(void ***)p)[2])(p); }

/* -----------------------------------------------------------------------
 * The object
 * ----------------------------------------------------------------------- */
#define MAX_SINKS 8
#define MAX_CTLS  64

typedef struct Dlg Dlg;
typedef struct { const void *vtbl; Dlg *self; } Face;
struct Dlg {
    Face fd, cust;                      /* IFileOpenDialog or IFileSaveDialog; IFileDialogCustomize */
    volatile LONG refs;
    BOOL save, showing;
    FileDlg d;
    FdFilter *types;
    UINT ntypes;
    WCHAR deffolder[MAX_PATH], folder[MAX_PATH];
    void *sinks[MAX_SINKS];             /* IFileDialogEvents, by cookie - 1 */
    HRESULT close_hr;
    struct { DWORD id, state; } ctl[MAX_CTLS];
    int nctl;
};

#define DLG(face) (((Face *)(face))->self)

static void free_types(Dlg *o)
{
    for (UINT i = 0; i < o->ntypes; i++) {
        HeapFree(GetProcessHeap(), 0, o->types[i].name);
        HeapFree(GetProcessHeap(), 0, o->types[i].spec);
    }
    HeapFree(GetProcessHeap(), 0, o->types);
    o->types = 0;
    o->ntypes = 0;
}

static HRESULT STDMETHODCALLTYPE fd_qi(Face *f, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    Dlg *o = f->self;
    if (same_guid(riid, &IID_IUnknown_) || same_guid(riid, &IID_IModalWindow_) || same_guid(riid, &IID_IFileDialog_) ||
        same_guid(riid, o->save ? &IID_IFileSaveDialog_ : &IID_IFileOpenDialog_)) *ppv = &o->fd;
    else if (same_guid(riid, &IID_IFileDialogCustomize_)) *ppv = &o->cust;
    else { *ppv = 0; return E_NOINTERFACE_; }
    InterlockedIncrement(&o->refs);
    return S_OK_;
}
static ULONG STDMETHODCALLTYPE fd_addref(Face *f) { return (ULONG)InterlockedIncrement(&f->self->refs); }
static ULONG STDMETHODCALLTYPE fd_release(Face *f)
{
    Dlg *o = f->self;
    LONG r = InterlockedDecrement(&o->refs);
    if (!r) {
        for (int i = 0; i < MAX_SINKS; i++) unk_release(o->sinks[i]);
        free_types(o);
        HeapFree(GetProcessHeap(), 0, o);
    }
    return (ULONG)r;
}

/* IFileDialogEvents: 3 OnFileOk, 4 OnFolderChanging, 5 OnFolderChange, 6 OnSelectionChange, 8 OnTypeChange */
static HRESULT notify(Dlg *o, int slot)
{
    HRESULT result = S_OK_;
    for (int i = 0; i < MAX_SINKS; i++) {
        void *s = o->sinks[i];
        if (!s) continue;
        HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void *))(*(void ***)s)[slot])(s, &o->fd);
        if (slot == 3 && hr != S_OK_) result = hr;
    }
    return result;
}

static BOOL ev_ok(FileDlg *d) { return notify((Dlg *)d->ctx, 3) == S_OK_; }
static void ev_folder(FileDlg *d) { notify((Dlg *)d->ctx, 5); }
static void ev_sel(FileDlg *d) { notify((Dlg *)d->ctx, 6); }
static void ev_type(FileDlg *d) { notify((Dlg *)d->ctx, 8); }

static HRESULT STDMETHODCALLTYPE fd_show(Face *f, HWND owner)
{
    Dlg *o = f->self;
    if (o->showing) return E_UNEXPECTED_;
    FileDlg *d = &o->d;
    d->owner = owner;
    d->save = o->save;
    d->filters = o->types;
    d->nfilters = (int)o->ntypes;
    d->on_ok = ev_ok; d->on_folder = ev_folder; d->on_sel = ev_sel; d->on_type = ev_type;
    d->ctx = o;
    wcopy(d->dir, o->folder[0] ? o->folder : o->deffolder, MAX_PATH);
    o->close_hr = 0;
    o->showing = TRUE;
    fd_addref(f);
    BOOL ok = fd_run(d);
    o->showing = FALSE;
    HRESULT hr = ok ? S_OK_ : o->close_hr ? o->close_hr : HR_CANCELLED;
    if (ok) {
        /* the folder chosen in becomes the one shown next time; keep the name typed */
        wcopy(o->folder, d->res_dir, MAX_PATH);
        if (!(d->fos & FOS_NOCHANGEDIR) && d->res_dir[0]) SetCurrentDirectoryW(d->res_dir);
    }
    fd_release(f);
    return hr;
}

typedef struct { LPCWSTR pszName, pszSpec; } FILTERSPEC_;

static HRESULT STDMETHODCALLTYPE fd_settypes(Face *f, UINT n, const FILTERSPEC_ *spec)
{
    Dlg *o = f->self;
    if (!spec || !n) return E_INVALIDARG_;
    if (o->ntypes) return E_UNEXPECTED_;                    /* only once, as on Windows */
    o->types = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FdFilter) * n);
    if (!o->types) return E_OUTOFMEMORY_;
    for (UINT i = 0; i < n; i++) {
        LPWSTR a = 0, b = 0;
        dup_task(spec[i].pszName ? spec[i].pszName : L"", &a);
        dup_task(spec[i].pszSpec ? spec[i].pszSpec : L"*.*", &b);
        o->types[i].name = a;
        o->types[i].spec = b;
    }
    o->ntypes = n;
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE fd_settypeindex(Face *f, UINT i)
{
    Dlg *o = f->self;
    if (i < 1) i = 1;
    if (o->ntypes && i > o->ntypes) i = o->ntypes;
    fd_set_filter(&o->d, (int)i - 1);
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE fd_gettypeindex(Face *f, UINT *i)
{
    if (!i) return E_INVALIDARG_;
    *i = (UINT)f->self->d.filter + 1;
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE fd_advise(Face *f, void *sink, DWORD *cookie)
{
    Dlg *o = f->self;
    if (!sink || !cookie) return E_INVALIDARG_;
    for (int i = 0; i < MAX_SINKS; i++)
        if (!o->sinks[i]) { o->sinks[i] = sink; unk_addref(sink); *cookie = (DWORD)i + 1; return S_OK_; }
    return E_OUTOFMEMORY_;
}

static HRESULT STDMETHODCALLTYPE fd_unadvise(Face *f, DWORD cookie)
{
    Dlg *o = f->self;
    if (!cookie || cookie > MAX_SINKS || !o->sinks[cookie - 1]) return E_INVALIDARG_;
    unk_release(o->sinks[cookie - 1]);
    o->sinks[cookie - 1] = 0;
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE fd_setoptions(Face *f, DWORD fos) { f->self->d.fos = fos; return S_OK_; }
static HRESULT STDMETHODCALLTYPE fd_getoptions(Face *f, DWORD *fos) { if (!fos) return E_INVALIDARG_; *fos = f->self->d.fos; return S_OK_; }

static HRESULT STDMETHODCALLTYPE fd_setdeffolder(Face *f, void *si)
{
    WCHAR p[MAX_PATH];
    if (!item_path(si, p)) return E_INVALIDARG_;
    wcopy(f->self->deffolder, p, MAX_PATH);
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE fd_setfolder(Face *f, void *si)
{
    Dlg *o = f->self;
    WCHAR p[MAX_PATH];
    if (!item_path(si, p)) return E_INVALIDARG_;
    wcopy(o->folder, p, MAX_PATH);
    if (o->showing) fd_set_folder(&o->d, p);
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE fd_getfolder(Face *f, void **out)
{
    Dlg *o = f->self;
    if (!out) return E_INVALIDARG_;
    const WCHAR *p = o->showing ? o->d.cur : o->folder[0] ? o->folder : o->deffolder;
    WCHAR cwd[MAX_PATH];
    if (!p[0]) { GetCurrentDirectoryW(MAX_PATH, cwd); p = cwd; }
    return item_for(p, out);
}

/* the name in the file name box, as a full path */
static BOOL typed_path(Dlg *o, WCHAR *out)
{
    WCHAR name[MAX_PATH], tmp[MAX_PATH];
    fd_get_name(&o->d, name, MAX_PATH);
    if (!name[0]) return FALSE;
    BOOL abs = (name[0] && name[1] == ':') || name[0] == '\\';
    const WCHAR *dir = o->showing ? o->d.cur : o->folder;
    if (abs || !dir[0]) wcopy(tmp, name, MAX_PATH);
    else {
        int n = wlen(dir);
        wcopy(tmp, dir, MAX_PATH);
        if (n && tmp[n - 1] != '\\') tmp[n++] = '\\';
        wcopy(tmp + n, name, MAX_PATH - n);
    }
    return GetFullPathNameW(tmp, MAX_PATH, out, 0) != 0;
}

static HRESULT STDMETHODCALLTYPE fd_getcursel(Face *f, void **out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    WCHAR p[MAX_PATH];
    if (!typed_path(f->self, p)) return E_FAIL_;
    return item_for(p, out);
}

static HRESULT STDMETHODCALLTYPE fd_setfilename(Face *f, LPCWSTR name) { fd_set_name(&f->self->d, name); return S_OK_; }

static HRESULT STDMETHODCALLTYPE fd_getfilename(Face *f, LPWSTR *out)
{
    if (!out) return E_INVALIDARG_;
    WCHAR n[MAX_PATH];
    fd_get_name(&f->self->d, n, MAX_PATH);
    return dup_task(n, out);
}

static HRESULT STDMETHODCALLTYPE fd_settitle(Face *f, LPCWSTR t) { wcopy(f->self->d.title, t, 256); return S_OK_; }
static HRESULT STDMETHODCALLTYPE fd_setoklabel(Face *f, LPCWSTR t) { wcopy(f->self->d.ok_label, t, 64); return S_OK_; }
static HRESULT STDMETHODCALLTYPE fd_setnamelabel(Face *f, LPCWSTR t) { wcopy(f->self->d.name_label, t, 64); return S_OK_; }

static HRESULT STDMETHODCALLTYPE fd_getresult(Face *f, void **out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    WCHAR p[MAX_PATH];
    if (!fd_result(&f->self->d, 0, p)) return E_UNEXPECTED_;
    return item_for(p, out);
}

static HRESULT STDMETHODCALLTYPE fd_addplace(Face *f, void *si, int where) { (void)f; (void)si; (void)where; return S_OK_; }
static HRESULT STDMETHODCALLTYPE fd_setdefext(Face *f, LPCWSTR ext) { wcopy(f->self->d.defext, ext ? ext : L"", 32); return S_OK_; }

static HRESULT STDMETHODCALLTYPE fd_closem(Face *f, HRESULT hr)
{
    Dlg *o = f->self;
    if (!o->showing) return E_UNEXPECTED_;
    o->close_hr = hr ? hr : HR_CANCELLED;
    fd_close(&o->d, IDCANCEL);
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE fd_setclientguid(Face *f, REFGUID g) { (void)f; (void)g; return S_OK_; }
static HRESULT STDMETHODCALLTYPE fd_clearclientdata(Face *f) { (void)f; return S_OK_; }
static HRESULT STDMETHODCALLTYPE fd_setfilter(Face *f, void *filter) { (void)f; (void)filter; return S_OK_; }

/* IFileOpenDialog */
static HRESULT STDMETHODCALLTYPE fd_getresults(Face *f, void **out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    Dlg *o = f->self;
    int n = o->d.res_count;
    if (!n) return E_UNEXPECTED_;
    LPITEMIDLIST *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LPITEMIDLIST) * n);
    if (!p) return E_OUTOFMEMORY_;
    HRESULT hr = S_OK_;
    for (int i = 0; i < n && !hr; i++) {
        WCHAR path[MAX_PATH];
        fd_result(&o->d, i, path);
        if (!(p[i] = ILCreateFromPathW(path))) hr = E_OUTOFMEMORY_;
    }
    if (!hr) hr = SHCreateShellItemArrayFromIDLists((UINT)n, (const ITEMIDLIST **)p, out);
    for (int i = 0; i < n; i++) if (p[i]) ILFree(p[i]);
    HeapFree(GetProcessHeap(), 0, p);
    return hr;
}

static HRESULT STDMETHODCALLTYPE fd_getselected(Face *f, void **out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    WCHAR path[MAX_PATH];
    if (!typed_path(f->self, path)) return E_FAIL_;
    LPITEMIDLIST p = ILCreateFromPathW(path);
    if (!p) return E_OUTOFMEMORY_;
    const ITEMIDLIST *one = p;
    HRESULT hr = SHCreateShellItemArrayFromIDLists(1, &one, out);
    ILFree(p);
    return hr;
}

/* IFileSaveDialog */
static HRESULT STDMETHODCALLTYPE fd_setsaveas(Face *f, void *si)
{
    Dlg *o = f->self;
    WCHAR p[MAX_PATH];
    if (!item_path(si, p)) return E_INVALIDARG_;
    const WCHAR *l = p;
    for (const WCHAR *s = p; *s; s++) if (*s == '\\' && s[1]) l = s + 1;
    fd_set_name(&o->d, l);
    if (l > p) {
        int k = (int)(l - p) - 1;
        if (k < 3) k = 3;
        WCHAR dir[MAX_PATH];
        wcopy(dir, p, k + 1);
        wcopy(o->folder, dir, MAX_PATH);
    }
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE fd_setprops(Face *f, void *ps) { (void)f; (void)ps; return S_OK_; }
static HRESULT STDMETHODCALLTYPE fd_setcollected(Face *f, void *list, BOOL append) { (void)f; (void)list; (void)append; return S_OK_; }
static HRESULT STDMETHODCALLTYPE fd_getprops(Face *f, void **ps) { (void)f; if (ps) *ps = 0; return E_NOTIMPL_; }
static HRESULT STDMETHODCALLTYPE fd_applyprops(Face *f, void *si, void *ps, HWND h, void *sink)
{
    (void)f; (void)si; (void)ps; (void)h; (void)sink;
    return S_OK_;
}

#define FD_COMMON fd_qi, fd_addref, fd_release, fd_show, fd_settypes, fd_settypeindex, fd_gettypeindex, fd_advise, \
                  fd_unadvise, fd_setoptions, fd_getoptions, fd_setdeffolder, fd_setfolder, fd_getfolder, fd_getcursel, \
                  fd_setfilename, fd_getfilename, fd_settitle, fd_setoklabel, fd_setnamelabel, fd_getresult, fd_addplace, \
                  fd_setdefext, fd_closem, fd_setclientguid, fd_clearclientdata, fd_setfilter
static const void *const g_open_vtbl[] = { FD_COMMON, fd_getresults, fd_getselected };
static const void *const g_save_vtbl[] = { FD_COMMON, fd_setsaveas, fd_setprops, fd_setcollected, fd_getprops, fd_applyprops };

/* -----------------------------------------------------------------------
 * IFileDialogCustomize: remembers control states, shows nothing
 * ----------------------------------------------------------------------- */
static DWORD *ctl_state(Dlg *o, DWORD id, BOOL add)
{
    for (int i = 0; i < o->nctl; i++) if (o->ctl[i].id == id) return &o->ctl[i].state;
    if (!add || o->nctl == MAX_CTLS) return 0;
    o->ctl[o->nctl].id = id;
    o->ctl[o->nctl].state = 0;
    return &o->ctl[o->nctl++].state;
}

static HRESULT STDMETHODCALLTYPE cu_qi(Face *f, REFIID riid, void **ppv) { return fd_qi(&f->self->fd, riid, ppv); }
static ULONG STDMETHODCALLTYPE cu_addref(Face *f) { return fd_addref(&f->self->fd); }
static ULONG STDMETHODCALLTYPE cu_release(Face *f) { return fd_release(&f->self->fd); }
static HRESULT STDMETHODCALLTYPE cu_id(Face *f, DWORD id) { ctl_state(f->self, id, TRUE); return S_OK_; }
static HRESULT STDMETHODCALLTYPE cu_id_str(Face *f, DWORD id, LPCWSTR s) { (void)s; ctl_state(f->self, id, TRUE); return S_OK_; }
static HRESULT STDMETHODCALLTYPE cu_id_str_bool(Face *f, DWORD id, LPCWSTR s, BOOL b)
{
    (void)s;
    DWORD *st = ctl_state(f->self, id, TRUE);
    if (st) *st = (DWORD)b;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE cu_getstate(Face *f, DWORD id, DWORD *st)
{
    (void)f; (void)id;
    if (!st) return E_INVALIDARG_;
    *st = 3;                                                /* CDCS_ENABLEDVISIBLE */
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE cu_setstate(Face *f, DWORD id, DWORD st) { (void)f; (void)id; (void)st; return S_OK_; }
static HRESULT STDMETHODCALLTYPE cu_getedit(Face *f, DWORD id, WCHAR **out) { (void)f; (void)id; if (!out) return E_INVALIDARG_; return dup_task(L"", out); }
static HRESULT STDMETHODCALLTYPE cu_getcheck(Face *f, DWORD id, BOOL *b)
{
    if (!b) return E_INVALIDARG_;
    DWORD *st = ctl_state(f->self, id, FALSE);
    *b = st ? (BOOL)*st : FALSE;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE cu_setcheck(Face *f, DWORD id, BOOL b)
{
    DWORD *st = ctl_state(f->self, id, TRUE);
    if (st) *st = (DWORD)b;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE cu_additem(Face *f, DWORD id, DWORD item, LPCWSTR label) { (void)f; (void)id; (void)item; (void)label; return S_OK_; }
static HRESULT STDMETHODCALLTYPE cu_id_item(Face *f, DWORD id, DWORD item) { (void)f; (void)id; (void)item; return S_OK_; }
static HRESULT STDMETHODCALLTYPE cu_getitemstate(Face *f, DWORD id, DWORD item, DWORD *st)
{
    (void)f; (void)id; (void)item;
    if (!st) return E_INVALIDARG_;
    *st = 3;                                                /* CDCS_ENABLEDVISIBLE */
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE cu_setitemstate(Face *f, DWORD id, DWORD item, DWORD st) { (void)f; (void)id; (void)item; (void)st; return S_OK_; }
static HRESULT STDMETHODCALLTYPE cu_getselitem(Face *f, DWORD id, DWORD *item)
{
    if (!item) return E_INVALIDARG_;
    DWORD *st = ctl_state(f->self, id, FALSE);
    if (!st) return E_FAIL_;
    *item = *st;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE cu_setselitem(Face *f, DWORD id, DWORD item)
{
    DWORD *st = ctl_state(f->self, id, TRUE);
    if (st) *st = item;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE cu_none(Face *f) { (void)f; return S_OK_; }
static HRESULT STDMETHODCALLTYPE cu_setitemtext(Face *f, DWORD id, DWORD item, LPCWSTR text) { (void)f; (void)id; (void)item; (void)text; return S_OK_; }

static const void *const g_cust_vtbl[] = {
    cu_qi, cu_addref, cu_release,
    cu_id,              /* EnableOpenDropDown */
    cu_id_str,          /* AddMenu */
    cu_id_str,          /* AddPushButton */
    cu_id,              /* AddComboBox */
    cu_id,              /* AddRadioButtonList */
    cu_id_str_bool,     /* AddCheckButton */
    cu_id_str,          /* AddEditBox */
    cu_id,              /* AddSeparator */
    cu_id_str,          /* AddText */
    cu_id_str,          /* SetControlLabel */
    cu_getstate,        /* GetControlState */
    cu_setstate,        /* SetControlState */
    cu_getedit,         /* GetEditBoxText */
    cu_id_str,          /* SetEditBoxText */
    cu_getcheck,        /* GetCheckButtonState */
    cu_setcheck,        /* SetCheckButtonState */
    cu_additem,         /* AddControlItem */
    cu_id_item,         /* RemoveControlItem */
    cu_id,              /* RemoveAllControlItems */
    cu_getitemstate,    /* GetControlItemState */
    cu_setitemstate,    /* SetControlItemState */
    cu_getselitem,      /* GetSelectedControlItem */
    cu_setselitem,      /* SetSelectedControlItem */
    cu_id_str,          /* StartVisualGroup */
    cu_none,            /* EndVisualGroup */
    cu_id,              /* MakeProminent */
    cu_setitemtext,     /* SetControlItemText */
};

static HRESULT dlg_create(BOOL save, REFIID riid, void **ppv)
{
    Dlg *o = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *o);
    if (!o) return E_OUTOFMEMORY_;
    o->fd.vtbl = save ? g_save_vtbl : g_open_vtbl; o->fd.self = o;
    o->cust.vtbl = g_cust_vtbl; o->cust.self = o;
    o->refs = 1;
    o->save = save;
    o->d.fos = save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR
                    : FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR;
    HRESULT hr = fd_qi(&o->fd, riid, ppv);
    fd_release(&o->fd);
    return hr;
}

/* -----------------------------------------------------------------------
 * The class factories
 * ----------------------------------------------------------------------- */
typedef struct { const void *vtbl; BOOL save; } Factory;

static HRESULT STDMETHODCALLTYPE cf_qi(Factory *t, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    if (same_guid(riid, &IID_IUnknown_) || same_guid(riid, &IID_IClassFactory_)) { *ppv = t; return S_OK_; }
    *ppv = 0;
    return E_NOINTERFACE_;
}
static ULONG STDMETHODCALLTYPE cf_addref(Factory *t) { (void)t; return 2; }
static ULONG STDMETHODCALLTYPE cf_release(Factory *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE cf_create(Factory *t, void *outer, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    *ppv = 0;
    if (outer) return CLASS_E_NOAGGREGATION_;
    return dlg_create(t->save, riid, ppv);
}
static HRESULT STDMETHODCALLTYPE cf_lock(Factory *t, BOOL lock) { (void)t; (void)lock; return S_OK_; }

static const void *const g_cf_vtbl[] = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static Factory g_open_factory = { g_cf_vtbl, FALSE }, g_save_factory = { g_cf_vtbl, TRUE };

CDAPI HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    *ppv = 0;
    if (same_guid(clsid, &CLSID_FileOpenDialog_)) return cf_qi(&g_open_factory, riid, ppv);
    if (same_guid(clsid, &CLSID_FileSaveDialog_)) return cf_qi(&g_save_factory, riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE_;
}

CDAPI HRESULT STDAPICALLTYPE DllCanUnloadNow(void) { return S_FALSE_; }
