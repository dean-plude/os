/*
 * shellitem.c — shell items: IShellItem for a file or folder by its path,
 * and IShellItemArray, which the file dialogs (IFileOpenDialog) hand back
 * and programs build with SHCreateItemFromParsingName and friends.
 *
 * NovaOS has no namespace folders, so an item is a file-system path; the
 * item ID lists of shell32.c carry the same path.
 */

#define NOVA_BUILD_SHELL32
#include <windows.h>
#include <objbase.h>

#define S_OK_          ((HRESULT)0)
#define S_FALSE_       ((HRESULT)1)
#define E_FAIL_        ((HRESULT)0x80004005L)
#define E_INVALIDARG_  ((HRESULT)0x80070057L)
#define E_NOINTERFACE_ ((HRESULT)0x80004002L)
#define E_OUTOFMEMORY_ ((HRESULT)0x8007000EL)
#define E_NOTIMPL_     ((HRESULT)0x80004001L)
#define HR_WIN32(e)    ((HRESULT)(0x80070000L | (e)))
#define MK_E_NOOBJECT_ ((HRESULT)0x800401E5L)

typedef struct { WORD cb; BYTE abID[1]; } SHITEMID;
typedef struct { SHITEMID mkid; } ITEMIDLIST, *LPITEMIDLIST;
typedef const ITEMIDLIST *LPCITEMIDLIST;

SHSTDAPI_(LPITEMIDLIST) ILCreateFromPathW(LPCWSTR path);
SHSTDAPI_(BOOL) SHGetPathFromIDListW(LPCITEMIDLIST pidl, LPWSTR path);
SHSTDAPI_(HRESULT) SHGetKnownFolderPath(REFGUID id, DWORD flags, HANDLE token, LPWSTR *out);

/* SIGDN: how GetDisplayName names the item */
#define SIGDN_NORMALDISPLAY               0x00000000
#define SIGDN_PARENTRELATIVEPARSING       0x80018001
#define SIGDN_DESKTOPABSOLUTEPARSING      0x80028000
#define SIGDN_PARENTRELATIVEEDITING       0x80031001
#define SIGDN_DESKTOPABSOLUTEEDITING      0x8004C000
#define SIGDN_FILESYSPATH                 0x80058000
#define SIGDN_URL                         0x80068000
#define SIGDN_PARENTRELATIVEFORADDRESSBAR 0x8007C001
#define SIGDN_PARENTRELATIVE              0x80080001
#define SIGDN_PARENTRELATIVEFORUI         0x80094001

/* SFGAO attributes */
#define SFGAO_CANCOPY      0x00000001
#define SFGAO_CANMOVE      0x00000002
#define SFGAO_CANRENAME    0x00000010
#define SFGAO_CANDELETE    0x00000020
#define SFGAO_STREAM       0x00400000
#define SFGAO_READONLY     0x00040000
#define SFGAO_HIDDEN       0x00080000
#define SFGAO_FILESYSANCESTOR 0x10000000
#define SFGAO_FOLDER       0x20000000
#define SFGAO_FILESYSTEM   0x40000000
#define SFGAO_HASSUBFOLDER 0x80000000

/* SICHINT: how Compare compares */
#define SICHINT_CANONICAL  0x10000000

static const GUID IID_IUnknown_       = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IShellItem_     = { 0x43826D1E, 0xE718, 0x42EE, { 0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE } };
static const GUID IID_IShellItemArray_ = { 0xB63EA76D, 0x1F85, 0x456F, { 0xA1, 0x9C, 0x48, 0x15, 0x9E, 0xFA, 0x85, 0x8B } };
static const GUID IID_IPersistIDList_ = { 0x1079ACFC, 0x29BD, 0x11D3, { 0x8E, 0x0D, 0x00, 0xC0, 0x4F, 0x68, 0x37, 0xD5 } };
static const GUID IID_IEnumShellItems_ = { 0x70629033, 0xE363, 0x4A28, { 0xA5, 0x67, 0x0D, 0xB7, 0x80, 0x06, 0xE6, 0xD7 } };

static int same_guid(const GUID *a, const GUID *b)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }

