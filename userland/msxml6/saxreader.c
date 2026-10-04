/*
 * msxml6.dll: SAXXMLReader (ISAXXMLReader) on libxml2's SAX2 callbacks.
 * The content handler gets UTF-16 strings with their lengths; a handler
 * that fails a call stops the parse, and parse() returns its HRESULT.
 */
#include "msxml_private.h"
#include <libxml/parserInternals.h>
#include <libxml/SAX2.h>

typedef struct Attrs {
    const ISAXAttributesVtbl *lpVtbl;
    int n;
    WCHAR **uri, **local, **qname, **value;
    int *nuri, *nlocal, *nqname, *nvalue;
} Attrs;

typedef struct Reader Reader;
typedef struct Locator { const ISAXLocatorVtbl *lpVtbl; Reader *r; } Locator;

struct Reader {
    const ISAXXMLReaderVtbl *lpVtbl;
    LONG refs;
    int version;
    ISAXContentHandler *content;
    ISAXErrorHandler *errors;
    IUnknown *entity, *dtd, *lexical, *decl;
    WCHAR *base, *secure_base;
    BOOL ns_prefixes;             /* "namespace-prefixes": declarations are attributes too */
    BOOL prohibit_dtd;
    xmlParserCtxtPtr ctx;         /* while parsing */
    HRESULT stop;                 /* the handler's failure that stopped the parse */
    Locator loc;
    int *nsstack;                 /* namespaces declared per open element */
    int depth, cap;
    xmlChar **prefixes;           /* the prefixes declared, innermost last */
    int nprefixes, cprefixes;
};

static void handler_failed(Reader *r, HRESULT hr)
{
    if (SUCCEEDED(hr) || FAILED(r->stop)) return;
    r->stop = hr;
    xmlStopParser(r->ctx);
}

