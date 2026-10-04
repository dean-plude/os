/*
 * shlink.c — shortcuts: the ShellLink class (CLSID_ShellLink) with
 * IShellLinkW, IShellLinkA, IPersistFile and IPropertyStore (kept with
 * the object, not written to the file), reading and writing .lnk
 * files in the Windows format (MS-SHLLINK), and shell32's
 * DllGetClassObject that hands it out.
 *
 * Installers create shortcuts with CoCreateInstance(CLSID_ShellLink),
 * set the target, arguments, working folder, icon and description, and
 * IPersistFile::Save to a .lnk in the Start menu or on the desktop.  The
 * file written here carries the target in a LinkInfo block (local base
 * path, ANSI and Unicode) and the strings as Unicode StringData; NovaOS's
 * Start menu, desktop and Explorer read the same format.
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
#define CLASS_E_NOAGGREGATION_      ((HRESULT)0x80040110L)
#define CLASS_E_CLASSNOTAVAILABLE_  ((HRESULT)0x80040111L)
#define HR_WIN32(e)    ((HRESULT)(0x80070000L | (e)))

#define LNK_STR 1024                         /* characters kept per string */

static const GUID CLSID_ShellLink_ = { 0x00021401, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IUnknown_    = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_ICF_         = { 0x00000001, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IShellLinkW_ = { 0x000214F9, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IShellLinkA_ = { 0x000214EE, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IPersistFile_ = { 0x0000010B, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IPersist_    = { 0x0000010C, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IPropertyStore_ = { 0x886D8EEB, 0x8CF2, 0x4446, { 0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99 } };

static int same_guid(const GUID *a, const GUID *b)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }

/* bounded copy of a UTF-16 string (NULL: empty) */
static void wset(WCHAR *d, const WCHAR *s, int cap)
{
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static void a2w(const char *s, WCHAR *w, int cap)
{
    if (!s) { w[0] = 0; return; }
    if (MultiByteToWideChar(CP_ACP, 0, s, -1, w, cap) <= 0) w[0] = 0;
    w[cap - 1] = 0;
}

static void w2a(const WCHAR *w, char *s, int cap)
{
    if (!cap) return;
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, s, cap, 0, 0) <= 0) s[0] = 0;
    s[cap - 1] = 0;
}

/* -----------------------------------------------------------------------
 * The object
 * ----------------------------------------------------------------------- */
typedef struct Link Link;
typedef struct { void *vtbl; Link *self; } Face;

/* A property (a PROPERTYKEY and a PROPVARIANT's bytes) set through
 * IPropertyStore, such as the System.AppUserModel.ID an installer gives
 * its shortcut */
#define LINK_PROPS 16
typedef struct { GUID fmtid; DWORD pid; } PropKey;
typedef struct { WORD vt, r1, r2, r3; BYTE val[sizeof(void *) * 2]; } PropVar;
typedef struct { PropKey key; PropVar v; } Prop;

struct Link {
    Face w, a, pf, ps;                       /* IShellLinkW, IShellLinkA, IPersistFile, IPropertyStore */
    Prop props[LINK_PROPS];
    int nprops;
    volatile LONG refs;
    WCHAR path[MAX_PATH], args[LNK_STR], dir[MAX_PATH], desc[LNK_STR], icon[MAX_PATH], relative[MAX_PATH];
    WCHAR file[MAX_PATH];                    /* the .lnk it was loaded from or saved to */
    int icon_index, show;
    WORD hotkey;
    BOOL dirty;
};

#define LINK(face) (((Face *)(face))->self)

static HRESULT link_qi(Link *l, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    if (same_guid(riid, &IID_IUnknown_) || same_guid(riid, &IID_IShellLinkW_)) *ppv = &l->w;
    else if (same_guid(riid, &IID_IShellLinkA_)) *ppv = &l->a;
    else if (same_guid(riid, &IID_IPersistFile_) || same_guid(riid, &IID_IPersist_)) *ppv = &l->pf;
    else if (same_guid(riid, &IID_IPropertyStore_)) *ppv = &l->ps;
    else { *ppv = 0; return E_NOINTERFACE_; }
    InterlockedIncrement(&l->refs);
    return S_OK_;
}

/* VT_LPWSTR (31) and VT_CLSID (72) point at memory of their own, VT_BSTR
 * (8) at oleaut32's; the other types kept are plain values */
static BOOL prop_points(WORD vt) { return vt == 31 || vt == 72; }

/* VT_BSTR (8), as Inno Setup gives the AppUserModelID: oleaut32's */
static void *oleaut(const char *fn) { return (void *)GetProcAddress(LoadLibraryW(L"oleaut32.dll"), fn); }
static BOOL prop_plain(WORD vt)
{
    return vt == 0 || vt == 2 || vt == 3 || vt == 11 || (vt >= 16 && vt <= 23) || vt == 64;
}

static void prop_free(PropVar *v)
{
    if (prop_points(v->vt)) LocalFree(*(void **)v->val);
    else if (v->vt == 8 && *(void **)v->val) {
        void(WINAPI * fr)(void *) = (void(WINAPI *)(void *))oleaut("SysFreeString");
        if (fr) fr(*(void **)v->val);
    }
    v->vt = 0;
}

/* a copy of @src whose memory (a string or GUID) is its own: LocalAlloc,
 * which is CoTaskMemAlloc's heap */
static HRESULT prop_copy(PropVar *dst, const PropVar *src)
{
    *dst = *src;
    if (src->vt == 8) {
        const WCHAR *b = *(WCHAR *const *)src->val;
        if (!b) return S_OK_;
        void *(WINAPI * al)(const WCHAR *, UINT) = (void *(WINAPI *)(const WCHAR *, UINT))oleaut("SysAllocStringLen");
        void *c = al ? al(b, ((const DWORD *)b)[-1] / 2) : NULL;
        if (!c) { dst->vt = 0; return E_OUTOFMEMORY_; }
        *(void **)dst->val = c;
        return S_OK_;
    }
    if (!prop_points(src->vt)) return prop_plain(src->vt) ? S_OK_ : E_NOTIMPL_;
    const void *from = *(void *const *)src->val;
    SIZE_T n = src->vt == 31 ? 2 * ((SIZE_T)wlen(from) + 1) : sizeof(GUID);
    void *to = from ? LocalAlloc(0, n) : NULL;
    if (from && !to) { dst->vt = 0; return E_OUTOFMEMORY_; }
    if (to) CopyMemory(to, from, n);
    *(void **)dst->val = to;
    return S_OK_;
}

static ULONG link_addref(Link *l) { return (ULONG)InterlockedIncrement(&l->refs); }

static ULONG link_release(Link *l)
{
    LONG r = InterlockedDecrement(&l->refs);
    if (!r) {
        for (int i = 0; i < l->nprops; i++) prop_free(&l->props[i].v);
        HeapFree(GetProcessHeap(), 0, l);
    }
    return (ULONG)r;
}

/* ---- the .lnk file ---- */
static void put(BYTE **p, const void *src, DWORD n) { CopyMemory(*p, src, n); *p += n; }
static void put32(BYTE **p, DWORD v) { put(p, &v, 4); }
static void put16(BYTE **p, WORD v) { put(p, &v, 2); }

static void put_string(BYTE **p, const WCHAR *s)
{
    WORD n = (WORD)wlen(s);
    put16(p, n);
    put(p, s, 2u * n);
}

static HRESULT link_save(Link *l, const WCHAR *file)
{
    char apath[MAX_PATH * 3];
    WideCharToMultiByte(CP_ACP, 0, l->path, -1, apath, sizeof apath, 0, 0);
    DWORD alen = (DWORD)lstrlenA(apath) + 1, wlen_ = (DWORD)wlen(l->path) + 1;
    BYTE *buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x4C + 0x100 + alen + 2 * wlen_ + 2 * 5 * (LNK_STR + 2) + 16);
    if (!buf) return E_OUTOFMEMORY_;
    BYTE *p = buf;

    DWORD flags = 0x80;                                     /* IsUnicode */
    if (l->path[0]) flags |= 0x02;                          /* HasLinkInfo */
    if (l->desc[0]) flags |= 0x04;                          /* HasName */
    if (l->relative[0]) flags |= 0x08;                      /* HasRelativePath */
    if (l->dir[0]) flags |= 0x10;                           /* HasWorkingDir */
    if (l->args[0]) flags |= 0x20;                          /* HasArguments */
    if (l->icon[0]) flags |= 0x40;                          /* HasIconLocation */

    /* ShellLinkHeader */
    WIN32_FILE_ATTRIBUTE_DATA fa;
    ZeroMemory(&fa, sizeof fa);
    if (l->path[0]) GetFileAttributesExW(l->path, GetFileExInfoStandard, &fa);
    put32(&p, 0x4C);
    put(&p, &CLSID_ShellLink_, 16);
    put32(&p, flags);
    put32(&p, fa.dwFileAttributes);
    put(&p, &fa.ftCreationTime, 8);
    put(&p, &fa.ftLastAccessTime, 8);
    put(&p, &fa.ftLastWriteTime, 8);
    put32(&p, fa.nFileSizeLow);
    put32(&p, (DWORD)l->icon_index);
    put32(&p, (DWORD)(l->show ? l->show : SW_SHOWNORMAL));
    put16(&p, l->hotkey);
    put16(&p, 0); put32(&p, 0); put32(&p, 0);

    /* LinkInfo: VolumeID + LocalBasePath (ANSI and Unicode) + empty suffix */
    if (flags & 0x02) {
        DWORD hdr = 0x24, vol_size = 0x11;
        DWORD base = hdr + vol_size, suffix = base + alen;
        DWORD ubase = suffix + 1, usuffix = ubase + 2 * wlen_;
        DWORD size = usuffix + 2;
        put32(&p, size);
        put32(&p, hdr);
        put32(&p, 1);                                       /* VolumeIDAndLocalBasePath */
        put32(&p, hdr);                                     /* VolumeIDOffset */
        put32(&p, base);                                    /* LocalBasePathOffset */
        put32(&p, 0);                                       /* CommonNetworkRelativeLinkOffset */
        put32(&p, suffix);                                  /* CommonPathSuffixOffset */
        put32(&p, ubase);                                   /* LocalBasePathOffsetUnicode */
        put32(&p, usuffix);                                 /* CommonPathSuffixOffsetUnicode */
        put32(&p, vol_size);                                /* VolumeID */
        put32(&p, 3);                                       /* DRIVE_FIXED */
        put32(&p, 0x4E4F5641);                              /* serial number */
        put32(&p, 0x10);                                    /* VolumeLabelOffset */
        put(&p, "", 1);                                     /* empty label */
        put(&p, apath, alen);
        put(&p, "", 1);
        put(&p, l->path, 2 * wlen_);
        put16(&p, 0);
    }

    /* StringData */
    if (flags & 0x04) put_string(&p, l->desc);
    if (flags & 0x08) put_string(&p, l->relative);
    if (flags & 0x10) put_string(&p, l->dir);
    if (flags & 0x20) put_string(&p, l->args);
    if (flags & 0x40) put_string(&p, l->icon);
    put32(&p, 0);                                           /* TerminalBlock */

    HRESULT hr = S_OK_;
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    DWORD done = 0;
    if (h == INVALID_HANDLE_VALUE) hr = HR_WIN32(GetLastError());
    else {
        if (!WriteFile(h, buf, (DWORD)(p - buf), &done, 0) || done != (DWORD)(p - buf)) hr = HR_WIN32(GetLastError());
        CloseHandle(h);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return hr;
}

static const BYTE *get_string(const BYTE *p, const BYTE *end, BOOL wide, WCHAR *out, int cap)
{
    out[0] = 0;
    if (p + 2 > end) return end;
    WORD n = *(const WORD *)p;
    p += 2;
    DWORD bytes = wide ? 2u * n : n;
    if (p + bytes > end) return end;
    if (wide) {
        int k = n < cap - 1 ? n : cap - 1;
        CopyMemory(out, p, 2u * (DWORD)k);
        out[k] = 0;
    } else {
        char tmp[LNK_STR + 1];
        int k = n < LNK_STR ? n : LNK_STR;
        CopyMemory(tmp, p, (DWORD)k);
        tmp[k] = 0;
        a2w(tmp, out, cap);
    }
    return p + bytes;
}

static HRESULT link_load(Link *l, const WCHAR *file)
{
    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return HR_WIN32(GetLastError());
    DWORD size = GetFileSize(h, 0), got = 0;
    if (size < 0x4C || size > 1024 * 1024) { CloseHandle(h); return E_FAIL_; }
    BYTE *buf = HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) { CloseHandle(h); return E_OUTOFMEMORY_; }
    BOOL ok = ReadFile(h, buf, size, &got, 0) && got == size;
    CloseHandle(h);
    const BYTE *p = buf, *end = buf + size;
    if (!ok || *(const DWORD *)p != 0x4C || !same_guid((const GUID *)(p + 4), &CLSID_ShellLink_)) {
        HeapFree(GetProcessHeap(), 0, buf);
        return E_FAIL_;
    }
    DWORD flags = *(const DWORD *)(p + 0x14);
    l->icon_index = *(const int *)(p + 0x38);
    l->show = *(const int *)(p + 0x3C);
    l->hotkey = *(const WORD *)(p + 0x40);
    p += 0x4C;
    l->path[0] = l->args[0] = l->dir[0] = l->desc[0] = l->icon[0] = l->relative[0] = 0;
    if ((flags & 0x01) && p + 2 <= end) p += 2 + *(const WORD *)p;       /* LinkTargetIDList: skipped */
    if ((flags & 0x02) && p + 0x1C <= end) {                             /* LinkInfo */
        DWORD li = *(const DWORD *)p, hdr = *(const DWORD *)(p + 4), lf = *(const DWORD *)(p + 8);
        if (li >= 0x1C && p + li <= end && (lf & 1)) {
            DWORD base = *(const DWORD *)(p + 0x10), suffix = *(const DWORD *)(p + 0x18);
            if (hdr >= 0x24 && *(const DWORD *)(p + 0x1C) && *(const DWORD *)(p + 0x1C) < li) {
                wset(l->path, (const WCHAR *)(p + *(const DWORD *)(p + 0x1C)), MAX_PATH);
                DWORD us = *(const DWORD *)(p + 0x20);
                if (us && us < li) {
                    int n = wlen(l->path);
                    wset(l->path + n, (const WCHAR *)(p + us), MAX_PATH - n);
                }
            } else if (base < li) {
                char tmp[MAX_PATH * 2];
                lstrcpynA(tmp, (const char *)(p + base), MAX_PATH);
                if (suffix && suffix < li) {
                    int n = lstrlenA(tmp);
                    lstrcpynA(tmp + n, (const char *)(p + suffix), MAX_PATH - n);
                }
                a2w(tmp, l->path, MAX_PATH);
            }
        }
        p += li;
    }
    BOOL wide = (flags & 0x80) != 0;
    if (flags & 0x04) p = get_string(p, end, wide, l->desc, LNK_STR);
    if (flags & 0x08) p = get_string(p, end, wide, l->relative, MAX_PATH);
    if (flags & 0x10) p = get_string(p, end, wide, l->dir, MAX_PATH);
    if (flags & 0x20) p = get_string(p, end, wide, l->args, LNK_STR);
    if (flags & 0x40) p = get_string(p, end, wide, l->icon, MAX_PATH);
    HeapFree(GetProcessHeap(), 0, buf);
    wset(l->file, file, MAX_PATH);
    l->dirty = FALSE;
    return S_OK_;
}

/* -----------------------------------------------------------------------
 * IShellLinkW
 * ----------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE w_qi(void *t, REFIID r, void **pp) { return link_qi(LINK(t), r, pp); }
static ULONG STDMETHODCALLTYPE w_addref(void *t)  { return link_addref(LINK(t)); }
static ULONG STDMETHODCALLTYPE w_release(void *t) { return link_release(LINK(t)); }

static void fill_find_w(const WCHAR *path, WIN32_FIND_DATAW *fd)
{
    ZeroMemory(fd, sizeof *fd);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a)) {
        fd->dwFileAttributes = a.dwFileAttributes;
        fd->ftCreationTime = a.ftCreationTime;
        fd->ftLastAccessTime = a.ftLastAccessTime;
        fd->ftLastWriteTime = a.ftLastWriteTime;
        fd->nFileSizeHigh = a.nFileSizeHigh;
        fd->nFileSizeLow = a.nFileSizeLow;
    }
    const WCHAR *leaf = path;
    for (const WCHAR *s = path; *s; s++) if (*s == '\\' || *s == '/') leaf = s + 1;
    wset(fd->cFileName, leaf, MAX_PATH);
}

static HRESULT STDMETHODCALLTYPE w_getpath(void *t, LPWSTR out, int cch, WIN32_FIND_DATAW *fd, DWORD flags)
{
    (void)flags;
    Link *l = LINK(t);
    if (out && cch > 0) wset(out, l->path, cch);
    if (fd) fill_find_w(l->path, fd);
    return l->path[0] ? S_OK_ : S_FALSE_;
}

/* item ID lists (shell32.c): one item holding the full path */
__declspec(dllexport) void *__stdcall ILCreateFromPathW(LPCWSTR path);
__declspec(dllexport) BOOL __stdcall SHGetPathFromIDListW(const void *pidl, LPWSTR path);

static HRESULT STDMETHODCALLTYPE w_getidlist(void *t, void **pidl)
{
    Link *l = LINK(t);
    if (!pidl) return E_INVALIDARG_;
    *pidl = l->path[0] ? ILCreateFromPathW(l->path) : 0;
    return *pidl ? S_OK_ : S_FALSE_;
}

static HRESULT STDMETHODCALLTYPE w_setidlist(void *t, const void *pidl)
{
    Link *l = LINK(t);
    WCHAR p[MAX_PATH];
    if (!pidl || !SHGetPathFromIDListW(pidl, p)) return E_INVALIDARG_;
    wset(l->path, p, MAX_PATH);
    l->dirty = TRUE;
    return S_OK_;
}

#define W_GETSET(field, cap, getname, setname)                                             \
    static HRESULT STDMETHODCALLTYPE getname(void *t, LPWSTR out, int cch)                 \
    {                                                                                      \
        if (!out || cch <= 0) return E_INVALIDARG_;                                        \
        wset(out, LINK(t)->field, cch);                                                    \
        return S_OK_;                                                                      \
    }                                                                                      \
    static HRESULT STDMETHODCALLTYPE setname(void *t, LPCWSTR s)                           \
    {                                                                                      \
        wset(LINK(t)->field, s, cap);                                                      \
        LINK(t)->dirty = TRUE;                                                             \
        return S_OK_;                                                                      \
    }
W_GETSET(desc, LNK_STR, w_getdesc, w_setdesc)
W_GETSET(dir, MAX_PATH, w_getdir, w_setdir)
W_GETSET(args, LNK_STR, w_getargs, w_setargs)

static HRESULT STDMETHODCALLTYPE w_gethotkey(void *t, WORD *k) { if (!k) return E_INVALIDARG_; *k = LINK(t)->hotkey; return S_OK_; }
static HRESULT STDMETHODCALLTYPE w_sethotkey(void *t, WORD k) { LINK(t)->hotkey = k; LINK(t)->dirty = TRUE; return S_OK_; }
static HRESULT STDMETHODCALLTYPE w_getshow(void *t, int *s)
{
    if (!s) return E_INVALIDARG_;
    *s = LINK(t)->show ? LINK(t)->show : SW_SHOWNORMAL;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE w_setshow(void *t, int s) { LINK(t)->show = s; LINK(t)->dirty = TRUE; return S_OK_; }

static HRESULT STDMETHODCALLTYPE w_geticon(void *t, LPWSTR out, int cch, int *index)
{
    Link *l = LINK(t);
    if (out && cch > 0) wset(out, l->icon, cch);
    if (index) *index = l->icon_index;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE w_seticon(void *t, LPCWSTR path, int index)
{
    Link *l = LINK(t);
    wset(l->icon, path, MAX_PATH);
    l->icon_index = index;
    l->dirty = TRUE;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE w_setrelative(void *t, LPCWSTR path, DWORD reserved)
{
    (void)reserved;
    wset(LINK(t)->relative, path, MAX_PATH);
    LINK(t)->dirty = TRUE;
    return S_OK_;
}
/* Resolve: the target is where it was (no search for moved targets) */
static HRESULT STDMETHODCALLTYPE w_resolve(void *t, HWND hwnd, DWORD flags)
{
    (void)hwnd; (void)flags;
    Link *l = LINK(t);
    return !l->path[0] || GetFileAttributesW(l->path) != INVALID_FILE_ATTRIBUTES ? S_OK_ : S_FALSE_;
}
static HRESULT STDMETHODCALLTYPE w_setpath(void *t, LPCWSTR path)
{
    Link *l = LINK(t);
    if (!path) return E_INVALIDARG_;
    WCHAR full[MAX_PATH];
    const WCHAR *src = path;
    if (*path == '"') {                                     /* quoted: take what is inside */
        int i = 0;
        for (src = path + 1; src[i] && src[i] != '"' && i < MAX_PATH - 1; i++) full[i] = src[i];
        full[i] = 0;
        src = full;
    }
    WCHAR abs[MAX_PATH];
    if (src[0] && GetFullPathNameW(src, MAX_PATH, abs, 0)) src = abs;
    wset(l->path, src, MAX_PATH);
    l->dirty = TRUE;
    return S_OK_;
}

static void *const g_link_w_vtbl[] = {
    w_qi, w_addref, w_release, w_getpath, w_getidlist, w_setidlist, w_getdesc, w_setdesc,
    w_getdir, w_setdir, w_getargs, w_setargs, w_gethotkey, w_sethotkey, w_getshow, w_setshow,
    w_geticon, w_seticon, w_setrelative, w_resolve, w_setpath,
};

/* -----------------------------------------------------------------------
 * IShellLinkA: the same, in ANSI
 * ----------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE a_getpath(void *t, LPSTR out, int cch, WIN32_FIND_DATAA *fd, DWORD flags)
{
    (void)flags;
    Link *l = LINK(t);
    if (out && cch > 0) w2a(l->path, out, cch);
    if (fd) {
        WIN32_FIND_DATAW w;
        fill_find_w(l->path, &w);
        ZeroMemory(fd, sizeof *fd);
        fd->dwFileAttributes = w.dwFileAttributes;
        fd->ftCreationTime = w.ftCreationTime;
        fd->ftLastAccessTime = w.ftLastAccessTime;
        fd->ftLastWriteTime = w.ftLastWriteTime;
        fd->nFileSizeHigh = w.nFileSizeHigh;
        fd->nFileSizeLow = w.nFileSizeLow;
        w2a(w.cFileName, fd->cFileName, MAX_PATH);
    }
    return l->path[0] ? S_OK_ : S_FALSE_;
}

#define A_GETSET(field, cap, getname, setname)                                             \
    static HRESULT STDMETHODCALLTYPE getname(void *t, LPSTR out, int cch)                  \
    {                                                                                      \
        if (!out || cch <= 0) return E_INVALIDARG_;                                        \
        w2a(LINK(t)->field, out, cch);                                                     \
        return S_OK_;                                                                      \
    }                                                                                      \
    static HRESULT STDMETHODCALLTYPE setname(void *t, LPCSTR s)                            \
    {                                                                                      \
        a2w(s, LINK(t)->field, cap);                                                       \
        LINK(t)->dirty = TRUE;                                                             \
        return S_OK_;                                                                      \
    }
A_GETSET(desc, LNK_STR, a_getdesc, a_setdesc)
A_GETSET(dir, MAX_PATH, a_getdir, a_setdir)
A_GETSET(args, LNK_STR, a_getargs, a_setargs)

static HRESULT STDMETHODCALLTYPE a_geticon(void *t, LPSTR out, int cch, int *index)
{
    Link *l = LINK(t);
    if (out && cch > 0) w2a(l->icon, out, cch);
    if (index) *index = l->icon_index;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE a_seticon(void *t, LPCSTR path, int index)
{
    WCHAR w[MAX_PATH];
    a2w(path, w, MAX_PATH);
    return w_seticon(t, w, index);
}
static HRESULT STDMETHODCALLTYPE a_setrelative(void *t, LPCSTR path, DWORD reserved)
{
    WCHAR w[MAX_PATH];
    a2w(path, w, MAX_PATH);
    return w_setrelative(t, w, reserved);
}
static HRESULT STDMETHODCALLTYPE a_setpath(void *t, LPCSTR path)
{
    if (!path) return E_INVALIDARG_;
    WCHAR w[MAX_PATH];
    a2w(path, w, MAX_PATH);
    return w_setpath(t, w);
}

static void *const g_link_a_vtbl[] = {
    w_qi, w_addref, w_release, a_getpath, w_getidlist, w_setidlist, a_getdesc, a_setdesc,
    a_getdir, a_setdir, a_getargs, a_setargs, w_gethotkey, w_sethotkey, w_getshow, w_setshow,
    a_geticon, a_seticon, a_setrelative, w_resolve, a_setpath,
};

/* -----------------------------------------------------------------------
 * IPersistFile
 * ----------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE pf_classid(void *t, CLSID *c)
{
    (void)t;
    if (!c) return E_INVALIDARG_;
    *c = CLSID_ShellLink_;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE pf_isdirty(void *t) { return LINK(t)->dirty ? S_OK_ : S_FALSE_; }
static HRESULT STDMETHODCALLTYPE pf_load(void *t, LPCOLESTR file, DWORD mode)
{
    (void)mode;
    return file ? link_load(LINK(t), file) : E_INVALIDARG_;
}
static HRESULT STDMETHODCALLTYPE pf_save(void *t, LPCOLESTR file, BOOL remember)
{
    Link *l = LINK(t);
    if (!file) file = l->file;
    if (!file[0]) return E_INVALIDARG_;
    HRESULT hr = link_save(l, file);
    if (hr == S_OK_) {
        if (remember) wset(l->file, file, MAX_PATH);
        l->dirty = FALSE;
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE pf_savecompleted(void *t, LPCOLESTR file) { (void)t; (void)file; return S_OK_; }
static HRESULT STDMETHODCALLTYPE pf_getcurfile(void *t, LPOLESTR *out)
{
    Link *l = LINK(t);
    if (!out) return E_INVALIDARG_;
    *out = LocalAlloc(0, 2 * ((SIZE_T)wlen(l->file) + 1));  /* freed with CoTaskMemFree (the same heap) */
    if (!*out) return E_OUTOFMEMORY_;
    wset(*out, l->file, wlen(l->file) + 1);
    return l->file[0] ? S_OK_ : S_FALSE_;
}

static void *const g_link_pf_vtbl[] = {
    w_qi, w_addref, w_release, pf_classid, pf_isdirty, pf_load, pf_save, pf_savecompleted, pf_getcurfile,
};

/* ---- IPropertyStore ---- */
static Prop *prop_find(Link *l, const PropKey *k)
{
    for (int i = 0; i < l->nprops; i++)
        if (l->props[i].key.pid == k->pid && same_guid(&l->props[i].key.fmtid, &k->fmtid)) return &l->props[i];
    return NULL;
}
static HRESULT STDMETHODCALLTYPE ps_getcount(void *t, DWORD *n)
{
    if (!n) return E_INVALIDARG_;
    *n = (DWORD)LINK(t)->nprops;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE ps_getat(void *t, DWORD i, PropKey *k)
{
    Link *l = LINK(t);
    if (!k || i >= (DWORD)l->nprops) return E_INVALIDARG_;
    *k = l->props[i].key;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE ps_getvalue(void *t, const PropKey *k, PropVar *v)
{
    if (!k || !v) return E_INVALIDARG_;
    Prop *p = prop_find(LINK(t), k);
    if (!p) { ZeroMemory(v, sizeof *v); return S_OK_; }     /* VT_EMPTY, as Windows */
    return prop_copy(v, &p->v);
}
static HRESULT STDMETHODCALLTYPE ps_setvalue(void *t, const PropKey *k, const PropVar *v)
{
    Link *l = LINK(t);
    if (!k || !v) return E_INVALIDARG_;
    PropVar c;
    HRESULT hr = prop_copy(&c, v);
    if (hr != S_OK_) return hr;
    Prop *p = prop_find(l, k);
    if (!p) {
        if (l->nprops == LINK_PROPS) { prop_free(&c); return E_OUTOFMEMORY_; }
        p = &l->props[l->nprops++];
        p->key = *k;
    } else
        prop_free(&p->v);
    p->v = c;
    l->dirty = TRUE;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE ps_commit(void *t) { (void)t; return S_OK_; }

static void *const g_link_ps_vtbl[] = {
    w_qi, w_addref, w_release, ps_getcount, ps_getat, ps_getvalue, ps_setvalue, ps_commit,
};

static HRESULT link_create(REFIID riid, void **ppv)
{
    Link *l = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *l);
    if (!l) return E_OUTOFMEMORY_;
    l->w.vtbl = (void *)g_link_w_vtbl;   l->w.self = l;
    l->a.vtbl = (void *)g_link_a_vtbl;   l->a.self = l;
    l->pf.vtbl = (void *)g_link_pf_vtbl; l->pf.self = l;
    l->ps.vtbl = (void *)g_link_ps_vtbl; l->ps.self = l;
    l->refs = 1;
    l->show = SW_SHOWNORMAL;
    HRESULT hr = link_qi(l, riid, ppv);
    link_release(l);
    return hr;
}

/* -----------------------------------------------------------------------
 * The class factory and DllGetClassObject
 * ----------------------------------------------------------------------- */
typedef struct { void *vtbl; } Factory;

static HRESULT STDMETHODCALLTYPE cf_qi(void *t, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    if (same_guid(riid, &IID_IUnknown_) || same_guid(riid, &IID_ICF_)) { *ppv = t; return S_OK_; }
    *ppv = 0;
    return E_NOINTERFACE_;
}
static ULONG STDMETHODCALLTYPE cf_addref(void *t)  { (void)t; return 2; }
static ULONG STDMETHODCALLTYPE cf_release(void *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE cf_create(void *t, IUnknown *outer, REFIID riid, void **ppv)
{
    (void)t;
    if (!ppv) return E_INVALIDARG_;
    *ppv = 0;
    if (outer) return CLASS_E_NOAGGREGATION_;
    return link_create(riid, ppv);
}
static HRESULT STDMETHODCALLTYPE cf_lock(void *t, BOOL lock) { (void)t; (void)lock; return S_OK_; }

static void *const g_cf_vtbl[] = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static Factory g_link_factory = { (void *)g_cf_vtbl };

/* urlshort.c: the InternetShortcut class */
static const GUID CLSID_InternetShortcut_ = { 0xFBF23B40, 0xE3F0, 0x101B, { 0x84, 0x88, 0x00, 0xAA, 0x00, 0x3E, 0x56, 0xF8 } };
HRESULT url_class_object(REFIID riid, void **ppv);

__declspec(dllexport) HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    *ppv = 0;
    if (same_guid(clsid, &CLSID_InternetShortcut_)) return url_class_object(riid, ppv);
    if (!same_guid(clsid, &CLSID_ShellLink_)) return CLASS_E_CLASSNOTAVAILABLE_;
    return cf_qi(&g_link_factory, riid, ppv);
}

__declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow(void) { return S_FALSE_; }
