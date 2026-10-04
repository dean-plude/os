/*
 * msxml6.dll: node lists (childNodes, live; selectNodes and
 * getElementsByTagName, a fixed set), attribute maps, parseError, the
 * DOM implementation object, and the IDispatch part every object shares.
 */
#include "msxml_private.h"

/* ---- IDispatch: no type information (programs call the vtables) ---- */
HRESULT STDMETHODCALLTYPE disp_count(void *This, UINT *n) { (void)This; if (n) *n = 0; return S_OK; }
HRESULT STDMETHODCALLTYPE disp_info(void *This, UINT i, LCID lcid, ITypeInfo **out)
{
    (void)This; (void)i; (void)lcid;
    if (out) *out = 0;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE disp_ids(void *This, REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids)
{
    (void)This; (void)riid; (void)lcid;
    for (UINT i = 0; i < n && ids; i++) ids[i] = -1;    /* DISPID_UNKNOWN */
    if (n && names && names[0]) {
        char m[160] = "msxml6: IDispatch name not supported: ";
        int k = (int)strlen(m);
        for (const WCHAR *p = names[0]; *p && k < 156; p++) m[k++] = (char)*p;
        m[k++] = '\n';
        m[k] = 0;
        OutputDebugStringA(m);
    }
    return DISP_E_UNKNOWNNAME;
}
HRESULT STDMETHODCALLTYPE disp_invoke(void *This, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *p,
                                      VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    (void)This; (void)id; (void)riid; (void)lcid; (void)flags; (void)p; (void)res; (void)ei; (void)argerr;
    return DISP_E_MEMBERNOTFOUND;
}

/* ---- IXMLDOMNodeList ---- */
typedef struct List {
    const IXMLDOMNodeListVtbl *lpVtbl;
    LONG refs;
    Doc *doc;
    xmlNodePtr parent;            /* live: parent's children */
    xmlNodePtr *items;            /* fixed */
    LONG n, pos;
} List;

static BOOL counted(xmlNodePtr c)
{
    return c->type != XML_ELEMENT_DECL && c->type != XML_ATTRIBUTE_DECL && c->type != XML_ENTITY_DECL &&
           c->type != XML_NAMESPACE_DECL;
}

static LONG list_len(List *l)
{
    if (!l->parent) return l->n;
    LONG n = 0;
    if (l->parent->type != XML_ENTITY_REF_NODE)
        for (xmlNodePtr c = l->parent->children; c; c = c->next) n += counted(c);
    return n;
}

static xmlNodePtr list_at(List *l, LONG i)
{
    if (i < 0) return 0;
    if (!l->parent) return i < l->n ? l->items[i] : 0;
    if (l->parent->type == XML_ENTITY_REF_NODE) return 0;
    for (xmlNodePtr c = l->parent->children; c; c = c->next)
        if (counted(c) && !i--) return c;
    return 0;
}

static HRESULT STDMETHODCALLTYPE ls_qi(List *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IXMLDOMNodeList)) {
        *out = This;
        InterlockedIncrement(&This->refs);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ls_addref(List *This) { return (ULONG)InterlockedIncrement(&This->refs); }
static ULONG STDMETHODCALLTYPE ls_release(List *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (!r) {
        doc_release(This->doc);
        mem_free(This->items);
        mem_free(This);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE ls_get_item(List *This, LONG i, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    return node_wrap(This->doc, list_at(This, i), &IID_IXMLDOMNode, (void **)out);
}

static HRESULT STDMETHODCALLTYPE ls_get_length(List *This, LONG *out)
{
    if (!out) return E_INVALIDARG;
    *out = list_len(This);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ls_nextNode(List *This, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = list_at(This, This->pos);
    if (x) This->pos++;
    return node_wrap(This->doc, x, &IID_IXMLDOMNode, (void **)out);
}

static HRESULT STDMETHODCALLTYPE ls_reset(List *This) { This->pos = 0; return S_OK; }

static HRESULT STDMETHODCALLTYPE ls_newEnum(List *This, IUnknown **out)
{
    (void)This;
    if (out) *out = 0;
    return E_NOTIMPL;
}

static const IXMLDOMNodeListVtbl list_vtbl = {
    .QueryInterface = (void *)ls_qi, .AddRef = (void *)ls_addref, .Release = (void *)ls_release, DISPATCH_SLOTS,
    .get_item = (void *)ls_get_item, .get_length = (void *)ls_get_length, .nextNode = (void *)ls_nextNode,
    .reset = (void *)ls_reset, .get__newEnum = (void *)ls_newEnum,
};

static HRESULT list_new(Doc *d, xmlNodePtr parent, xmlNodePtr *items, int n, IXMLDOMNodeList **out)
{
    *out = 0;
    List *l = mem_alloc(sizeof *l);
    if (!l) { mem_free(items); return E_OUTOFMEMORY; }
    l->lpVtbl = &list_vtbl;
    l->refs = 1;
    l->doc = d;
    l->parent = parent;
    l->items = items;
    l->n = n;
    doc_addref(d);
    *out = (IXMLDOMNodeList *)l;
    return S_OK;
}

HRESULT list_children(Doc *d, xmlNodePtr parent, IXMLDOMNodeList **out) { return list_new(d, parent, 0, 0, out); }
HRESULT list_static(Doc *d, xmlNodePtr *items, int n, IXMLDOMNodeList **out) { return list_new(d, 0, items, n, out); }

/* ---- IXMLDOMNamedNodeMap: an element's attributes (declarations first) ---- */
typedef struct Map {
    const IXMLDOMNamedNodeMapVtbl *lpVtbl;
    LONG refs;
    Doc *doc;                     /* NULL: an empty map (a document type's entities) */
    xmlNodePtr el;
    LONG pos;
} Map;

static xmlNodePtr map_at(Map *m, LONG i)
{
    if (!m->doc || i < 0) return 0;
    for (xmlNsPtr ns = m->el->nsDef; ns; ns = ns->next)
        if (!i--) return (xmlNodePtr)ns_attr(m->doc, m->el, ns);
    for (xmlAttrPtr a = m->el->properties; a; a = a->next)
        if (!i--) return (xmlNodePtr)a;
    return 0;
}

static LONG map_len(Map *m)
{
    if (!m->doc) return 0;
    LONG n = 0;
    for (xmlNsPtr ns = m->el->nsDef; ns; ns = ns->next) n++;
    for (xmlAttrPtr a = m->el->properties; a; a = a->next) n++;
    return n;
}

static HRESULT STDMETHODCALLTYPE mp_qi(Map *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IXMLDOMNamedNodeMap)) {
        *out = This;
        InterlockedIncrement(&This->refs);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE mp_addref(Map *This) { return (ULONG)InterlockedIncrement(&This->refs); }
static ULONG STDMETHODCALLTYPE mp_release(Map *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (!r) {
        if (This->doc) doc_release(This->doc);
        mem_free(This);
    }
    return (ULONG)r;
}

/* the element's own methods do the work: the map is a view of it */
static IXMLDOMElement *map_element(Map *m)
{
    IXMLDOMElement *el = 0;
    node_wrap(m->doc, m->el, &IID_IXMLDOMElement, (void **)&el);
    return el;
}

static HRESULT STDMETHODCALLTYPE mp_getNamedItem(Map *This, BSTR name, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (!This->doc) return S_FALSE;
    IXMLDOMElement *el = map_element(This);
    if (!el) return E_OUTOFMEMORY;
    IXMLDOMAttribute *a = 0;
    HRESULT hr = el->lpVtbl->getAttributeNode(el, name, &a);
    el->lpVtbl->Release(el);
    *out = (IXMLDOMNode *)a;
    return hr;
}

static HRESULT STDMETHODCALLTYPE mp_setNamedItem(Map *This, IXMLDOMNode *node, IXMLDOMNode **out)
{
    if (out) *out = 0;
    if (!This->doc) return E_FAIL;
    IXMLDOMAttribute *a;
    if (!node || FAILED(node->lpVtbl->QueryInterface(node, &IID_IXMLDOMAttribute, (void **)&a))) return E_INVALIDARG;
    IXMLDOMElement *el = map_element(This);
    HRESULT hr = el ? el->lpVtbl->setAttributeNode(el, a, 0) : E_OUTOFMEMORY;
    if (el) el->lpVtbl->Release(el);
    if (SUCCEEDED(hr) && out) { *out = node; node->lpVtbl->AddRef(node); }
    a->lpVtbl->Release(a);
    return hr;
}

static HRESULT STDMETHODCALLTYPE mp_removeNamedItem(Map *This, BSTR name, IXMLDOMNode **out)
{
    if (out) *out = 0;
    if (!This->doc) return S_FALSE;
    IXMLDOMNode *n = 0;
    HRESULT hr = mp_getNamedItem(This, name, &n);
    if (hr != S_OK) return hr;
    IXMLDOMElement *el = map_element(This);
    hr = el ? el->lpVtbl->removeAttributeNode(el, (IXMLDOMAttribute *)n, 0) : E_OUTOFMEMORY;
    if (el) el->lpVtbl->Release(el);
    if (SUCCEEDED(hr) && out) *out = n;
    else n->lpVtbl->Release(n);
    return hr;
}

static HRESULT STDMETHODCALLTYPE mp_get_item(Map *This, LONG i, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    return This->doc ? node_wrap(This->doc, map_at(This, i), &IID_IXMLDOMNode, (void **)out) : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE mp_get_length(Map *This, LONG *out)
{
    if (!out) return E_INVALIDARG;
    *out = map_len(This);
    return S_OK;
}

static xmlAttrPtr qualified(Map *m, BSTR name, BSTR uri)
{
    xmlChar *n = utf8_wide(name, -1), *u = uri && *uri ? utf8_wide(uri, -1) : 0;
    xmlAttrPtr a = n ? xmlHasNsProp(m->el, n, u) : 0;
    if (a && !u && a->ns) a = 0;                        /* (xmlHasNsProp(NULL) also finds a namespaced one) */
    xmlFree(n);
    xmlFree(u);
    return a;
}

static HRESULT STDMETHODCALLTYPE mp_getQualifiedItem(Map *This, BSTR name, BSTR uri, IXMLDOMNode **out)
{
    if (!out || !name) return E_INVALIDARG;
    *out = 0;
    if (!This->doc) return S_FALSE;
    return node_wrap(This->doc, (xmlNodePtr)qualified(This, name, uri), &IID_IXMLDOMNode, (void **)out);
}

static HRESULT STDMETHODCALLTYPE mp_removeQualifiedItem(Map *This, BSTR name, BSTR uri, IXMLDOMNode **out)
{
    if (out) *out = 0;
    if (!name) return E_INVALIDARG;
    if (!This->doc) return S_FALSE;
    xmlAttrPtr a = qualified(This, name, uri);
    if (!a) return S_FALSE;
    xmlUnlinkNode((xmlNodePtr)a);
    doc_orphan(This->doc, (xmlNodePtr)a);
    return out ? node_wrap(This->doc, (xmlNodePtr)a, &IID_IXMLDOMNode, (void **)out) : S_OK;
}

static HRESULT STDMETHODCALLTYPE mp_nextNode(Map *This, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    xmlNodePtr x = map_at(This, This->pos);
    if (!x) return S_FALSE;
    This->pos++;
    return node_wrap(This->doc, x, &IID_IXMLDOMNode, (void **)out);
}

static HRESULT STDMETHODCALLTYPE mp_reset(Map *This) { This->pos = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE mp_newEnum(Map *This, IUnknown **out) { (void)This; if (out) *out = 0; return E_NOTIMPL; }

static const IXMLDOMNamedNodeMapVtbl map_vtbl = {
    .QueryInterface = (void *)mp_qi, .AddRef = (void *)mp_addref, .Release = (void *)mp_release, DISPATCH_SLOTS,
    .getNamedItem = (void *)mp_getNamedItem, .setNamedItem = (void *)mp_setNamedItem,
    .removeNamedItem = (void *)mp_removeNamedItem, .get_item = (void *)mp_get_item, .get_length = (void *)mp_get_length,
    .getQualifiedItem = (void *)mp_getQualifiedItem, .removeQualifiedItem = (void *)mp_removeQualifiedItem,
    .nextNode = (void *)mp_nextNode, .reset = (void *)mp_reset, .get__newEnum = (void *)mp_newEnum,
};

HRESULT attrmap_create(Doc *d, xmlNodePtr el, IXMLDOMNamedNodeMap **out)
{
    *out = 0;
    Map *m = mem_alloc(sizeof *m);
    if (!m) return E_OUTOFMEMORY;
    m->lpVtbl = &map_vtbl;
    m->refs = 1;
    m->doc = d;
    m->el = el;
    if (d) doc_addref(d);
    *out = (IXMLDOMNamedNodeMap *)m;
    return S_OK;
}

HRESULT empty_map(IXMLDOMNamedNodeMap **out) { return attrmap_create(0, 0, out); }

/* ---- IXMLDOMParseError: a copy of the document's last error ---- */
typedef struct PErr {
    const IXMLDOMParseErrorVtbl *lpVtbl;
    LONG refs;
    ParseErr e;
} PErr;

static HRESULT STDMETHODCALLTYPE pe_qi(PErr *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IXMLDOMParseError)) {
        *out = This;
        InterlockedIncrement(&This->refs);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE pe_addref(PErr *This) { return (ULONG)InterlockedIncrement(&This->refs); }
static ULONG STDMETHODCALLTYPE pe_release(PErr *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (!r) {
        SysFreeString(This->e.reason);
        SysFreeString(This->e.src);
        SysFreeString(This->e.url);
        mem_free(This);
    }
    return (ULONG)r;
}

static HRESULT copy_bstr(BSTR s, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    if (!s) { *out = 0; return S_FALSE; }
    *out = SysAllocStringLen(s, SysStringLen(s));
    return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE pe_get_errorCode(PErr *This, LONG *out)
{
    if (!out) return E_INVALIDARG;
    *out = This->e.code;
    return This->e.code ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE pe_get_url(PErr *This, BSTR *out) { return copy_bstr(This->e.url, out); }
static HRESULT STDMETHODCALLTYPE pe_get_reason(PErr *This, BSTR *out) { return copy_bstr(This->e.reason, out); }
static HRESULT STDMETHODCALLTYPE pe_get_srcText(PErr *This, BSTR *out) { return copy_bstr(This->e.src, out); }
static HRESULT STDMETHODCALLTYPE pe_get_line(PErr *This, LONG *out) { if (!out) return E_INVALIDARG; *out = This->e.line; return S_OK; }
static HRESULT STDMETHODCALLTYPE pe_get_linepos(PErr *This, LONG *out) { if (!out) return E_INVALIDARG; *out = This->e.linepos; return S_OK; }
static HRESULT STDMETHODCALLTYPE pe_get_filepos(PErr *This, LONG *out) { if (!out) return E_INVALIDARG; *out = This->e.filepos; return S_OK; }

static const IXMLDOMParseErrorVtbl perr_vtbl = {
    .QueryInterface = (void *)pe_qi, .AddRef = (void *)pe_addref, .Release = (void *)pe_release, DISPATCH_SLOTS,
    .get_errorCode = (void *)pe_get_errorCode, .get_url = (void *)pe_get_url, .get_reason = (void *)pe_get_reason,
    .get_srcText = (void *)pe_get_srcText, .get_line = (void *)pe_get_line, .get_linepos = (void *)pe_get_linepos,
    .get_filepos = (void *)pe_get_filepos,
};

HRESULT parseerr_create(const ParseErr *e, IXMLDOMParseError **out)
{
    *out = 0;
    PErr *p = mem_alloc(sizeof *p);
    if (!p) return E_OUTOFMEMORY;
    p->lpVtbl = &perr_vtbl;
    p->refs = 1;
    p->e = *e;
    p->e.reason = e->reason ? SysAllocStringLen(e->reason, SysStringLen(e->reason)) : 0;
    p->e.src = e->src ? SysAllocStringLen(e->src, SysStringLen(e->src)) : 0;
    p->e.url = e->url ? SysAllocStringLen(e->url, SysStringLen(e->url)) : 0;
    *out = (IXMLDOMParseError *)p;
    return S_OK;
}

/* ---- IXMLDOMImplementation ---- */
typedef struct Impl { const IXMLDOMImplementationVtbl *lpVtbl; LONG refs; } Impl;

static HRESULT STDMETHODCALLTYPE im_qi(Impl *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IXMLDOMImplementation)) {
        *out = This;
        InterlockedIncrement(&This->refs);
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE im_addref(Impl *This) { return (ULONG)InterlockedIncrement(&This->refs); }
static ULONG STDMETHODCALLTYPE im_release(Impl *This)
{
    LONG r = InterlockedDecrement(&This->refs);
    if (!r) mem_free(This);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE im_hasFeature(Impl *This, BSTR feature, BSTR version, VARIANT_BOOL *out)
{
    (void)This;
    if (!out || !feature) return E_INVALIDARG;
    BOOL f = !_wcsicmp(feature, L"XML") || !_wcsicmp(feature, L"DOM") || !_wcsicmp(feature, L"MS-DOM");
    BOOL v = !version || !*version || !wcscmp(version, L"1.0");
    *out = f && v ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static const IXMLDOMImplementationVtbl impl_vtbl = {
    .QueryInterface = (void *)im_qi, .AddRef = (void *)im_addref, .Release = (void *)im_release, DISPATCH_SLOTS,
    .hasFeature = (void *)im_hasFeature,
};

HRESULT impl_create(IXMLDOMImplementation **out)
{
    Impl *i = mem_alloc(sizeof *i);
    *out = 0;
    if (!i) return E_OUTOFMEMORY;
    i->lpVtbl = &impl_vtbl;
    i->refs = 1;
    *out = (IXMLDOMImplementation *)i;
    return S_OK;
}