/* ---- ISAXLocator ---- */
static HRESULT STDMETHODCALLTYPE lc_qi(Locator *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISAXLocator)) { *out = This; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE lc_addref(Locator *This) { return (ULONG)InterlockedIncrement(&This->r->refs); }
static ULONG STDMETHODCALLTYPE lc_release(Locator *This) { return ((IUnknown *)This->r)->lpVtbl->Release((IUnknown *)This->r); }
static HRESULT STDMETHODCALLTYPE lc_col(Locator *This, int *out)
{
    if (!out) return E_POINTER;
    *out = This->r->ctx ? xmlSAX2GetColumnNumber(This->r->ctx) : 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE lc_line(Locator *This, int *out)
{
    if (!out) return E_POINTER;
    *out = This->r->ctx ? xmlSAX2GetLineNumber(This->r->ctx) : 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE lc_pub(Locator *This, const WCHAR **out) { (void)This; if (out) *out = 0; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE lc_sys(Locator *This, const WCHAR **out) { if (out) *out = This->r->base; return This->r->base ? S_OK : S_FALSE; }

static const ISAXLocatorVtbl locator_vtbl = {
    .QueryInterface = (void *)lc_qi, .AddRef = (void *)lc_addref, .Release = (void *)lc_release,
    .getColumnNumber = (void *)lc_col, .getLineNumber = (void *)lc_line, .getPublicId = (void *)lc_pub,
    .getSystemId = (void *)lc_sys,
};

/* ---- ISAXAttributes (lives on the stack for one startElement) ---- */
static HRESULT STDMETHODCALLTYPE sa_qi(Attrs *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISAXAttributes)) { *out = This; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sa_addref(Attrs *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE sa_release(Attrs *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE sa_getLength(Attrs *This, int *n) { if (!n) return E_POINTER; *n = This->n; return S_OK; }

#define CHECK_INDEX(i) if ((i) < 0 || (i) >= This->n) return E_INVALIDARG
static HRESULT STDMETHODCALLTYPE sa_getURI(Attrs *This, int i, const WCHAR **s, int *n)
{
    CHECK_INDEX(i); if (!s || !n) return E_POINTER; *s = This->uri[i]; *n = This->nuri[i]; return S_OK;
}
static HRESULT STDMETHODCALLTYPE sa_getLocalName(Attrs *This, int i, const WCHAR **s, int *n)
{
    CHECK_INDEX(i); if (!s || !n) return E_POINTER; *s = This->local[i]; *n = This->nlocal[i]; return S_OK;
}
static HRESULT STDMETHODCALLTYPE sa_getQName(Attrs *This, int i, const WCHAR **s, int *n)
{
    CHECK_INDEX(i); if (!s || !n) return E_POINTER; *s = This->qname[i]; *n = This->nqname[i]; return S_OK;
}
static HRESULT STDMETHODCALLTYPE sa_getName(Attrs *This, int i, const WCHAR **u, int *nu, const WCHAR **l, int *nl,
                                            const WCHAR **q, int *nq)
{
    CHECK_INDEX(i);
    if (!u || !nu || !l || !nl || !q || !nq) return E_POINTER;
    *u = This->uri[i]; *nu = This->nuri[i];
    *l = This->local[i]; *nl = This->nlocal[i];
    *q = This->qname[i]; *nq = This->nqname[i];
    return S_OK;
}
static BOOL eqn(const WCHAR *a, int na, const WCHAR *b, int nb) { return na == nb && !memcmp(a, b, (size_t)na * sizeof(WCHAR)); }
static HRESULT STDMETHODCALLTYPE sa_getIndexFromName(Attrs *This, const WCHAR *u, int nu, const WCHAR *l, int nl, int *out)
{
    if (!out) return E_POINTER;
    for (int i = 0; i < This->n; i++)
        if (eqn(This->uri[i], This->nuri[i], u, nu) && eqn(This->local[i], This->nlocal[i], l, nl)) { *out = i; return S_OK; }
    *out = -1;
    return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE sa_getIndexFromQName(Attrs *This, const WCHAR *q, int nq, int *out)
{
    if (!out) return E_POINTER;
    for (int i = 0; i < This->n; i++)
        if (eqn(This->qname[i], This->nqname[i], q, nq)) { *out = i; return S_OK; }
    *out = -1;
    return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE sa_getType(Attrs *This, int i, const WCHAR **s, int *n)
{
    CHECK_INDEX(i); if (!s || !n) return E_POINTER; *s = L"CDATA"; *n = 5; return S_OK;
}
static HRESULT STDMETHODCALLTYPE sa_getTypeFromName(Attrs *This, const WCHAR *u, int nu, const WCHAR *l, int nl, const WCHAR **s, int *n)
{
    int i;
    HRESULT hr = sa_getIndexFromName(This, u, nu, l, nl, &i);
    return FAILED(hr) ? hr : sa_getType(This, i, s, n);
}
static HRESULT STDMETHODCALLTYPE sa_getTypeFromQName(Attrs *This, const WCHAR *q, int nq, const WCHAR **s, int *n)
{
    int i;
    HRESULT hr = sa_getIndexFromQName(This, q, nq, &i);
    return FAILED(hr) ? hr : sa_getType(This, i, s, n);
}
static HRESULT STDMETHODCALLTYPE sa_getValue(Attrs *This, int i, const WCHAR **s, int *n)
{
    CHECK_INDEX(i); if (!s || !n) return E_POINTER; *s = This->value[i]; *n = This->nvalue[i]; return S_OK;
}
static HRESULT STDMETHODCALLTYPE sa_getValueFromName(Attrs *This, const WCHAR *u, int nu, const WCHAR *l, int nl, const WCHAR **s, int *n)
{
    int i;
    HRESULT hr = sa_getIndexFromName(This, u, nu, l, nl, &i);
    return FAILED(hr) ? hr : sa_getValue(This, i, s, n);
}
static HRESULT STDMETHODCALLTYPE sa_getValueFromQName(Attrs *This, const WCHAR *q, int nq, const WCHAR **s, int *n)
{
    int i;
    HRESULT hr = sa_getIndexFromQName(This, q, nq, &i);
    return FAILED(hr) ? hr : sa_getValue(This, i, s, n);
}

static const ISAXAttributesVtbl attrs_vtbl = {
    .QueryInterface = (void *)sa_qi, .AddRef = (void *)sa_addref, .Release = (void *)sa_release,
    .getLength = (void *)sa_getLength, .getURI = (void *)sa_getURI, .getLocalName = (void *)sa_getLocalName,
    .getQName = (void *)sa_getQName, .getName = (void *)sa_getName, .getIndexFromName = (void *)sa_getIndexFromName,
    .getIndexFromQName = (void *)sa_getIndexFromQName, .getType = (void *)sa_getType,
    .getTypeFromName = (void *)sa_getTypeFromName, .getTypeFromQName = (void *)sa_getTypeFromQName,
    .getValue = (void *)sa_getValue, .getValueFromName = (void *)sa_getValueFromName,
    .getValueFromQName = (void *)sa_getValueFromQName,
};

/* ---- libxml2 SAX2 callbacks ---- */
#define READER(ctx) ((Reader *)((xmlParserCtxtPtr)(ctx))->_private)

static void cb_start_document(void *ctx)
{
    Reader *r = READER(ctx);
    xmlSAX2StartDocument(ctx);                          /* (sets up the context's document state) */
    if (r->content) {
        handler_failed(r, r->content->lpVtbl->putDocumentLocator(r->content, (ISAXLocator *)&r->loc));
        handler_failed(r, r->content->lpVtbl->startDocument(r->content));
    }
}

static void cb_end_document(void *ctx)
{
    Reader *r = READER(ctx);
    if (r->content && SUCCEEDED(r->stop)) handler_failed(r, r->content->lpVtbl->endDocument(r->content));
}

static WCHAR *wide_qname(const xmlChar *prefix, const xmlChar *local, int *n)
{
    if (!prefix) return wide_utf8(local, -1, n);
    xmlChar *q = xmlBuildQName(local, prefix, 0, 0);
    WCHAR *w = wide_utf8(q, -1, n);
    xmlFree(q);
    return w;
}

static void cb_start_element(void *ctx, const xmlChar *local, const xmlChar *prefix, const xmlChar *uri,
                             int nns, const xmlChar **ns, int nattrs, int ndefaulted, const xmlChar **attrs)
{
    (void)ndefaulted;
    Reader *r = READER(ctx);
    if (FAILED(r->stop) || !r->content) return;
    /* declarations: startPrefixMapping each, remembered for endPrefixMapping */
    if (r->depth == r->cap) {
        int c = r->cap ? r->cap * 2 : 32;
        int *s = mem_realloc(r->nsstack, (SIZE_T)c * sizeof *s);
        if (!s) { handler_failed(r, E_OUTOFMEMORY); return; }
        r->nsstack = s;
        r->cap = c;
    }
    r->nsstack[r->depth++] = nns;
    for (int i = 0; i < nns; i++) {
        if (r->nprefixes == r->cprefixes) {
            int c = r->cprefixes ? r->cprefixes * 2 : 16;
            xmlChar **p = mem_realloc(r->prefixes, (SIZE_T)c * sizeof *p);
            if (!p) { handler_failed(r, E_OUTOFMEMORY); return; }
            r->prefixes = p;
            r->cprefixes = c;
        }
        r->prefixes[r->nprefixes++] = xmlStrdup(ns[2 * i] ? ns[2 * i] : BAD_CAST "");
        int np, nu;
        WCHAR *p = wide_utf8(ns[2 * i], -1, &np), *u = wide_utf8(ns[2 * i + 1], -1, &nu);
        if (p && u) handler_failed(r, r->content->lpVtbl->startPrefixMapping(r->content, p, np, u, nu));
        mem_free(p);
        mem_free(u);
    }
    /* attributes (and, with namespace-prefixes, the declarations first) */
    int total = nattrs + (r->ns_prefixes ? nns : 0);
    Attrs a = { &attrs_vtbl, 0 };
    SIZE_T sz = (SIZE_T)(total ? total : 1);
    a.uri = mem_alloc(sz * sizeof(WCHAR *)); a.local = mem_alloc(sz * sizeof(WCHAR *));
    a.qname = mem_alloc(sz * sizeof(WCHAR *)); a.value = mem_alloc(sz * sizeof(WCHAR *));
    a.nuri = mem_alloc(sz * sizeof(int)); a.nlocal = mem_alloc(sz * sizeof(int));
    a.nqname = mem_alloc(sz * sizeof(int)); a.nvalue = mem_alloc(sz * sizeof(int));
    if (a.uri && a.local && a.qname && a.value && a.nuri && a.nlocal && a.nqname && a.nvalue) {
        if (r->ns_prefixes)
            for (int i = 0; i < nns; i++, a.n++) {
                a.uri[a.n] = wide_utf8(BAD_CAST "", 0, &a.nuri[a.n]);
                a.local[a.n] = wide_utf8(ns[2 * i] ? ns[2 * i] : BAD_CAST "", -1, &a.nlocal[a.n]);
                a.qname[a.n] = wide_qname(ns[2 * i] ? BAD_CAST "xmlns" : 0, ns[2 * i] ? ns[2 * i] : BAD_CAST "xmlns", &a.nqname[a.n]);
                a.value[a.n] = wide_utf8(ns[2 * i + 1], -1, &a.nvalue[a.n]);
            }
        for (int i = 0; i < nattrs; i++, a.n++) {          /* localname, prefix, URI, value, end */
            const xmlChar **at = attrs + 5 * i;
            a.uri[a.n] = wide_utf8(at[2] ? at[2] : BAD_CAST "", -1, &a.nuri[a.n]);
            a.local[a.n] = wide_utf8(at[0], -1, &a.nlocal[a.n]);
            a.qname[a.n] = wide_qname(at[1], at[0], &a.nqname[a.n]);
            a.value[a.n] = wide_utf8(at[3], (int)(at[4] - at[3]), &a.nvalue[a.n]);
        }
        int nl, nq, nu;
        WCHAR *l = wide_utf8(local, -1, &nl), *q = wide_qname(prefix, local, &nq), *u = wide_utf8(uri ? uri : BAD_CAST "", -1, &nu);
        if (l && q && u) handler_failed(r, r->content->lpVtbl->startElement(r->content, u, nu, l, nl, q, nq, (ISAXAttributes *)&a));
        mem_free(l); mem_free(q); mem_free(u);
    } else
        handler_failed(r, E_OUTOFMEMORY);
    for (int i = 0; i < a.n; i++) { mem_free(a.uri[i]); mem_free(a.local[i]); mem_free(a.qname[i]); mem_free(a.value[i]); }
    mem_free(a.uri); mem_free(a.local); mem_free(a.qname); mem_free(a.value);
    mem_free(a.nuri); mem_free(a.nlocal); mem_free(a.nqname); mem_free(a.nvalue);
}

static void cb_end_element(void *ctx, const xmlChar *local, const xmlChar *prefix, const xmlChar *uri)
{
    Reader *r = READER(ctx);
    if (FAILED(r->stop) || !r->content) return;
    int nl, nq, nu;
    WCHAR *l = wide_utf8(local, -1, &nl), *q = wide_qname(prefix, local, &nq), *u = wide_utf8(uri ? uri : BAD_CAST "", -1, &nu);
    if (l && q && u) handler_failed(r, r->content->lpVtbl->endElement(r->content, u, nu, l, nl, q, nq));
    mem_free(l); mem_free(q); mem_free(u);
    int nns = r->depth ? r->nsstack[--r->depth] : 0;
    for (int i = 0; i < nns && r->nprefixes; i++) {
        xmlChar *p = r->prefixes[--r->nprefixes];
        int np;
        WCHAR *w = wide_utf8(p, -1, &np);
        if (w && SUCCEEDED(r->stop)) handler_failed(r, r->content->lpVtbl->endPrefixMapping(r->content, w, np));
        mem_free(w);
        xmlFree(p);
    }
}

static void cb_characters(void *ctx, const xmlChar *ch, int len)
{
    Reader *r = READER(ctx);
    if (FAILED(r->stop) || !r->content) return;
    int n;
    WCHAR *w = wide_utf8(ch, len, &n);
    if (w) handler_failed(r, r->content->lpVtbl->characters(r->content, w, n));
    mem_free(w);
}

static void cb_pi(void *ctx, const xmlChar *target, const xmlChar *data)
{
    Reader *r = READER(ctx);
    if (FAILED(r->stop) || !r->content) return;
    int nt, nd;
    WCHAR *t = wide_utf8(target, -1, &nt), *d = wide_utf8(data ? data : BAD_CAST "", -1, &nd);
    if (t && d) handler_failed(r, r->content->lpVtbl->processingInstruction(r->content, t, nt, d, nd));
    mem_free(t);
    mem_free(d);
}

static void cb_internal_subset(void *ctx, const xmlChar *name, const xmlChar *ext, const xmlChar *sys)
{
    Reader *r = READER(ctx);
    if (r->prohibit_dtd) {
        handler_failed(r, XML_E_BADXML);
        if (r->errors) r->errors->lpVtbl->fatalError(r->errors, (ISAXLocator *)&r->loc, L"DTD is prohibited.\r\n", XML_E_BADXML);
        return;
    }
    xmlSAX2InternalSubset(ctx, name, ext, sys);
}

static void cb_error(void *data, const xmlError *e)
{
    Reader *r = data;
    if (e->level < XML_ERR_ERROR || FAILED(r->stop)) return;
    xmlChar *m = xmlStrncatNew(BAD_CAST (e->message ? e->message : "parse error"), BAD_CAST "", -1);
    int n = xmlStrlen(m);
    while (n && (m[n - 1] == '\n' || m[n - 1] == '\r')) m[--n] = 0;
    xmlChar *crlf = xmlStrncatNew(m, BAD_CAST "\r\n", -1);
    WCHAR *w = wide_utf8(crlf, -1, 0);
    xmlFree(m);
    xmlFree(crlf);
    if (r->errors && w) r->errors->lpVtbl->fatalError(r->errors, (ISAXLocator *)&r->loc, w, XML_E_BADXML);
    mem_free(w);
    r->stop = XML_E_BADXML;
    if (r->ctx) xmlStopParser(r->ctx);
}

static HRESULT parse_bytes(Reader *r, const char *data, int len, BOOL utf8)
{
    msxml_init();
    xmlSAXHandler sax;
    memset(&sax, 0, sizeof sax);
    xmlSAXVersion(&sax, 2);
    sax.startDocument = cb_start_document;
    sax.endDocument = cb_end_document;
    sax.startElementNs = cb_start_element;
    sax.endElementNs = cb_end_element;
    sax.characters = cb_characters;
    sax.cdataBlock = cb_characters;
    sax.ignorableWhitespace = cb_characters;
    sax.processingInstruction = cb_pi;
    sax.internalSubset = cb_internal_subset;
    sax.comment = 0;
    sax.serror = 0;
    sax.error = 0;
    sax.warning = 0;
    xmlParserCtxtPtr ctx = xmlNewSAXParserCtxt(&sax, 0);
    if (!ctx) return E_OUTOFMEMORY;
    ctx->_private = r;
    xmlCtxtSetErrorHandler(ctx, cb_error, r);
    r->ctx = ctx;
    r->stop = S_OK;
    r->depth = 0;
    int opts = XML_PARSE_NONET | XML_PARSE_NOENT | XML_PARSE_NO_XXE | XML_PARSE_NODICT;
    if (utf8) opts |= XML_PARSE_IGNORE_ENC;
    char *url = r->base ? (char *)utf8_wide(r->base, -1) : 0;
    xmlDocPtr doc = xmlCtxtReadMemory(ctx, data, len, url, utf8 ? "UTF-8" : 0, opts);
    xmlFree(url);
    if (doc) xmlFreeDoc(doc);                           /* (SAX2's own callbacks are not set: nothing is built) */
    if (!len && SUCCEEDED(r->stop)) {
        if (r->errors) r->errors->lpVtbl->fatalError(r->errors, (ISAXLocator *)&r->loc,
                                                     L"XML document must have a top level element.\r\n", XML_E_MISSINGROOT);
        r->stop = XML_E_MISSINGROOT;
    }
    HRESULT hr = r->stop;
    r->ctx = 0;
    xmlFreeParserCtxt(ctx);
    while (r->nprefixes) xmlFree(r->prefixes[--r->nprefixes]);
    return hr;
}

/* ---- ISAXXMLReader ---- */
static HRESULT STDMETHODCALLTYPE rd_qi(Reader *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISAXXMLReader)) {
        *out = This;
        InterlockedIncrement(&This->refs);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE rd_addref(Reader *This) { return (ULONG)InterlockedIncrement(&This->refs); }

static void set_unk(IUnknown **slot, IUnknown *v)
{
    if (v) v->lpVtbl->AddRef(v);
    if (*slot) (*slot)->lpVtbl->Release(*slot);
    *slot = v;
}

static ULONG STDMETHODCALLTYPE rd_release(Reader *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (!r) {
        set_unk((IUnknown **)&This->content, 0);
        set_unk((IUnknown **)&This->errors, 0);
        set_unk(&This->entity, 0);
        set_unk(&This->dtd, 0);
        set_unk(&This->lexical, 0);
        set_unk(&This->decl, 0);
        mem_free(This->base);
        mem_free(This->secure_base);
        mem_free(This->nsstack);
        mem_free(This->prefixes);
        mem_free(This);
        InterlockedDecrement(&g_objects);
    }
    return (ULONG)r;
}

static BOOL feature_is(const WCHAR *name, const WCHAR *what) { return name && !wcscmp(name, what); }

static HRESULT STDMETHODCALLTYPE rd_getFeature(Reader *This, const WCHAR *name, VARIANT_BOOL *v)
{
    if (!v) return E_POINTER;
    if (feature_is(name, L"http://xml.org/sax/features/namespaces")) *v = VARIANT_TRUE;
    else if (feature_is(name, L"http://xml.org/sax/features/namespace-prefixes")) *v = This->ns_prefixes ? VARIANT_TRUE : VARIANT_FALSE;
    else if (feature_is(name, L"prohibit-dtd")) *v = This->prohibit_dtd ? VARIANT_TRUE : VARIANT_FALSE;
    else *v = VARIANT_FALSE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE rd_putFeature(Reader *This, const WCHAR *name, VARIANT_BOOL v)
{
    if (feature_is(name, L"http://xml.org/sax/features/namespace-prefixes")) This->ns_prefixes = v != 0;
    else if (feature_is(name, L"prohibit-dtd")) This->prohibit_dtd = v != 0;
    return S_OK;                                        /* (the others: accepted, no effect) */
}

static HRESULT STDMETHODCALLTYPE rd_getProperty(Reader *This, const WCHAR *name, VARIANT *v)
{
    if (!v) return E_POINTER;
    VariantInit(v);
    IUnknown *u = feature_is(name, L"http://xml.org/sax/properties/lexical-handler") ? This->lexical :
                  feature_is(name, L"http://xml.org/sax/properties/declaration-handler") ? This->decl : 0;
    if (u) { v->vt = VT_UNKNOWN; v->punkVal = u; u->lpVtbl->AddRef(u); }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE rd_putProperty(Reader *This, const WCHAR *name, VARIANT v)
{
    IUnknown *u = (v.vt == VT_UNKNOWN || v.vt == VT_DISPATCH) ? v.punkVal : 0;
    if (feature_is(name, L"http://xml.org/sax/properties/lexical-handler")) set_unk(&This->lexical, u);
    else if (feature_is(name, L"http://xml.org/sax/properties/declaration-handler")) set_unk(&This->decl, u);
    return S_OK;
}

#define HANDLER(Name, field, T) \
    static HRESULT STDMETHODCALLTYPE rd_get##Name(Reader *This, T **out) \
    { if (!out) return E_POINTER; *out = (T *)This->field; if (*out) ((IUnknown *)*out)->lpVtbl->AddRef((IUnknown *)*out); return S_OK; } \
    static HRESULT STDMETHODCALLTYPE rd_put##Name(Reader *This, T *h) { set_unk((IUnknown **)&This->field, (IUnknown *)h); return S_OK; }
HANDLER(EntityResolver, entity, IUnknown)
HANDLER(ContentHandler, content, ISAXContentHandler)
HANDLER(DTDHandler, dtd, IUnknown)
HANDLER(ErrorHandler, errors, ISAXErrorHandler)

static HRESULT STDMETHODCALLTYPE rd_getBaseURL(Reader *This, const WCHAR **out) { if (!out) return E_POINTER; *out = This->base; return S_OK; }
static HRESULT STDMETHODCALLTYPE rd_putBaseURL(Reader *This, const WCHAR *url)
{
    mem_free(This->base);
    This->base = 0;
    if (url) {
        size_t n = wcslen(url);
        This->base = mem_alloc((n + 1) * sizeof(WCHAR));
        if (!This->base) return E_OUTOFMEMORY;
        memcpy(This->base, url, (n + 1) * sizeof(WCHAR));
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE rd_getSecureBaseURL(Reader *This, const WCHAR **out)
{
    if (!out) return E_POINTER;
    *out = This->secure_base;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE rd_putSecureBaseURL(Reader *This, const WCHAR *url)
{
    mem_free(This->secure_base);
    This->secure_base = 0;
    if (url) {
        size_t n = wcslen(url);
        This->secure_base = mem_alloc((n + 1) * sizeof(WCHAR));
        if (!This->secure_base) return E_OUTOFMEMORY;
        memcpy(This->secure_base, url, (n + 1) * sizeof(WCHAR));
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE rd_parse(Reader *This, VARIANT input)
{
    const VARIANT *v = input.vt == (VT_BYREF | VT_VARIANT) ? input.pvarVal : &input;
    switch (v->vt) {
    case VT_BSTR: {
        const WCHAR *w = v->bstrVal ? v->bstrVal : L"";
        int n = (int)SysStringLen(v->bstrVal);
        if (n && w[0] == 0xFEFF) { w++; n--; }
        xmlChar *u = utf8_wide(w, n);
        if (!u) return E_OUTOFMEMORY;
        HRESULT hr = parse_bytes(This, (const char *)u, xmlStrlen(u), TRUE);
        xmlFree(u);
        return hr;
    }
    case VT_ARRAY | VT_UI1: {
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(v->parray, 1, &lo);
        SafeArrayGetUBound(v->parray, 1, &hi);
        void *p;
        if (FAILED(SafeArrayAccessData(v->parray, &p))) return E_INVALIDARG;
        HRESULT hr = parse_bytes(This, p, (int)(hi - lo + 1), FALSE);
        SafeArrayUnaccessData(v->parray);
        return hr;
    }
    case VT_UNKNOWN:
    case VT_DISPATCH: {
        if (!v->punkVal) return E_INVALIDARG;
        Node *doc = node_from_iface(v->punkVal);
        if (doc) {                                      /* a DOM document: its text */
            xmlChar *t;
            HRESULT hr = node_xml(doc->doc, node_x(doc), FALSE, &t);
            if (FAILED(hr)) return hr;
            hr = parse_bytes(This, (const char *)t, xmlStrlen(t), TRUE);
            xmlFree(t);
            return hr;
        }
        char *data;
        DWORD len;
        HRESULT hr = read_stream(v->punkVal, &data, &len);
        if (FAILED(hr)) return hr;
        hr = parse_bytes(This, data, (int)len, FALSE);
        mem_free(data);
        return hr;
    }
    case VT_EMPTY:
    case VT_NULL:
        return parse_bytes(This, "", 0, TRUE);
    default:
        return E_INVALIDARG;
    }
}

static HRESULT STDMETHODCALLTYPE rd_parseURL(Reader *This, const WCHAR *url)
{
    if (!url) return E_INVALIDARG;
    char *data;
    DWORD len;
    HRESULT hr = read_url(url, &data, &len);
    if (FAILED(hr)) return hr;
    rd_putBaseURL(This, url);
    hr = parse_bytes(This, data, (int)len, FALSE);
    mem_free(data);
    return hr;
}

static const ISAXXMLReaderVtbl reader_vtbl = {
    .QueryInterface = (void *)rd_qi, .AddRef = (void *)rd_addref, .Release = (void *)rd_release,
    .getFeature = (void *)rd_getFeature, .putFeature = (void *)rd_putFeature,
    .getProperty = (void *)rd_getProperty, .putProperty = (void *)rd_putProperty,
    .getEntityResolver = (void *)rd_getEntityResolver, .putEntityResolver = (void *)rd_putEntityResolver,
    .getContentHandler = (void *)rd_getContentHandler, .putContentHandler = (void *)rd_putContentHandler,
    .getDTDHandler = (void *)rd_getDTDHandler, .putDTDHandler = (void *)rd_putDTDHandler,
    .getErrorHandler = (void *)rd_getErrorHandler, .putErrorHandler = (void *)rd_putErrorHandler,
    .getBaseURL = (void *)rd_getBaseURL, .putBaseURL = (void *)rd_putBaseURL,
    .getSecureBaseURL = (void *)rd_getSecureBaseURL, .putSecureBaseURL = (void *)rd_putSecureBaseURL,
    .parse = (void *)rd_parse, .parseURL = (void *)rd_parseURL,
};

HRESULT saxreader_create(int version, REFIID riid, void **out)
{
    *out = 0;
    Reader *r = mem_alloc(sizeof *r);
    if (!r) return E_OUTOFMEMORY;
    r->lpVtbl = &reader_vtbl;
    r->refs = 1;
    r->version = version;
    r->prohibit_dtd = version >= 6;
    r->loc.lpVtbl = &locator_vtbl;
    r->loc.r = r;
    InterlockedIncrement(&g_objects);
    HRESULT hr = rd_qi(r, riid, out);
    rd_release(r);
    return hr;
}
