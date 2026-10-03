/*
 * filestream.c — SHCreateStreamOnFile*: an IStream on a file (the handle
 * underneath), and the STRRET helpers (StrRetToStr/StrRetToBuf) shell
 * folders' GetDisplayNameOf answers are read with.
 */
#define NOVA_BUILD_SHLWAPI
#include <windows.h>
#include <objbase.h>

#define LWAPI __declspec(dllexport)
#define S_OK_          ((HRESULT)0)
#define E_INVALIDARG_  ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY_ ((HRESULT)0x8007000EL)
#define E_NOINTERFACE_ ((HRESULT)0x80004002L)
#define HR_WIN32(e)    ((HRESULT)(0x80070000L | ((e) & 0xFFFF)))

#define STGM_READ_       0x00000000
#define STGM_WRITE_      0x00000001
#define STGM_READWRITE_  0x00000002
#define STGM_SHARE_DENY_NONE_  0x00000040
#define STGM_SHARE_DENY_READ_  0x00000030
#define STGM_SHARE_DENY_WRITE_ 0x00000020
#define STGM_SHARE_EXCLUSIVE_  0x00000010
#define STGM_CREATE_     0x00001000
#define STGM_FAILIFTHERE_ 0x00000000
#define STGM_DELETEONRELEASE_ 0x04000000
#define STG_E_FILENOTFOUND_ ((HRESULT)0x80030002L)

WINBASEAPI BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER pos, DWORD how);
WINBASEAPI BOOL WINAPI SetEndOfFile(HANDLE h);
WINBASEAPI BOOL WINAPI GetFileSizeEx(HANDLE h, PLARGE_INTEGER size);
WINBASEAPI BOOL WINAPI FlushFileBuffers(HANDLE h);
WINBASEAPI BOOL WINAPI GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w);

typedef struct {
    IStream iface;
    LONG refs;
    HANDLE file;
    DWORD mode;
    WCHAR *name;
} FStream;

static HRESULT last_hr(void) { DWORD e = GetLastError(); return e ? HR_WIN32(e) : E_FAIL; }