/* a copy of S in task memory (CoTaskMemFree frees it: ole32's task memory is the process heap) */
static HRESULT dup_task(const WCHAR *s, LPWSTR *out)
{
    int n = wlen(s);
    WCHAR *d = HeapAlloc(GetProcessHeap(), 0, 2 * (SIZE_T)(n + 1));
    if (!d) { *out = 0; return E_OUTOFMEMORY_; }
    for (int i = 0; i <= n; i++) d[i] = s[i];
    *out = d;
    return S_OK_;
}

/* -----------------------------------------------------------------------
 * IShellItem
 * ----------------------------------------------------------------------- */
typedef struct Item Item;
typedef struct { const void *vtbl; Item *self; } Face;
struct Item {
    Face item, pid;                         /* IShellItem, IPersistIDList */
    volatile LONG refs;
    WCHAR path[MAX_PATH];                   /* "" = the desktop (the namespace root) */
};
typedef struct ItemVtbl ItemVtbl;
struct ItemVtbl {
    HRESULT (STDMETHODCALLTYPE *qi)(Face *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *addref)(Face *);
    ULONG (STDMETHODCALLTYPE *release)(Face *);
    HRESULT (STDMETHODCALLTYPE *bind)(Face *, void *, REFGUID, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *parent)(Face *, void **);
    HRESULT (STDMETHODCALLTYPE *name)(Face *, DWORD, LPWSTR *);
    HRESULT (STDMETHODCALLTYPE *attrs)(Face *, DWORD, DWORD *);
    HRESULT (STDMETHODCALLTYPE *compare)(Face *, void *, DWORD, int *);
};
static const ItemVtbl g_item_vtbl;

static HRESULT item_new(const WCHAR *path, REFIID riid, void **out);

static HRESULT STDMETHODCALLTYPE item_qi(Face *f, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    Item *it = f->self;
    if (same_guid(riid, &IID_IUnknown_) || same_guid(riid, &IID_IShellItem_)) *ppv = &it->item;
    else if (same_guid(riid, &IID_IPersistIDList_)) *ppv = &it->pid;
    else { *ppv = 0; return E_NOINTERFACE_; }
    InterlockedIncrement(&it->refs);
    return S_OK_;
}
static ULONG STDMETHODCALLTYPE item_addref(Face *f) { return (ULONG)InterlockedIncrement(&f->self->refs); }
static ULONG STDMETHODCALLTYPE item_release(Face *f)
{
    Item *it = f->self;
    LONG r = InterlockedDecrement(&it->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, it);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE item_bind(Face *f, void *bc, REFGUID bhid, REFIID riid, void **out)
{
    (void)f; (void)bc; (void)bhid; (void)riid;
    if (out) *out = 0;
    return MK_E_NOOBJECT_;                  /* no shell folders or stream handlers */
}

/* the parent folder: "C:\a\b" -> "C:\a", "C:\a" -> "C:\", "C:\" -> the desktop */
static void parent_of(const WCHAR *p, WCHAR *out)
{
    int n = wlen(p);
    if (n <= 3) { out[0] = 0; return; }
    while (n > 0 && p[n - 1] != '\\') n--;
    if (n > 0 && n - 1 >= 3) n--;            /* drop the separator, keep the root's */
    for (int i = 0; i < n; i++) out[i] = p[i];
    out[n] = 0;
}

static HRESULT STDMETHODCALLTYPE item_parent(Face *f, void **out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!f->self->path[0]) return MK_E_NOOBJECT_;
    WCHAR p[MAX_PATH];
    parent_of(f->self->path, p);
    return item_new(p, &IID_IShellItem_, out);
}

static const WCHAR *leaf(const WCHAR *p)
{
    const WCHAR *l = p;
    for (const WCHAR *s = p; *s; s++) if (*s == '\\' && s[1]) l = s + 1;
    return l;
}

static HRESULT STDMETHODCALLTYPE item_name(Face *f, DWORD sigdn, LPWSTR *out)
{
    if (!out) return E_INVALIDARG_;
    const WCHAR *p = f->self->path;
    switch (sigdn) {
    case SIGDN_FILESYSPATH:
        if (!p[0]) { *out = 0; return E_INVALIDARG_; }       /* the desktop has no path */
        return dup_task(p, out);
    case SIGDN_DESKTOPABSOLUTEPARSING:
    case SIGDN_DESKTOPABSOLUTEEDITING:
        return dup_task(p, out);
    case SIGDN_URL: {
        WCHAR u[MAX_PATH + 8] = L"file:///";
        int n = 8;
        for (int i = 0; p[i] && n < MAX_PATH + 7; i++) u[n++] = p[i] == '\\' ? '/' : p[i];
        u[n] = 0;
        return dup_task(u, out);
    }
    default:                                /* the display and parent-relative names */
        if (!p[0]) return dup_task(L"Desktop", out);
        return dup_task(wlen(p) <= 3 ? p : leaf(p), out);
    }
}

