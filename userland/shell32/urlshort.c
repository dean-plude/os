/*
 * urlshort.c — internet shortcuts: the InternetShortcut class
 * (CLSID_InternetShortcut) with IUniformResourceLocatorW/A and
 * IPersistFile, reading and writing .url files:
 *
 *   [InternetShortcut]
 *   URL=https://example.org/
 *
 * Installers (WiX's util extension among them) make Start menu links to
 * web pages this way; opening one goes to ShellExecute.
 */

#define NOVA_BUILD_SHELL32
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

HINSTANCE WINAPI ShellExecuteW(HWND hwnd, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show);
/* CoTaskMemAlloc is ole32's process-heap allocation; shell32 does not link ole32 */
static void *task_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }

#define S_OK_          ((HRESULT)0)
#define S_FALSE_       ((HRESULT)1)
#define E_FAIL_        ((HRESULT)0x80004005L)
#define E_INVALIDARG_  ((HRESULT)0x80070057L)
#define E_NOINTERFACE_ ((HRESULT)0x80004002L)
#define E_OUTOFMEMORY_ ((HRESULT)0x8007000EL)
#define CLASS_E_NOAGGREGATION_ ((HRESULT)0x80040110L)
#define HR_WIN32(e)    ((HRESULT)(0x80070000L | (e)))

#define URL_MAX 2084

