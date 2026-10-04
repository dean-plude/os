/*
 * fontset.c — NovaOS DirectWrite: Windows 10's font model (IDWriteFactory3)
 * and font fallback (IDWriteFactory2)
 *
 *  - IDWriteFontFaceReference: a face in a font file, with simulations,
 *    that makes font faces on demand.
 *  - IDWriteFontSet / IDWriteFontSetBuilder: lists of face references with
 *    their properties (family, face, full and PostScript names, weight,
 *    stretch, style) to filter and match on.  The system font set is the
 *    system font collection's faces.
 *  - IDWriteFontFallback / IDWriteFontFallbackBuilder: which font draws
 *    which characters.  The system fallback keeps the base font for what it
 *    has and picks a system font that has the rest; built fallbacks try
 *    their own mappings first.
 *  - IDWriteFontDownloadQueue: every font is a local file, so it is
 *    always empty.
 */
#include "dwrite_int.h"

#define IS(riid, iid) IsEqualGUID(riid, &(iid))

static UINT32 wlen(const WCHAR *s) { UINT32 n = 0; while (s && s[n]) n++; return n; }

static void utoa_w(UINT32 v, WCHAR *out)
{
    WCHAR t[12];
    int k = 0;
    do { t[k++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    while (k) *out++ = t[--k];
    *out = 0;
}

/* -----------------------------------------------------------------------
 * IDWriteFontFaceReference
 * ----------------------------------------------------------------------- */
typedef struct {
    const void *const *vtbl;
    LONG      ref;
    FontFile *file;
    UINT32    index, sims;
} FaceRef;

extern const void *const ref_vtbl[];

static FaceRef *face_ref_from(void *iface) { return iface && *(void **)iface == (void *)ref_vtbl ? iface : NULL; }

static HRESULT STDMETHODCALLTYPE ref_qi(FaceRef *r, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontFaceReference)) {
        *out = r; InterlockedIncrement(&r->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ref_addref(FaceRef *r) { return (ULONG)InterlockedIncrement(&r->ref); }
static ULONG STDMETHODCALLTYPE ref_release(FaceRef *r)
{
    LONG n = InterlockedDecrement(&r->ref);
    if (!n) { COM_RELEASE(r->file); dw_free(r); }
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE ref_face_sims(FaceRef *r, UINT32 sims, void **out)
{
    if (sims & ~(SIM_BOLD | SIM_OBLIQUE)) { *out = NULL; return E_INVALIDARG; }
    return font_face_create(r->file, r->index, sims, (FontFace **)out);
}
static HRESULT STDMETHODCALLTYPE ref_face(FaceRef *r, void **out) { return ref_face_sims(r, r->sims, out); }
static BOOL STDMETHODCALLTYPE ref_equals(FaceRef *r, void *other)
{
    FaceRef *o = face_ref_from(other);
    return o && o->index == r->index && o->sims == r->sims && font_file_equal(o->file, r->file);
}
static UINT32 STDMETHODCALLTYPE ref_index(FaceRef *r) { return r->index; }
static UINT32 STDMETHODCALLTYPE ref_sims(FaceRef *r) { return r->sims; }
static HRESULT STDMETHODCALLTYPE ref_file(FaceRef *r, void **out)
{
    COM_ADDREF(r->file);
    *out = r->file;
    return S_OK;
}
static UINT64 STDMETHODCALLTYPE ref_size(FaceRef *r)
{
    FontData *d = NULL;
    font_file_data(r->file, &d);
    UINT64 n = d ? d->size : 0;
    font_data_release(d);
    return n;
}
static HRESULT STDMETHODCALLTYPE ref_time(FaceRef *r, FILETIME *t)
{
    typedef HRESULT (STDMETHODCALLTYPE *GetLastWriteTime)(void *, const void *, UINT32, FILETIME *);
    memset(t, 0, sizeof(*t));
    void *local = NULL;
    if (SUCCEEDED(COM_QI(r->file->loader, &IID_IDWriteLocalFontFileLoader, &local)) && local) {
        HRESULT hr = ((GetLastWriteTime)VT(local)[6])(local, r->file->key, r->file->key_size, t);
        COM_RELEASE(local);
        return hr;
    }
    return S_OK;
}
static UINT32 STDMETHODCALLTYPE ref_locality(FaceRef *r) { (void)r; return LOCALITY_LOCAL; }
/* nothing is remote, so there is nothing to download */
static HRESULT STDMETHODCALLTYPE ref_enqueue(FaceRef *r) { (void)r; return S_OK; }
static HRESULT STDMETHODCALLTYPE ref_enqueue_chars(FaceRef *r, const WCHAR *s, UINT32 n) { (void)r; (void)s; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE ref_enqueue_glyphs(FaceRef *r, const UINT16 *g, UINT32 n) { (void)r; (void)g; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE ref_enqueue_fragment(FaceRef *r, UINT64 off, UINT64 n) { (void)r; (void)off; (void)n; return S_OK; }
const void *const ref_vtbl[] = {
    ref_qi, ref_addref, ref_release, ref_face, ref_face_sims, ref_equals, ref_index, ref_sims, ref_file,
    ref_size, ref_size, ref_time, ref_locality, ref_enqueue, ref_enqueue_chars, ref_enqueue_glyphs, ref_enqueue_fragment,
};

HRESULT face_ref_create(FontFile *file, UINT32 index, UINT32 sims, void **out)
{
    FaceRef *r = dw_zalloc(sizeof(*r));
    if (!r) { *out = NULL; return E_OUTOFMEMORY; }
    r->vtbl = ref_vtbl;
    r->ref = 1;
    r->file = file;
    COM_ADDREF(file);
    r->index = index;
    r->sims = sims & (SIM_BOLD | SIM_OBLIQUE);
    *out = r;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * IDWriteStringList
 * ----------------------------------------------------------------------- */
typedef struct {
    const void *const *vtbl;
    LONG    ref;
    UINT32  n;
    WCHAR **strs;
} StrList;

static HRESULT STDMETHODCALLTYPE sl_qi(StrList *l, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteStringList)) {
        *out = l; InterlockedIncrement(&l->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sl_addref(StrList *l) { return (ULONG)InterlockedIncrement(&l->ref); }
static ULONG STDMETHODCALLTYPE sl_release(StrList *l)
{
    LONG r = InterlockedDecrement(&l->ref);
    if (!r) {
        for (UINT32 i = 0; i < l->n; i++) dw_free(l->strs[i]);
        dw_free(l->strs);
        dw_free(l);
    }
    return (ULONG)r;
}
static UINT32 STDMETHODCALLTYPE sl_count(StrList *l) { return l->n; }
static HRESULT copy_out(const WCHAR *s, WCHAR *buf, UINT32 size)
{
    UINT32 n = wlen(s);
    if (!buf || size <= n) { if (buf && size) buf[0] = 0; return E_NOT_SUFFICIENT_BUFFER_; }
    memcpy(buf, s, (n + 1) * sizeof(WCHAR));
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sl_locale_len(StrList *l, UINT32 i, UINT32 *len)
{
    if (i >= l->n) { *len = 0; return E_INVALIDARG; }
    *len = 5;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sl_locale(StrList *l, UINT32 i, WCHAR *buf, UINT32 size)
{
    if (i >= l->n) { if (buf && size) buf[0] = 0; return E_INVALIDARG; }
    return copy_out(L"en-us", buf, size);
}
static HRESULT STDMETHODCALLTYPE sl_string_len(StrList *l, UINT32 i, UINT32 *len)
{
    if (i >= l->n) { *len = 0; return E_INVALIDARG; }
    *len = wlen(l->strs[i]);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sl_string(StrList *l, UINT32 i, WCHAR *buf, UINT32 size)
{
    if (i >= l->n) { if (buf && size) buf[0] = 0; return E_INVALIDARG; }
    return copy_out(l->strs[i], buf, size);
}
static const void *const sl_vtbl[] = { sl_qi, sl_addref, sl_release, sl_count, sl_locale_len, sl_locale, sl_string_len, sl_string };

/* -----------------------------------------------------------------------
 * IDWriteFontSet
 * ----------------------------------------------------------------------- */
typedef struct {
    FontFile         *file;
    UINT32            index, sims;
    FontData         *data;          /* NULL if the file does not load */
    FaceData         *face;
    UINT32            nprops;        /* properties a set builder was given */
    DW_FONT_PROPERTY *props;         /* owns the strings */
} SetEntry;

typedef struct {
    const void *const *vtbl;
    LONG      ref;
    UINT32    n;
    SetEntry *e;
    BOOL      system;
} FontSet;

extern const void *const set_vtbl[];

static FontSet *font_set_from(void *iface) { return iface && *(void **)iface == (void *)set_vtbl ? iface : NULL; }

static void props_free(DW_FONT_PROPERTY *p, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++) { dw_free((void *)p[i].propertyValue); dw_free((void *)p[i].localeName); }
    dw_free(p);
}
static DW_FONT_PROPERTY *props_copy(const DW_FONT_PROPERTY *p, UINT32 n)
{
    DW_FONT_PROPERTY *c = n ? dw_zalloc(n * sizeof(*c)) : NULL;
    if (!c) return NULL;
    for (UINT32 i = 0; i < n; i++) {
        c[i].propertyId = p[i].propertyId;
        c[i].propertyValue = dw_wcsdup(p[i].propertyValue ? p[i].propertyValue : L"");
        c[i].localeName = dw_wcsdup(p[i].localeName ? p[i].localeName : L"");
    }
    return c;
}

static void entry_init(SetEntry *e, FontFile *file, UINT32 index, UINT32 sims, const DW_FONT_PROPERTY *props, UINT32 nprops)
{
    memset(e, 0, sizeof(*e));
    e->file = file;
    COM_ADDREF(file);
    e->index = index;
    e->sims = sims;
    if (SUCCEEDED(font_file_data(file, &e->data)) && e->data) {
        e->face = font_face_data(e->data, index);
        if (!e->face) { font_data_release(e->data); e->data = NULL; }
    } else e->data = NULL;
    if (props && nprops) {
        e->props = props_copy(props, nprops);
        if (e->props) e->nprops = nprops;
    }
}
static void entry_free(SetEntry *e)
{
    COM_RELEASE(e->file);
    font_data_release(e->data);
    props_free(e->props, e->nprops);
}

/* an entry's value of a property; 0 if it has none */
static int entry_prop(const SetEntry *e, UINT32 id, WCHAR *out, int cap)
{
    for (UINT32 i = 0; i < e->nprops; i++)
        if (e->props[i].propertyId == id) {
            const WCHAR *v = e->props[i].propertyValue;
            UINT32 n = wlen(v);
            if ((int)n >= cap) n = (UINT32)cap - 1;
            memcpy(out, v, n * sizeof(WCHAR));
            out[n] = 0;
            return 1;
        }
    const FaceData *f = e->face;
    if (!f) return 0;
    switch (id) {
    case 1:  return face_info_string(f, 19, out, cap);                                 /* weight/stretch/style family */
    case 2:  return face_info_string(f, 13, out, cap);                                 /* typographic family */
    case 3:  return face_info_string(f, 14, out, cap);                                 /* weight/stretch/style face */
    case 4:  return face_info_string(f, 16, out, cap);                                 /* full name */
    case 5:  return face_info_string(f, 11, out, cap);                                 /* Win32 family */
    case 6:  return face_info_string(f, 17, out, cap);                                 /* PostScript name */
    case 10: utoa_w(f->weight, out); return 1;
    case 11: utoa_w(f->stretch, out); return 1;
    case 12: utoa_w((e->sims & SIM_OBLIQUE) && f->style == STYLE_NORMAL ? STYLE_OBLIQUE : f->style, out); return 1;
    case 13: return face_info_string(f, 14, out, cap);                                 /* typographic face */
    default: return 0;   /* script and language tags, semantic tags */
    }
}
static BOOL entry_has(const SetEntry *e, const DW_FONT_PROPERTY *p)
{
    WCHAR v[256];
    return p->propertyValue && entry_prop(e, p->propertyId, v, 256) && dw_wcsieq(v, p->propertyValue);
}

static FontSet *set_new(UINT32 cap)
{
    FontSet *s = dw_zalloc(sizeof(*s));
    if (!s) return NULL;
    s->e = dw_zalloc((cap ? cap : 1) * sizeof(SetEntry));
    if (!s->e) { dw_free(s); return NULL; }
    s->vtbl = set_vtbl;
    s->ref = 1;
    return s;
}

static HRESULT STDMETHODCALLTYPE set_qi(FontSet *s, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontSet)) {
        *out = s; InterlockedIncrement(&s->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE set_addref(FontSet *s) { return (ULONG)InterlockedIncrement(&s->ref); }
static ULONG STDMETHODCALLTYPE set_release(FontSet *s)
{
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        for (UINT32 i = 0; i < s->n; i++) entry_free(&s->e[i]);
        dw_free(s->e);
        dw_free(s);
    }
    return (ULONG)r;
}
static UINT32 STDMETHODCALLTYPE set_count(FontSet *s) { return s->n; }
static HRESULT STDMETHODCALLTYPE set_face_ref(FontSet *s, UINT32 i, void **out)
{
    if (i >= s->n) { *out = NULL; return E_INVALIDARG; }
    return face_ref_create(s->e[i].file, s->e[i].index, s->e[i].sims, out);
}
static HRESULT STDMETHODCALLTYPE set_find_ref(FontSet *s, void *ref, UINT32 *index, BOOL *exists)
{
    FaceRef *r = face_ref_from(ref);
    *index = 0xFFFFFFFF;
    *exists = FALSE;
    if (!r) return E_INVALIDARG;
    for (UINT32 i = 0; i < s->n; i++)
        if (s->e[i].index == r->index && s->e[i].sims == r->sims && font_file_equal(s->e[i].file, r->file)) {
            *index = i;
            *exists = TRUE;
            break;
        }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE set_find_face(FontSet *s, void *face, UINT32 *index, BOOL *exists)
{
    FontFace *f = font_face_from(face);
    *index = 0xFFFFFFFF;
    *exists = FALSE;
    if (!f) return E_INVALIDARG;
    for (UINT32 i = 0; i < s->n; i++)
        if (s->e[i].index == f->index && s->e[i].sims == f->sims && font_file_equal(s->e[i].file, f->file)) {
            *index = i;
            *exists = TRUE;
            break;
        }
    return S_OK;
}
/* the distinct values of a property across the set, in the order they first appear */
static HRESULT STDMETHODCALLTYPE set_values(FontSet *s, UINT32 id, void **out)
{
    *out = NULL;
    if (id == 0 || id > 13) return E_INVALIDARG;
    StrList *l = dw_zalloc(sizeof(*l));
    if (!l || !(l->strs = dw_zalloc((s->n + 1) * sizeof(WCHAR *)))) { dw_free(l); return E_OUTOFMEMORY; }
    l->vtbl = sl_vtbl;
    l->ref = 1;
    for (UINT32 i = 0; i < s->n; i++) {
        WCHAR v[256];
        if (!entry_prop(&s->e[i], id, v, 256)) continue;
        UINT32 k = 0;
        while (k < l->n && !dw_wcsieq(l->strs[k], v)) k++;
        if (k == l->n) l->strs[l->n++] = dw_wcsdup(v);
    }
    *out = l;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE set_values_locales(FontSet *s, UINT32 id, const WCHAR *locales, void **out)
{
    (void)locales;          /* every name is English */
    return set_values(s, id, out);
}
static HRESULT STDMETHODCALLTYPE set_values_at(FontSet *s, UINT32 i, UINT32 id, BOOL *exists, void **out)
{
    WCHAR v[256];
    *exists = FALSE;
    *out = NULL;
    if (i >= s->n || id == 0 || id > 13) return E_INVALIDARG;
    if (!entry_prop(&s->e[i], id, v, 256)) return S_OK;
    *exists = TRUE;
    return dw_locstrings(v, out);
}
static HRESULT STDMETHODCALLTYPE set_occurrences(FontSet *s, const DW_FONT_PROPERTY *p, UINT32 *count)
{
    *count = 0;
    if (!p) return E_INVALIDARG;
    for (UINT32 i = 0; i < s->n; i++) if (entry_has(&s->e[i], p)) (*count)++;
    return S_OK;
}
static HRESULT subset(FontSet *s, const UINT32 *pick, UINT32 n, void **out)
{
    FontSet *r = set_new(n);
    *out = NULL;
    if (!r) return E_OUTOFMEMORY;
    for (UINT32 i = 0; i < n; i++) {
        const SetEntry *e = &s->e[pick[i]];
        entry_init(&r->e[r->n++], e->file, e->index, e->sims, e->props, e->nprops);
    }
    *out = r;
    return S_OK;
}
/* every font whose properties include all of these */
static HRESULT STDMETHODCALLTYPE set_matching_props(FontSet *s, const DW_FONT_PROPERTY *p, UINT32 np, void **out)
{
    *out = NULL;
    if (np && !p) return E_INVALIDARG;
    UINT32 *pick = dw_alloc((s->n + 1) * sizeof(UINT32)), n = 0;
    if (!pick) return E_OUTOFMEMORY;
    for (UINT32 i = 0; i < s->n; i++) {
        UINT32 k = 0;
        while (k < np && entry_has(&s->e[i], &p[k])) k++;
        if (k == np) pick[n++] = i;
    }
    HRESULT hr = subset(s, pick, n, out);
    dw_free(pick);
    return hr;
}
static BOOL entry_in_family(const SetEntry *e, const WCHAR *family)
{
    static const UINT32 ids[] = { 1, 2, 5 };
    WCHAR v[256];
    for (int i = 0; i < 3; i++)
        if (entry_prop(e, ids[i], v, 256) && dw_wcsieq(v, family)) return TRUE;
    return FALSE;
}
/* the fonts of a family, best match for weight, stretch and style first */
static HRESULT STDMETHODCALLTYPE set_matching(FontSet *s, const WCHAR *family, UINT32 weight, UINT32 stretch, UINT32 style,
                                              void **out)
{
    *out = NULL;
    if (!family) return E_INVALIDARG;
    UINT32 *pick = dw_alloc((s->n + 1) * sizeof(UINT32)), n = 0;
    if (!pick) return E_OUTOFMEMORY;
    for (UINT32 i = 0; i < s->n; i++)
        if (s->e[i].face && entry_in_family(&s->e[i], family)) pick[n++] = i;
    if (!n && s->system) {
        /* the default names ("Segoe UI", "Consolas"...) stand for the fonts that back them */
        void *c = dw_system_collection(), *font = NULL;
        typedef HRESULT (STDMETHODCALLTYPE *FindFamily)(void *, const WCHAR *, UINT32 *, BOOL *);
        typedef HRESULT (STDMETHODCALLTYPE *GetFamily)(void *, UINT32, void **);
        typedef UINT32  (STDMETHODCALLTYPE *GetCount)(void *);
        typedef HRESULT (STDMETHODCALLTYPE *GetFont)(void *, UINT32, void **);
        typedef HRESULT (STDMETHODCALLTYPE *CreateFace)(void *, void **);
        UINT32 fi = 0;
        BOOL found = FALSE;
        void *fam = NULL;
        if (c && SUCCEEDED(((FindFamily)VT(c)[5])(c, family, &fi, &found)) && found &&
            SUCCEEDED(((GetFamily)VT(c)[4])(c, fi, &fam)) && fam) {
            UINT32 count = ((GetCount)VT(fam)[4])(fam);
            for (UINT32 k = 0; k < count; k++) {
                void *face = NULL;
                if (FAILED(((GetFont)VT(fam)[5])(fam, k, &font)) || !font) continue;
                ((CreateFace)VT(font)[13])(font, &face);
                COM_RELEASE(font);
                UINT32 idx = 0;
                BOOL has = FALSE;
                if (face) { set_find_face(s, face, &idx, &has); COM_RELEASE(face); }
                if (has) {
                    UINT32 j = 0;
                    while (j < n && pick[j] != idx) j++;
                    if (j == n) pick[n++] = idx;
                }
            }
            COM_RELEASE(fam);
        }
        if (c) COM_RELEASE(c);
    }
    for (UINT32 i = 1; i < n; i++)
        for (UINT32 k = i; k > 0 && dw_match_score(s->e[pick[k]].face, weight, stretch, style) <
                                   dw_match_score(s->e[pick[k - 1]].face, weight, stretch, style); k--) {
            UINT32 t = pick[k]; pick[k] = pick[k - 1]; pick[k - 1] = t;
        }
    HRESULT hr = subset(s, pick, n, out);
    dw_free(pick);
    return hr;
}
const void *const set_vtbl[] = {
    set_qi, set_addref, set_release, set_count, set_face_ref, set_find_ref, set_find_face,
    set_values, set_values_locales, set_values_at, set_occurrences, set_matching, set_matching_props,
};

HRESULT font_set_create(FontFile *const *files, const UINT32 *index, UINT32 n, BOOL system, void **out)
{
    FontSet *s = set_new(n);
    *out = NULL;
    if (!s) return E_OUTOFMEMORY;
    s->system = system;
    for (UINT32 i = 0; i < n; i++) entry_init(&s->e[s->n++], files[i], index[i], 0, NULL, 0);
    *out = s;
    return S_OK;
}

HRESULT font_set_faces(void *set, FontFile ***files, UINT32 **index, UINT32 *n)
{
    FontSet *s = font_set_from(set);
    *files = NULL; *index = NULL; *n = 0;
    if (!s) return E_INVALIDARG;
    *files = dw_alloc((s->n + 1) * sizeof(FontFile *));
    *index = dw_alloc((s->n + 1) * sizeof(UINT32));
    if (!*files || !*index) { dw_free(*files); dw_free(*index); *files = NULL; *index = NULL; return E_OUTOFMEMORY; }
    for (UINT32 i = 0; i < s->n; i++) { (*files)[i] = s->e[i].file; (*index)[i] = s->e[i].index; }
    *n = s->n;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * IDWriteFontSetBuilder
 * ----------------------------------------------------------------------- */
typedef struct {
    const void *const *vtbl;
    LONG      ref;
    UINT32    n, cap;
    SetEntry *e;
} SetBuilder;

static HRESULT STDMETHODCALLTYPE sb_qi(SetBuilder *b, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontSetBuilder)) {
        *out = b; InterlockedIncrement(&b->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sb_addref(SetBuilder *b) { return (ULONG)InterlockedIncrement(&b->ref); }
static ULONG STDMETHODCALLTYPE sb_release(SetBuilder *b)
{
    LONG r = InterlockedDecrement(&b->ref);
    if (!r) {
        for (UINT32 i = 0; i < b->n; i++) entry_free(&b->e[i]);
        dw_free(b->e);
        dw_free(b);
    }
    return (ULONG)r;
}
static SetEntry *sb_slot(SetBuilder *b)
{
    if (b->n == b->cap) {
        UINT32 cap = b->cap ? b->cap * 2 : 16;
        SetEntry *e = dw_zalloc(cap * sizeof(SetEntry));
        if (!e) return NULL;
        if (b->e) memcpy(e, b->e, b->n * sizeof(SetEntry));
        dw_free(b->e);
        b->e = e;
        b->cap = cap;
    }
    return &b->e[b->n++];
}
static HRESULT STDMETHODCALLTYPE sb_add_props(SetBuilder *b, void *ref, const DW_FONT_PROPERTY *p, UINT32 np)
{
    FaceRef *r = face_ref_from(ref);
    if (!r || (np && !p)) return E_INVALIDARG;
    SetEntry *e = sb_slot(b);
    if (!e) return E_OUTOFMEMORY;
    entry_init(e, r->file, r->index, r->sims, p, np);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sb_add(SetBuilder *b, void *ref) { return sb_add_props(b, ref, NULL, 0); }
static HRESULT STDMETHODCALLTYPE sb_add_set(SetBuilder *b, void *set)
{
    FontSet *s = font_set_from(set);
    if (!s) return E_INVALIDARG;
    for (UINT32 i = 0; i < s->n; i++) {
        SetEntry *e = sb_slot(b);
        if (!e) return E_OUTOFMEMORY;
        entry_init(e, s->e[i].file, s->e[i].index, s->e[i].sims, s->e[i].props, s->e[i].nprops);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sb_create(SetBuilder *b, void **out)
{
    FontSet *s = set_new(b->n);
    *out = NULL;
    if (!s) return E_OUTOFMEMORY;
    for (UINT32 i = 0; i < b->n; i++)
        entry_init(&s->e[s->n++], b->e[i].file, b->e[i].index, b->e[i].sims, b->e[i].props, b->e[i].nprops);
    *out = s;
    return S_OK;
}
static const void *const sb_vtbl[] = { sb_qi, sb_addref, sb_release, sb_add_props, sb_add, sb_add_set, sb_create };

HRESULT font_set_builder_create(void **out)
{
    SetBuilder *b = dw_zalloc(sizeof(*b));
    if (!b) { *out = NULL; return E_OUTOFMEMORY; }
    b->vtbl = sb_vtbl;
    b->ref = 1;
    *out = b;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * IDWriteFontFallback
 * ----------------------------------------------------------------------- */
typedef struct {
    DW_UNICODE_RANGE *ranges;
    UINT32            nranges;
    WCHAR           **families;
    UINT32            nfamilies;
    void             *coll;           /* IDWriteFontCollection, or NULL for the system's */
    WCHAR            *base;           /* only for this base family, if set */
    float             scale;
} Mapping;

typedef struct {
    const void *const *vtbl;
    LONG     ref;
    UINT32   n;
    Mapping *m;
    BOOL     system;                  /* then the system fallback */
} Fallback;

extern const void *const fb_vtbl[];

typedef HRESULT (STDMETHODCALLTYPE *FindFamily)(void *, const WCHAR *, UINT32 *, BOOL *);
typedef HRESULT (STDMETHODCALLTYPE *GetFamily)(void *, UINT32, void **);
typedef UINT32  (STDMETHODCALLTYPE *GetCount)(void *);
typedef HRESULT (STDMETHODCALLTYPE *FirstMatching)(void *, UINT32, UINT32, UINT32, void **);
typedef HRESULT (STDMETHODCALLTYPE *HasChar)(void *, UINT32, BOOL *);
typedef HRESULT (STDMETHODCALLTYPE *TextAt)(void *, UINT32, const WCHAR **, UINT32 *);
typedef BOOL    (STDMETHODCALLTYPE *FontEquals)(void *, void *);     /* IDWriteFont3::Equals */

static void mapping_free(Mapping *m)
{
    dw_free(m->ranges);
    for (UINT32 i = 0; i < m->nfamilies; i++) dw_free(m->families[i]);
    dw_free(m->families);
    if (m->coll) COM_RELEASE(m->coll);
    dw_free(m->base);
}
static BOOL mapping_copy(Mapping *d, const Mapping *s)
{
    memset(d, 0, sizeof(*d));
    d->ranges = dw_alloc((s->nranges + 1) * sizeof(DW_UNICODE_RANGE));
    d->families = dw_zalloc((s->nfamilies + 1) * sizeof(WCHAR *));
    if (!d->ranges || !d->families) { dw_free(d->ranges); dw_free(d->families); return FALSE; }
    memcpy(d->ranges, s->ranges, s->nranges * sizeof(DW_UNICODE_RANGE));
    d->nranges = s->nranges;
    for (UINT32 i = 0; i < s->nfamilies; i++) d->families[i] = dw_wcsdup(s->families[i]);
    d->nfamilies = s->nfamilies;
    d->coll = s->coll;
    if (d->coll) COM_ADDREF(d->coll);
    d->base = s->base ? dw_wcsdup(s->base) : NULL;
    d->scale = s->scale;
    return TRUE;
}

static void *match_font(void *coll, const WCHAR *family, UINT32 weight, UINT32 style, UINT32 stretch)
{
    UINT32 idx = 0;
    BOOL exists = FALSE;
    void *fam = NULL, *font = NULL;
    if (!coll || !family || FAILED(((FindFamily)VT(coll)[5])(coll, family, &idx, &exists)) || !exists) return NULL;
    if (FAILED(((GetFamily)VT(coll)[4])(coll, idx, &fam)) || !fam) return NULL;
    ((FirstMatching)VT(fam)[7])(fam, weight, stretch, style, &font);
    COM_RELEASE(fam);
    return font;
}
static BOOL font_has(void *font, UINT32 c)
{
    BOOL has = FALSE;
    return font && SUCCEEDED(((HasChar)VT(font)[12])(font, c, &has)) && has;
}

/* characters any font can carry: spaces, controls, joiners, variation selectors, combining marks */
static BOOL no_font_needed(UINT32 c)
{
    return c < 0x20 || c == ' ' || c == 0xA0 || (c >= 0x2000 && c <= 0x200F) || (c >= 0x2028 && c <= 0x202F) ||
           c == 0x2060 || c == 0xFEFF || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x300 && c <= 0x36F) || c == 0x3000;
}

static const WCHAR *const fallback_families[] = {
    L"Noto Sans Arabic", L"Noto Sans Hebrew", L"Noto Sans Devanagari", L"DejaVu Sans", L"Inter", L"DejaVu Sans Mono", 0 };

/* a system font with c in it */
static void *system_font_for(void *sys, UINT32 c, UINT32 weight, UINT32 style, UINT32 stretch)
{
    for (int i = 0; fallback_families[i]; i++) {
        void *f = match_font(sys, fallback_families[i], weight, style, stretch);
        if (font_has(f, c)) return f;
        if (f) COM_RELEASE(f);
    }
    UINT32 n = sys ? ((GetCount)VT(sys)[3])(sys) : 0;
    for (UINT32 i = 0; i < n; i++) {
        void *fam = NULL, *f = NULL;
        if (FAILED(((GetFamily)VT(sys)[4])(sys, i, &fam)) || !fam) continue;
        ((FirstMatching)VT(fam)[7])(fam, weight, stretch, style, &f);
        COM_RELEASE(fam);
        if (font_has(f, c)) return f;
        if (f) COM_RELEASE(f);
    }
    return NULL;
}

typedef struct {
    Fallback   *fb;
    void       *sys, *base;            /* the system collection; the base font, if any */
    const WCHAR *base_family;
    UINT32      weight, style, stretch;
} MapCtx;

/* the font for c, referenced; *is_base when it is the base font; *scale from the mapping */
static void *font_for(MapCtx *x, UINT32 c, BOOL *is_base, float *scale)
{
    *is_base = FALSE;
    *scale = 1.0f;
    if (font_has(x->base, c)) { *is_base = TRUE; COM_ADDREF(x->base); return x->base; }
    for (UINT32 i = 0; i < x->fb->n; i++) {
        Mapping *m = &x->fb->m[i];
        if (m->base && *m->base && !(x->base_family && dw_wcsieq(m->base, x->base_family))) continue;
        UINT32 r = 0;
        while (r < m->nranges && !(c >= m->ranges[r].first && c <= m->ranges[r].last)) r++;
        if (r == m->nranges) continue;
        for (UINT32 k = 0; k < m->nfamilies; k++) {
            void *f = match_font(m->coll ? m->coll : x->sys, m->families[k], x->weight, x->style, x->stretch);
            if (font_has(f, c)) { *scale = m->scale; return f; }
            if (f) COM_RELEASE(f);
        }
    }
    return x->fb->system ? system_font_for(x->sys, c, x->weight, x->style, x->stretch) : NULL;
}

static UINT32 next_char(const WCHAR *s, UINT32 n, UINT32 i, UINT32 *len)
{
    *len = 1;
    if (s[i] >= 0xD800 && s[i] <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
        *len = 2;
        return 0x10000 + ((UINT32)(s[i] - 0xD800) << 10) + (s[i + 1] - 0xDC00);
    }
    return s[i];
}

static HRESULT STDMETHODCALLTYPE fb_qi(Fallback *f, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontFallback)) {
        *out = f; InterlockedIncrement(&f->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fb_addref(Fallback *f) { return (ULONG)InterlockedIncrement(&f->ref); }
static ULONG STDMETHODCALLTYPE fb_release(Fallback *f)
{
    LONG r = InterlockedDecrement(&f->ref);
    if (!r) {
        for (UINT32 i = 0; i < f->n; i++) mapping_free(&f->m[i]);
        dw_free(f->m);
        dw_free(f);
    }
    return (ULONG)r;
}

/*
 * The font for the text at position, and how much of the text it covers:
 * the base font where it has the characters, otherwise a mapped or system
 * font.  Characters no font is needed for go with their neighbours.  Text
 * no font has comes back with no font, so the caller can draw it as missing.
 */
static HRESULT STDMETHODCALLTYPE fb_map(Fallback *fb, void *source, UINT32 pos, UINT32 length, void *coll,
                                        const WCHAR *family, UINT32 weight, UINT32 style, UINT32 stretch,
                                        UINT32 *mapped_len, void **mapped_font, float *scale)
{
    if (!mapped_len || !mapped_font || !scale) return E_INVALIDARG;
    *mapped_len = 0;
    *mapped_font = NULL;
    *scale = 1.0f;
    if (!source) return E_INVALIDARG;
    if (!length) return S_OK;

    /* the text, which the source may hand out in pieces */
    WCHAR *text = dw_alloc(length * sizeof(WCHAR));
    if (!text) return E_OUTOFMEMORY;
    UINT32 n = 0;
    while (n < length) {
        const WCHAR *p = NULL;
        UINT32 got = 0;
        if (FAILED(((TextAt)VT(source)[3])(source, pos + n, &p, &got)) || !p || !got) break;
        if (got > length - n) got = length - n;
        memcpy(text + n, p, got * sizeof(WCHAR));
        n += got;
    }
    if (!n) { dw_free(text); return S_OK; }

    MapCtx x = { fb, dw_system_collection(), NULL, family, weight, style, stretch };
    x.base = match_font(coll ? coll : x.sys, family, weight, style, stretch);

    /* the first character that needs a font decides */
    UINT32 i = 0, len = 1;
    void *font = NULL;
    BOOL is_base = FALSE;
    while (i < n && no_font_needed(next_char(text, n, i, &len))) i += len;
    if (i == n) {
        /* nothing but spaces and the like: the base font carries them */
        if (x.base) { COM_ADDREF(x.base); font = x.base; }
        *mapped_len = n;
    } else {
        font = font_for(&x, next_char(text, n, i, &len), &is_base, scale);
        i += len;
        /* the run goes on while each character would pick the same font */
        while (i < n) {
            UINT32 c = next_char(text, n, i, &len);
            if (!no_font_needed(c) && !(is_base && font_has(font, c))) {
                BOOL b;
                float s;
                void *f = font_for(&x, c, &b, &s);
                BOOL same = f && font ? ((FontEquals)VT(font)[20])(font, f) : f == font;
                if (f) COM_RELEASE(f);
                if (!same) break;
            }
            i += len;
        }
        *mapped_len = i;
    }
    *mapped_font = font;
    if (x.base) COM_RELEASE(x.base);
    if (x.sys) COM_RELEASE(x.sys);
    dw_free(text);
    return S_OK;
}
const void *const fb_vtbl[] = { fb_qi, fb_addref, fb_release, fb_map };

static Fallback *fallback_new(void)
{
    Fallback *f = dw_zalloc(sizeof(*f));
    if (!f) return NULL;
    f->vtbl = fb_vtbl;
    f->ref = 1;
    return f;
}

HRESULT font_fallback_system(void **out)
{
    Fallback *f = fallback_new();
    *out = f;
    if (!f) return E_OUTOFMEMORY;
    f->system = TRUE;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * IDWriteFontFallbackBuilder
 * ----------------------------------------------------------------------- */
typedef struct {
    const void *const *vtbl;
    LONG     ref;
    UINT32   n, cap;
    Mapping *m;
    BOOL     system;
} FallbackBuilder;

static HRESULT STDMETHODCALLTYPE fbb_qi(FallbackBuilder *b, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontFallbackBuilder)) {
        *out = b; InterlockedIncrement(&b->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fbb_addref(FallbackBuilder *b) { return (ULONG)InterlockedIncrement(&b->ref); }
static ULONG STDMETHODCALLTYPE fbb_release(FallbackBuilder *b)
{
    LONG r = InterlockedDecrement(&b->ref);
    if (!r) {
        for (UINT32 i = 0; i < b->n; i++) mapping_free(&b->m[i]);
        dw_free(b->m);
        dw_free(b);
    }
    return (ULONG)r;
}
static BOOL fbb_push(FallbackBuilder *b, const Mapping *m)
{
    if (b->n == b->cap) {
        UINT32 cap = b->cap ? b->cap * 2 : 8;
        Mapping *nm = dw_zalloc(cap * sizeof(Mapping));
        if (!nm) return FALSE;
        if (b->m) memcpy(nm, b->m, b->n * sizeof(Mapping));
        dw_free(b->m);
        b->m = nm;
        b->cap = cap;
    }
    if (!mapping_copy(&b->m[b->n], m)) return FALSE;
    b->n++;
    return TRUE;
}
static HRESULT STDMETHODCALLTYPE fbb_add(FallbackBuilder *b, const DW_UNICODE_RANGE *ranges, UINT32 nranges,
                                         const WCHAR **families, UINT32 nfamilies, void *coll, const WCHAR *locale,
                                         const WCHAR *base, float scale)
{
    (void)locale;
    if (!ranges || !nranges || !families || !nfamilies || scale <= 0) return E_INVALIDARG;
    for (UINT32 i = 0; i < nranges; i++) if (ranges[i].first > ranges[i].last) return E_INVALIDARG;
    for (UINT32 i = 0; i < nfamilies; i++) if (!families[i]) return E_INVALIDARG;
    Mapping m = { (DW_UNICODE_RANGE *)ranges, nranges, (WCHAR **)families, nfamilies, coll, (WCHAR *)base, scale };
    return fbb_push(b, &m) ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE fbb_add_fallback(FallbackBuilder *b, void *other)
{
    Fallback *f = other && *(void **)other == (void *)fb_vtbl ? other : NULL;
    if (!f) return E_INVALIDARG;
    for (UINT32 i = 0; i < f->n; i++) if (!fbb_push(b, &f->m[i])) return E_OUTOFMEMORY;
    if (f->system) b->system = TRUE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fbb_create(FallbackBuilder *b, void **out)
{
    Fallback *f = fallback_new();
    *out = NULL;
    if (!f) return E_OUTOFMEMORY;
    f->m = dw_zalloc((b->n + 1) * sizeof(Mapping));
    if (!f->m) { dw_free(f); return E_OUTOFMEMORY; }
    for (UINT32 i = 0; i < b->n; i++)
        if (mapping_copy(&f->m[f->n], &b->m[i])) f->n++;
    f->system = b->system;
    *out = f;
    return S_OK;
}
static const void *const fbb_vtbl[] = { fbb_qi, fbb_addref, fbb_release, fbb_add, fbb_add_fallback, fbb_create };

HRESULT font_fallback_builder_create(void **out)
{
    FallbackBuilder *b = dw_zalloc(sizeof(*b));
    if (!b) { *out = NULL; return E_OUTOFMEMORY; }
    b->vtbl = fbb_vtbl;
    b->ref = 1;
    *out = b;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * IDWriteFontDownloadQueue: one per process, always empty
 * ----------------------------------------------------------------------- */
#define MAX_LISTENERS 32
typedef struct { const void *const *vtbl; } DownloadQueue;
static void *g_listeners[MAX_LISTENERS];
static SRWLOCK g_listeners_lock = SRWLOCK_INIT;

static HRESULT STDMETHODCALLTYPE dq_qi(DownloadQueue *q, REFIID riid, void **out)
{
    if (IS(riid, IID_IUnknown) || IS(riid, IID_IDWriteFontDownloadQueue)) { *out = q; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dq_addref(DownloadQueue *q) { (void)q; return 2; }
static ULONG STDMETHODCALLTYPE dq_release(DownloadQueue *q) { (void)q; return 1; }
static HRESULT STDMETHODCALLTYPE dq_add_listener(DownloadQueue *q, void *l, UINT32 *token)
{
    (void)q;
    if (!l || !token) return E_INVALIDARG;
    HRESULT hr = E_OUTOFMEMORY;
    AcquireSRWLockExclusive(&g_listeners_lock);
    for (UINT32 i = 0; i < MAX_LISTENERS; i++)
        if (!g_listeners[i]) { g_listeners[i] = l; COM_ADDREF(l); *token = i; hr = S_OK; break; }
    ReleaseSRWLockExclusive(&g_listeners_lock);
    return hr;
}
static HRESULT STDMETHODCALLTYPE dq_remove_listener(DownloadQueue *q, UINT32 token)
{
    (void)q;
    void *l = NULL;
    AcquireSRWLockExclusive(&g_listeners_lock);
    if (token < MAX_LISTENERS) { l = g_listeners[token]; g_listeners[token] = NULL; }
    ReleaseSRWLockExclusive(&g_listeners_lock);
    if (!l) return E_INVALIDARG;
    COM_RELEASE(l);
    return S_OK;
}
static BOOL STDMETHODCALLTYPE dq_empty(DownloadQueue *q) { (void)q; return TRUE; }
static HRESULT STDMETHODCALLTYPE dq_begin(DownloadQueue *q, void *ctx) { (void)q; (void)ctx; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE dq_cancel(DownloadQueue *q) { (void)q; return S_OK; }
static UINT64 STDMETHODCALLTYPE dq_generation(DownloadQueue *q) { (void)q; return 0; }
static const void *const dq_vtbl[] = {
    dq_qi, dq_addref, dq_release, dq_add_listener, dq_remove_listener, dq_empty, dq_begin, dq_cancel, dq_generation,
};
static DownloadQueue g_queue = { dq_vtbl };

HRESULT font_download_queue(void **out)
{
    if (!out) return E_INVALIDARG;
    *out = &g_queue;
    return S_OK;
}