static DWORD attrs_of(const WCHAR *p)
{
    if (!p[0]) return SFGAO_FOLDER | SFGAO_FILESYSANCESTOR | SFGAO_HASSUBFOLDER;
    DWORD a = GetFileAttributesW(p), r = SFGAO_FILESYSTEM | SFGAO_CANCOPY;
    if (a == INVALID_FILE_ATTRIBUTES) return r;
    if (a & FILE_ATTRIBUTE_DIRECTORY) r |= SFGAO_FOLDER | SFGAO_FILESYSANCESTOR | SFGAO_HASSUBFOLDER;
    else r |= SFGAO_STREAM;
    if (wlen(p) > 3) r |= SFGAO_CANMOVE | SFGAO_CANRENAME | SFGAO_CANDELETE;
    if (a & FILE_ATTRIBUTE_READONLY) r |= SFGAO_READONLY;
    if (a & FILE_ATTRIBUTE_HIDDEN) r |= SFGAO_HIDDEN;
    return r;
}

static HRESULT STDMETHODCALLTYPE item_attrs(Face *f, DWORD mask, DWORD *out)
{
    if (!out) return E_INVALIDARG_;
    *out = attrs_of(f->self->path) & mask;
    return *out == mask ? S_OK_ : S_FALSE_;
}

