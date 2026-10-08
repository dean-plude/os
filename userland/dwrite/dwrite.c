/*
 * dwrite.c — NovaOS DirectWrite: the factory, font files and loaders,
 * font collections, families, fonts, localized strings, rendering params
 * and GDI interop
 *
 * The system font collection is every font file in C:\Windows\Fonts,
 * grouped by typographic family name.  As in gdi32, the family names
 * programs ask for by default ("Segoe UI", "Arial", "Consolas"...) are
 * also listed, backed by Inter and DejaVu Sans Mono.
 *
 * The factory is an IDWriteFactory3 (Windows 10's font sets, font face
 * references and font fallback, in fontset.c); text formats and layouts are
 * in layout.c.  IDWriteTextAnalyzer is not provided: browsers and toolkits
 * shape text themselves and use DirectWrite for fonts, metrics and glyph
 * rendering.
 */
#include "dwrite_int.h"

int _fltused = 0x9875;

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
void *dw_alloc(SIZE_T n)  { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
void *dw_zalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
void  dw_free(void *p)    { if (p) HeapFree(GetProcessHeap(), 0, p); }

double dw_floor(double x)
{
    if (x > 9e15 || x < -9e15) return x;
    double f = (double)(long long)x;
    return f > x ? f - 1 : f;
}
double dw_ceil(double x)
{
    if (x > 9e15 || x < -9e15) return x;
    double f = (double)(long long)x;
    return f < x ? f + 1 : f;
}
static int iround(double x) { return (int)dw_floor(x + 0.5); }

static UINT32 wlen(const WCHAR *s) { UINT32 n = 0; while (s && s[n]) n++; return n; }

WCHAR *dw_wcsdup(const WCHAR *s)
{
    UINT32 n = wlen(s);
    WCHAR *d = dw_alloc((n + 1) * sizeof(WCHAR));
    if (d) { memcpy(d, s, n * sizeof(WCHAR)); d[n] = 0; }
    return d;
}

static WCHAR lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
int dw_wcsieq(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b) return 0;
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return lower(*a) == lower(*b);
}