static const GUID CLSID_InternetShortcut_ = { 0xFBF23B40, 0xE3F0, 0x101B, { 0x84, 0x88, 0x00, 0xAA, 0x00, 0x3E, 0x56, 0xF8 } };
static const GUID IID_IUnknown_  = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_ICF_       = { 0x00000001, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IPersistFile_ = { 0x0000010B, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IPersist_  = { 0x0000010C, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IUrlW_     = { 0xCABB0DA0, 0xDA57, 0x11CF, { 0x99, 0x74, 0x00, 0x20, 0xAF, 0xD7, 0x97, 0x62 } };
static const GUID IID_IPropSetStg_ = { 0x0000013A, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IPropStg_  = { 0x00000138, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID FMTID_Intshcut_ = { 0x000214A0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IUrlA_     = { 0xFBF23B80, 0xE3F0, 0x101B, { 0x84, 0x88, 0x00, 0xAA, 0x00, 0x3E, 0x56, 0xF8 } };

static int same_guid(const GUID *a, const GUID *b)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static void wset(WCHAR *d, const WCHAR *s, int cap)
{
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

typedef struct Url Url;
typedef struct { void *vtbl; Url *self; } Face;

struct Url {
    Face w, a, pf, pss, ps;                  /* IUniformResourceLocatorW, ...A, IPersistFile,
                                                IPropertySetStorage, IPropertyStorage (FMTID_Intshcut) */
    volatile LONG refs;
    WCHAR url[URL_MAX];
    WCHAR icon[MAX_PATH];
    int icon_index;
    BOOL has_icon;
    WCHAR file[MAX_PATH];
    BOOL dirty;
};

static Url *of(void *t) { return ((Face *)t)->self; }

static HRESULT url_qi(Url *u, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG_;
    if (same_guid(riid, &IID_IUnknown_) || same_guid(riid, &IID_IUrlW_)) *ppv = &u->w;
    else if (same_guid(riid, &IID_IUrlA_)) *ppv = &u->a;
    else if (same_guid(riid, &IID_IPersistFile_) || same_guid(riid, &IID_IPersist_)) *ppv = &u->pf;
    else if (same_guid(riid, &IID_IPropSetStg_)) *ppv = &u->pss;
    else if (same_guid(riid, &IID_IPropStg_)) *ppv = &u->ps;
    else { *ppv = 0; return E_NOINTERFACE_; }
    InterlockedIncrement(&u->refs);
    return S_OK_;
}

static ULONG url_release(Url *u)
{
    LONG n = InterlockedDecrement(&u->refs);
    if (!n) HeapFree(GetProcessHeap(), 0, u);
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE f_qi(void *t, REFIID riid, void **ppv) { return url_qi(of(t), riid, ppv); }
static ULONG STDMETHODCALLTYPE f_addref(void *t) { return (ULONG)InterlockedIncrement(&of(t)->refs); }
static ULONG STDMETHODCALLTYPE f_release(void *t) { return url_release(of(t)); }

/* IUniformResourceLocatorW */
static HRESULT STDMETHODCALLTYPE w_seturl(void *t, LPCWSTR url, DWORD flags)
{
    (void)flags;
    Url *u = of(t);
    wset(u->url, url, URL_MAX);
    u->dirty = TRUE;
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE w_geturl(void *t, LPWSTR *out)
{
    Url *u = of(t);
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!u->url[0]) return S_FALSE_;
    int n = wlen(u->url) + 1;
    *out = task_alloc((SIZE_T)n * sizeof(WCHAR));
    if (!*out) return E_OUTOFMEMORY_;
    wset(*out, u->url, n);
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE w_invoke(void *t, void *info)
{
    (void)info;
    Url *u = of(t);
    if (!u->url[0]) return E_FAIL_;
    return (INT_PTR)ShellExecuteW(NULL, L"open", u->url, NULL, NULL, SW_SHOWNORMAL) > 32 ? S_OK_ : E_FAIL_;
}

/* IUniformResourceLocatorA */
static HRESULT STDMETHODCALLTYPE a_seturl(void *t, LPCSTR url, DWORD flags)
{
    (void)flags;
    Url *u = of(t);
    if (!url) url = "";
    if (MultiByteToWideChar(CP_ACP, 0, url, -1, u->url, URL_MAX) <= 0) u->url[0] = 0;
    u->url[URL_MAX - 1] = 0;
    u->dirty = TRUE;
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE a_geturl(void *t, LPSTR *out)
{
    Url *u = of(t);
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!u->url[0]) return S_FALSE_;
    int n = WideCharToMultiByte(CP_ACP, 0, u->url, -1, NULL, 0, NULL, NULL);
    *out = task_alloc((SIZE_T)n);
    if (!*out) return E_OUTOFMEMORY_;
    WideCharToMultiByte(CP_ACP, 0, u->url, -1, *out, n, NULL, NULL);
    return S_OK_;
}

/* IPersistFile */
static HRESULT STDMETHODCALLTYPE pf_classid(void *t, CLSID *out) { (void)t; if (!out) return E_INVALIDARG_; *out = CLSID_InternetShortcut_; return S_OK_; }
static HRESULT STDMETHODCALLTYPE pf_isdirty(void *t) { return of(t)->dirty ? S_OK_ : S_FALSE_; }

static HRESULT STDMETHODCALLTYPE pf_load(void *t, LPCOLESTR name, DWORD mode)
{
    (void)mode;
    Url *u = of(t);
    HANDLE h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return HR_WIN32(GetLastError());
    char buf[8192];
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &n, NULL);
    CloseHandle(h);
    if (!ok) return E_FAIL_;
    buf[n] = 0;
    /* URL= in the [InternetShortcut] section */
    BOOL in_section = FALSE;
    u->url[0] = 0;
    for (char *line = buf; *line; ) {
        char *end = line;
        while (*end && *end != '\r' && *end != '\n') end++;
        char save = *end;
        *end = 0;
        if (line[0] == '[') {
            const char *want = "[internetshortcut]";
            int i = 0;
            while (want[i] && lower(line[i]) == want[i]) i++;
            in_section = !want[i];
        } else if (in_section && lower(line[0]) == 'u' && lower(line[1]) == 'r' && lower(line[2]) == 'l' && line[3] == '=') {
            if (MultiByteToWideChar(CP_UTF8, 0, line + 4, -1, u->url, URL_MAX) <= 0) u->url[0] = 0;
            u->url[URL_MAX - 1] = 0;
        }
        *end = save;
        line = end;
        while (*line == '\r' || *line == '\n') line++;
    }
    wset(u->file, name, MAX_PATH);
    u->dirty = FALSE;
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE pf_save(void *t, LPCOLESTR name, BOOL remember)
{
    Url *u = of(t);
    if (!name) name = u->file;
    if (!name || !name[0]) return E_INVALIDARG_;
    char text[URL_MAX * 3 + 64];
    const char *head = "[InternetShortcut]\r\nURL=";
    int n = 0;
    while (head[n]) { text[n] = head[n]; n++; }
    int m = WideCharToMultiByte(CP_UTF8, 0, u->url, -1, text + n, (int)sizeof(text) - n - 3, NULL, NULL);
    n += m > 0 ? m - 1 : 0;
    text[n++] = '\r';
    text[n++] = '\n';
    if (u->has_icon && u->icon[0]) {
        const char *k = "IconFile=";
        for (int i = 0; k[i]; i++) text[n++] = k[i];
        m = WideCharToMultiByte(CP_UTF8, 0, u->icon, -1, text + n, (int)sizeof(text) - n - 40, NULL, NULL);
        n += m > 0 ? m - 1 : 0;
        n += wsprintfA(text + n, "\r\nIconIndex=%d\r\n", u->icon_index);
    }
    HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return HR_WIN32(GetLastError());
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, text, (DWORD)n, &wrote, NULL);
    CloseHandle(h);
    if (!ok || wrote != (DWORD)n) return E_FAIL_;
    if (remember) { wset(u->file, name, MAX_PATH); u->dirty = FALSE; }
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE pf_saved(void *t, LPCOLESTR name) { (void)t; (void)name; return S_OK_; }

static HRESULT STDMETHODCALLTYPE pf_curfile(void *t, LPOLESTR *out)
{
    Url *u = of(t);
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!u->file[0]) return S_FALSE_;
    int n = wlen(u->file) + 1;
    *out = task_alloc((SIZE_T)n * sizeof(WCHAR));
    if (!*out) return E_OUTOFMEMORY_;
    wset(*out, u->file, n);
    return S_OK_;
}

/* IPropertySetStorage: the one property set an internet shortcut has */
static HRESULT STDMETHODCALLTYPE pss_open(void *t, REFGUID fmtid, DWORD mode, void **out)
{
    (void)mode;
    Url *u = of(t);
    if (!out) return E_INVALIDARG_;
    *out = 0;
    if (!same_guid(fmtid, &FMTID_Intshcut_)) return (HRESULT)0x80030002L;   /* STG_E_FILENOTFOUND */
    InterlockedIncrement(&u->refs);
    *out = &u->ps;
    return S_OK_;
}
static HRESULT STDMETHODCALLTYPE pss_create(void *t, REFGUID fmtid, const CLSID *cls, DWORD flags, DWORD mode, void **out)
{
    (void)cls; (void)flags;
    return pss_open(t, fmtid, mode, out);
}
static HRESULT STDMETHODCALLTYPE pss_delete(void *t, REFGUID fmtid) { (void)t; (void)fmtid; return S_OK_; }
static HRESULT STDMETHODCALLTYPE pss_enum(void *t, void **out) { (void)t; if (out) *out = 0; return (HRESULT)0x80004001L; }

/* IPropertyStorage over FMTID_Intshcut: the URL (2), the icon index (8)
 * and file (9) */
#define PID_IS_URL_       2
#define PID_IS_ICONINDEX_ 8
#define PID_IS_ICONFILE_  9
typedef struct { ULONG kind; union { ULONG propid; LPOLESTR name; }; } PropSpec;
typedef struct { WORD vt, r1, r2, r3; union { LONG l; LPWSTR s; LPSTR a; ULONGLONG pad[2]; }; } PropVar;

static HRESULT STDMETHODCALLTYPE ps_read(void *t, ULONG n, const PropSpec *spec, PropVar *var)
{
    Url *u = of(t);
    for (ULONG i = 0; i < n; i++) {
        PropVar *v = &var[i];
        v->vt = 0;                                           /* VT_EMPTY */
        if (spec[i].kind != 1) continue;                     /* PRSPEC_PROPID */
        const WCHAR *src = spec[i].propid == PID_IS_URL_ ? u->url : spec[i].propid == PID_IS_ICONFILE_ ? u->icon : 0;
        if (src && src[0]) {
            int len = wlen(src) + 1;
            v->s = task_alloc((SIZE_T)len * sizeof(WCHAR));
            if (v->s) { wset(v->s, src, len); v->vt = 31; }  /* VT_LPWSTR */
        } else if (spec[i].propid == PID_IS_ICONINDEX_ && u->has_icon) {
            v->vt = 3;                                       /* VT_I4 */
            v->l = u->icon_index;
        }
    }
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE ps_write(void *t, ULONG n, const PropSpec *spec, const PropVar *var, ULONG first)
{
    (void)first;
    Url *u = of(t);
    for (ULONG i = 0; i < n; i++) {
        if (spec[i].kind != 1) continue;
        const PropVar *v = &var[i];
        if (spec[i].propid == PID_IS_ICONINDEX_ && (v->vt == 3 || v->vt == 19 || v->vt == 22 || v->vt == 23)) {
            u->icon_index = (int)v->l;
            u->has_icon = TRUE;
        } else if (spec[i].propid == PID_IS_ICONFILE_ || spec[i].propid == PID_IS_URL_) {
            WCHAR *dst = spec[i].propid == PID_IS_URL_ ? u->url : u->icon;
            int cap = spec[i].propid == PID_IS_URL_ ? URL_MAX : MAX_PATH;
            if (v->vt == 31) wset(dst, v->s, cap);
            else if (v->vt == 30 && v->a) { if (MultiByteToWideChar(CP_ACP, 0, v->a, -1, dst, cap) <= 0) dst[0] = 0; dst[cap - 1] = 0; }
            if (spec[i].propid == PID_IS_ICONFILE_) u->has_icon = TRUE;
        }
        u->dirty = TRUE;
    }
    return S_OK_;
}

static HRESULT STDMETHODCALLTYPE ps_ok0(void *t) { (void)t; return S_OK_; }
static HRESULT STDMETHODCALLTYPE ps_ok1(void *t, void *a) { (void)t; (void)a; return S_OK_; }
static HRESULT STDMETHODCALLTYPE ps_ok2(void *t, void *a, void *b) { (void)t; (void)a; (void)b; return S_OK_; }
static HRESULT STDMETHODCALLTYPE ps_ok3(void *t, void *a, void *b, void *c) { (void)t; (void)a; (void)b; (void)c; return S_OK_; }
static HRESULT STDMETHODCALLTYPE ps_commit(void *t, DWORD flags) { (void)t; (void)flags; return S_OK_; }
static HRESULT STDMETHODCALLTYPE ps_enum(void *t, void **out) { (void)t; if (out) *out = 0; return (HRESULT)0x80004001L; }
static HRESULT STDMETHODCALLTYPE ps_stat(void *t, void *st) { (void)t; (void)st; return (HRESULT)0x80004001L; }

static void *const g_pss_vtbl[] = { f_qi, f_addref, f_release, pss_create, pss_open, pss_delete, pss_enum };
static void *const g_ps_vtbl[] = { f_qi, f_addref, f_release, ps_read, ps_write, ps_ok2 /* DeleteMultiple */,
                                   ps_ok3 /* ReadPropertyNames */, ps_ok3 /* WritePropertyNames */, ps_ok2 /* DeletePropertyNames */,
                                   ps_commit, ps_ok0 /* Revert */, ps_enum, ps_ok3 /* SetTimes */, ps_ok1 /* SetClass */, ps_stat };

static void *const g_w_vtbl[] = { f_qi, f_addref, f_release, w_seturl, w_geturl, w_invoke };
static void *const g_a_vtbl[] = { f_qi, f_addref, f_release, a_seturl, a_geturl, w_invoke };
static void *const g_pf_vtbl[] = { f_qi, f_addref, f_release, pf_classid, pf_isdirty, pf_load, pf_save, pf_saved, pf_curfile };

static HRESULT url_create(REFIID riid, void **ppv)
{
    Url *u = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Url));
    if (!u) return E_OUTOFMEMORY_;
    u->w.vtbl = (void *)g_w_vtbl; u->w.self = u;
    u->a.vtbl = (void *)g_a_vtbl; u->a.self = u;
    u->pf.vtbl = (void *)g_pf_vtbl; u->pf.self = u;
    u->pss.vtbl = (void *)g_pss_vtbl; u->pss.self = u;
    u->ps.vtbl = (void *)g_ps_vtbl; u->ps.self = u;
    u->refs = 1;
    HRESULT hr = url_qi(u, riid, ppv);
    url_release(u);
    return hr;
}

/* The class factory */
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
    return url_create(riid, ppv);
}
static HRESULT STDMETHODCALLTYPE cf_lock(void *t, BOOL lock) { (void)t; (void)lock; return S_OK_; }
static void *const g_cf_vtbl[] = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static Factory g_url_factory = { (void *)g_cf_vtbl };

HRESULT url_class_object(REFIID riid, void **ppv) { return cf_qi(&g_url_factory, riid, ppv); }