static HRESULT STDMETHODCALLTYPE fs_qi(IStream *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IStream) || IsEqualIID(riid, &IID_ISequentialStream)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK_;
    }
    *ppv = 0;
    return E_NOINTERFACE_;
}
static ULONG STDMETHODCALLTYPE fs_addref(IStream *This) { return (ULONG)InterlockedIncrement(&((FStream *)This)->refs); }
static ULONG STDMETHODCALLTYPE fs_release(IStream *This)
{
    FStream *s = (FStream *)This;
    LONG r = InterlockedDecrement(&s->refs);
    if (!r) {
        CloseHandle(s->file);
        if (s->mode & STGM_DELETEONRELEASE_) DeleteFileW(s->name);
        LocalFree(s->name);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE fs_read(IStream *This, void *pv, ULONG cb, ULONG *read)
{
    DWORD n = 0;
    if (!ReadFile(((FStream *)This)->file, pv, cb, &n, 0)) { if (read) *read = 0; return last_hr(); }
    if (read) *read = n;
    return n < cb ? S_FALSE : S_OK_;
}
static HRESULT STDMETHODCALLTYPE fs_write(IStream *This, const void *pv, ULONG cb, ULONG *written)
{
    DWORD n = 0;
    if (!WriteFile(((FStream *)This)->file, pv, cb, &n, 0)) { if (written) *written = 0; return last_hr(); }
    if (written) *written = n;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE fs_seek(IStream *This, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *pos)
{
    LARGE_INTEGER np;
    if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
    if (!SetFilePointerEx(((FStream *)This)->file, move, &np, origin)) return last_hr();
    if (pos) pos->QuadPart = (ULONGLONG)np.QuadPart;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE fs_setsize(IStream *This, ULARGE_INTEGER size)
{
    FStream *s = (FStream *)This;
    LARGE_INTEGER zero = { { 0, 0 } }, cur, to;
    if (!SetFilePointerEx(s->file, zero, &cur, FILE_CURRENT)) return last_hr();
    to.QuadPart = (LONGLONG)size.QuadPart;
    if (!SetFilePointerEx(s->file, to, 0, FILE_BEGIN) || !SetEndOfFile(s->file)) return last_hr();
    SetFilePointerEx(s->file, cur, 0, FILE_BEGIN);
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE fs_copyto(IStream *This, IStream *to, ULARGE_INTEGER cb, ULARGE_INTEGER *read,
                                           ULARGE_INTEGER *written)
{
    BYTE buf[8192];
    ULONGLONG left = cb.QuadPart, rd = 0, wr = 0;
    HRESULT hr = S_OK_;
    while (left) {
        ULONG want = left > sizeof(buf) ? sizeof(buf) : (ULONG)left, got = 0, put = 0;
        hr = fs_read(This, buf, want, &got);
        if (FAILED(hr) || !got) break;
        rd += got;
        hr = to->lpVtbl->Write(to, buf, got, &put);
        wr += put;
        if (FAILED(hr) || put < got) break;
        left -= got;
    }
    if (read) read->QuadPart = rd;
    if (written) written->QuadPart = wr;
    return FAILED(hr) ? hr : S_OK_;
}
static HRESULT STDMETHODCALLTYPE fs_commit(IStream *This, DWORD flags)
{
    (void)flags;
    FlushFileBuffers(((FStream *)This)->file);
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE fs_revert(IStream *This) { (void)This; return S_OK_; }
static HRESULT STDMETHODCALLTYPE fs_lock(IStream *This, ULARGE_INTEGER off, ULARGE_INTEGER cb, DWORD type)
{
    (void)This; (void)off; (void)cb; (void)type;
    return STG_E_INVALIDFUNCTION;
}
static HRESULT STDMETHODCALLTYPE fs_stat(IStream *This, STATSTG *st, DWORD flags)
{
    FStream *s = (FStream *)This;
    LARGE_INTEGER size;
    if (!st) return STG_E_INVALIDPOINTER;
    ZeroMemory(st, sizeof(*st));
    if (!GetFileSizeEx(s->file, &size)) return last_hr();
    st->type = STGTY_STREAM;
    st->cbSize.QuadPart = (ULONGLONG)size.QuadPart;
    GetFileTime(s->file, &st->ctime, &st->atime, &st->mtime);
    st->grfMode = s->mode;
    if (!(flags & STATFLAG_NONAME)) {
        int n = lstrlenW(s->name) + 1;
        st->pwcsName = LocalAlloc(0, n * sizeof(WCHAR));    /* the process heap, like CoTaskMemAlloc */
        if (st->pwcsName) CopyMemory(st->pwcsName, s->name, n * sizeof(WCHAR));
    }
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE fs_clone(IStream *This, IStream **out)
{
    (void)This;
    if (out) *out = 0;
    return E_NOTIMPL;
}

static IStreamVtbl g_fs_vtbl = {
    fs_qi, fs_addref, fs_release, fs_read, fs_write, fs_seek, fs_setsize, fs_copyto, fs_commit, fs_revert,
    fs_lock, fs_lock, fs_stat, fs_clone,
};

LWAPI HRESULT WINAPI SHCreateStreamOnFileEx(LPCWSTR path, DWORD mode, DWORD attrs, BOOL create, IStream *tmpl,
                                            IStream **out)
{
    (void)tmpl;
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!path) return E_INVALIDARG_;
    DWORD access = (mode & 3) == STGM_WRITE_ ? GENERIC_WRITE : (mode & 3) == STGM_READWRITE_ ? GENERIC_READ | GENERIC_WRITE
                                                                                            : GENERIC_READ;
    DWORD share;
    switch (mode & 0x70) {
    case STGM_SHARE_EXCLUSIVE_:  share = 0; break;
    case STGM_SHARE_DENY_WRITE_: share = FILE_SHARE_READ; break;
    case STGM_SHARE_DENY_READ_:  share = FILE_SHARE_WRITE; break;
    default:                     share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE; break;
    }
    DWORD disp = (mode & STGM_CREATE_) ? CREATE_ALWAYS : create ? OPEN_ALWAYS : OPEN_EXISTING;
    if (create && !(mode & STGM_CREATE_) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        disp = OPEN_EXISTING;
    HANDLE h = CreateFileW(path, access, share, 0, disp, attrs ? attrs : FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return last_hr();
    FStream *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    int n = lstrlenW(path) + 1;
    WCHAR *name = LocalAlloc(0, n * sizeof(WCHAR));
    if (!s || !name) {
        CloseHandle(h);
        if (s) HeapFree(GetProcessHeap(), 0, s);
        if (name) LocalFree(name);
        return E_OUTOFMEMORY_;
    }
    CopyMemory(name, path, n * sizeof(WCHAR));
    s->iface.lpVtbl = &g_fs_vtbl;
    s->refs = 1;
    s->file = h;
    s->mode = mode;
    s->name = name;
    *out = &s->iface;
    return S_OK_;
}

LWAPI HRESULT WINAPI SHCreateStreamOnFileW(LPCWSTR path, DWORD mode, IStream **out)
{
    return SHCreateStreamOnFileEx(path, mode, 0, (mode & STGM_CREATE_) != 0, 0, out);
}

LWAPI HRESULT WINAPI SHCreateStreamOnFileA(LPCSTR path, DWORD mode, IStream **out)
{
    WCHAR w[MAX_PATH];
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!path || !MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH)) return E_INVALIDARG_;
    return SHCreateStreamOnFileW(w, mode, out);
}

/* -----------------------------------------------------------------------
 * STRRET
 * ----------------------------------------------------------------------- */
typedef struct {
    UINT uType;
    union { LPWSTR pOleStr; UINT uOffset; char cStr[260]; };
} STRRET_;
#define STRRET_WSTR   0
#define STRRET_OFFSET 1
#define STRRET_CSTR   2

static HRESULT dup_a(const char *s, LPWSTR *out)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, 0, 0);
    *out = LocalAlloc(0, (n > 0 ? n : 1) * sizeof(WCHAR));
    if (!*out) return E_OUTOFMEMORY_;
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, *out, n); else (*out)[0] = 0;
    return S_OK_;
}

LWAPI HRESULT WINAPI StrRetToStrW(STRRET_ *sr, const void *pidl, LPWSTR *out)
{
    if (!sr || !out) return E_INVALIDARG_;
    *out = 0;
    switch (sr->uType) {
    case STRRET_WSTR: {
        if (!sr->pOleStr) return E_INVALIDARG_;
        int n = lstrlenW(sr->pOleStr) + 1;
        *out = LocalAlloc(0, n * sizeof(WCHAR));
        if (!*out) return E_OUTOFMEMORY_;
        CopyMemory(*out, sr->pOleStr, n * sizeof(WCHAR));
        LocalFree(sr->pOleStr);                         /* the STRRET's string is consumed */
        sr->pOleStr = 0;
        return S_OK_;
    }
    case STRRET_CSTR:   return dup_a(sr->cStr, out);
    case STRRET_OFFSET: return pidl ? dup_a((const char *)pidl + sr->uOffset, out) : E_INVALIDARG_;
    }
    return E_INVALIDARG_;
}

LWAPI HRESULT WINAPI StrRetToStrA(STRRET_ *sr, const void *pidl, LPSTR *out)
{
    LPWSTR w;
    if (!out) return E_INVALIDARG_;
    *out = 0;
    HRESULT hr = StrRetToStrW(sr, pidl, &w);
    if (FAILED(hr)) return hr;
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, 0, 0, 0, 0);
    *out = LocalAlloc(0, n > 0 ? n : 1);
    if (*out) { if (n > 0) WideCharToMultiByte(CP_ACP, 0, w, -1, *out, n, 0, 0); else (*out)[0] = 0; }
    LocalFree(w);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

LWAPI HRESULT WINAPI StrRetToBufW(STRRET_ *sr, const void *pidl, LPWSTR buf, UINT cap)
{
    LPWSTR w;
    if (!buf || !cap) return E_INVALIDARG_;
    buf[0] = 0;
    HRESULT hr = StrRetToStrW(sr, pidl, &w);
    if (FAILED(hr)) return hr;
    lstrcpynW(buf, w, (int)cap);
    LocalFree(w);
    return S_OK_;
}

LWAPI HRESULT WINAPI StrRetToBufA(STRRET_ *sr, const void *pidl, LPSTR buf, UINT cap)
{
    LPSTR a;
    if (!buf || !cap) return E_INVALIDARG_;
    buf[0] = 0;
    HRESULT hr = StrRetToStrA(sr, pidl, &a);
    if (FAILED(hr)) return hr;
    lstrcpynA(buf, a, (int)cap);
    LocalFree(a);
    return S_OK_;
}

/* -----------------------------------------------------------------------
 * SHCreateMemStream: an IStream over a growing block of memory
 * ----------------------------------------------------------------------- */
typedef struct {
    IStream iface;
    LONG refs;
    BYTE *data;
    ULONG size, cap, pos;
} MStream;

static HRESULT STDMETHODCALLTYPE ms_qi(IStream *This, REFIID riid, void **ppv) { return fs_qi(This, riid, ppv); }
static ULONG STDMETHODCALLTYPE ms_addref(IStream *This) { return (ULONG)InterlockedIncrement(&((MStream *)This)->refs); }
static ULONG STDMETHODCALLTYPE ms_release(IStream *This)
{
    MStream *s = (MStream *)This;
    LONG r = InterlockedDecrement(&s->refs);
    if (!r) { HeapFree(GetProcessHeap(), 0, s->data); HeapFree(GetProcessHeap(), 0, s); }
    return (ULONG)r;
}
static BOOL ms_reserve(MStream *s, ULONG n)
{
    if (n <= s->cap) return TRUE;
    ULONG cap = s->cap ? s->cap : 256;
    while (cap < n) { if (cap > 0x7FFFFFFF / 2) return FALSE; cap *= 2; }
    BYTE *d = s->data ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, s->data, cap) : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap);
    if (!d) return FALSE;
    s->data = d;
    s->cap = cap;
    return TRUE;
}
static HRESULT STDMETHODCALLTYPE ms_read(IStream *This, void *buf, ULONG n, ULONG *got)
{
    MStream *s = (MStream *)This;
    ULONG k = s->pos < s->size ? s->size - s->pos : 0;
    if (k > n) k = n;
    for (ULONG i = 0; i < k; i++) ((BYTE *)buf)[i] = s->data[s->pos + i];
    s->pos += k;
    if (got) *got = k;
    return k == n ? S_OK_ : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE ms_write(IStream *This, const void *buf, ULONG n, ULONG *done)
{
    MStream *s = (MStream *)This;
    if (!ms_reserve(s, s->pos + n)) return STG_E_MEDIUMFULL;
    for (ULONG i = 0; i < n; i++) s->data[s->pos + i] = ((const BYTE *)buf)[i];
    s->pos += n;
    if (s->pos > s->size) s->size = s->pos;
    if (done) *done = n;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE ms_seek(IStream *This, LARGE_INTEGER d, DWORD how, ULARGE_INTEGER *np)
{
    MStream *s = (MStream *)This;
    LONGLONG base = how == STREAM_SEEK_SET ? 0 : how == STREAM_SEEK_CUR ? (LONGLONG)s->pos : (LONGLONG)s->size;
    LONGLONG p = base + d.QuadPart;
    if (how > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
    if (p < 0) return STG_E_INVALIDFUNCTION;
    s->pos = (ULONG)p;
    if (np) np->QuadPart = s->pos;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE ms_setsize(IStream *This, ULARGE_INTEGER n)
{
    MStream *s = (MStream *)This;
    if (n.QuadPart > 0x7FFFFFFF || !ms_reserve(s, (ULONG)n.QuadPart)) return STG_E_MEDIUMFULL;
    s->size = (ULONG)n.QuadPart;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE ms_copyto(IStream *This, IStream *to, ULARGE_INTEGER n, ULARGE_INTEGER *rd, ULARGE_INTEGER *wr)
{
    MStream *s = (MStream *)This;
    ULONG k = s->pos < s->size ? s->size - s->pos : 0, done = 0;
    if (n.QuadPart < k) k = (ULONG)n.QuadPart;
    HRESULT hr = to->lpVtbl->Write(to, s->data + s->pos, k, &done);
    s->pos += k;
    if (rd) rd->QuadPart = k;
    if (wr) wr->QuadPart = done;
    return hr;
}
static HRESULT STDMETHODCALLTYPE ms_commit(IStream *This, DWORD f) { (void)This; (void)f; return S_OK_; }
static HRESULT STDMETHODCALLTYPE ms_revert(IStream *This) { (void)This; return S_OK_; }
static HRESULT STDMETHODCALLTYPE ms_lock(IStream *This, ULARGE_INTEGER o, ULARGE_INTEGER n, DWORD t)
{ (void)This; (void)o; (void)n; (void)t; return STG_E_INVALIDFUNCTION; }
static HRESULT STDMETHODCALLTYPE ms_stat(IStream *This, STATSTG *st, DWORD flags)
{
    (void)flags;
    if (!st) return E_INVALIDARG_;
    for (size_t i = 0; i < sizeof *st; i++) ((BYTE *)st)[i] = 0;
    st->type = STGTY_STREAM;
    st->cbSize.QuadPart = ((MStream *)This)->size;
    st->grfMode = STGM_READWRITE_;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE ms_clone(IStream *This, IStream **out);
static IStreamVtbl g_ms_vtbl = { ms_qi, ms_addref, ms_release, ms_read, ms_write, ms_seek, ms_setsize, ms_copyto,
                                 ms_commit, ms_revert, ms_lock, ms_lock, ms_stat, ms_clone };

LWAPI IStream *WINAPI SHCreateMemStream(const BYTE *init, UINT n)
{
    MStream *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    if (!s) return NULL;
    s->iface.lpVtbl = &g_ms_vtbl;
    s->refs = 1;
    if (init && n) {
        if (!ms_reserve(s, n)) { HeapFree(GetProcessHeap(), 0, s); return NULL; }
        for (UINT i = 0; i < n; i++) s->data[i] = init[i];
        s->size = n;
    }
    return &s->iface;
}
static HRESULT STDMETHODCALLTYPE ms_clone(IStream *This, IStream **out)
{
    MStream *s = (MStream *)This;
    if (!out) return E_INVALIDARG_;
    IStream *c = SHCreateMemStream(s->data, s->size);
    if (!c) return E_OUTOFMEMORY_;
    ((MStream *)c)->pos = s->pos;
    *out = c;
    return S_OK_;
}