/* the path of any shell item (ours, or another implementation's) */
static BOOL path_of_item(void *si, WCHAR *out)
{
    if (!si) return FALSE;
    if (*(const void **)si == &g_item_vtbl) {
        Item *it = ((Face *)si)->self;
        for (int i = 0; ; i++) if (!(out[i] = it->path[i])) break;
        return TRUE;
    }
    const ItemVtbl *v = *(const ItemVtbl **)si;
    LPWSTR s = 0;
    if (v->name((Face *)si, SIGDN_FILESYSPATH, &s) || !s) return FALSE;
    int n = wlen(s);
    if (n >= MAX_PATH) { HeapFree(GetProcessHeap(), 0, s); return FALSE; }
    for (int i = 0; i <= n; i++) out[i] = s[i];
    HeapFree(GetProcessHeap(), 0, s);
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE item_compare(Face *f, void *other, DWORD hint, int *order)
{
    (void)hint;
    if (!order) return E_INVALIDARG_;
    WCHAR o[MAX_PATH];
    if (!path_of_item(other, o)) return E_INVALIDARG_;
    *order = lstrcmpiW(f->self->path, o);
    return *order ? S_FALSE_ : S_OK_;
}

static const ItemVtbl g_item_vtbl = { item_qi, item_addref, item_release, item_bind, item_parent, item_name,
                                      item_attrs, item_compare };

/* IPersistIDList: GetClassID, SetIDList, GetIDList */
static HRESULT STDMETHODCALLTYPE pid_classid(Face *f, GUID *id)
{
    (void)f;
    static const GUID CLSID_ShellItem_ = { 0x9AC9FBE1, 0xE0A2, 0x4AD6, { 0xB4, 0xEE, 0xE2, 0x12, 0x01, 0x3E, 0xA9, 0x17 } };
    if (!id) return E_INVALIDARG_;
    *id = CLSID_ShellItem_;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE pid_set(Face *f, LPCITEMIDLIST pidl)
{
    WCHAR p[MAX_PATH];
    if (!pidl || !SHGetPathFromIDListW(pidl, p)) return E_INVALIDARG_;
    for (int i = 0; ; i++) if (!(f->self->path[i] = p[i])) break;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE pid_get(Face *f, LPITEMIDLIST *out)
{
    if (!out) return E_INVALIDARG_;
    *out = ILCreateFromPathW(f->self->path);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static const void *const g_pid_vtbl[] = { item_qi, item_addref, item_release, pid_classid, pid_set, pid_get };

static HRESULT item_new(const WCHAR *path, REFIID riid, void **out)
{
    Item *it = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *it);
    if (!it) return E_OUTOFMEMORY_;
    it->item.vtbl = &g_item_vtbl; it->item.self = it;
    it->pid.vtbl = g_pid_vtbl; it->pid.self = it;
    it->refs = 1;
    int n = wlen(path);
    if (n >= MAX_PATH) n = MAX_PATH - 1;
    for (int i = 0; i < n; i++) it->path[i] = path[i] == '/' ? '\\' : path[i];
    it->path[n] = 0;
    while (n > 3 && it->path[n - 1] == '\\') it->path[--n] = 0;
    HRESULT hr = item_qi(&it->item, riid, out);
    item_release(&it->item);
    return hr;
}

/* -----------------------------------------------------------------------
 * IShellItemArray and IEnumShellItems
 * ----------------------------------------------------------------------- */
typedef struct {
    const void *vtbl;
    volatile LONG refs;
    UINT n;
    void **items;                           /* IShellItem *, each holding a reference */
} Array;

typedef struct {
    const void *vtbl;
    volatile LONG refs;
    Array *a;
    UINT at;
} Enum;

static HRESULT STDMETHODCALLTYPE arr_qi(Array *a, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    if (!same_guid(riid, &IID_IUnknown_) && !same_guid(riid, &IID_IShellItemArray_)) { *ppv = 0; return E_NOINTERFACE_; }
    InterlockedIncrement(&a->refs);
    *ppv = a;
    return S_OK_;
}
static ULONG STDMETHODCALLTYPE arr_addref(Array *a) { return (ULONG)InterlockedIncrement(&a->refs); }
static ULONG STDMETHODCALLTYPE arr_release(Array *a)
{
    LONG r = InterlockedDecrement(&a->refs);
    if (!r) {
        for (UINT i = 0; i < a->n; i++) (*(const ItemVtbl **)a->items[i])->release(a->items[i]);
        HeapFree(GetProcessHeap(), 0, a->items);
        HeapFree(GetProcessHeap(), 0, a);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE arr_bind(Array *a, void *bc, REFGUID bhid, REFIID riid, void **out)
{
    (void)a; (void)bc; (void)bhid; (void)riid;
    if (out) *out = 0;
    return MK_E_NOOBJECT_;
}
static HRESULT STDMETHODCALLTYPE arr_props(Array *a, int flags, REFIID riid, void **out)
{
    (void)a; (void)flags; (void)riid;
    if (out) *out = 0;
    return E_NOTIMPL_;
}
static HRESULT STDMETHODCALLTYPE arr_descs(Array *a, const void *key, REFIID riid, void **out)
{
    (void)a; (void)key; (void)riid;
    if (out) *out = 0;
    return E_NOTIMPL_;
}
/* SIATTRIBFLAGS: 1 AND, 2 OR */
static HRESULT STDMETHODCALLTYPE arr_attrs(Array *a, int how, DWORD mask, DWORD *out)
{
    if (!out) return E_INVALIDARG_;
    DWORD r = how == 2 ? 0 : 0xFFFFFFFF;
    for (UINT i = 0; i < a->n; i++) {
        DWORD v = 0;
        (*(const ItemVtbl **)a->items[i])->attrs(a->items[i], mask, &v);
        r = how == 2 ? r | v : r & v;
    }
    *out = a->n ? r & mask : 0;
    return *out == mask ? S_OK_ : S_FALSE_;
}
static HRESULT STDMETHODCALLTYPE arr_count(Array *a, DWORD *n) { if (!n) return E_INVALIDARG_; *n = a->n; return S_OK_; }
static HRESULT STDMETHODCALLTYPE arr_at(Array *a, DWORD i, void **out)
{
    if (!out) return E_INVALIDARG_;
    if (i >= a->n) { *out = 0; return E_FAIL_; }
    (*(const ItemVtbl **)a->items[i])->addref(a->items[i]);
    *out = a->items[i];
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE arr_enum(Array *a, void **out);
static const void *const g_arr_vtbl[] = { arr_qi, arr_addref, arr_release, arr_bind, arr_props, arr_descs, arr_attrs,
                                          arr_count, arr_at, arr_enum };

static HRESULT STDMETHODCALLTYPE en_qi(Enum *e, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    if (!same_guid(riid, &IID_IUnknown_) && !same_guid(riid, &IID_IEnumShellItems_)) { *ppv = 0; return E_NOINTERFACE_; }
    InterlockedIncrement(&e->refs);
    *ppv = e;
    return S_OK_;
}
static ULONG STDMETHODCALLTYPE en_addref(Enum *e) { return (ULONG)InterlockedIncrement(&e->refs); }
static ULONG STDMETHODCALLTYPE en_release(Enum *e)
{
    LONG r = InterlockedDecrement(&e->refs);
    if (!r) { arr_release(e->a); HeapFree(GetProcessHeap(), 0, e); }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE en_next(Enum *e, ULONG want, void **out, ULONG *got)
{
    ULONG k = 0;
    while (k < want && e->at < e->a->n) arr_at(e->a, e->at++, &out[k++]);
    if (got) *got = k;
    return k == want ? S_OK_ : S_FALSE_;
}
static HRESULT STDMETHODCALLTYPE en_skip(Enum *e, ULONG n)
{
    e->at = e->at + n > e->a->n ? e->a->n : e->at + n;
    return e->at < e->a->n ? S_OK_ : S_FALSE_;
}
static HRESULT STDMETHODCALLTYPE en_reset(Enum *e) { e->at = 0; return S_OK_; }
static HRESULT STDMETHODCALLTYPE en_clone(Enum *e, void **out);
static const void *const g_en_vtbl[] = { en_qi, en_addref, en_release, en_next, en_skip, en_reset, en_clone };

static HRESULT enum_new(Array *a, UINT at, void **out)
{
    Enum *e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *e);
    if (!e) { *out = 0; return E_OUTOFMEMORY_; }
    e->vtbl = g_en_vtbl; e->refs = 1; e->a = a; e->at = at;
    arr_addref(a);
    *out = e;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE en_clone(Enum *e, void **out) { if (!out) return E_INVALIDARG_; return enum_new(e->a, e->at, out); }
static HRESULT STDMETHODCALLTYPE arr_enum(Array *a, void **out) { if (!out) return E_INVALIDARG_; return enum_new(a, 0, out); }

/* an array of N shell items (each AddRef'd) */
static HRESULT array_new(UINT n, void **items, REFIID riid, void **out)
{
    Array *a = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *a);
    void **v = HeapAlloc(GetProcessHeap(), 0, sizeof(void *) * (n ? n : 1));
    if (!a || !v) { HeapFree(GetProcessHeap(), 0, a); HeapFree(GetProcessHeap(), 0, v); return E_OUTOFMEMORY_; }
    a->vtbl = g_arr_vtbl; a->refs = 1; a->n = n; a->items = v;
    for (UINT i = 0; i < n; i++) { v[i] = items[i]; (*(const ItemVtbl **)v[i])->addref(v[i]); }
    HRESULT hr = arr_qi(a, riid, out);
    arr_release(a);
    return hr;
}

/* -----------------------------------------------------------------------
 * Exports
 * ----------------------------------------------------------------------- */
SHSTDAPI_(HRESULT) SHCreateItemFromParsingName(PCWSTR name, void *bc, REFIID riid, void **out)
{
    (void)bc;
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!name) return E_INVALIDARG_;
    WCHAR full[MAX_PATH];
    if (!name[0]) return item_new(L"", riid, out);         /* the desktop */
    if (!GetFullPathNameW(name, MAX_PATH, full, 0)) return HR_WIN32(GetLastError());
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return HR_WIN32(GetLastError());
    return item_new(full, riid, out);
}

SHSTDAPI_(HRESULT) SHCreateItemFromIDList(LPCITEMIDLIST pidl, REFIID riid, void **out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    WCHAR p[MAX_PATH];
    if (!pidl) return E_INVALIDARG_;
    if (!pidl->mkid.cb) return item_new(L"", riid, out);    /* the empty list: the desktop */
    if (!SHGetPathFromIDListW(pidl, p)) return E_INVALIDARG_;
    return item_new(p, riid, out);
}

/* SHCreateShellItem(parent pidl, parent folder, child pidl): our ID lists are absolute paths */
SHSTDAPI_(HRESULT) SHCreateShellItem(LPCITEMIDLIST parent, void *folder, LPCITEMIDLIST pidl, void **out)
{
    (void)parent; (void)folder;
    return SHCreateItemFromIDList(pidl, &IID_IShellItem_, out);
}

SHSTDAPI_(HRESULT) SHCreateItemFromRelativeName(void *parent, PCWSTR name, void *bc, REFIID riid, void **out)
{
    (void)bc;
    if (!out) return E_INVALIDARG_;
    *out = 0;
    WCHAR p[MAX_PATH], full[MAX_PATH];
    if (!name || !path_of_item(parent, p)) return E_INVALIDARG_;
    int n = wlen(p), k = wlen(name);
    if (n + 1 + k >= MAX_PATH) return E_INVALIDARG_;
    if (n && p[n - 1] != '\\') p[n++] = '\\';
    for (int i = 0; i <= k; i++) p[n + i] = name[i];
    if (!GetFullPathNameW(p, MAX_PATH, full, 0)) return E_INVALIDARG_;
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return HR_WIN32(GetLastError());
    return item_new(full, riid, out);
}

SHSTDAPI_(HRESULT) SHGetKnownFolderItem(REFGUID id, DWORD flags, HANDLE token, REFIID riid, void **out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    LPWSTR p;
    HRESULT hr = SHGetKnownFolderPath(id, flags, token, &p);
    if (hr) return hr;
    hr = item_new(p, riid, out);
    LocalFree(p);
    return hr;
}

SHSTDAPI_(HRESULT) SHGetIDListFromObject(void *obj, LPITEMIDLIST *out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    WCHAR p[MAX_PATH];
    if (!path_of_item(obj, p)) return E_NOINTERFACE_;
    *out = ILCreateFromPathW(p);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

SHSTDAPI_(HRESULT) SHGetNameFromIDList(LPCITEMIDLIST pidl, DWORD sigdn, LPWSTR *out)
{
    void *it;
    if (!out) return E_INVALIDARG_;
    *out = 0;
    HRESULT hr = SHCreateItemFromIDList(pidl, &IID_IShellItem_, &it);
    if (hr) return hr;
    hr = item_name(it, sigdn, out);
    item_release(it);
    return hr;
}

SHSTDAPI_(HRESULT) SHCreateShellItemArrayFromShellItem(void *item, REFIID riid, void **out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!item) return E_INVALIDARG_;
    return array_new(1, &item, riid, out);
}

SHSTDAPI_(HRESULT) SHCreateShellItemArrayFromIDLists(UINT n, LPCITEMIDLIST *pidls, void **out)
{
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!n || !pidls) return E_INVALIDARG_;
    void **items = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(void *) * n);
    if (!items) return E_OUTOFMEMORY_;
    HRESULT hr = S_OK_;
    for (UINT i = 0; i < n && !hr; i++) hr = SHCreateItemFromIDList(pidls[i], &IID_IShellItem_, &items[i]);
    if (!hr) hr = array_new(n, items, &IID_IShellItemArray_, out);
    for (UINT i = 0; i < n; i++) if (items[i]) item_release(items[i]);
    HeapFree(GetProcessHeap(), 0, items);
    return hr;
}

/* SHCreateShellItemArray(parent, folder, n, children): the children are absolute ID lists here */
SHSTDAPI_(HRESULT) SHCreateShellItemArray(LPCITEMIDLIST parent, void *folder, UINT n, LPCITEMIDLIST *children, void **out)
{
    (void)parent; (void)folder;
    return SHCreateShellItemArrayFromIDLists(n, children, out);
}

/* property stores (System.* properties of a file): not kept */
SHSTDAPI_(HRESULT) SHGetPropertyStoreFromParsingName(LPCWSTR path, void *bc, int flags, REFIID iid, void **out)
{
    (void)path; (void)bc; (void)flags; (void)iid;
    if (out) *out = NULL;
    return E_NOTIMPL;
}

/* the shell's context menu for items of a folder: no handlers to fill one */
SHSTDAPI_(HRESULT) CDefFolderMenu_Create2(const void *folder, HWND w, UINT n, const void **items, void *sf,
                                          void *cb, UINT nkeys, const HKEY *keys, void **out)
{
    (void)folder; (void)w; (void)n; (void)items; (void)sf; (void)cb; (void)nkeys; (void)keys;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