/* dw_log: OutputDebugString with %s (char *), %S (WCHAR *), %d, %u and %x */
void dw_log(const char *fmt, ...)
{
    char out[256];
    int n = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    const char *pre = "dwrite: ";
    while (*pre) out[n++] = *pre++;
    for (const char *p = fmt; *p && n < 250; p++) {
        if (*p != '%' || !p[1]) { out[n++] = *p; continue; }
        char c = *++p;
        if (c == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            while (s && *s && n < 250) out[n++] = *s++;
        } else if (c == 'S') {
            const WCHAR *s = __builtin_va_arg(ap, const WCHAR *);
            while (s && *s && n < 250) { out[n++] = *s < 0x80 ? (char)*s : '?'; s++; }
        } else if (c == 'd' || c == 'u' || c == 'x') {
            unsigned v = __builtin_va_arg(ap, unsigned);
            char t[12];
            int k = 0;
            if (c == 'd' && (int)v < 0) { out[n++] = '-'; v = (unsigned)-(int)v; }
            unsigned base = c == 'x' ? 16 : 10;
            do { t[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            while (k && n < 250) out[n++] = t[--k];
        } else {
            out[n++] = c;
        }
    }
    __builtin_va_end(ap);
    out[n++] = '\n';
    out[n] = 0;
    OutputDebugStringA(out);
}

#define IS(riid, iid) IsEqualGUID(riid, &(iid))

static HRESULT locstrings_new(const WCHAR *str, void **out);

/* -----------------------------------------------------------------------
 * IDWriteLocalizedStrings
 * ----------------------------------------------------------------------- */
typedef struct { WCHAR *locale, *str; } LocStr;
typedef struct { const void *const *vtbl; LONG ref; UINT32 n; LocStr items[2]; } LocStrings;

static HRESULT STDMETHODCALLTYPE ls_qi(LocStrings *s, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteLocalizedStrings)) {
        *out = s; InterlockedIncrement(&s->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ls_addref(LocStrings *s) { return (ULONG)InterlockedIncrement(&s->ref); }
static ULONG STDMETHODCALLTYPE ls_release(LocStrings *s)
{
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        for (UINT32 i = 0; i < s->n; i++) { dw_free(s->items[i].locale); dw_free(s->items[i].str); }
        dw_free(s);
    }
    return (ULONG)r;
}
static UINT32 STDMETHODCALLTYPE ls_count(LocStrings *s) { return s->n; }
static HRESULT STDMETHODCALLTYPE ls_find(LocStrings *s, const WCHAR *name, UINT32 *index, BOOL *exists)
{
    *index = 0xFFFFFFFF;
    *exists = FALSE;
    for (UINT32 i = 0; i < s->n; i++)
        if (dw_wcsieq(s->items[i].locale, name)) { *index = i; *exists = TRUE; break; }
    return S_OK;
}
static HRESULT copy_out(const WCHAR *src, WCHAR *buf, UINT32 size)
{
    UINT32 n = wlen(src);
    if (!buf || size <= n) { if (buf && size) buf[0] = 0; return E_NOT_SUFFICIENT_BUFFER_; }
    memcpy(buf, src, n * sizeof(WCHAR));
    buf[n] = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ls_locale_len(LocStrings *s, UINT32 i, UINT32 *len)
{
    if (i >= s->n) { *len = 0; return E_INVALIDARG; }
    *len = wlen(s->items[i].locale);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ls_locale(LocStrings *s, UINT32 i, WCHAR *buf, UINT32 size)
{
    if (i >= s->n) { if (buf && size) buf[0] = 0; return E_INVALIDARG; }
    return copy_out(s->items[i].locale, buf, size);
}
static HRESULT STDMETHODCALLTYPE ls_string_len(LocStrings *s, UINT32 i, UINT32 *len)
{
    if (i >= s->n) { *len = 0; return E_INVALIDARG; }
    *len = wlen(s->items[i].str);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ls_string(LocStrings *s, UINT32 i, WCHAR *buf, UINT32 size)
{
    if (i >= s->n) { if (buf && size) buf[0] = 0; return E_INVALIDARG; }
    return copy_out(s->items[i].str, buf, size);
}
static const void *const ls_vtbl[] = {
    ls_qi, ls_addref, ls_release, ls_count, ls_find, ls_locale_len, ls_locale, ls_string_len, ls_string,
};

HRESULT dw_locstrings(const WCHAR *str, void **out) { return locstrings_new(str, out); }

static HRESULT locstrings_new(const WCHAR *str, void **out)
{
    LocStrings *s = dw_zalloc(sizeof(*s));
    if (!s) { *out = NULL; return E_OUTOFMEMORY; }
    s->vtbl = ls_vtbl;
    s->ref = 1;
    s->n = 1;
    s->items[0].locale = dw_wcsdup(L"en-us");
    s->items[0].str = dw_wcsdup(str);
    *out = s;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * IDWriteFontFileStream over a font file in memory
 * ----------------------------------------------------------------------- */
typedef struct { const void *const *vtbl; LONG ref; FontData *data; } Stream;

static HRESULT STDMETHODCALLTYPE st_qi(Stream *s, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontFileStream)) {
        *out = s; InterlockedIncrement(&s->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE st_addref(Stream *s) { return (ULONG)InterlockedIncrement(&s->ref); }
static ULONG STDMETHODCALLTYPE st_release(Stream *s)
{
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) { font_data_release(s->data); dw_free(s); }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE st_read(Stream *s, const void **start, UINT64 off, UINT64 size, void **ctx)
{
    *ctx = NULL;
    if (off > s->data->size || size > s->data->size - off) { *start = NULL; return E_FAIL; }
    *start = s->data->bytes + off;
    return S_OK;
}
static void STDMETHODCALLTYPE st_release_frag(Stream *s, void *ctx) { (void)s; (void)ctx; }
static HRESULT STDMETHODCALLTYPE st_size(Stream *s, UINT64 *size) { *size = s->data->size; return S_OK; }
static HRESULT STDMETHODCALLTYPE st_time(Stream *s, UINT64 *t) { (void)s; *t = 0; return S_OK; }
static const void *const st_vtbl[] = { st_qi, st_addref, st_release, st_read, st_release_frag, st_size, st_time };

/* -----------------------------------------------------------------------
 * The local font file loader: keys are NUL-terminated paths
 * ----------------------------------------------------------------------- */
typedef struct { const void *const *vtbl; } LocalLoader;

static HRESULT STDMETHODCALLTYPE ll_qi(LocalLoader *l, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontFileLoader) || IS(riid, IID_IDWriteLocalFontFileLoader)) {
        *out = l; return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ll_addref(LocalLoader *l) { (void)l; return 2; }
static ULONG STDMETHODCALLTYPE ll_release(LocalLoader *l) { (void)l; return 1; }

/* loaded system fonts are kept, so a path is read once per process */
typedef struct PathCache { struct PathCache *next; WCHAR *path; FontData *data; } PathCache;
static PathCache *g_paths;
static SRWLOCK g_paths_lock = SRWLOCK_INIT;

static FontData *load_path_cached(const WCHAR *path)
{
    AcquireSRWLockExclusive(&g_paths_lock);
    for (PathCache *c = g_paths; c; c = c->next)
        if (dw_wcsieq(c->path, path)) {
            font_data_addref(c->data);
            ReleaseSRWLockExclusive(&g_paths_lock);
            return c->data;
        }
    FontData *d = font_data_load_path(path);
    PathCache *c = d ? dw_zalloc(sizeof(*c)) : NULL;
    if (c) {
        c->path = dw_wcsdup(path);
        c->data = d;
        font_data_addref(d);
        c->next = g_paths;
        g_paths = c;
    }
    ReleaseSRWLockExclusive(&g_paths_lock);
    return d;
}

static HRESULT STDMETHODCALLTYPE ll_stream(LocalLoader *l, const void *key, UINT32 size, void **out)
{
    (void)l; (void)size;
    FontData *d = load_path_cached((const WCHAR *)key);
    if (!d) { *out = NULL; return DWRITE_E_FILENOTFOUND; }
    Stream *s = dw_zalloc(sizeof(*s));
    if (!s) { font_data_release(d); *out = NULL; return E_OUTOFMEMORY; }
    s->vtbl = st_vtbl;
    s->ref = 1;
    s->data = d;
    *out = s;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ll_path_len(LocalLoader *l, const void *key, UINT32 size, UINT32 *len)
{
    (void)l; (void)size;
    *len = wlen(key);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ll_path(LocalLoader *l, const void *key, UINT32 size, WCHAR *buf, UINT32 n)
{
    (void)l; (void)size;
    return copy_out(key, buf, n);
}
static HRESULT STDMETHODCALLTYPE ll_time(LocalLoader *l, const void *key, UINT32 size, FILETIME *t)
{
    (void)l; (void)size;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(key, GetFileExInfoStandard, &a)) { memset(t, 0, sizeof(*t)); return DWRITE_E_FILENOTFOUND; }
    *t = a.ftLastWriteTime;
    return S_OK;
}
static const void *const ll_vtbl[] = { ll_qi, ll_addref, ll_release, ll_stream, ll_path_len, ll_path, ll_time };
static LocalLoader g_local_loader = { ll_vtbl };

/* -----------------------------------------------------------------------
 * IDWriteFontFile
 * ----------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE ff_qi(FontFile *f, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontFile)) {
        *out = f; InterlockedIncrement(&f->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ff_addref(FontFile *f) { return (ULONG)InterlockedIncrement(&f->ref); }
static ULONG STDMETHODCALLTYPE ff_release(FontFile *f)
{
    LONG r = InterlockedDecrement(&f->ref);
    if (!r) {
        if (f->loader != &g_local_loader) COM_RELEASE(f->loader);
        font_data_release(f->data);
        dw_free(f->key);
        dw_free(f);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE ff_key(FontFile *f, const void **key, UINT32 *size)
{
    *key = f->key;
    *size = f->key_size;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ff_loader(FontFile *f, void **out)
{
    COM_ADDREF(f->loader);
    *out = f->loader;
    return S_OK;
}

typedef HRESULT (STDMETHODCALLTYPE *StreamFromKey)(void *, const void *, UINT32, void **);
typedef HRESULT (STDMETHODCALLTYPE *ReadFragment)(void *, const void **, UINT64, UINT64, void **);
typedef void    (STDMETHODCALLTYPE *ReleaseFragment)(void *, void *);
typedef HRESULT (STDMETHODCALLTYPE *GetStreamSize)(void *, UINT64 *);

HRESULT font_file_data(FontFile *f, FontData **out)
{
    AcquireSRWLockExclusive(&f->lock);
    HRESULT hr = S_OK;
    if (!f->data) {
        if (f->loader == &g_local_loader) {
            f->data = load_path_cached((const WCHAR *)f->key);
            if (!f->data) hr = DWRITE_E_FILENOTFOUND;
        } else {
            void *stream = NULL;
            hr = ((StreamFromKey)VT(f->loader)[3])(f->loader, f->key, f->key_size, &stream);
            UINT64 size = 0;
            if (SUCCEEDED(hr)) hr = ((GetStreamSize)VT(stream)[5])(stream, &size);
            if (SUCCEEDED(hr) && (size < 12 || size > 0x40000000)) hr = DWRITE_E_FILEFORMAT;
            const void *frag = NULL;
            void *ctx = NULL;
            if (SUCCEEDED(hr)) hr = ((ReadFragment)VT(stream)[3])(stream, &frag, 0, size, &ctx);
            if (SUCCEEDED(hr)) {
                BYTE *copy = dw_alloc((SIZE_T)size);
                if (copy) memcpy(copy, frag, (SIZE_T)size);
                ((ReleaseFragment)VT(stream)[4])(stream, ctx);
                f->data = copy ? font_data_from_bytes(copy, (UINT32)size) : NULL;
                if (!f->data) hr = DWRITE_E_FILEFORMAT;
            }
            if (stream) COM_RELEASE(stream);
        }
    }
    if (f->data) font_data_addref(f->data);
    *out = f->data;
    ReleaseSRWLockExclusive(&f->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE ff_analyze(FontFile *f, BOOL *supported, UINT32 *file_type, UINT32 *face_type, UINT32 *faces)
{
    FontData *d = NULL;
    HRESULT hr = font_file_data(f, &d);
    *supported = d != NULL;
    if (file_type) *file_type = d ? d->file_type : FILE_UNKNOWN;
    if (face_type) *face_type = d ? d->face_type : FACE_UNKNOWN;
    *faces = d ? d->num_faces : 0;
    font_data_release(d);
    return hr == DWRITE_E_FILENOTFOUND ? hr : S_OK;
}
static const void *const ff_vtbl[] = { ff_qi, ff_addref, ff_release, ff_key, ff_loader, ff_analyze };

static FontFile *font_file_new(void *loader, const void *key, UINT32 key_size)
{
    FontFile *f = dw_zalloc(sizeof(*f));
    if (!f) return NULL;
    f->vtbl = ff_vtbl;
    f->ref = 1;
    f->loader = loader;
    if (loader != &g_local_loader) COM_ADDREF(loader);
    f->key = dw_alloc(key_size);
    if (f->key) memcpy(f->key, key, key_size);
    f->key_size = key_size;
    InitializeSRWLock(&f->lock);
    return f;
}

static FontFile *local_file_new(const WCHAR *path)
{
    return font_file_new(&g_local_loader, path, (wlen(path) + 1) * sizeof(WCHAR));
}
FontFile *font_file_local(const WCHAR *path) { return local_file_new(path); }
FontFile *font_file_from(void *iface) { return iface && *(void **)iface == (void *)ff_vtbl ? iface : NULL; }
/* the same file: one object, or the same loader and key */
BOOL font_file_equal(FontFile *a, FontFile *b)
{
    if (a == b) return TRUE;
    if (!a || !b || a->loader != b->loader || a->key_size != b->key_size) return FALSE;
    if (a->loader == &g_local_loader) return dw_wcsieq((const WCHAR *)a->key, (const WCHAR *)b->key);
    return !memcmp(a->key, b->key, a->key_size);
}

/* -----------------------------------------------------------------------
 * Font collections
 * ----------------------------------------------------------------------- */
typedef struct {
    FontFile *file;
    FaceData *face;
    UINT32    index;
    WCHAR     face_name[64];
} FontEntry;

typedef struct {
    WCHAR      *name;
    UINT32      n, cap;
    FontEntry **fonts;
} Family;

typedef struct Collection {
    const void *const *vtbl;
    LONG    ref;
    UINT32  n, cap;
    Family *fams;
} Collection;

typedef struct { const void *const *vtbl; LONG ref; Collection *c; UINT32 fi; } FamilyObj;
typedef struct { const void *const *vtbl; LONG ref; Collection *c; UINT32 fi; FontEntry *e; UINT32 sims; UINT32 style; } FontObj;
typedef struct { const void *const *vtbl; LONG ref; Collection *c; UINT32 fi; UINT32 n; FontEntry **fonts; } FontListObj;

extern const void *const coll_vtbl[], *const fam_vtbl[], *const font_vtbl[], *const list_vtbl[];

static Family *coll_family(Collection *c, const WCHAR *name, BOOL create)
{
    for (UINT32 i = 0; i < c->n; i++)
        if (dw_wcsieq(c->fams[i].name, name)) return &c->fams[i];
    if (!create) return NULL;
    if (c->n == c->cap) {
        UINT32 cap = c->cap ? c->cap * 2 : 16;
        Family *nf = dw_zalloc(cap * sizeof(Family));
        if (!nf) return NULL;
        if (c->fams) memcpy(nf, c->fams, c->n * sizeof(Family));
        dw_free(c->fams);
        c->fams = nf;
        c->cap = cap;
    }
    Family *f = &c->fams[c->n++];
    memset(f, 0, sizeof(*f));
    f->name = dw_wcsdup(name);
    return f;
}

static void family_add(Family *f, FontEntry *e)
{
    if (f->n == f->cap) {
        UINT32 cap = f->cap ? f->cap * 2 : 4;
        FontEntry **nf = dw_zalloc(cap * sizeof(FontEntry *));
        if (!nf) return;
        if (f->fonts) memcpy(nf, f->fonts, f->n * sizeof(FontEntry *));
        dw_free(f->fonts);
        f->fonts = nf;
        f->cap = cap;
    }
    f->fonts[f->n++] = e;
}

/* adds one face of a font file to the collection; d is the file's data, already loaded */
static BOOL coll_add_face(Collection *c, FontFile *file, FontData *d, UINT32 i)
{
    FaceData *face = font_face_data(d, i);
    if (!face) return TRUE;
    WCHAR fam[64];
    if (!sfnt_name(face, 16, fam, 64) && !sfnt_name(face, 1, fam, 64)) return TRUE;
    FontEntry *e = dw_zalloc(sizeof(*e));
    if (!e) return FALSE;
    e->file = file;
    ff_addref(file);
    e->face = face;
    e->index = i;
    if (!sfnt_name(face, 17, e->face_name, 64) && !sfnt_name(face, 2, e->face_name, 64))
        memcpy(e->face_name, L"Regular", 16);
    Family *f = coll_family(c, fam, TRUE);
    if (f) family_add(f, e);
    return TRUE;
}

/* adds every face of a font file to the collection */
static void coll_add_file(Collection *c, FontFile *file)
{
    FontData *d = NULL;
    if (FAILED(font_file_data(file, &d)) || !d) return;
    for (UINT32 i = 0; i < d->num_faces; i++)
        if (!coll_add_face(c, file, d, i)) break;
    font_data_release(d);
}

static Collection *coll_new(void)
{
    Collection *c = dw_zalloc(sizeof(*c));
    if (!c) return NULL;
    c->vtbl = coll_vtbl;
    c->ref = 1;
    return c;
}

/* The names programs ask for, as gdi32 lists them */
static const WCHAR *const g_sans_aliases[] = {
    L"Segoe UI", L"MS Shell Dlg", L"MS Shell Dlg 2", L"Tahoma", L"Arial", L"Microsoft Sans Serif",
    L"Verdana", L"Calibri", L"Times New Roman", L"Georgia", L"Segoe UI Symbol", 0 };
static const WCHAR *const g_mono_aliases[] = {
    L"Consolas", L"Courier New", L"Lucida Console", L"Cascadia Mono", L"Cascadia Code", 0 };

static Collection *g_system;
static SRWLOCK g_system_lock = SRWLOCK_INIT;

static void add_aliases(Collection *c, const WCHAR *const *names, const WCHAR *target)
{
    Family *t = coll_family(c, target, FALSE);
    if (!t && c->n) t = &c->fams[0];
    if (!t) return;
    UINT32 ti = (UINT32)(t - c->fams);
    for (int i = 0; names[i]; i++) {
        if (coll_family(c, names[i], FALSE)) continue;
        Family *a = coll_family(c, names[i], TRUE);   /* may move c->fams */
        if (!a) continue;
        t = &c->fams[ti];
        for (UINT32 k = 0; k < t->n; k++) family_add(a, t->fonts[k]);
    }
}

static Collection *system_collection(void)
{
    AcquireSRWLockExclusive(&g_system_lock);
    if (!g_system) {
        Collection *c = coll_new();
        WCHAR dir[MAX_PATH], pat[MAX_PATH + 8];
        UINT n = GetWindowsDirectoryW(dir, MAX_PATH - 16);
        if (!n) { memcpy(dir, L"C:\\Windows", 22); n = 10; }
        memcpy(dir + n, L"\\Fonts\\", 16);
        n += 7;
        memcpy(pat, dir, n * sizeof(WCHAR));
        pat[n] = '*'; pat[n + 1] = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE && c) {
            do {
                UINT32 len = wlen(fd.cFileName);
                if (len < 5 || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                const WCHAR *ext = fd.cFileName + len - 4;
                if (!dw_wcsieq(ext, L".ttf") && !dw_wcsieq(ext, L".otf") &&
                    !dw_wcsieq(ext, L".ttc") && !dw_wcsieq(ext, L".otc")) continue;
                WCHAR path[MAX_PATH * 2];
                if (n + len >= MAX_PATH * 2) continue;
                memcpy(path, dir, n * sizeof(WCHAR));
                memcpy(path + n, fd.cFileName, (len + 1) * sizeof(WCHAR));
                FontFile *file = local_file_new(path);
                if (file) { coll_add_file(c, file); ff_release(file); }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (c) {
            add_aliases(c, g_mono_aliases, L"DejaVu Sans Mono");
            add_aliases(c, g_sans_aliases, L"Inter");
            c->ref = 0x40000000;          /* lives as long as the process */
            dw_log("system font collection: %u families", c->n);
        }
        g_system = c;
    }
    ReleaseSRWLockExclusive(&g_system_lock);
    return g_system;
}

/* ---- matching (CSS-style: stretch, then style, then weight) ---- */
UINT32 dw_match_score(const FaceData *f, UINT32 weight, UINT32 stretch, UINT32 style)
{
    UINT32 s = (UINT32)(f->stretch > stretch ? f->stretch - stretch : stretch - f->stretch) * 10000;
    if (f->style != style) s += (style != STYLE_NORMAL && f->style != STYLE_NORMAL) ? 1000 : 3000;
    UINT32 w = f->weight;
    if (w != weight) {
        UINT32 d = w > weight ? w - weight : weight - w;
        /* lighter faces are preferred for weights up to 500, heavier ones above */
        BOOL wrong_side = weight <= 500 ? w > weight : w < weight;
        s += d + (wrong_side ? 1000 : 0);
    }
    return s;
}

static HRESULT font_obj_new(Collection *c, UINT32 fi, FontEntry *e, UINT32 sims, UINT32 style, void **out)
{
    FontObj *o = dw_zalloc(sizeof(*o));
    if (!o) { *out = NULL; return E_OUTOFMEMORY; }
    o->vtbl = font_vtbl;
    o->ref = 1;
    o->c = c;
    InterlockedIncrement(&c->ref);
    o->fi = fi;
    o->e = e;
    o->sims = sims;
    o->style = style;
    *out = o;
    return S_OK;
}

static HRESULT first_matching(Collection *c, UINT32 fi, UINT32 weight, UINT32 stretch, UINT32 style, void **out)
{
    Family *f = &c->fams[fi];
    if (!f->n) { *out = NULL; return DWRITE_E_NOFONT; }
    FontEntry *best = f->fonts[0];
    UINT32 bs = 0xFFFFFFFF;
    for (UINT32 i = 0; i < f->n; i++) {
        UINT32 s = dw_match_score(f->fonts[i]->face, weight, stretch, style);
        if (s < bs) { bs = s; best = f->fonts[i]; }
    }
    UINT32 sims = 0, st = best->face->style;
    if (style != STYLE_NORMAL && st == STYLE_NORMAL) { sims |= SIM_OBLIQUE; st = STYLE_OBLIQUE; }
    if (weight >= 600 && best->face->weight < 500) sims |= SIM_BOLD;
    return font_obj_new(c, fi, best, sims, st, out);
}

/* ---- IDWriteFontCollection ---- */
static HRESULT STDMETHODCALLTYPE coll_qi(Collection *c, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontCollection) || IS(riid, IID_IDWriteFontCollection1)) {
        *out = c; InterlockedIncrement(&c->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE coll_addref(Collection *c) { return (ULONG)InterlockedIncrement(&c->ref); }
static ULONG STDMETHODCALLTYPE coll_release(Collection *c)
{
    LONG r = InterlockedDecrement(&c->ref);
    if (!r) {
        for (UINT32 i = 0; i < c->n; i++) {
            for (UINT32 k = 0; k < c->fams[i].n; k++) { ff_release(c->fams[i].fonts[k]->file); dw_free(c->fams[i].fonts[k]); }
            dw_free(c->fams[i].fonts);
            dw_free(c->fams[i].name);
        }
        dw_free(c->fams);
        dw_free(c);
    }
    return (ULONG)r;
}
static UINT32 STDMETHODCALLTYPE coll_count(Collection *c) { return c->n; }
static HRESULT STDMETHODCALLTYPE coll_family_at(Collection *c, UINT32 i, void **out)
{
    if (i >= c->n) { *out = NULL; return E_INVALIDARG; }
    FamilyObj *o = dw_zalloc(sizeof(*o));
    if (!o) { *out = NULL; return E_OUTOFMEMORY; }
    o->vtbl = fam_vtbl;
    o->ref = 1;
    o->c = c;
    InterlockedIncrement(&c->ref);
    o->fi = i;
    *out = o;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE coll_find(Collection *c, const WCHAR *name, UINT32 *index, BOOL *exists)
{
    Family *f = name ? coll_family(c, name, FALSE) : NULL;
    *index = f ? (UINT32)(f - c->fams) : 0xFFFFFFFF;
    *exists = f != NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE coll_from_face(Collection *c, void *face, void **out)
{
    FontFace *ff = font_face_from(face);
    *out = NULL;
    if (!ff) return E_INVALIDARG;
    for (UINT32 i = 0; i < c->n; i++)
        for (UINT32 k = 0; k < c->fams[i].n; k++)
            if (c->fams[i].fonts[k]->face == ff->face) {
                FontEntry *e = c->fams[i].fonts[k];
                UINT32 st = (ff->sims & SIM_OBLIQUE) && e->face->style == STYLE_NORMAL ? STYLE_OBLIQUE : e->face->style;
                return font_obj_new(c, i, e, ff->sims, st, out);
            }
    return DWRITE_E_NOFONT;
}
/* ---- IDWriteFontCollection1 ---- */
/* the collection's fonts as a font set: each face once (the default names share their fonts) */
static HRESULT STDMETHODCALLTYPE coll_font_set(Collection *c, void **out)
{
    UINT32 total = 0, n = 0;
    *out = NULL;
    for (UINT32 i = 0; i < c->n; i++) total += c->fams[i].n;
    FontEntry **seen = dw_alloc((total + 1) * sizeof(FontEntry *));
    FontFile **files = dw_alloc((total + 1) * sizeof(FontFile *));
    UINT32 *index = dw_alloc((total + 1) * sizeof(UINT32));
    HRESULT hr = E_OUTOFMEMORY;
    if (seen && files && index) {
        for (UINT32 i = 0; i < c->n; i++)
            for (UINT32 k = 0; k < c->fams[i].n; k++) {
                FontEntry *e = c->fams[i].fonts[k];
                UINT32 j = 0;
                while (j < n && seen[j] != e) j++;
                if (j < n) continue;
                seen[n] = e;
                files[n] = e->file;
                index[n++] = e->index;
            }
        hr = font_set_create(files, index, n, c == g_system, out);
    }
    dw_free(seen); dw_free(files); dw_free(index);
    return hr;
}
const void *const coll_vtbl[] = {
    coll_qi, coll_addref, coll_release, coll_count, coll_family_at, coll_find, coll_from_face,
    coll_font_set, coll_family_at,
};

void *dw_system_collection(void)
{
    Collection *c = system_collection();
    if (c) coll_addref(c);
    return c;
}

HRESULT dw_collection_from_faces(FontFile *const *files, const UINT32 *index, UINT32 n, void **out)
{
    Collection *c = coll_new();
    *out = c;
    if (!c) return E_OUTOFMEMORY;
    for (UINT32 i = 0; i < n; i++) {
        FontData *d = NULL;
        if (FAILED(font_file_data(files[i], &d)) || !d) continue;
        coll_add_face(c, files[i], d, index[i]);
        font_data_release(d);
    }
    return S_OK;
}

/* ---- IDWriteFontFamily (and the IDWriteFontList it extends) ---- */
static HRESULT STDMETHODCALLTYPE fam_qi(FamilyObj *f, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontFamily) || IS(riid, IID_IDWriteFontList) ||
        IS(riid, IID_IDWriteFontFamily1)) {
        *out = f; InterlockedIncrement(&f->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fam_addref(FamilyObj *f) { return (ULONG)InterlockedIncrement(&f->ref); }
static ULONG STDMETHODCALLTYPE fam_release(FamilyObj *f)
{
    LONG r = InterlockedDecrement(&f->ref);
    if (!r) { coll_release(f->c); dw_free(f); }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE fam_collection(FamilyObj *f, void **out)
{
    coll_addref(f->c);
    *out = f->c;
    return S_OK;
}
static UINT32 STDMETHODCALLTYPE fam_count(FamilyObj *f) { return f->c->fams[f->fi].n; }
static HRESULT STDMETHODCALLTYPE fam_font(FamilyObj *f, UINT32 i, void **out)
{
    Family *fam = &f->c->fams[f->fi];
    if (i >= fam->n) { *out = NULL; return E_INVALIDARG; }
    return font_obj_new(f->c, f->fi, fam->fonts[i], 0, fam->fonts[i]->face->style, out);
}
static HRESULT STDMETHODCALLTYPE fam_names(FamilyObj *f, void **out) { return locstrings_new(f->c->fams[f->fi].name, out); }
static HRESULT STDMETHODCALLTYPE fam_first(FamilyObj *f, UINT32 weight, UINT32 stretch, UINT32 style, void **out)
{
    return first_matching(f->c, f->fi, weight, stretch, style, out);
}
static HRESULT STDMETHODCALLTYPE fam_matching(FamilyObj *f, UINT32 weight, UINT32 stretch, UINT32 style, void **out)
{
    Family *fam = &f->c->fams[f->fi];
    FontListObj *l = dw_zalloc(sizeof(*l));
    FontEntry **v = dw_zalloc((fam->n + 1) * sizeof(FontEntry *));
    if (!l || !v) { dw_free(l); dw_free(v); *out = NULL; return E_OUTOFMEMORY; }
    memcpy(v, fam->fonts, fam->n * sizeof(FontEntry *));
    for (UINT32 i = 1; i < fam->n; i++)          /* best match first */
        for (UINT32 k = i; k > 0 && dw_match_score(v[k]->face, weight, stretch, style) <
                                   dw_match_score(v[k - 1]->face, weight, stretch, style); k--) {
            FontEntry *t = v[k]; v[k] = v[k - 1]; v[k - 1] = t;
        }
    l->vtbl = list_vtbl;
    l->ref = 1;
    l->c = f->c;
    coll_addref(f->c);
    l->fi = f->fi;
    l->n = fam->n;
    l->fonts = v;
    *out = l;
    return S_OK;
}
/* ---- IDWriteFontFamily1 (every font is a local file) ---- */
static UINT32 STDMETHODCALLTYPE fam_locality(FamilyObj *f, UINT32 i) { (void)f; (void)i; return LOCALITY_LOCAL; }
static HRESULT STDMETHODCALLTYPE fam_face_ref(FamilyObj *f, UINT32 i, void **out)
{
    Family *fam = &f->c->fams[f->fi];
    if (i >= fam->n) { *out = NULL; return E_INVALIDARG; }
    return face_ref_create(fam->fonts[i]->file, fam->fonts[i]->index, 0, out);
}
const void *const fam_vtbl[] = {
    fam_qi, fam_addref, fam_release, fam_collection, fam_count, fam_font, fam_names, fam_first, fam_matching,
    fam_locality, fam_font, fam_face_ref,
};

/* ---- IDWriteFontList ---- */
static HRESULT STDMETHODCALLTYPE list_qi(FontListObj *l, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontList) || IS(riid, IID_IDWriteFontList1)) {
        *out = l; InterlockedIncrement(&l->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE list_addref(FontListObj *l) { return (ULONG)InterlockedIncrement(&l->ref); }
static ULONG STDMETHODCALLTYPE list_release(FontListObj *l)
{
    LONG r = InterlockedDecrement(&l->ref);
    if (!r) { coll_release(l->c); dw_free(l->fonts); dw_free(l); }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE list_collection(FontListObj *l, void **out) { coll_addref(l->c); *out = l->c; return S_OK; }
static UINT32 STDMETHODCALLTYPE list_count(FontListObj *l) { return l->n; }
static HRESULT STDMETHODCALLTYPE list_font(FontListObj *l, UINT32 i, void **out)
{
    if (i >= l->n) { *out = NULL; return E_INVALIDARG; }
    return font_obj_new(l->c, l->fi, l->fonts[i], 0, l->fonts[i]->face->style, out);
}
static UINT32 STDMETHODCALLTYPE list_locality(FontListObj *l, UINT32 i) { (void)l; (void)i; return LOCALITY_LOCAL; }
static HRESULT STDMETHODCALLTYPE list_face_ref(FontListObj *l, UINT32 i, void **out)
{
    if (i >= l->n) { *out = NULL; return E_INVALIDARG; }
    return face_ref_create(l->fonts[i]->file, l->fonts[i]->index, 0, out);
}
const void *const list_vtbl[] = {
    list_qi, list_addref, list_release, list_collection, list_count, list_font,
    list_locality, list_font, list_face_ref,
};

/* ---- IDWriteFont / IDWriteFont1 ---- */
static HRESULT STDMETHODCALLTYPE font_qi(FontObj *f, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFont) || IS(riid, IID_IDWriteFont1) || IS(riid, IID_IDWriteFont3)) {
        *out = f; InterlockedIncrement(&f->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE font_addref(FontObj *f) { return (ULONG)InterlockedIncrement(&f->ref); }
static ULONG STDMETHODCALLTYPE font_release(FontObj *f)
{
    LONG r = InterlockedDecrement(&f->ref);
    if (!r) { coll_release(f->c); dw_free(f); }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE font_family(FontObj *f, void **out) { return coll_family_at(f->c, f->fi, out); }
static UINT32 STDMETHODCALLTYPE font_weight(FontObj *f)
{
    return (f->sims & SIM_BOLD) && f->e->face->weight < 700 ? 700 : f->e->face->weight;
}
static UINT32 STDMETHODCALLTYPE font_stretch(FontObj *f) { return f->e->face->stretch; }
static UINT32 STDMETHODCALLTYPE font_style(FontObj *f) { return f->style; }
static BOOL STDMETHODCALLTYPE font_symbol(FontObj *f) { return f->e->face->symbol; }
static HRESULT STDMETHODCALLTYPE font_face_names(FontObj *f, void **out)
{
    if (!f->sims) return locstrings_new(f->e->face_name, out);
    WCHAR n[80];
    UINT32 k = 0;
    const WCHAR *base = f->e->face_name;
    if (!dw_wcsieq(base, L"Regular")) while (*base && k < 40) n[k++] = *base++;
    const WCHAR *add = (f->sims & SIM_BOLD) && (f->sims & SIM_OBLIQUE) ? L"Bold Oblique" :
                       (f->sims & SIM_BOLD) ? L"Bold" : L"Oblique";
    if (k) n[k++] = ' ';
    while (*add) n[k++] = *add++;
    n[k] = 0;
    return locstrings_new(n, out);
}
int face_info_string(const FaceData *f, UINT32 id, WCHAR *out, int cap)
{
    static const signed char name_ids[] = { -1, 0, 5, 7, 8, 9, 12, 10, 11, 13, 14, 1, 2, 16, 17, 19, 4, 6, 20, 21 };
    if (id >= sizeof(name_ids) || name_ids[id] < 0) return 0;
    if (sfnt_name(f, name_ids[id], out, cap)) return 1;
    if (id == 13) return sfnt_name(f, 1, out, cap);        /* typographic family falls back to the family */
    if (id == 14) return sfnt_name(f, 2, out, cap);
    if (id == 19) return sfnt_name(f, 16, out, cap) || sfnt_name(f, 1, out, cap);   /* weight/stretch/style family */
    return 0;
}
static HRESULT STDMETHODCALLTYPE font_info_strings(FontObj *f, UINT32 id, void **out, BOOL *exists)
{
    WCHAR s[256];
    *out = NULL;
    *exists = face_info_string(f->e->face, id, s, 256) != 0;
    return *exists ? locstrings_new(s, out) : S_OK;
}
static UINT32 STDMETHODCALLTYPE font_sims(FontObj *f) { return f->sims; }
static void STDMETHODCALLTYPE font_metrics(FontObj *f, DW_FONT_METRICS *m) { *m = f->e->face->metrics.m; }
static HRESULT STDMETHODCALLTYPE font_has_char(FontObj *f, UINT32 cp, BOOL *exists)
{
    *exists = face_glyph_index(f->e->face, cp) != 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE font_create_face(FontObj *f, void **out)
{
    return font_face_create(f->e->file, f->e->index, f->sims, (FontFace **)out);
}
static void STDMETHODCALLTYPE font_metrics1(FontObj *f, DW_FONT_METRICS1 *m) { *m = f->e->face->metrics; }
static void STDMETHODCALLTYPE font_panose(FontObj *f, BYTE *p) { memcpy(p, f->e->face->panose, 10); }
static HRESULT STDMETHODCALLTYPE font_ranges(FontObj *f, UINT32 max, DW_UNICODE_RANGE *r, UINT32 *count)
{
    *count = face_unicode_ranges(f->e->face, r, max);
    return *count > max ? E_NOT_SUFFICIENT_BUFFER_ : S_OK;
}
static BOOL STDMETHODCALLTYPE font_mono(FontObj *f) { return f->e->face->mono; }
/* IDWriteFont2, IDWriteFont3 */
static BOOL STDMETHODCALLTYPE font_color(FontObj *f) { return face_is_color(f->e->face); }
static BOOL STDMETHODCALLTYPE font_equals(FontObj *f, FontObj *o)
{
    if (!o || o->vtbl != font_vtbl) return FALSE;
    return o->sims == f->sims && (o->e == f->e || (o->e->index == f->e->index && font_file_equal(o->e->file, f->e->file)));
}
static HRESULT STDMETHODCALLTYPE font_face_ref(FontObj *f, void **out) { return face_ref_create(f->e->file, f->e->index, f->sims, out); }
static BOOL STDMETHODCALLTYPE font_has_char3(FontObj *f, UINT32 cp) { return face_glyph_index(f->e->face, cp) != 0; }
static UINT32 STDMETHODCALLTYPE font_locality(FontObj *f) { (void)f; return LOCALITY_LOCAL; }
const void *const font_vtbl[] = {
    font_qi, font_addref, font_release, font_family, font_weight, font_stretch, font_style, font_symbol,
    font_face_names, font_info_strings, font_sims, font_metrics, font_has_char, font_create_face,
    font_metrics1, font_panose, font_ranges, font_mono,
    font_color, font_create_face, font_equals, font_face_ref, font_has_char3, font_locality,
};

/* -----------------------------------------------------------------------
 * IDWriteRenderingParams1
 * ----------------------------------------------------------------------- */
typedef struct {
    const void *const *vtbl;
    LONG  ref;
    float gamma, contrast, gray_contrast, cleartype;
    UINT32 geometry, mode, gridfit;     /* mode is a DWRITE_RENDERING_MODE1 */
} Params;

static HRESULT STDMETHODCALLTYPE rp_qi(Params *p, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteRenderingParams) || IS(riid, IID_IDWriteRenderingParams1) ||
        IS(riid, IID_IDWriteRenderingParams2) || IS(riid, IID_IDWriteRenderingParams3)) {
        *out = p; InterlockedIncrement(&p->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE rp_addref(Params *p) { return (ULONG)InterlockedIncrement(&p->ref); }
static ULONG STDMETHODCALLTYPE rp_release(Params *p)
{
    LONG r = InterlockedDecrement(&p->ref);
    if (!r) dw_free(p);
    return (ULONG)r;
}
static float STDMETHODCALLTYPE rp_gamma(Params *p) { return p->gamma; }
static float STDMETHODCALLTYPE rp_contrast(Params *p) { return p->contrast; }
static float STDMETHODCALLTYPE rp_cleartype(Params *p) { return p->cleartype; }
static UINT32 STDMETHODCALLTYPE rp_geometry(Params *p) { return p->geometry; }
/* DWRITE_RENDERING_MODE has no NATURAL_SYMMETRIC_DOWNSAMPLED */
static UINT32 STDMETHODCALLTYPE rp_mode(Params *p) { return p->mode == RMODE1_NATURAL_SYMMETRIC_DOWNSAMPLED ? RMODE_NATURAL_SYMMETRIC : p->mode; }
static float STDMETHODCALLTYPE rp_gray_contrast(Params *p) { return p->gray_contrast; }
static UINT32 STDMETHODCALLTYPE rp_gridfit(Params *p) { return p->gridfit; }
static UINT32 STDMETHODCALLTYPE rp_mode1(Params *p) { return p->mode; }
static const void *const rp_vtbl[] = {
    rp_qi, rp_addref, rp_release, rp_gamma, rp_contrast, rp_cleartype, rp_geometry, rp_mode, rp_gray_contrast,
    rp_gridfit, rp_mode1,
};

static HRESULT params_new3(float gamma, float contrast, float gray, float ct, UINT32 geometry, UINT32 mode,
                           UINT32 gridfit, void **out)
{
    Params *p = dw_zalloc(sizeof(*p));
    if (!p) { *out = NULL; return E_OUTOFMEMORY; }
    p->vtbl = rp_vtbl;
    p->ref = 1;
    p->gamma = gamma; p->contrast = contrast; p->gray_contrast = gray; p->cleartype = ct;
    p->geometry = geometry; p->mode = mode; p->gridfit = gridfit;
    *out = p;
    return S_OK;
}
static HRESULT params_new(float gamma, float contrast, float gray, float ct, UINT32 geometry, UINT32 mode, void **out)
{
    return params_new3(gamma, contrast, gray, ct, geometry, mode, GRID_FIT_DEFAULT, out);
}

/* -----------------------------------------------------------------------
 * IDWriteGdiInterop
 * ----------------------------------------------------------------------- */
typedef struct { const void *const *vtbl; } GdiInterop;

static HRESULT STDMETHODCALLTYPE gi_qi(GdiInterop *g, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteGdiInterop)) { *out = g; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE gi_addref(GdiInterop *g) { (void)g; return 2; }
static ULONG STDMETHODCALLTYPE gi_release(GdiInterop *g) { (void)g; return 1; }

static HRESULT font_from_logfont(const LOGFONTW *lf, void **out)
{
    Collection *c = system_collection();
    *out = NULL;
    if (!c || !lf) return E_INVALIDARG;
    Family *f = coll_family(c, lf->lfFaceName, FALSE);
    if (!f) {
        BOOL mono = (lf->lfPitchAndFamily & 3) == 1 || (lf->lfPitchAndFamily & 0xF0) == 0x30;
        f = coll_family(c, mono ? L"Consolas" : L"Segoe UI", FALSE);
        if (!f && c->n) f = &c->fams[0];
        if (!f) return DWRITE_E_NOFONT;
    }
    UINT32 weight = lf->lfWeight ? (UINT32)lf->lfWeight : 400;
    return first_matching(c, (UINT32)(f - c->fams), weight, 5, lf->lfItalic ? STYLE_ITALIC : STYLE_NORMAL, out);
}

static HRESULT STDMETHODCALLTYPE gi_from_logfont(GdiInterop *g, const LOGFONTW *lf, void **out)
{
    (void)g;
    return font_from_logfont(lf, out);
}

static void fill_logfont(Collection *c, UINT32 fi, const FaceData *face, UINT32 sims, UINT32 style, LOGFONTW *lf)
{
    memset(lf, 0, sizeof(*lf));
    lf->lfWeight = (LONG)((sims & SIM_BOLD) && face->weight < 700 ? 700 : face->weight);
    lf->lfItalic = style != STYLE_NORMAL;
    lf->lfCharSet = DEFAULT_CHARSET;
    lf->lfOutPrecision = 7;            /* OUT_OUTLINE_PRECIS */
    lf->lfPitchAndFamily = face->mono ? 0x31 : 0x22;
    const WCHAR *name = c->fams[fi].name;
    for (int i = 0; name[i] && i < 31; i++) lf->lfFaceName[i] = name[i];
}

static HRESULT STDMETHODCALLTYPE gi_to_logfont(GdiInterop *g, FontObj *font, LOGFONTW *lf, BOOL *system)
{
    (void)g;
    if (!font || font->vtbl != font_vtbl) { if (system) *system = FALSE; return E_INVALIDARG; }
    fill_logfont(font->c, font->fi, font->e->face, font->sims, font->style, lf);
    if (system) *system = font->c == g_system;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE gi_face_to_logfont(GdiInterop *g, void *face, LOGFONTW *lf)
{
    (void)g;
    FontFace *ff = font_face_from(face);
    Collection *c = system_collection();
    memset(lf, 0, sizeof(*lf));
    if (!ff || !c) return E_INVALIDARG;
    for (UINT32 i = 0; i < c->n; i++)
        for (UINT32 k = 0; k < c->fams[i].n; k++)
            if (c->fams[i].fonts[k]->face == ff->face) {
                fill_logfont(c, i, ff->face, ff->sims, (ff->sims & SIM_OBLIQUE) ? STYLE_OBLIQUE : ff->face->style, lf);
                return S_OK;
            }
    /* not a system font: describe it by its own names */
    WCHAR fam[32];
    if (!sfnt_name(ff->face, 1, fam, 32)) fam[0] = 0;
    memcpy(lf->lfFaceName, fam, sizeof(fam));
    lf->lfWeight = ff->face->weight;
    lf->lfItalic = ff->face->style != STYLE_NORMAL;
    lf->lfCharSet = DEFAULT_CHARSET;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE gi_face_from_hdc(GdiInterop *g, HDC dc, void **out)
{
    (void)g;
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    HFONT hf = (HFONT)GetCurrentObject(dc, 6 /* OBJ_FONT */);
    if (!hf || !GetObjectW(hf, sizeof(lf), &lf)) {
        lf.lfWeight = 400;
        memcpy(lf.lfFaceName, L"Segoe UI", 18);
    }
    void *font = NULL;
    HRESULT hr = font_from_logfont(&lf, &font);
    *out = NULL;
    if (SUCCEEDED(hr)) {
        hr = font_create_face(font, out);
        font_release(font);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE gi_bitmap_target(GdiInterop *g, HDC dc, UINT32 w, UINT32 h, void **out)
{
    (void)g;
    if (!out) return E_INVALIDARG;
    return bitmap_target_create(dc, w, h, out);
}
static const void *const gi_vtbl[] = {
    gi_qi, gi_addref, gi_release, gi_from_logfont, gi_to_logfont, gi_face_to_logfont, gi_face_from_hdc, gi_bitmap_target,
};
static GdiInterop g_gdi_interop = { gi_vtbl };

/* -----------------------------------------------------------------------
 * IDWriteFactory through IDWriteFactory3
 * ----------------------------------------------------------------------- */
typedef struct { const void *const *vtbl; } Factory;

#define MAX_LOADERS 64
static void *g_file_loaders[MAX_LOADERS], *g_coll_loaders[MAX_LOADERS];
static SRWLOCK g_loaders_lock = SRWLOCK_INIT;

static HRESULT register_loader(void **list, void *loader)
{
    if (!loader) return E_INVALIDARG;
    HRESULT hr = E_FAIL;
    AcquireSRWLockExclusive(&g_loaders_lock);
    for (int i = 0; i < MAX_LOADERS; i++) if (list[i] == loader) { hr = DWRITE_E_ALREADYREGISTERED; goto out; }
    for (int i = 0; i < MAX_LOADERS; i++)
        if (!list[i]) { list[i] = loader; COM_ADDREF(loader); hr = S_OK; break; }
out:
    ReleaseSRWLockExclusive(&g_loaders_lock);
    return hr;
}

static HRESULT unregister_loader(void **list, void *loader)
{
    HRESULT hr = E_INVALIDARG;
    void *drop = NULL;
    AcquireSRWLockExclusive(&g_loaders_lock);
    for (int i = 0; i < MAX_LOADERS; i++)
        if (list[i] == loader) { drop = list[i]; list[i] = NULL; hr = S_OK; break; }
    ReleaseSRWLockExclusive(&g_loaders_lock);
    if (drop) COM_RELEASE(drop);
    return hr;
}

static HRESULT STDMETHODCALLTYPE fa_qi(Factory *f, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFactory) || IS(riid, IID_IDWriteFactory1) ||
        IS(riid, IID_IDWriteFactory2) || IS(riid, IID_IDWriteFactory3)) {
        *out = f; return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fa_addref(Factory *f) { (void)f; return 2; }
static ULONG STDMETHODCALLTYPE fa_release(Factory *f) { (void)f; return 1; }

static HRESULT STDMETHODCALLTYPE fa_system_collection(Factory *f, void **out, BOOL check)
{
    (void)f; (void)check;
    Collection *c = system_collection();
    *out = c;
    if (!c) return E_OUTOFMEMORY;
    coll_addref(c);
    return S_OK;
}

typedef HRESULT (STDMETHODCALLTYPE *EnumFromKey)(void *, void *, const void *, UINT32, void **);
typedef HRESULT (STDMETHODCALLTYPE *MoveNext)(void *, BOOL *);
typedef HRESULT (STDMETHODCALLTYPE *CurrentFile)(void *, void **);

static HRESULT STDMETHODCALLTYPE fa_custom_collection(Factory *f, void *loader, const void *key, UINT32 size, void **out)
{
    *out = NULL;
    if (!loader) return E_INVALIDARG;
    void *en = NULL;
    HRESULT hr = ((EnumFromKey)VT(loader)[3])(loader, f, key, size, &en);
    if (FAILED(hr)) return hr;
    Collection *c = coll_new();
    if (!c) { COM_RELEASE(en); return E_OUTOFMEMORY; }
    BOOL more = FALSE;
    while (SUCCEEDED(((MoveNext)VT(en)[3])(en, &more)) && more) {
        void *file = NULL;
        if (FAILED(((CurrentFile)VT(en)[4])(en, &file)) || !file) continue;
        if (*(void **)file == (void *)ff_vtbl) coll_add_file(c, file);
        COM_RELEASE(file);
    }
    COM_RELEASE(en);
    *out = c;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fa_register_coll_loader(Factory *f, void *l) { (void)f; return register_loader(g_coll_loaders, l); }
static HRESULT STDMETHODCALLTYPE fa_unregister_coll_loader(Factory *f, void *l) { (void)f; return unregister_loader(g_coll_loaders, l); }

static HRESULT STDMETHODCALLTYPE fa_file_ref(Factory *f, const WCHAR *path, const FILETIME *t, void **out)
{
    (void)f; (void)t;
    *out = NULL;
    if (!path) return E_INVALIDARG;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return DWRITE_E_FILENOTFOUND;
    FontFile *file = local_file_new(path);
    if (!file) return E_OUTOFMEMORY;
    *out = file;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fa_custom_file_ref(Factory *f, const void *key, UINT32 size, void *loader, void **out)
{
    (void)f;
    *out = NULL;
    if (!loader) return E_INVALIDARG;
    FontFile *file = font_file_new(loader, key, size);
    if (!file) return E_OUTOFMEMORY;
    *out = file;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fa_font_face(Factory *f, UINT32 type, UINT32 nfiles, void **files, UINT32 index, UINT32 sims, void **out)
{
    (void)f; (void)type;
    *out = NULL;
    if (!nfiles || !files || !files[0] || *(void **)files[0] != (void *)ff_vtbl) return E_INVALIDARG;
    return font_face_create(files[0], index, sims, (FontFace **)out);
}
static HRESULT STDMETHODCALLTYPE fa_params(Factory *f, void **out)
{
    (void)f;
    return params_new(1.8f, 0.5f, 1.0f, 0.0f, 0, RMODE_DEFAULT, out);
}
static HRESULT STDMETHODCALLTYPE fa_monitor_params(Factory *f, void *m, void **out) { (void)m; return fa_params(f, out); }
static HRESULT STDMETHODCALLTYPE fa_custom_params(Factory *f, float gamma, float contrast, float ct, UINT32 geometry, UINT32 mode, void **out)
{
    (void)f;
    return params_new(gamma, contrast, 1.0f, ct, geometry, mode, out);
}
static HRESULT STDMETHODCALLTYPE fa_register_file_loader(Factory *f, void *l) { (void)f; return register_loader(g_file_loaders, l); }
static HRESULT STDMETHODCALLTYPE fa_unregister_file_loader(Factory *f, void *l) { (void)f; return unregister_loader(g_file_loaders, l); }

static HRESULT STDMETHODCALLTYPE fa_text_format(Factory *f, const WCHAR *family, void *coll, UINT32 weight, UINT32 style,
                                                UINT32 stretch, float size, const WCHAR *locale, void **out)
{
    (void)f;
    return text_format_create(family, coll, weight, style, stretch, size, locale, out);
}
static HRESULT STDMETHODCALLTYPE fa_typography(Factory *f, void **out)
{
    (void)f;
    dw_log("unimplemented IDWriteFactory::CreateTypography");
    *out = NULL;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE fa_gdi_interop(Factory *f, void **out) { (void)f; *out = &g_gdi_interop; return S_OK; }
static HRESULT STDMETHODCALLTYPE fa_text_layout(Factory *f, const WCHAR *s, UINT32 n, void *fmt, float w, float h, void **out)
{
    (void)f;
    return text_layout_create(s, n, fmt, w, h, out);
}
/* GDI-compatible layouts are laid out like ideal ones */
static HRESULT STDMETHODCALLTYPE fa_gdi_text_layout(Factory *f, const WCHAR *s, UINT32 n, void *fmt, float w, float h,
                                                    float ppd, const DW_MATRIX *m, BOOL natural, void **out)
{
    (void)f; (void)ppd; (void)m; (void)natural;
    return text_layout_create(s, n, fmt, w, h, out);
}
static HRESULT STDMETHODCALLTYPE fa_ellipsis(Factory *f, void *fmt, void **out)
{
    (void)f; (void)fmt;
    dw_log("unimplemented IDWriteFactory::CreateEllipsisTrimmingSign");
    *out = NULL;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE fa_text_analyzer(Factory *f, void **out)
{
    (void)f;
    dw_log("unimplemented IDWriteFactory::CreateTextAnalyzer");
    *out = NULL;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE fa_number_subst(Factory *f, UINT32 method, const WCHAR *locale, BOOL ignore, void **out)
{
    (void)f; (void)method; (void)locale; (void)ignore;
    dw_log("unimplemented IDWriteFactory::CreateNumberSubstitution");
    *out = NULL;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE fa_glyph_run_analysis(Factory *f, const DW_GLYPH_RUN *run, float ppd, const DW_MATRIX *m,
                                                       UINT32 mode, UINT32 measuring, float ox, float oy, void **out)
{
    (void)f; (void)measuring;
    return glyph_run_analysis_create(run, ppd, m, mode, AA_CLEARTYPE, ox, oy, out);
}
static HRESULT STDMETHODCALLTYPE fa_eudc(Factory *f, void **out, BOOL check)
{
    (void)f; (void)check;
    Collection *c = coll_new();
    *out = c;
    return c ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE fa_custom_params1(Factory *f, float gamma, float contrast, float gray, float ct,
                                                   UINT32 geometry, UINT32 mode, void **out)
{
    (void)f;
    return params_new(gamma, contrast, gray, ct, geometry, mode, out);
}

/* ---- IDWriteFactory2 ---- */
static HRESULT STDMETHODCALLTYPE fa_system_fallback(Factory *f, void **out) { (void)f; return font_fallback_system(out); }
static HRESULT STDMETHODCALLTYPE fa_fallback_builder(Factory *f, void **out) { (void)f; return font_fallback_builder_create(out); }
static HRESULT STDMETHODCALLTYPE fa_translate_color(Factory *f, float ox, float oy, const DW_GLYPH_RUN *run,
                                                    const DW_GLYPH_RUN_DESCRIPTION *desc, UINT32 measuring,
                                                    const DW_MATRIX *m, UINT32 palette, void **out)
{
    (void)f; (void)measuring; (void)m;
    if (!out) return E_INVALIDARG;
    return color_glyph_run_translate(ox, oy, run, desc, palette, out);
}
static BOOL params_ok(float gamma, float contrast, float gray, float ct, UINT32 geometry)
{
    return gamma > 0 && gamma <= 256 && contrast >= 0 && gray >= 0 && ct >= 0 && ct <= 1 && geometry <= 2;
}
static HRESULT STDMETHODCALLTYPE fa_custom_params2(Factory *f, float gamma, float contrast, float gray, float ct,
                                                   UINT32 geometry, UINT32 mode, UINT32 gridfit, void **out)
{
    (void)f;
    *out = NULL;
    if (!params_ok(gamma, contrast, gray, ct, geometry) || mode > RMODE_OUTLINE || gridfit > GRID_FIT_ENABLED) return E_INVALIDARG;
    return params_new3(gamma, contrast, gray, ct, geometry, mode, gridfit, out);
}
static HRESULT analysis2(const DW_GLYPH_RUN *run, const DW_MATRIX *m, UINT32 mode, UINT32 gridfit, UINT32 aa,
                         float ox, float oy, void **out)
{
    *out = NULL;
    /* the DEFAULT and OUTLINE modes are not ones to rasterize in; ClearType coverage needs a ClearType mode */
    if (mode == RMODE_DEFAULT || mode == RMODE_OUTLINE || gridfit > GRID_FIT_ENABLED || aa > AA_GRAYSCALE) return E_INVALIDARG;
    if (mode == RMODE_ALIASED && aa != AA_CLEARTYPE) return E_INVALIDARG;
    return glyph_run_analysis_create(run, 1.0f, m, mode, aa, ox, oy, out);
}
static HRESULT STDMETHODCALLTYPE fa_glyph_run_analysis2(Factory *f, const DW_GLYPH_RUN *run, const DW_MATRIX *m, UINT32 mode,
                                                        UINT32 measuring, UINT32 gridfit, UINT32 aa, float ox, float oy,
                                                        void **out)
{
    (void)f; (void)measuring;
    if (mode > RMODE_OUTLINE) { *out = NULL; return E_INVALIDARG; }
    return analysis2(run, m, mode, gridfit, aa, ox, oy, out);
}

/* ---- IDWriteFactory3 ---- */
static HRESULT STDMETHODCALLTYPE fa_glyph_run_analysis3(Factory *f, const DW_GLYPH_RUN *run, const DW_MATRIX *m, UINT32 mode,
                                                        UINT32 measuring, UINT32 gridfit, UINT32 aa, float ox, float oy,
                                                        void **out)
{
    (void)f; (void)measuring;
    if (mode > RMODE1_NATURAL_SYMMETRIC_DOWNSAMPLED) { *out = NULL; return E_INVALIDARG; }
    return analysis2(run, m, mode, gridfit, aa, ox, oy, out);
}
static HRESULT STDMETHODCALLTYPE fa_custom_params3(Factory *f, float gamma, float contrast, float gray, float ct,
                                                   UINT32 geometry, UINT32 mode, UINT32 gridfit, void **out)
{
    (void)f;
    *out = NULL;
    if (!params_ok(gamma, contrast, gray, ct, geometry) || mode > RMODE1_NATURAL_SYMMETRIC_DOWNSAMPLED ||
        gridfit > GRID_FIT_ENABLED) return E_INVALIDARG;
    return params_new3(gamma, contrast, gray, ct, geometry, mode, gridfit, out);
}
static HRESULT STDMETHODCALLTYPE fa_face_ref_file(Factory *f, void *file, UINT32 index, UINT32 sims, void **out)
{
    (void)f;
    *out = NULL;
    FontFile *ff = font_file_from(file);
    if (!ff || (sims & ~(SIM_BOLD | SIM_OBLIQUE))) return E_INVALIDARG;
    return face_ref_create(ff, index, sims, out);
}
static HRESULT STDMETHODCALLTYPE fa_face_ref_path(Factory *f, const WCHAR *path, const FILETIME *t, UINT32 index,
                                                  UINT32 sims, void **out)
{
    void *file = NULL;
    HRESULT hr = fa_file_ref(f, path, t, &file);
    if (FAILED(hr)) { *out = NULL; return hr; }
    hr = fa_face_ref_file(f, file, index, sims, out);
    ff_release(file);
    return hr;
}
static HRESULT STDMETHODCALLTYPE fa_system_font_set(Factory *f, void **out)
{
    (void)f;
    Collection *c = system_collection();
    *out = NULL;
    return c ? coll_font_set(c, out) : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE fa_font_set_builder(Factory *f, void **out) { (void)f; return font_set_builder_create(out); }
static HRESULT STDMETHODCALLTYPE fa_collection_from_set(Factory *f, void *set, void **out)
{
    (void)f;
    FontFile **files = NULL;
    UINT32 *index = NULL, n = 0;
    *out = NULL;
    HRESULT hr = font_set_faces(set, &files, &index, &n);
    if (SUCCEEDED(hr)) hr = dw_collection_from_faces(files, index, n, out);
    dw_free(files);
    dw_free(index);
    return hr;
}
static HRESULT STDMETHODCALLTYPE fa_system_collection3(Factory *f, BOOL downloadable, void **out, BOOL check)
{
    (void)downloadable;
    return fa_system_collection(f, out, check);
}
static HRESULT STDMETHODCALLTYPE fa_download_queue(Factory *f, void **out) { (void)f; return font_download_queue(out); }

static const void *const fa_vtbl[] = {
    fa_qi, fa_addref, fa_release, fa_system_collection, fa_custom_collection, fa_register_coll_loader,
    fa_unregister_coll_loader, fa_file_ref, fa_custom_file_ref, fa_font_face, fa_params, fa_monitor_params,
    fa_custom_params, fa_register_file_loader, fa_unregister_file_loader, fa_text_format, fa_typography,
    fa_gdi_interop, fa_text_layout, fa_gdi_text_layout, fa_ellipsis, fa_text_analyzer, fa_number_subst,
    fa_glyph_run_analysis, fa_eudc, fa_custom_params1,
    fa_system_fallback, fa_fallback_builder, fa_translate_color, fa_custom_params2, fa_glyph_run_analysis2,
    fa_glyph_run_analysis3, fa_custom_params3, fa_face_ref_file, fa_face_ref_path, fa_system_font_set,
    fa_font_set_builder, fa_collection_from_set, fa_system_collection3, fa_download_queue,
};
static Factory g_factory = { fa_vtbl };

DWAPI HRESULT WINAPI DWriteCreateFactory(UINT32 type, REFIID riid, void **out)
{
    (void)type;
    if (!out) return E_INVALIDARG;
    HRESULT hr = fa_qi(&g_factory, riid, out);
    if (FAILED(hr)) dw_log("DWriteCreateFactory: interface %x not supported", riid->Data1);
    return hr;
}

