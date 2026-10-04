/*
 * msxml6.dll: the DOM.  A DOMDocument owns a libxml2 tree; every node a
 * caller sees is a small COM wrapper (Node) over one libxml2 node that
 * holds a reference on the document, so a tree stays alive while any of
 * its nodes do.  Nodes a caller creates or removes are kept on the
 * document's orphan list and freed with it if they never got a parent.
 *
 * Differences from libxml2's own model that MSXML programs see:
 * - the XML declaration is a processing-instruction child named "xml";
 * - namespace declarations are attributes ("xmlns", "xmlns:p");
 * - whitespace-only text is dropped unless preserveWhiteSpace is set;
 * - `xml` returns UTF-16 text without the declaration's encoding.
 */
#include "msxml_private.h"

static const IXMLDOMElementVtbl element_vtbl;
static const IXMLDOMAttributeVtbl attr_vtbl;
static const IXMLDOMTextVtbl text_vtbl;
static const IXMLDOMCommentVtbl comment_vtbl;
static const IXMLDOMProcessingInstructionVtbl pi_vtbl;
static const IXMLDOMDocumentTypeVtbl doctype_vtbl;
static const IXMLDOMNodeVtbl other_vtbl;
static const IXMLDOMDocument3Vtbl doc_vtbl;

/* ---- the document's bookkeeping ---- */
void doc_addref(Doc *d) { InterlockedIncrement(&d->refs); }

static void err_clear(ParseErr *e)
{
    SysFreeString(e->reason);
    SysFreeString(e->src);
    SysFreeString(e->url);
    memset(e, 0, sizeof *e);
}

void doc_orphan(Doc *d, xmlNodePtr x)
{
    for (int i = 0; i < d->norphans; i++)
        if (d->orphans[i] == x) return;
    if (d->norphans == d->corphans) {
        int c = d->corphans ? d->corphans * 2 : 16;
        xmlNodePtr *o = mem_realloc(d->orphans, (SIZE_T)c * sizeof *o);
        if (!o) return;                       /* (leaks the node rather than failing the call) */
        d->orphans = o;
        d->corphans = c;
    }
    d->orphans[d->norphans++] = x;
}

static void doc_destroy(Doc *d)
{
    /* parentless orphans first (an orphan inside another orphan goes with it), then the trees */
    for (int i = 0; i < d->norphans; i++) {
        xmlNodePtr x = d->orphans[i];
        if (!x || x->parent) continue;
        for (int j = i + 1; j < d->norphans; j++)
            if (d->orphans[j] == x) d->orphans[j] = 0;
        if (x->type == XML_ATTRIBUTE_NODE) xmlFreeProp((xmlAttrPtr)x);
        else xmlFreeNode(x);
    }
    for (NsAttr *a = d->nsattrs, *n; a; a = n) { n = a->next; mem_free(a); }
    mem_free(d->orphans);
    for (int i = 0; i < d->nold; i++) xmlFreeDoc(d->old[i]);
    mem_free(d->old);
    if (d->x) xmlFreeDoc(d->x);
    xmlFree(d->sel_ns);
    SysFreeString(d->url);
    err_clear(&d->err);
    if (d->onready) d->onready->lpVtbl->Release(d->onready);
    mem_free(d);
    InterlockedDecrement(&g_objects);
}

void doc_release(Doc *d)
{
    if (!InterlockedDecrement(&d->refs)) doc_destroy(d);
}

xmlNodePtr node_x(Node *n) { return n->x ? n->x : (xmlNodePtr)n->doc->x; }

static BOOL is_doc_node(Node *n) { return n == &n->doc->self; }

Node *node_from_iface(void *iface)
{
    if (!iface) return 0;
    const void *v = ((Node *)iface)->lpVtbl;
    if (v == &element_vtbl || v == &attr_vtbl || v == &text_vtbl || v == &comment_vtbl || v == &pi_vtbl ||
        v == &doctype_vtbl || v == &other_vtbl || v == &doc_vtbl)
        return iface;
    return 0;
}

static const void *vtbl_for(xmlNodePtr x)
{
    switch (x->type) {
    case XML_ELEMENT_NODE: return &element_vtbl;
    case XML_ATTRIBUTE_NODE: return &attr_vtbl;
    case XML_TEXT_NODE: case XML_CDATA_SECTION_NODE: return &text_vtbl;
    case XML_COMMENT_NODE: return &comment_vtbl;
    case XML_PI_NODE: return &pi_vtbl;
    case XML_DTD_NODE: case XML_DOCUMENT_TYPE_NODE: return &doctype_vtbl;
    default: return &other_vtbl;
    }
}

HRESULT node_wrap(Doc *d, xmlNodePtr x, REFIID riid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (!x) return S_FALSE;
    if (x->type == XML_DOCUMENT_NODE) {
        if ((xmlDocPtr)x != d->x) return S_FALSE;        /* (a replaced tree's document) */
        return d->self.lpVtbl ? ((IUnknown *)&d->self)->lpVtbl->QueryInterface((IUnknown *)&d->self, riid, out) : E_FAIL;
    }
    Node *n = mem_alloc(sizeof *n);
    if (!n) return E_OUTOFMEMORY;
    n->lpVtbl = vtbl_for(x);
    n->refs = 1;
    n->doc = d;
    n->x = x;
    doc_addref(d);
    HRESULT hr = ((IUnknown *)n)->lpVtbl->QueryInterface((IUnknown *)n, riid, out);
    ((IUnknown *)n)->lpVtbl->Release((IUnknown *)n);
    return hr;
}

static HRESULT wrap_node(Doc *d, xmlNodePtr x, IXMLDOMNode **out) { return node_wrap(d, x, &IID_IXMLDOMNode, (void **)out); }

/* ---- IUnknown ---- */
static HRESULT STDMETHODCALLTYPE nd_qi(Node *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    BOOL ok = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IXMLDOMNode);
    if (!ok) {
        if (is_doc_node(This))
            ok = IsEqualIID(riid, &IID_IXMLDOMDocument) || IsEqualIID(riid, &IID_IXMLDOMDocument2) ||
                 IsEqualIID(riid, &IID_IXMLDOMDocument3);
        else switch (This->x->type) {
        case XML_ELEMENT_NODE: ok = IsEqualIID(riid, &IID_IXMLDOMElement); break;
        case XML_ATTRIBUTE_NODE: ok = IsEqualIID(riid, &IID_IXMLDOMAttribute); break;
        case XML_CDATA_SECTION_NODE: ok = IsEqualIID(riid, &IID_IXMLDOMCDATASection); /* fall through */
        case XML_TEXT_NODE: ok = ok || IsEqualIID(riid, &IID_IXMLDOMText) || IsEqualIID(riid, &IID_IXMLDOMCharacterData); break;
        case XML_COMMENT_NODE: ok = IsEqualIID(riid, &IID_IXMLDOMComment) || IsEqualIID(riid, &IID_IXMLDOMCharacterData); break;
        case XML_PI_NODE: ok = IsEqualIID(riid, &IID_IXMLDOMProcessingInstruction); break;
        case XML_DOCUMENT_FRAG_NODE: ok = IsEqualIID(riid, &IID_IXMLDOMDocumentFragment); break;
        case XML_ENTITY_REF_NODE: ok = IsEqualIID(riid, &IID_IXMLDOMEntityReference); break;
        case XML_DTD_NODE: case XML_DOCUMENT_TYPE_NODE: ok = IsEqualIID(riid, &IID_IXMLDOMDocumentType); break;
        default: break;
        }
    }
    if (!ok) return E_NOINTERFACE;
    *out = This;
    ((IUnknown *)This)->lpVtbl->AddRef((IUnknown *)This);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE nd_addref(Node *This)
{
    if (is_doc_node(This)) return (ULONG)InterlockedIncrement(&This->doc->refs);
    return (ULONG)InterlockedIncrement(&This->refs);
}

static ULONG STDMETHODCALLTYPE nd_release(Node *This)
{
    if (is_doc_node(This)) {
        LONG r = InterlockedDecrement(&This->doc->refs);
        if (!r) doc_destroy(This->doc);
        return (ULONG)r;
    }
    LONG r = InterlockedDecrement(&This->refs);
    if (!r) {
        Doc *d = This->doc;
        mem_free(This);
        doc_release(d);
    }
    return (ULONG)r;
}

/* ---- names ---- */
static BOOL is_nsdecl_attr(xmlNodePtr x)               /* our stand-in for an xmlns declaration */
{
    return x->type == XML_ATTRIBUTE_NODE && !x->ns && x->name &&
           (!strcmp((const char *)x->name, "xmlns") || !strncmp((const char *)x->name, "xmlns:", 6));
}

static xmlChar *qname(xmlNodePtr x)                     /* xmlFree it */
{
    if (x->ns && x->ns->prefix && (x->type == XML_ELEMENT_NODE || x->type == XML_ATTRIBUTE_NODE))
        return xmlBuildQName(x->name, x->ns->prefix, 0, 0);
    return xmlStrdup(x->name ? x->name : (const xmlChar *)"");
}

static const char *const type_names[] = { "", "element", "attribute", "text", "cdatasection", "entityreference", "entity",
                                          "processinginstruction", "comment", "document", "documenttype",
                                          "documentfragment", "notation" };

static DOMNodeType dom_type(xmlNodePtr x)
{
    switch (x->type) {
    case XML_ELEMENT_NODE: return NODE_ELEMENT;
    case XML_ATTRIBUTE_NODE: return NODE_ATTRIBUTE;
    case XML_TEXT_NODE: return NODE_TEXT;
    case XML_CDATA_SECTION_NODE: return NODE_CDATA_SECTION;
    case XML_ENTITY_REF_NODE: return NODE_ENTITY_REFERENCE;
    case XML_ENTITY_NODE: case XML_ENTITY_DECL: return NODE_ENTITY;
    case XML_PI_NODE: return NODE_PROCESSING_INSTRUCTION;
    case XML_COMMENT_NODE: return NODE_COMMENT;
    case XML_DOCUMENT_NODE: case XML_HTML_DOCUMENT_NODE: return NODE_DOCUMENT;
    case XML_DOCUMENT_TYPE_NODE: case XML_DTD_NODE: return NODE_DOCUMENT_TYPE;
    case XML_DOCUMENT_FRAG_NODE: return NODE_DOCUMENT_FRAGMENT;
    case XML_NOTATION_NODE: return NODE_NOTATION;
    default: return NODE_INVALID;
    }
}

static HRESULT STDMETHODCALLTYPE nd_get_nodeName(Node *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    switch (dom_type(x)) {
    case NODE_TEXT: return ret_bstr(out, BAD_CAST "#text");
    case NODE_CDATA_SECTION: return ret_bstr(out, BAD_CAST "#cdata-section");
    case NODE_COMMENT: return ret_bstr(out, BAD_CAST "#comment");
    case NODE_DOCUMENT: return ret_bstr(out, BAD_CAST "#document");
    case NODE_DOCUMENT_FRAGMENT: return ret_bstr(out, BAD_CAST "#document-fragment");
    default: {
        xmlChar *q = qname(x);
        HRESULT hr = ret_bstr(out, q);
        xmlFree(q);
        return hr;
    }
    }
}

static BOOL has_value(xmlNodePtr x)
{
    DOMNodeType t = dom_type(x);
    return t == NODE_ATTRIBUTE || t == NODE_TEXT || t == NODE_CDATA_SECTION || t == NODE_COMMENT ||
           t == NODE_PROCESSING_INSTRUCTION;
}

static HRESULT STDMETHODCALLTYPE nd_get_nodeValue(Node *This, VARIANT *v)
{
    if (!v) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    VariantInit(v);
    if (!has_value(x)) { v->vt = VT_NULL; return S_FALSE; }
    xmlChar *c = xmlNodeGetContent(x);
    v->vt = VT_BSTR;
    v->bstrVal = bstr_utf8(c);
    xmlFree(c);
    return v->bstrVal ? S_OK : E_OUTOFMEMORY;
}

static HRESULT set_content(xmlNodePtr x, BSTR s)
{
    xmlChar *u = utf8_wide(s, (int)SysStringLen(s));
    if (!u) return E_OUTOFMEMORY;
    if (x->type == XML_ATTRIBUTE_NODE || x->type == XML_ELEMENT_NODE || x->type == XML_DOCUMENT_FRAG_NODE) {
        xmlChar *esc = xmlEncodeSpecialChars(x->doc, u);   /* xmlNodeSetContent parses entity references */
        xmlNodeSetContent(x, esc);
        xmlFree(esc);
    } else
        xmlNodeSetContent(x, u);
    xmlFree(u);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nd_put_nodeValue(Node *This, VARIANT v)
{
    xmlNodePtr x = node_x(This);
    if (!has_value(x)) return E_FAIL;
    BSTR s;
    HRESULT hr = variant_string(&v, &s);
    if (FAILED(hr)) return hr;
    hr = set_content(x, s);
    SysFreeString(s);
    return hr;
}

static HRESULT STDMETHODCALLTYPE nd_get_nodeType(Node *This, DOMNodeType *t)
{
    if (!t) return E_INVALIDARG;
    *t = dom_type(node_x(This));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nd_get_parentNode(Node *This, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    xmlNodePtr x = node_x(This);
    if (x->type == XML_ATTRIBUTE_NODE || x->type == XML_DOCUMENT_NODE) return S_FALSE;
    return wrap_node(This->doc, x->parent, out);
}

static HRESULT STDMETHODCALLTYPE nd_get_childNodes(Node *This, IXMLDOMNodeList **out)
{
    if (!out) return E_INVALIDARG;
    return list_children(This->doc, node_x(This), out);
}

static BOOL listed_child(xmlNodePtr c)                  /* (libxml2 keeps DTD declarations as children) */
{
    return c->type != XML_ELEMENT_DECL && c->type != XML_ATTRIBUTE_DECL && c->type != XML_ENTITY_DECL &&
           c->type != XML_NAMESPACE_DECL && c->type != XML_XINCLUDE_START && c->type != XML_XINCLUDE_END;
}

static HRESULT STDMETHODCALLTYPE nd_get_firstChild(Node *This, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = node_x(This), c = x->children;
    if (x->type == XML_ENTITY_REF_NODE) c = 0;
    while (c && !listed_child(c)) c = c->next;
    return wrap_node(This->doc, c, out);
}

static HRESULT STDMETHODCALLTYPE nd_get_lastChild(Node *This, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = node_x(This), c = x->last;
    if (x->type == XML_ENTITY_REF_NODE) c = 0;
    while (c && !listed_child(c)) c = c->prev;
    return wrap_node(This->doc, c, out);
}

static HRESULT STDMETHODCALLTYPE nd_get_previousSibling(Node *This, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    if (x->type == XML_ATTRIBUTE_NODE || x->type == XML_DOCUMENT_NODE) { *out = 0; return S_FALSE; }
    xmlNodePtr c = x->prev;
    while (c && !listed_child(c)) c = c->prev;
    return wrap_node(This->doc, c, out);
}

static HRESULT STDMETHODCALLTYPE nd_get_nextSibling(Node *This, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    if (x->type == XML_ATTRIBUTE_NODE || x->type == XML_DOCUMENT_NODE) { *out = 0; return S_FALSE; }
    xmlNodePtr c = x->next;
    while (c && !listed_child(c)) c = c->next;
    return wrap_node(This->doc, c, out);
}

static HRESULT STDMETHODCALLTYPE nd_get_attributes(Node *This, IXMLDOMNamedNodeMap **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    xmlNodePtr x = node_x(This);
    if (x->type != XML_ELEMENT_NODE) return S_FALSE;
    return attrmap_create(This->doc, x, out);
}

/* ---- changing the tree ---- */

/* drop @child's namespace declarations that @parent already has in scope
 * with the same URI (createNode's element meeting its parent's declaration) */
static void retarget_ns(xmlNodePtr x, xmlNsPtr from, xmlNsPtr to)
{
    if (x->ns == from) x->ns = to;
    if (x->type == XML_ELEMENT_NODE) {
        for (xmlAttrPtr a = x->properties; a; a = a->next)
            if (a->ns == from) a->ns = to;
        for (xmlNodePtr c = x->children; c; c = c->next) retarget_ns(c, from, to);
    }
}

static void reconcile_ns(xmlNodePtr parent, xmlNodePtr child)
{
    if (child->type != XML_ELEMENT_NODE || !parent || parent->type != XML_ELEMENT_NODE) return;
    xmlNsPtr *pp = &child->nsDef;
    while (*pp) {
        xmlNsPtr ns = *pp;
        xmlNsPtr up = xmlSearchNs(parent->doc, parent, ns->prefix);
        if (up && up->href && ns->href && xmlStrEqual(up->href, ns->href)) {
            *pp = ns->next;
            retarget_ns(child, ns, up);
            ns->next = 0;
            xmlFreeNs(ns);
        } else
            pp = &ns->next;
    }
}

static BOOL is_ancestor(xmlNodePtr a, xmlNodePtr x)
{
    for (; x; x = x->parent)
        if (x == a) return TRUE;
    return FALSE;
}

static HRESULT can_contain(xmlNodePtr parent, xmlNodePtr child)
{
    switch (parent->type) {
    case XML_ELEMENT_NODE: case XML_DOCUMENT_FRAG_NODE: case XML_ENTITY_REF_NODE:
        if (child->type == XML_DOCUMENT_NODE || child->type == XML_ATTRIBUTE_NODE || child->type == XML_DTD_NODE)
            return E_FAIL;
        return S_OK;
    case XML_DOCUMENT_NODE:
        if (child->type == XML_ELEMENT_NODE) {
            for (xmlNodePtr c = parent->children; c; c = c->next)
                if (c->type == XML_ELEMENT_NODE && c != child) return E_FAIL;   /* one document element */
            return S_OK;
        }
        if (child->type == XML_PI_NODE || child->type == XML_COMMENT_NODE || child->type == XML_DOCUMENT_FRAG_NODE ||
            child->type == XML_DTD_NODE)
            return S_OK;
        if (child->type == XML_TEXT_NODE) {            /* only whitespace may sit beside the document element */
            for (const xmlChar *p = child->content; p && *p; p++)
                if (!IS_BLANK_CH(*p)) return E_FAIL;
            return S_OK;
        }
        return E_FAIL;
    case XML_ATTRIBUTE_NODE:
        return child->type == XML_TEXT_NODE || child->type == XML_ENTITY_REF_NODE ? S_OK : E_FAIL;
    default:
        return E_FAIL;
    }
}

/* put @child (unlinked) under @parent before @ref (NULL: at the end), without libxml2's text merging */
static void link_before(xmlNodePtr parent, xmlNodePtr ref, xmlNodePtr child)
{
    if (child->doc != parent->doc) xmlSetTreeDoc(child, parent->doc);
    child->parent = parent;
    if (ref) {
        child->next = ref;
        child->prev = ref->prev;
        if (ref->prev) ref->prev->next = child;
        else parent->children = child;
        ref->prev = child;
    } else {
        child->prev = parent->last;
        child->next = 0;
        if (parent->last) parent->last->next = child;
        else parent->children = child;
        parent->last = child;
    }
    reconcile_ns(parent, child);
}

static void unlink(Doc *d, xmlNodePtr x)
{
    if (!x->parent && !x->prev && !x->next) return;
    xmlUnlinkNode(x);
    doc_orphan(d, x);
}

/* insert @newc (a node of this document, or a copy of another's) under @x before @ref */
static HRESULT insert(Node *This, xmlNodePtr x, Node *nc, xmlNodePtr ref, IXMLDOMNode **out)
{
    if (out) *out = 0;
    if (!nc) return E_INVALIDARG;
    if (is_doc_node(nc)) return E_FAIL;
    xmlNodePtr c = nc->x;
    if (nc->doc != This->doc) {                          /* from another document: a copy (MSXML imports it) */
        c = c->type == XML_ATTRIBUTE_NODE ? (xmlNodePtr)xmlCopyProp(0, (xmlAttrPtr)c) : xmlDocCopyNode(c, This->doc->x, 1);
        if (!c) return E_OUTOFMEMORY;
        doc_orphan(This->doc, c);
    }
    if (ref && ref->parent != x) return E_INVALIDARG;
    if (c == ref) return out ? wrap_node(This->doc, c, out) : S_OK;
    if (is_ancestor(c, x)) return E_FAIL;
    if (FAILED(can_contain(x, c))) return E_FAIL;
    if (c->type == XML_DOCUMENT_FRAG_NODE) {             /* a fragment's children move; the fragment stays */
        for (xmlNodePtr k = c->children; k; k = c->children) {
            if (FAILED(can_contain(x, k))) return E_FAIL;
            xmlUnlinkNode(k);
            link_before(x, ref, k);
        }
    } else {
        unlink(This->doc, c);
        link_before(x, ref, c);
    }
    return out ? wrap_node(This->doc, c, out) : S_OK;
}

static HRESULT STDMETHODCALLTYPE nd_insertBefore(Node *This, IXMLDOMNode *child, VARIANT refv, IXMLDOMNode **out)
{
    xmlNodePtr ref = 0;
    const VARIANT *r = refv.vt == (VT_BYREF | VT_VARIANT) ? refv.pvarVal : &refv;
    if (r->vt == VT_DISPATCH || r->vt == VT_UNKNOWN) {
        Node *rn = node_from_iface(r->punkVal);
        if (r->punkVal && !rn) return E_INVALIDARG;
        ref = rn ? node_x(rn) : 0;
    } else if (r->vt != VT_EMPTY && r->vt != VT_NULL && r->vt != VT_ERROR)
        return E_INVALIDARG;
    return insert(This, node_x(This), node_from_iface(child), ref, out);
}

static HRESULT STDMETHODCALLTYPE nd_appendChild(Node *This, IXMLDOMNode *child, IXMLDOMNode **out)
{
    return insert(This, node_x(This), node_from_iface(child), 0, out);
}

static HRESULT STDMETHODCALLTYPE nd_removeChild(Node *This, IXMLDOMNode *child, IXMLDOMNode **out)
{
    if (out) *out = 0;
    Node *c = node_from_iface(child);
    if (!c || is_doc_node(c)) return E_INVALIDARG;
    if (c->x->parent != node_x(This) || c->x->type == XML_ATTRIBUTE_NODE) return E_INVALIDARG;
    unlink(This->doc, c->x);
    if (out) { *out = child; child->lpVtbl->AddRef(child); }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nd_replaceChild(Node *This, IXMLDOMNode *newc, IXMLDOMNode *oldc, IXMLDOMNode **out)
{
    if (out) *out = 0;
    Node *o = node_from_iface(oldc), *n = node_from_iface(newc);
    if (!o || !n || is_doc_node(o)) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    if (o->x->parent != x) return E_INVALIDARG;
    if (n == o || n->x == o->x) {
        if (out) { *out = oldc; oldc->lpVtbl->AddRef(oldc); }
        return S_OK;
    }
    xmlNodePtr ref = o->x->next;
    if (ref == n->x) ref = ref->next;
    unlink(This->doc, o->x);                             /* (first: a document's element can be replaced) */
    HRESULT hr = insert(This, x, n, ref, 0);
    if (FAILED(hr)) { link_before(x, ref, o->x); return hr; }
    if (out) { *out = oldc; oldc->lpVtbl->AddRef(oldc); }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nd_hasChildNodes(Node *This, VARIANT_BOOL *out)
{
    xmlNodePtr x = node_x(This), c = x->type == XML_ENTITY_REF_NODE ? 0 : x->children;
    while (c && !listed_child(c)) c = c->next;
    if (out) *out = c ? VARIANT_TRUE : VARIANT_FALSE;
    return c ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE nd_get_ownerDocument(Node *This, IXMLDOMDocument **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (is_doc_node(This)) return S_FALSE;
    *out = (IXMLDOMDocument *)&This->doc->self;
    nd_addref(&This->doc->self);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nd_cloneNode(Node *This, VARIANT_BOOL deep, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (is_doc_node(This)) {
        IXMLDOMDocument *nd;
        HRESULT hr = doc_create(This->doc->version, This->doc->free_threaded, &IID_IXMLDOMDocument, (void **)&nd);
        if (FAILED(hr)) return hr;
        Doc *d2 = ((Node *)nd)->doc;
        d2->preserve = This->doc->preserve;
        d2->xslpattern = This->doc->xslpattern;
        if (This->doc->sel_ns) d2->sel_ns = xmlStrdup(This->doc->sel_ns);
        if (deep) {
            xmlDocPtr copy = xmlCopyDoc(This->doc->x, 1);
            if (copy) { xmlFreeDoc(d2->x); d2->x = copy; }
        }
        *out = (IXMLDOMNode *)nd;
        return S_OK;
    }
    xmlNodePtr x = This->x, c;
    if (x->type == XML_ATTRIBUTE_NODE) c = (xmlNodePtr)xmlCopyProp(0, (xmlAttrPtr)x);
    else c = xmlDocCopyNode(x, x->doc, deep ? 1 : 2);
    if (!c) return E_OUTOFMEMORY;
    doc_orphan(This->doc, c);
    return wrap_node(This->doc, c, out);
}

static HRESULT STDMETHODCALLTYPE nd_get_nodeTypeString(Node *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    DOMNodeType t = dom_type(node_x(This));
    return ret_bstr(out, BAD_CAST (t <= NODE_NOTATION ? type_names[t] : ""));
}

/* the text of a node and its descendants (comments and processing instructions left out) */
static void gather_text(xmlNodePtr x, xmlBufferPtr b)
{
    for (xmlNodePtr c = x->children; c; c = c->next) {
        if (c->type == XML_TEXT_NODE || c->type == XML_CDATA_SECTION_NODE) xmlBufferCat(b, c->content);
        else if (c->type == XML_ELEMENT_NODE || c->type == XML_ENTITY_REF_NODE || c->type == XML_DOCUMENT_FRAG_NODE)
            gather_text(c, b);
    }
}

static HRESULT STDMETHODCALLTYPE nd_get_text(Node *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    xmlChar *s;
    if (x->type == XML_ELEMENT_NODE || x->type == XML_DOCUMENT_NODE || x->type == XML_DOCUMENT_FRAG_NODE ||
        x->type == XML_ENTITY_REF_NODE) {
        xmlBufferPtr b = xmlBufferCreate();
        if (!b) return E_OUTOFMEMORY;
        gather_text(x, b);
        s = xmlStrdup(xmlBufferContent(b));
        xmlBufferFree(b);
    } else if (x->type == XML_DTD_NODE)
        s = xmlStrdup(BAD_CAST "");
    else
        s = xmlNodeGetContent(x);
    if (!s) return E_OUTOFMEMORY;
    const xmlChar *p = s, *e = s + xmlStrlen(s);
    if (!This->doc->preserve && x->type != XML_ATTRIBUTE_NODE && x->type != XML_CDATA_SECTION_NODE) {
        while (*p && IS_BLANK_CH(*p)) p++;              /* MSXML trims the ends unless whitespace is preserved */
        while (e > p && IS_BLANK_CH(e[-1])) e--;
    }
    *out = bstr_utf8n(p, (int)(e - p));
    xmlFree(s);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE nd_put_text(Node *This, BSTR text)
{
    xmlNodePtr x = node_x(This);
    if (x->type == XML_DOCUMENT_NODE || x->type == XML_DTD_NODE || x->type == XML_ENTITY_REF_NODE) return E_FAIL;
    if (x->type == XML_ELEMENT_NODE || x->type == XML_DOCUMENT_FRAG_NODE) {
        /* the children become orphans (wrappers may hold them); one text node replaces them */
        while (x->children) unlink(This->doc, x->children);
        xmlChar *u = utf8_wide(text, (int)SysStringLen(text));
        if (!u) return E_OUTOFMEMORY;
        xmlNodePtr t = xmlNewDocText(x->doc, u);
        xmlFree(u);
        if (!t) return E_OUTOFMEMORY;
        link_before(x, 0, t);
        return S_OK;
    }
    return set_content(x, text);
}

static HRESULT STDMETHODCALLTYPE nd_get_specified(Node *This, VARIANT_BOOL *out)
{
    (void)This;
    if (!out) return E_INVALIDARG;
    *out = VARIANT_TRUE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nd_get_definition(Node *This, IXMLDOMNode **out)
{
    (void)This;
    if (!out) return E_INVALIDARG;
    *out = 0;
    return S_FALSE;
}

/* ---- typed values: the datatypes namespace's dt:dt (bin.base64, bin.hex) ---- */
static const xmlChar DT_NS[] = "urn:schemas-microsoft-com:datatypes";

static xmlChar *data_type(xmlNodePtr x)
{
    if (x->type != XML_ELEMENT_NODE) return 0;
    return xmlGetNsProp(x, BAD_CAST "dt", DT_NS);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(int c)
{
    const char *p = c ? strchr(B64, c) : 0;
    return p ? (int)(p - B64) : -1;
}

static HRESULT decode_binary(const xmlChar *s, BOOL hex, VARIANT *v)
{
    int n = xmlStrlen(s), o = 0;
    BYTE *buf = mem_alloc((SIZE_T)n + 1);
    if (!buf) return E_OUTOFMEMORY;
    if (hex) {
        int hi = -1;
        for (int i = 0; i < n; i++) {
            int c = s[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                              c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) continue;
            if (hi < 0) hi = d;
            else { buf[o++] = (BYTE)(hi * 16 + d); hi = -1; }
        }
    } else {
        unsigned acc = 0;
        int bits = 0;
        for (int i = 0; i < n; i++) {
            int d = b64val(s[i]);
            if (d < 0) continue;                         /* whitespace, padding */
            acc = acc << 6 | (unsigned)d;
            bits += 6;
            if (bits >= 8) { bits -= 8; buf[o++] = (BYTE)(acc >> bits); }
        }
    }
    SAFEARRAY *a = SafeArrayCreateVector(VT_UI1, 0, (ULONG)o);
    if (!a) { mem_free(buf); return E_OUTOFMEMORY; }
    void *data;
    SafeArrayAccessData(a, &data);
    memcpy(data, buf, (size_t)o);
    SafeArrayUnaccessData(a);
    mem_free(buf);
    v->vt = VT_ARRAY | VT_UI1;
    v->parray = a;
    return S_OK;
}

static xmlChar *encode_binary(SAFEARRAY *a, BOOL hex)
{
    LONG lo = 0, hi = -1;
    SafeArrayGetLBound(a, 1, &lo);
    SafeArrayGetUBound(a, 1, &hi);
    int n = (int)(hi - lo + 1);
    if (n < 0) n = 0;
    BYTE *p;
    if (FAILED(SafeArrayAccessData(a, (void **)&p))) return 0;
    xmlChar *out = xmlMalloc((size_t)n * 2 + 8);
    int o = 0;
    if (out && hex) {
        static const char hx[] = "0123456789abcdef";
        for (int i = 0; i < n; i++) { out[o++] = (xmlChar)hx[p[i] >> 4]; out[o++] = (xmlChar)hx[p[i] & 15]; }
    } else if (out) {
        for (int i = 0; i < n; i += 3) {
            unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
            out[o++] = (xmlChar)B64[v >> 18];
            out[o++] = (xmlChar)B64[v >> 12 & 63];
            out[o++] = (xmlChar)(i + 1 < n ? B64[v >> 6 & 63] : '=');
            out[o++] = (xmlChar)(i + 2 < n ? B64[v & 63] : '=');
        }
    }
    SafeArrayUnaccessData(a);
    if (out) out[o] = 0;
    return out;
}

static HRESULT STDMETHODCALLTYPE nd_get_nodeTypedValue(Node *This, VARIANT *v)
{
    if (!v) return E_INVALIDARG;
    VariantInit(v);
    xmlNodePtr x = node_x(This);
    xmlChar *dt = data_type(x);
    if (dt && (xmlStrEqual(dt, BAD_CAST "bin.base64") || xmlStrEqual(dt, BAD_CAST "bin.hex"))) {
        xmlChar *c = xmlNodeGetContent(x);
        HRESULT hr = decode_binary(c ? c : BAD_CAST "", dt[4] == 'h', v);
        xmlFree(c);
        xmlFree(dt);
        return hr;
    }
    xmlFree(dt);
    if (x->type == XML_ELEMENT_NODE) {
        v->vt = VT_BSTR;
        return nd_get_text(This, &v->bstrVal);
    }
    return nd_get_nodeValue(This, v);
}

static HRESULT STDMETHODCALLTYPE nd_put_nodeTypedValue(Node *This, VARIANT v)
{
    xmlNodePtr x = node_x(This);
    xmlChar *dt = data_type(x);
    if (dt && (xmlStrEqual(dt, BAD_CAST "bin.base64") || xmlStrEqual(dt, BAD_CAST "bin.hex")) &&
        v.vt == (VT_ARRAY | VT_UI1)) {
        xmlChar *s = encode_binary(v.parray, dt[4] == 'h');
        xmlFree(dt);
        if (!s) return E_OUTOFMEMORY;
        BSTR b = bstr_utf8(s);
        xmlFree(s);
        HRESULT hr = nd_put_text(This, b);
        SysFreeString(b);
        return hr;
    }
    xmlFree(dt);
    BSTR s;
    HRESULT hr = variant_string(&v, &s);
    if (FAILED(hr)) return hr;
    hr = nd_put_text(This, s);
    SysFreeString(s);
    return hr;
}

static HRESULT STDMETHODCALLTYPE nd_get_dataType(Node *This, VARIANT *v)
{
    if (!v) return E_INVALIDARG;
    VariantInit(v);
    xmlChar *dt = data_type(node_x(This));
    if (!dt) { v->vt = VT_NULL; return S_FALSE; }
    v->vt = VT_BSTR;
    v->bstrVal = bstr_utf8(dt);
    xmlFree(dt);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nd_put_dataType(Node *This, BSTR type)
{
    xmlNodePtr x = node_x(This);
    if (x->type != XML_ELEMENT_NODE) return E_FAIL;
    xmlNsPtr ns = xmlSearchNsByHref(x->doc, x, DT_NS);
    if (!ns) ns = xmlNewNs(x, DT_NS, BAD_CAST "dt");
    xmlChar *u = utf8_wide(type, (int)SysStringLen(type));
    if (!ns || !u) { xmlFree(u); return E_OUTOFMEMORY; }
    xmlSetNsProp(x, ns, BAD_CAST "dt", u);
    xmlFree(u);
    return S_OK;
}

/* ---- serialization ---- */
static void decl_text(xmlBufferPtr b, xmlNodePtr pi, BOOL keep_encoding)
{
    /* the "xml" declaration; `xml` (UTF-16 text) leaves its encoding out, save() keeps it */
    xmlBufferCat(b, BAD_CAST "<?xml ");
    const xmlChar *c = pi->content ? pi->content : BAD_CAST "";
    if (keep_encoding) xmlBufferCat(b, c);
    else {
        const xmlChar *e = xmlStrstr(c, BAD_CAST "encoding");
        if (!e) xmlBufferCat(b, c);
        else {
            const xmlChar *q = e + 8;
            while (*q && *q != '"' && *q != '\'') q++;
            xmlChar quote = *q;
            if (quote) { q++; while (*q && *q != quote) q++; if (*q) q++; }
            const xmlChar *start = e;
            while (start > c && IS_BLANK_CH(start[-1])) start--;
            xmlBufferAdd(b, c, (int)(start - c));
            xmlBufferCat(b, q);
        }
    }
    xmlBufferCat(b, BAD_CAST "?>");
}

static void dump(xmlBufferPtr b, xmlDocPtr doc, xmlNodePtr x)
{
    if (x->type == XML_ATTRIBUTE_NODE) {
        xmlChar *q = qname(x), *v = xmlNodeGetContent(x);
        xmlChar *ev = xmlEncodeSpecialChars(doc, v ? v : BAD_CAST "");
        xmlBufferCat(b, q);
        xmlBufferCat(b, BAD_CAST "=\"");
        xmlBufferCat(b, ev);
        xmlBufferCat(b, BAD_CAST "\"");
        xmlFree(q); xmlFree(v); xmlFree(ev);
        return;
    }
    if (x->type == XML_DOCUMENT_FRAG_NODE) {
        for (xmlNodePtr c = x->children; c; c = c->next) dump(b, doc, c);
        return;
    }
    xmlNodeDump(b, doc, x, 0, 0);
}

HRESULT node_xml(Doc *d, xmlNodePtr x, BOOL decl_encoding, xmlChar **out)
{
    (void)d;
    xmlBufferPtr b = xmlBufferCreate();
    if (!b) return E_OUTOFMEMORY;
    if (x->type == XML_DOCUMENT_NODE) {
        /* top-level nodes, each followed by a line break, as MSXML writes them */
        for (xmlNodePtr c = x->children; c; c = c->next) {
            if (!listed_child(c)) continue;
            if (c->type == XML_PI_NODE && xmlStrEqual(c->name, BAD_CAST "xml")) decl_text(b, c, decl_encoding);
            else if (c->type == XML_TEXT_NODE) continue;
            else dump(b, (xmlDocPtr)x, c);
            xmlBufferCat(b, BAD_CAST "\r\n");
        }
    } else if (x->type == XML_PI_NODE && xmlStrEqual(x->name, BAD_CAST "xml"))
        decl_text(b, x, decl_encoding);
    else
        dump(b, x->doc, x);
    *out = xmlStrdup(xmlBufferContent(b));
    xmlBufferFree(b);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE nd_get_xml(Node *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    xmlChar *s;
    HRESULT hr = node_xml(This->doc, node_x(This), FALSE, &s);
    if (FAILED(hr)) return hr;
    hr = ret_bstr(out, s);
    xmlFree(s);
    return hr;
}

static HRESULT STDMETHODCALLTYPE nd_transformNode(Node *This, IXMLDOMNode *style, BSTR *out)
{
    (void)This; (void)style;
    if (out) *out = 0;
    OutputDebugStringA("msxml6: transformNode: no XSLT\n");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE nd_transformNodeToObject(Node *This, IXMLDOMNode *style, VARIANT out)
{
    (void)This; (void)style; (void)out;
    OutputDebugStringA("msxml6: transformNodeToObject: no XSLT\n");
    return E_NOTIMPL;
}

/* ---- XPath ---- */

/* SelectionNamespaces: "xmlns:a='uri' xmlns:b="uri2"" */
static void register_ns(xmlXPathContextPtr ctx, const xmlChar *s)
{
    while (s && *s) {
        while (IS_BLANK_CH(*s)) s++;
        if (xmlStrncmp(s, BAD_CAST "xmlns", 5)) break;
        s += 5;
        const xmlChar *pre = 0;
        int npre = 0;
        if (*s == ':') { pre = ++s; while (*s && *s != '=' && !IS_BLANK_CH(*s)) s++; npre = (int)(s - pre); }
        while (IS_BLANK_CH(*s)) s++;
        if (*s != '=') break;
        s++;
        while (IS_BLANK_CH(*s)) s++;
        xmlChar q = *s;
        if (q != '\'' && q != '"') break;
        const xmlChar *uri = ++s;
        while (*s && *s != q) s++;
        xmlChar *p = pre ? xmlStrndup(pre, npre) : 0, *u = xmlStrndup(uri, (int)(s - uri));
        if (p && u) xmlXPathRegisterNs(ctx, p, u);  /* (a default namespace cannot be used by XPath 1.0) */
        xmlFree(p);
        xmlFree(u);
        if (*s) s++;
    }
}

static void quiet_errors(void *ctx, const xmlError *e) { (void)ctx; (void)e; }

/* XSL Patterns (MSXML 3's default SelectionLanguage) name elements by their
 * qualified name as written in the document: "UX" is any element whose
 * nodeName is UX, whatever default namespace it sits in, and "x:UX" one
 * written with the prefix x.  A prefix declared in SelectionNamespaces
 * still means its namespace.  XPath 1.0 would only match UX in no
 * namespace, so each such element name test becomes *[name()='...'].  The
 * tokens follow XPath's lexical rules (section 3.7): a name after an
 * operand is an operator (and, or, div, mod), a name before "(" is a
 * function or node type, before "::" an axis; attribute names stay as
 * they are (unprefixed attributes have no namespace either way). */
static const xmlChar *skip_blank(const xmlChar *s) { while (IS_BLANK_CH(*s)) s++; return s; }

static BOOL name_char(xmlChar c, BOOL first)
{
    if (c >= 0x80 || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return TRUE;
    return !first && (c == '-' || c == '.' || (c >= '0' && c <= '9'));
}

static xmlChar *xslpattern_to_xpath(xmlXPathContextPtr ctx, const xmlChar *q)
{
    xmlBufferPtr b = xmlBufferCreate();
    if (!b) return 0;
    BOOL operand = FALSE;            /* the previous token ends an operand */
    int axis = 0;                    /* 1 after "@" or attribute::, 2 after "$" or namespace:: */
    const xmlChar *s = q;
    while (*s) {
        const xmlChar *t = s;
        if (IS_BLANK_CH(*s)) { s++; xmlBufferAdd(b, t, 1); continue; }
        if (*s == '\'' || *s == '"') {
            xmlChar e = *s++;
            while (*s && *s != e) s++;
            if (*s) s++;
            xmlBufferAdd(b, t, (int)(s - t));
            operand = TRUE;
            axis = 0;
            continue;
        }
        if ((*s >= '0' && *s <= '9') || (*s == '.' && s[1] >= '0' && s[1] <= '9')) {
            while ((*s >= '0' && *s <= '9') || *s == '.') s++;
            xmlBufferAdd(b, t, (int)(s - t));
            operand = TRUE;
            continue;
        }
        if (name_char(*s, TRUE) || (*s == '*' && !operand)) {
            const xmlChar *colon = 0;
            if (*s == '*') s++;
            else {
                while (name_char(*s, FALSE)) s++;
                if (*s == ':' && s[1] != ':') {
                    colon = s++;
                    if (*s == '*') s++;
                    else while (name_char(*s, FALSE)) s++;
                }
            }
            int len = (int)(s - t);
            const xmlChar *next = skip_blank(s);
            if (operand && !colon) {                     /* and, or, div, mod */
                xmlBufferAdd(b, t, len);
                operand = FALSE;
                continue;
            }
            if (*next == '(' || (next[0] == ':' && next[1] == ':')) {  /* a function, node type or axis */
                if (*next == ':')
                    axis = len == 9 && !xmlStrncmp(t, BAD_CAST "attribute", 9) ? 1 :
                           len == 9 && !xmlStrncmp(t, BAD_CAST "namespace", 9) ? 2 : 0;
                xmlBufferAdd(b, t, len);
                operand = FALSE;
                continue;
            }
            /* an unprefixed attribute is in no namespace either way; a
             * prefixed one is matched as written too */
            BOOL rewrite = axis == 0 ? !(len == 1 && *t == '*') : axis == 1 && colon;
            if (rewrite && colon) {                      /* a SelectionNamespaces prefix keeps its meaning */
                xmlChar *pre = xmlStrndup(t, (int)(colon - t));
                if (pre && xmlXPathNsLookup(ctx, pre)) rewrite = FALSE;
                xmlFree(pre);
            }
            if (!rewrite) xmlBufferAdd(b, t, len);
            else if (colon && s[-1] == '*') {
                xmlBufferCCat(b, "*[starts-with(name(),'");
                xmlBufferAdd(b, t, (int)(colon - t) + 1);
                xmlBufferCCat(b, "')]");
            } else {
                xmlBufferCCat(b, "*[name()='");
                xmlBufferAdd(b, t, len);
                xmlBufferCCat(b, "']");
            }
            operand = TRUE;
            axis = 0;
            continue;
        }
        /* punctuation and operators */
        int len = 1;
        if ((s[0] == '/' && s[1] == '/') || (s[0] == ':' && s[1] == ':') || (s[0] == '.' && s[1] == '.') ||
            ((s[0] == '!' || s[0] == '<' || s[0] == '>') && s[1] == '='))
            len = 2;
        operand = *s == ')' || *s == ']' || *s == '.';   /* (a "*" here is the multiply operator) */
        if (len == 1 && (*s == '@' || *s == '$')) axis = *s == '@' ? 1 : 2;
        else if (!(len == 2 && *s == ':')) axis = 0;
        xmlBufferAdd(b, s, len);
        s += len;
    }
    xmlChar *out = xmlStrdup(xmlBufferContent(b));
    xmlBufferFree(b);
    return out;
}

static HRESULT select(Node *This, BSTR query, xmlNodePtr **items, int *count)
{
    *items = 0;
    *count = 0;
    if (!query) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    xmlChar *q = utf8_wide(query, (int)SysStringLen(query));
    xmlXPathContextPtr ctx = xmlXPathNewContext(x->doc ? x->doc : This->doc->x);
    if (!q || !ctx) { xmlFree(q); xmlXPathFreeContext(ctx); return E_OUTOFMEMORY; }
    xmlXPathSetErrorHandler(ctx, quiet_errors, 0);
    ctx->node = x;
    register_ns(ctx, This->doc->sel_ns);
    if (This->doc->xslpattern) {
        xmlChar *x = xslpattern_to_xpath(ctx, q);
        xmlFree(q);
        if (!(q = x)) { xmlXPathFreeContext(ctx); return E_OUTOFMEMORY; }
    }
    xmlXPathObjectPtr r = xmlXPathEvalExpression(q, ctx);
    xmlFree(q);
    HRESULT hr = S_OK;
    if (!r) hr = E_FAIL;                                /* a bad expression */
    else if (r->type != XPATH_NODESET) hr = E_FAIL;     /* MSXML wants a node-set */
    else if (r->nodesetval && r->nodesetval->nodeNr) {
        xmlNodeSetPtr ns = r->nodesetval;
        xmlNodePtr *out = mem_alloc((SIZE_T)ns->nodeNr * sizeof *out);
        if (!out) hr = E_OUTOFMEMORY;
        else {
            int n = 0;
            for (int i = 0; i < ns->nodeNr; i++) {
                xmlNodePtr k = ns->nodeTab[i];
                if (k->type == XML_NAMESPACE_DECL) continue;
                out[n++] = k;
            }
            *items = out;
            *count = n;
        }
    }
    xmlXPathFreeObject(r);
    xmlXPathFreeContext(ctx);
    return hr;
}

static HRESULT STDMETHODCALLTYPE nd_selectNodes(Node *This, BSTR query, IXMLDOMNodeList **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    xmlNodePtr *items;
    int n;
    HRESULT hr = select(This, query, &items, &n);
    if (FAILED(hr)) return hr;
    return list_static(This->doc, items, n, out);
}

static HRESULT STDMETHODCALLTYPE nd_selectSingleNode(Node *This, BSTR query, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    xmlNodePtr *items;
    int n;
    HRESULT hr = select(This, query, &items, &n);
    if (FAILED(hr)) return hr;
    hr = n ? wrap_node(This->doc, items[0], out) : S_FALSE;
    mem_free(items);
    return hr;
}

static HRESULT STDMETHODCALLTYPE nd_get_parsed(Node *This, VARIANT_BOOL *out)
{
    (void)This;
    if (!out) return E_INVALIDARG;
    *out = VARIANT_TRUE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nd_get_namespaceURI(Node *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    if (is_nsdecl_attr(x)) return ret_bstr(out, BAD_CAST (This->doc->version >= 6 ? "http://www.w3.org/2000/xmlns/" : ""));
    if ((x->type == XML_ELEMENT_NODE || x->type == XML_ATTRIBUTE_NODE) && x->ns) return ret_bstr(out, x->ns->href);
    return ret_bstr(out, BAD_CAST "");
}

static HRESULT STDMETHODCALLTYPE nd_get_prefix(Node *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    if (is_nsdecl_attr(x)) return ret_bstr(out, BAD_CAST (x->name[5] == ':' ? "xmlns" : ""));
    if ((x->type == XML_ELEMENT_NODE || x->type == XML_ATTRIBUTE_NODE) && x->ns && x->ns->prefix)
        return ret_bstr(out, x->ns->prefix);
    if (x->type == XML_ELEMENT_NODE || x->type == XML_ATTRIBUTE_NODE) {    /* createElement("p:x") */
        const xmlChar *colon = xmlStrchr(x->name, ':');
        if (colon) { *out = bstr_utf8n(x->name, (int)(colon - x->name)); return *out ? S_OK : E_OUTOFMEMORY; }
    }
    return ret_bstr(out, BAD_CAST "");
}

static HRESULT STDMETHODCALLTYPE nd_get_baseName(Node *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    xmlNodePtr x = node_x(This);
    if (is_nsdecl_attr(x)) return ret_bstr(out, BAD_CAST (x->name[5] == ':' ? (const char *)x->name + 6 : "xmlns"));
    switch (x->type) {
    case XML_ELEMENT_NODE: case XML_ATTRIBUTE_NODE: {
        const xmlChar *colon = x->ns ? 0 : xmlStrchr(x->name, ':');
        return ret_bstr(out, colon ? colon + 1 : x->name);
    }
    case XML_PI_NODE: case XML_ENTITY_REF_NODE: case XML_DTD_NODE: return ret_bstr(out, x->name);
    default: *out = 0; return S_FALSE;
    }
}

#define NODE_SLOTS \
    .QueryInterface = (void *)nd_qi, .AddRef = (void *)nd_addref, .Release = (void *)nd_release, DISPATCH_SLOTS, \
    .get_nodeName = (void *)nd_get_nodeName, .get_nodeValue = (void *)nd_get_nodeValue, \
    .put_nodeValue = (void *)nd_put_nodeValue, .get_nodeType = (void *)nd_get_nodeType, \
    .get_parentNode = (void *)nd_get_parentNode, .get_childNodes = (void *)nd_get_childNodes, \
    .get_firstChild = (void *)nd_get_firstChild, .get_lastChild = (void *)nd_get_lastChild, \
    .get_previousSibling = (void *)nd_get_previousSibling, .get_nextSibling = (void *)nd_get_nextSibling, \
    .get_attributes = (void *)nd_get_attributes, .insertBefore = (void *)nd_insertBefore, \
    .replaceChild = (void *)nd_replaceChild, .removeChild = (void *)nd_removeChild, \
    .appendChild = (void *)nd_appendChild, .hasChildNodes = (void *)nd_hasChildNodes, \
    .get_ownerDocument = (void *)nd_get_ownerDocument, .cloneNode = (void *)nd_cloneNode, \
    .get_nodeTypeString = (void *)nd_get_nodeTypeString, .get_text = (void *)nd_get_text, \
    .put_text = (void *)nd_put_text, .get_specified = (void *)nd_get_specified, \
    .get_definition = (void *)nd_get_definition, .get_nodeTypedValue = (void *)nd_get_nodeTypedValue, \
    .put_nodeTypedValue = (void *)nd_put_nodeTypedValue, .get_dataType = (void *)nd_get_dataType, \
    .put_dataType = (void *)nd_put_dataType, .get_xml = (void *)nd_get_xml, \
    .transformNode = (void *)nd_transformNode, .selectNodes = (void *)nd_selectNodes, \
    .selectSingleNode = (void *)nd_selectSingleNode, .get_parsed = (void *)nd_get_parsed, \
    .get_namespaceURI = (void *)nd_get_namespaceURI, .get_prefix = (void *)nd_get_prefix, \
    .get_baseName = (void *)nd_get_baseName, .transformNodeToObject = (void *)nd_transformNodeToObject

static const IXMLDOMNodeVtbl other_vtbl = { NODE_SLOTS };

/* ---- IXMLDOMElement ---- */

/* the attribute named @name ("a", "p:a", "xmlns", "xmlns:p") */
static xmlAttrPtr find_attr(Doc *d, xmlNodePtr el, const xmlChar *name)
{
    if (!xmlStrcmp(name, BAD_CAST "xmlns") || !xmlStrncmp(name, BAD_CAST "xmlns:", 6)) {
        const xmlChar *pre = name[5] ? name + 6 : 0;
        for (xmlNsPtr ns = el->nsDef; ns; ns = ns->next)
            if (pre ? ns->prefix && xmlStrEqual(ns->prefix, pre) : !ns->prefix) return ns_attr(d, el, ns);
        return 0;
    }
    for (xmlAttrPtr a = el->properties; a; a = a->next) {
        if (a->ns && a->ns->prefix) {
            int pl = xmlStrlen(a->ns->prefix);
            if (!xmlStrncmp(name, a->ns->prefix, pl) && name[pl] == ':' && xmlStrEqual(name + pl + 1, a->name)) return a;
        } else if (xmlStrEqual(a->name, name))
            return a;
    }
    return 0;
}

static HRESULT STDMETHODCALLTYPE el_getAttribute(Node *This, BSTR name, VARIANT *v)
{
    if (!v || !name) return E_INVALIDARG;
    VariantInit(v);
    v->vt = VT_NULL;
    xmlChar *n = utf8_wide(name, -1);
    if (!n) return E_OUTOFMEMORY;
    xmlAttrPtr a = find_attr(This->doc, This->x, n);
    xmlFree(n);
    if (!a) return S_FALSE;
    xmlChar *c = xmlNodeGetContent((xmlNodePtr)a);
    v->vt = VT_BSTR;
    v->bstrVal = bstr_utf8(c);
    xmlFree(c);
    return v->bstrVal ? S_OK : E_OUTOFMEMORY;
}

static HRESULT set_attr(Doc *d, xmlNodePtr el, const xmlChar *name, const xmlChar *val)
{
    if (!xmlStrcmp(name, BAD_CAST "xmlns") || !xmlStrncmp(name, BAD_CAST "xmlns:", 6)) {
        const xmlChar *pre = name[5] ? name + 6 : 0;
        for (xmlNsPtr ns = el->nsDef; ns; ns = ns->next)
            if (pre ? ns->prefix && xmlStrEqual(ns->prefix, pre) : !ns->prefix) {
                xmlFree((xmlChar *)ns->href);
                ns->href = xmlStrdup(val);
                return S_OK;
            }
        if (!pre && el->ns && !el->ns->prefix && !xmlStrEqual(el->ns->href, val)) return E_FAIL;
        return xmlNewNs(el, val, pre) ? S_OK : E_OUTOFMEMORY;
    }
    (void)d;
    const xmlChar *colon = xmlStrchr(name, ':');
    if (colon) {
        xmlChar *pre = xmlStrndup(name, (int)(colon - name));
        xmlNsPtr ns = pre ? xmlSearchNs(el->doc, el, pre) : 0;
        xmlFree(pre);
        if (ns) return xmlSetNsProp(el, ns, colon + 1, val) ? S_OK : E_OUTOFMEMORY;
    }
    return xmlSetProp(el, name, val) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE el_setAttribute(Node *This, BSTR name, VARIANT value)
{
    if (!name) return E_INVALIDARG;
    BSTR s;
    HRESULT hr = variant_string(&value, &s);
    if (FAILED(hr)) return hr;
    xmlChar *n = utf8_wide(name, -1), *v = utf8_wide(s, (int)SysStringLen(s));
    SysFreeString(s);
    hr = n && v ? set_attr(This->doc, This->x, n, v) : E_OUTOFMEMORY;
    xmlFree(n);
    xmlFree(v);
    return hr;
}

static void remove_attr(Doc *d, xmlNodePtr el, xmlAttrPtr a)
{
    if (is_nsdecl_attr((xmlNodePtr)a)) {             /* a declaration: only when nothing uses it */
        for (NsAttr *na = d->nsattrs; na; na = na->next)
            if (na->attr == a) {
                xmlNsPtr *pp = &el->nsDef;
                while (*pp && *pp != na->ns) pp = &(*pp)->next;
                if (*pp) {
                    xmlNodePtr x = el;
                    BOOL used = x->ns == na->ns;
                    for (xmlAttrPtr p = el->properties; p && !used; p = p->next) used = p->ns == na->ns;
                    if (!used) { *pp = na->ns->next; na->ns->next = 0; }  /* (kept alive: the NsAttr points at it) */
                }
            }
        return;
    }
    xmlUnlinkNode((xmlNodePtr)a);
    doc_orphan(d, (xmlNodePtr)a);
}

static HRESULT STDMETHODCALLTYPE el_removeAttribute(Node *This, BSTR name)
{
    if (!name) return E_INVALIDARG;
    xmlChar *n = utf8_wide(name, -1);
    if (!n) return E_OUTOFMEMORY;
    xmlAttrPtr a = find_attr(This->doc, This->x, n);
    xmlFree(n);
    if (!a) return S_FALSE;
    remove_attr(This->doc, This->x, a);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE el_getAttributeNode(Node *This, BSTR name, IXMLDOMAttribute **out)
{
    if (!out || !name) return E_INVALIDARG;
    *out = 0;
    xmlChar *n = utf8_wide(name, -1);
    if (!n) return E_OUTOFMEMORY;
    xmlAttrPtr a = find_attr(This->doc, This->x, n);
    xmlFree(n);
    return node_wrap(This->doc, (xmlNodePtr)a, &IID_IXMLDOMAttribute, (void **)out);
}

static HRESULT attr_attach(Doc *d, xmlNodePtr el, xmlAttrPtr a, xmlAttrPtr *old)
{
    *old = 0;
    if (a->parent) return a->parent == el ? S_OK : E_FAIL;    /* (in use on another element) */
    if (a->doc != el->doc) xmlSetTreeDoc((xmlNodePtr)a, el->doc);
    if (a->ns) {                                        /* the element must declare the namespace */
        xmlNsPtr ns = xmlSearchNsByHref(el->doc, el, a->ns->href);
        if (!ns) ns = xmlNewNs(el, a->ns->href, a->ns->prefix);
        a->ns = ns;
    }
    for (xmlAttrPtr p = el->properties; p; p = p->next)
        if (xmlStrEqual(p->name, a->name) && p->ns == a->ns) { *old = p; break; }
    if (*old) { xmlUnlinkNode((xmlNodePtr)*old); doc_orphan(d, (xmlNodePtr)*old); }
    a->parent = el;
    a->next = 0;
    if (!el->properties) { el->properties = a; a->prev = 0; }
    else {
        xmlAttrPtr last = el->properties;
        while (last->next) last = last->next;
        last->next = a;
        a->prev = last;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE el_setAttributeNode(Node *This, IXMLDOMAttribute *attr, IXMLDOMAttribute **old)
{
    if (old) *old = 0;
    Node *a = node_from_iface(attr);
    if (!a || is_doc_node(a) || a->x->type != XML_ATTRIBUTE_NODE) return E_INVALIDARG;
    xmlAttrPtr ap = (xmlAttrPtr)a->x;
    if (a->doc != This->doc) {
        ap = xmlCopyProp(0, ap);
        if (!ap) return E_OUTOFMEMORY;
    }
    xmlAttrPtr prev;
    HRESULT hr = attr_attach(This->doc, This->x, ap, &prev);
    if (FAILED(hr)) return hr;
    if (old && prev) return node_wrap(This->doc, (xmlNodePtr)prev, &IID_IXMLDOMAttribute, (void **)old);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE el_removeAttributeNode(Node *This, IXMLDOMAttribute *attr, IXMLDOMAttribute **out)
{
    if (out) *out = 0;
    Node *a = node_from_iface(attr);
    if (!a || is_doc_node(a) || a->x->type != XML_ATTRIBUTE_NODE) return E_INVALIDARG;
    if (a->x->parent != This->x && !is_nsdecl_attr(a->x)) return E_INVALIDARG;
    remove_attr(This->doc, This->x, (xmlAttrPtr)a->x);
    if (out) { *out = attr; attr->lpVtbl->AddRef(attr); }
    return S_OK;
}

/* descendants named @name ("*": all) in document order */
static void by_tag(xmlNodePtr x, const xmlChar *name, xmlNodePtr **items, int *n, int *cap)
{
    for (xmlNodePtr c = x->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        BOOL match = name[0] == '*' && !name[1];
        if (!match) {
            xmlChar *q = qname(c);
            match = xmlStrEqual(q, name);
            xmlFree(q);
        }
        if (match) {
            if (*n == *cap) {
                int nc = *cap ? *cap * 2 : 16;
                xmlNodePtr *p = mem_realloc(*items, (SIZE_T)nc * sizeof *p);
                if (!p) return;
                *items = p;
                *cap = nc;
            }
            (*items)[(*n)++] = c;
        }
        by_tag(c, name, items, n, cap);
    }
}

static HRESULT elements_by_tag(Doc *d, xmlNodePtr x, BSTR name, IXMLDOMNodeList **out)
{
    if (!out || !name) return E_INVALIDARG;
    *out = 0;
    xmlChar *n = utf8_wide(name, -1);
    if (!n) return E_OUTOFMEMORY;
    xmlNodePtr *items = 0;
    int count = 0, cap = 0;
    by_tag(x, n, &items, &count, &cap);
    xmlFree(n);
    return list_static(d, items, count, out);
}

static HRESULT STDMETHODCALLTYPE el_getElementsByTagName(Node *This, BSTR name, IXMLDOMNodeList **out)
{
    return elements_by_tag(This->doc, This->x, name, out);
}

static void normalize(Doc *d, xmlNodePtr x)
{
    for (xmlNodePtr c = x->children; c; ) {
        xmlNodePtr next = c->next;
        if (c->type == XML_TEXT_NODE && next && next->type == XML_TEXT_NODE) {
            xmlNodeAddContent(c, next->content);
            unlink(d, next);
            continue;                                    /* (c may merge with the one after too) */
        }
        if (c->type == XML_TEXT_NODE && (!c->content || !*c->content)) unlink(d, c);
        else if (c->type == XML_ELEMENT_NODE) normalize(d, c);
        c = next;
    }
}

static HRESULT STDMETHODCALLTYPE el_normalize(Node *This)
{
    normalize(This->doc, This->x);
    return S_OK;
}

static const IXMLDOMElementVtbl element_vtbl = {
    NODE_SLOTS,
    .get_tagName = (void *)nd_get_nodeName, .getAttribute = (void *)el_getAttribute,
    .setAttribute = (void *)el_setAttribute, .removeAttribute = (void *)el_removeAttribute,
    .getAttributeNode = (void *)el_getAttributeNode, .setAttributeNode = (void *)el_setAttributeNode,
    .removeAttributeNode = (void *)el_removeAttributeNode, .getElementsByTagName = (void *)el_getElementsByTagName,
    .normalize = (void *)el_normalize,
};

/* namespace declarations shown as attributes: one detached attribute per xmlNs, kept with the document */
xmlAttrPtr ns_attr(Doc *d, xmlNodePtr el, xmlNsPtr ns)
{
    for (NsAttr *a = d->nsattrs; a; a = a->next)
        if (a->ns == ns) return a->attr;
    xmlChar *name = ns->prefix ? xmlStrncatNew(BAD_CAST "xmlns:", ns->prefix, -1) : xmlStrdup(BAD_CAST "xmlns");
    xmlAttrPtr at = name ? xmlNewDocProp(el->doc, name, 0) : 0;
    xmlFree(name);
    NsAttr *na = at ? mem_alloc(sizeof *na) : 0;
    if (!na) { if (at) xmlFreeProp(at); return 0; }
    xmlNodePtr t = xmlNewDocText(el->doc, ns->href);
    if (t) { at->children = at->last = t; t->parent = (xmlNodePtr)at; }
    na->ns = ns;
    na->attr = at;
    na->next = d->nsattrs;
    d->nsattrs = na;
    doc_orphan(d, (xmlNodePtr)at);
    return at;
}

/* ---- IXMLDOMAttribute ---- */
static HRESULT STDMETHODCALLTYPE at_get_value(Node *This, VARIANT *v) { return nd_get_nodeValue(This, v); }
static HRESULT STDMETHODCALLTYPE at_put_value(Node *This, VARIANT v) { return nd_put_nodeValue(This, v); }

static const IXMLDOMAttributeVtbl attr_vtbl = {
    NODE_SLOTS,
    .get_name = (void *)nd_get_nodeName, .get_value = (void *)at_get_value, .put_value = (void *)at_put_value,
};

/* ---- IXMLDOMCharacterData, IXMLDOMText (offsets count UTF-16 units) ---- */
static WCHAR *chars(Node *This, int *n)
{
    xmlChar *c = xmlNodeGetContent(This->x);
    WCHAR *w = wide_utf8(c, -1, n);
    xmlFree(c);
    return w;
}

static HRESULT set_chars(Node *This, const WCHAR *w, int n)
{
    xmlChar *u = utf8_wide(w, n);
    if (!u) return E_OUTOFMEMORY;
    xmlNodeSetContent(This->x, u);
    xmlFree(u);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cd_get_data(Node *This, BSTR *out) { return nd_get_text(This, out); }

static HRESULT STDMETHODCALLTYPE cd_get_data_raw(Node *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    xmlChar *c = xmlNodeGetContent(This->x);
    HRESULT hr = ret_bstr(out, c);
    xmlFree(c);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cd_put_data(Node *This, BSTR data) { return set_chars(This, data, (int)SysStringLen(data)); }

static HRESULT STDMETHODCALLTYPE cd_get_length(Node *This, LONG *out)
{
    if (!out) return E_INVALIDARG;
    int n;
    WCHAR *w = chars(This, &n);
    if (!w) return E_OUTOFMEMORY;
    mem_free(w);
    *out = n;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cd_substringData(Node *This, LONG off, LONG count, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    int n;
    WCHAR *w = chars(This, &n);
    if (!w) return E_OUTOFMEMORY;
    HRESULT hr = S_OK;
    if (off < 0 || count < 0 || off > n) hr = E_INVALIDARG;
    else {
        if (count > n - off) count = n - off;
        *out = SysAllocStringLen(w + off, (UINT)count);
        hr = !count ? S_FALSE : *out ? S_OK : E_OUTOFMEMORY;
    }
    mem_free(w);
    return hr;
}

static HRESULT splice(Node *This, LONG off, LONG count, const WCHAR *ins, int nins)
{
    int n;
    WCHAR *w = chars(This, &n);
    if (!w) return E_OUTOFMEMORY;
    if (off < 0 || count < 0 || off > n) { mem_free(w); return E_INVALIDARG; }
    if (count > n - off) count = n - off;
    WCHAR *r = mem_alloc((SIZE_T)(n - count + nins + 1) * sizeof(WCHAR));
    if (!r) { mem_free(w); return E_OUTOFMEMORY; }
    memcpy(r, w, (size_t)off * sizeof(WCHAR));
    memcpy(r + off, ins, (size_t)nins * sizeof(WCHAR));
    memcpy(r + off + nins, w + off + count, (size_t)(n - off - count) * sizeof(WCHAR));
    HRESULT hr = set_chars(This, r, n - count + nins);
    mem_free(r);
    mem_free(w);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cd_appendData(Node *This, BSTR data)
{
    LONG n;
    cd_get_length(This, &n);
    return splice(This, n, 0, data ? data : L"", (int)SysStringLen(data));
}
static HRESULT STDMETHODCALLTYPE cd_insertData(Node *This, LONG off, BSTR data)
{
    return splice(This, off, 0, data ? data : L"", (int)SysStringLen(data));
}
static HRESULT STDMETHODCALLTYPE cd_deleteData(Node *This, LONG off, LONG count) { return splice(This, off, count, L"", 0); }
static HRESULT STDMETHODCALLTYPE cd_replaceData(Node *This, LONG off, LONG count, BSTR data)
{
    return splice(This, off, count, data ? data : L"", (int)SysStringLen(data));
}

static HRESULT STDMETHODCALLTYPE tx_splitText(Node *This, LONG off, IXMLDOMText **out)
{
    if (out) *out = 0;
    int n;
    WCHAR *w = chars(This, &n);
    if (!w) return E_OUTOFMEMORY;
    if (off < 0 || off > n) { mem_free(w); return E_INVALIDARG; }
    xmlChar *rest = utf8_wide(w + off, n - off);
    HRESULT hr = rest ? set_chars(This, w, off) : E_OUTOFMEMORY;
    mem_free(w);
    if (FAILED(hr)) { xmlFree(rest); return hr; }
    xmlNodePtr t = This->x->type == XML_CDATA_SECTION_NODE ? xmlNewCDataBlock(This->x->doc, rest, xmlStrlen(rest))
                                                           : xmlNewDocText(This->x->doc, rest);
    xmlFree(rest);
    if (!t) return E_OUTOFMEMORY;
    if (This->x->parent) link_before(This->x->parent, This->x->next, t);
    else doc_orphan(This->doc, t);
    return out ? node_wrap(This->doc, t, &IID_IXMLDOMText, (void **)out) : S_OK;
}

#define CHARDATA_SLOTS \
    .get_data = (void *)cd_get_data_raw, .put_data = (void *)cd_put_data, .get_length = (void *)cd_get_length, \
    .substringData = (void *)cd_substringData, .appendData = (void *)cd_appendData, \
    .insertData = (void *)cd_insertData, .deleteData = (void *)cd_deleteData, .replaceData = (void *)cd_replaceData

static const IXMLDOMTextVtbl text_vtbl = { NODE_SLOTS, CHARDATA_SLOTS, .splitText = (void *)tx_splitText };
static const IXMLDOMCommentVtbl comment_vtbl = { NODE_SLOTS, CHARDATA_SLOTS };

/* ---- IXMLDOMProcessingInstruction ---- */
static HRESULT STDMETHODCALLTYPE pi_get_target(Node *This, BSTR *out) { return ret_bstr(out, This->x->name); }

static const IXMLDOMProcessingInstructionVtbl pi_vtbl = {
    NODE_SLOTS,
    .get_target = (void *)pi_get_target, .get_data = (void *)cd_get_data_raw, .put_data = (void *)cd_put_data,
};

/* ---- IXMLDOMDocumentType ---- */
static HRESULT STDMETHODCALLTYPE dt_get_name(Node *This, BSTR *out) { return ret_bstr(out, This->x->name); }
static HRESULT STDMETHODCALLTYPE dt_get_map(Node *This, IXMLDOMNamedNodeMap **out)
{
    (void)This;
    if (!out) return E_INVALIDARG;
    return empty_map(out);
}

static const IXMLDOMDocumentTypeVtbl doctype_vtbl = {
    NODE_SLOTS,
    .get_name = (void *)dt_get_name, .get_entities = (void *)dt_get_map, .get_notations = (void *)dt_get_map,
};

/* ==== the document ==== */
#define DOC(This) (((Node *)(This))->doc)

static void err_set(Doc *d, LONG code, const char *reason, int line, int col, LONG filepos, const char *src, int nsrc)
{
    err_clear(&d->err);
    d->err.code = code;
    d->err.line = line;
    d->err.linepos = col;
    d->err.filepos = filepos;
    if (reason) {
        xmlChar *r = xmlStrdup(BAD_CAST reason);
        int n = xmlStrlen(r);
        while (n && (r[n - 1] == '\n' || r[n - 1] == '\r')) r[--n] = 0;
        xmlChar *withcrlf = r ? xmlStrncatNew(r, BAD_CAST "\r\n", -1) : 0;
        d->err.reason = bstr_utf8(withcrlf);
        xmlFree(withcrlf);
        xmlFree(r);
    }
    if (src) d->err.src = bstr_utf8n(BAD_CAST src, nsrc);
    if (d->url) d->err.url = SysAllocString(d->url);
}

/* drop whitespace-only text (MSXML's preserveWhiteSpace false), honouring xml:space="preserve" */
static void strip_blanks(Doc *d, xmlNodePtr x, BOOL keep)
{
    for (xmlNodePtr c = x->children, next; c; c = next) {
        next = c->next;
        if (c->type == XML_TEXT_NODE && !keep) {
            BOOL blank = TRUE;
            for (const xmlChar *p = c->content; p && *p && blank; p++) blank = IS_BLANK_CH(*p);
            if (blank) { xmlUnlinkNode(c); xmlFreeNode(c); }
        } else if (c->type == XML_ELEMENT_NODE) {
            int sp = xmlNodeGetSpacePreserve(c);
            strip_blanks(d, c, sp == 1 ? TRUE : sp == 0 ? FALSE : keep);
        }
    }
}

/* the source's XML declaration, as a processing instruction child named "xml"
 * holding its text as written ("version=\"1.0\" encoding=\"utf-8\"") */
static xmlChar *declaration_text(const char *data, int len)
{
    int i = 0, step = 1;
    if (len >= 3 && (BYTE)data[0] == 0xEF && (BYTE)data[1] == 0xBB && (BYTE)data[2] == 0xBF) i = 3;
    else if (len >= 2 && (BYTE)data[0] == 0xFF && (BYTE)data[1] == 0xFE) { i = 2; step = 2; }
    else if (len >= 2 && (BYTE)data[0] == 0xFE && (BYTE)data[1] == 0xFF) { i = 3; step = 2; }
    else if (len >= 2 && data[0] == '<' && !data[1]) step = 2;
    char buf[256];
    int n = 0;
    for (; i < len && n < (int)sizeof buf - 1; i += step) {
        buf[n++] = data[i];
        if (n >= 2 && buf[n - 2] == '?' && buf[n - 1] == '>') break;
    }
    buf[n] = 0;
    if (n < 7 || strncmp(buf, "<?xml", 5) || !IS_BLANK_CH(buf[5]) || buf[n - 1] != '>') return 0;
    int s = 5, e = n - 2;
    while (s < e && IS_BLANK_CH(buf[s])) s++;
    while (e > s && IS_BLANK_CH(buf[e - 1])) e--;
    return xmlStrndup(BAD_CAST buf + s, e - s);
}

static void put_declaration(xmlDocPtr x, const char *data, int len)
{
    xmlChar *text = declaration_text(data, len);
    if (!text) return;
    xmlNodePtr pi = xmlNewDocPI(x, BAD_CAST "xml", text);
    xmlFree(text);
    if (!pi) return;
    pi->parent = (xmlNodePtr)x;
    pi->next = x->children;
    if (x->children) x->children->prev = pi;
    else x->last = pi;
    x->children = pi;
}

typedef struct LoadErr { BOOL set; xmlError e; char msg[512]; int code; } LoadErr;

static void collect_error(void *ctx, const xmlError *e)
{
    LoadErr *le = ctx;
    if (le->set || e->level < XML_ERR_ERROR) return;
    le->set = TRUE;
    le->code = e->code;
    le->e.line = e->line;
    le->e.int2 = e->int2;
    int n = 0;
    for (const char *m = e->message ? e->message : "parse error"; *m && n < (int)sizeof le->msg - 1; m++) le->msg[n++] = *m;
    le->msg[n] = 0;
}

/* the text of line @line (1-based) of the input, for parseError.srcText */
static void line_text(const char *data, int len, BOOL utf8, int line, const char **start, int *n)
{
    *start = 0;
    *n = 0;
    if (!utf8 || line < 1) return;
    int cur = 1, i = 0;
    while (i < len && cur < line) { if (data[i] == '\n') cur++; i++; }
    int j = i;
    while (j < len && data[j] != '\n' && data[j] != '\r') j++;
    *start = data + i;
    *n = j - i;
}

HRESULT doc_load_bytes(Doc *d, const char *data, int len, BOOL utf8)
{
    msxml_init();
    int opts = XML_PARSE_NONET | XML_PARSE_NODICT | XML_PARSE_NOENT | XML_PARSE_BIG_LINES;
    if (!d->resolve) opts |= XML_PARSE_NO_XXE;
    else opts |= XML_PARSE_DTDLOAD | XML_PARSE_DTDATTR;
    if (d->validate && d->resolve) opts |= XML_PARSE_DTDVALID;
    if (utf8) opts |= XML_PARSE_IGNORE_ENC;
    xmlParserCtxtPtr ctx = xmlNewParserCtxt();
    if (!ctx) return E_OUTOFMEMORY;
    LoadErr le = { 0 };
    xmlCtxtSetErrorHandler(ctx, collect_error, &le);
    char *url = d->url ? (char *)utf8_wide(d->url, -1) : 0;
    xmlDocPtr x = len ? xmlCtxtReadMemory(ctx, data, len, url, utf8 ? "UTF-8" : 0, opts) : 0;
    xmlFree(url);
    BOOL valid_fail = x && (opts & XML_PARSE_DTDVALID) && x->intSubset && !ctx->valid;
    xmlFreeParserCtxt(ctx);

    /* the old tree stays (wrappers may point into it); the document starts empty */
    if (d->x) {
        if (d->nold == d->cold) {
            int c = d->cold ? d->cold * 2 : 4;
            xmlDocPtr *o = mem_realloc(d->old, (SIZE_T)c * sizeof *o);
            if (o) { d->old = o; d->cold = c; }
        }
        if (d->nold < d->cold) d->old[d->nold++] = d->x;
        else xmlFreeDoc(d->x);
        d->x = 0;
    }
    if (x && d->prohibit_dtd && x->intSubset) {
        xmlFreeDoc(x);
        x = 0;
        le.set = TRUE;
        le.code = -1;
        strcpy(le.msg, "DTD is prohibited.");
    }
    if (x && valid_fail) { xmlFreeDoc(x); x = 0; if (!le.set) { le.set = TRUE; strcpy(le.msg, "The document is not valid."); } }
    if (!x) {
        d->x = xmlNewDoc(BAD_CAST "1.0");
        if (!d->x) return E_OUTOFMEMORY;
        const char *src;
        int nsrc;
        line_text(data, len, utf8, le.e.line, &src, &nsrc);
        LONG code = !len || le.code == XML_ERR_DOCUMENT_EMPTY || le.code == XML_ERR_DOCUMENT_END ? XML_E_MISSINGROOT : XML_E_BADXML;
        if (!len) err_set(d, code, "XML document must have a top level element.", 0, 0, 0, 0, 0);
        else err_set(d, code, le.msg, le.e.line, le.e.int2, 0, src, nsrc);
        return S_FALSE;
    }
    if (!d->preserve) strip_blanks(d, (xmlNodePtr)x, FALSE);
    put_declaration(x, data, len);
    d->x = x;
    err_clear(&d->err);
    return S_OK;
}

static void ready(Doc *d)
{
    if (!d->onready) return;
    DISPPARAMS none = { 0 };
    d->onready->lpVtbl->Invoke(d->onready, 0 /* DISPID_VALUE */, &IID_NULL, 0, DISPATCH_METHOD, &none, 0, 0, 0);
}

static HRESULT STDMETHODCALLTYPE doc_get_doctype(void *This, IXMLDOMDocumentType **out)
{
    if (!out) return E_INVALIDARG;
    xmlDtdPtr dtd = xmlGetIntSubset(DOC(This)->x);
    return node_wrap(DOC(This), (xmlNodePtr)dtd, &IID_IXMLDOMDocumentType, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_get_implementation(void *This, IXMLDOMImplementation **out)
{
    (void)This;
    if (!out) return E_INVALIDARG;
    return impl_create(out);
}

static HRESULT STDMETHODCALLTYPE doc_get_documentElement(void *This, IXMLDOMElement **out)
{
    if (!out) return E_INVALIDARG;
    return node_wrap(DOC(This), xmlDocGetRootElement(DOC(This)->x), &IID_IXMLDOMElement, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_putref_documentElement(void *This, IXMLDOMElement *el)
{
    Doc *d = DOC(This);
    Node *n = node_from_iface(el);
    if (!n || is_doc_node(n) || n->x->type != XML_ELEMENT_NODE) return E_INVALIDARG;
    xmlNodePtr root = xmlDocGetRootElement(d->x);
    if (root == n->x) return S_OK;
    xmlNodePtr ref = root ? root->next : 0;
    if (root) unlink(d, root);
    return insert(&d->self, (xmlNodePtr)d->x, n, ref, 0);
}

static HRESULT made(Doc *d, xmlNodePtr x, REFIID riid, void **out)
{
    if (!out) { if (x) { doc_orphan(d, x); } return E_INVALIDARG; }
    if (!x) return E_OUTOFMEMORY;
    doc_orphan(d, x);
    return node_wrap(d, x, riid, out);
}

static HRESULT STDMETHODCALLTYPE doc_createElement(void *This, BSTR name, IXMLDOMElement **out)
{
    if (out) *out = 0;
    if (!name || !*name) return E_FAIL;
    xmlChar *n = utf8_wide(name, -1);
    if (!n) return E_OUTOFMEMORY;
    xmlNodePtr x = xmlNewDocNode(DOC(This)->x, 0, n, 0);
    xmlFree(n);
    return made(DOC(This), x, &IID_IXMLDOMElement, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_createDocumentFragment(void *This, IXMLDOMDocumentFragment **out)
{
    return made(DOC(This), xmlNewDocFragment(DOC(This)->x), &IID_IXMLDOMDocumentFragment, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_createTextNode(void *This, BSTR data, IXMLDOMText **out)
{
    xmlChar *u = utf8_wide(data, (int)SysStringLen(data));
    xmlNodePtr x = u ? xmlNewDocText(DOC(This)->x, u) : 0;
    xmlFree(u);
    return made(DOC(This), x, &IID_IXMLDOMText, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_createComment(void *This, BSTR data, IXMLDOMComment **out)
{
    xmlChar *u = utf8_wide(data, (int)SysStringLen(data));
    xmlNodePtr x = u ? xmlNewDocComment(DOC(This)->x, u) : 0;
    xmlFree(u);
    return made(DOC(This), x, &IID_IXMLDOMComment, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_createCDATASection(void *This, BSTR data, IXMLDOMCDATASection **out)
{
    xmlChar *u = utf8_wide(data, (int)SysStringLen(data));
    xmlNodePtr x = u ? xmlNewCDataBlock(DOC(This)->x, u, xmlStrlen(u)) : 0;
    xmlFree(u);
    return made(DOC(This), x, &IID_IXMLDOMCDATASection, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_createProcessingInstruction(void *This, BSTR target, BSTR data,
                                                                 IXMLDOMProcessingInstruction **out)
{
    if (out) *out = 0;
    if (!target || !*target) return E_FAIL;
    xmlChar *t = utf8_wide(target, -1), *u = utf8_wide(data, (int)SysStringLen(data));
    xmlNodePtr x = t && u ? xmlNewDocPI(DOC(This)->x, t, u) : 0;
    xmlFree(t);
    xmlFree(u);
    return made(DOC(This), x, &IID_IXMLDOMProcessingInstruction, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_createAttribute(void *This, BSTR name, IXMLDOMAttribute **out)
{
    if (out) *out = 0;
    if (!name || !*name) return E_FAIL;
    xmlChar *n = utf8_wide(name, -1);
    xmlAttrPtr a = n ? xmlNewDocProp(DOC(This)->x, n, 0) : 0;
    xmlFree(n);
    return made(DOC(This), (xmlNodePtr)a, &IID_IXMLDOMAttribute, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_createEntityReference(void *This, BSTR name, IXMLDOMEntityReference **out)
{
    if (out) *out = 0;
    if (!name || !*name) return E_FAIL;
    xmlChar *n = utf8_wide(name, -1);
    xmlNodePtr x = n ? xmlNewReference(DOC(This)->x, n) : 0;
    xmlFree(n);
    return made(DOC(This), x, &IID_IXMLDOMEntityReference, (void **)out);
}

static HRESULT STDMETHODCALLTYPE doc_getElementsByTagName(void *This, BSTR name, IXMLDOMNodeList **out)
{
    return elements_by_tag(DOC(This), (xmlNodePtr)DOC(This)->x, name, out);
}

static int node_type_from_variant(const VARIANT *v)
{
    const VARIANT *s = v->vt == (VT_BYREF | VT_VARIANT) ? v->pvarVal : v;
    if (s->vt == VT_BSTR) {
        for (int t = 1; t <= NODE_NOTATION; t++) {
            WCHAR w[32];
            int i = 0;
            for (const char *p = type_names[t]; *p; p++) w[i++] = (WCHAR)*p;
            w[i] = 0;
            if (!_wcsicmp(s->bstrVal, w)) return t;
        }
        return 0;
    }
    VARIANT t;
    VariantInit(&t);
    if (FAILED(VariantChangeType(&t, (VARIANT *)s, 0, VT_I4))) return 0;
    return t.lVal;
}

static HRESULT STDMETHODCALLTYPE doc_createNode(void *This, VARIANT type, BSTR name, BSTR uri, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    Doc *d = DOC(This);
    int t = node_type_from_variant(&type);
    xmlChar *n = utf8_wide(name, (int)SysStringLen(name)), *u = uri && *uri ? utf8_wide(uri, -1) : 0;
    xmlNodePtr x = 0;
    HRESULT hr = S_OK;
    const xmlChar *colon = n ? xmlStrchr(n, ':') : 0;
    xmlChar *pre = colon ? xmlStrndup(n, (int)(colon - n)) : 0;
    const xmlChar *local = colon ? colon + 1 : n;
    switch (t) {
    case NODE_ELEMENT:
        if (!n || !*n) { hr = E_FAIL; break; }
        x = xmlNewDocNode(d->x, 0, u ? local : n, 0);
        if (x && u) x->ns = xmlNewNs(x, u, pre);       /* declared on the node; dropped if its parent has it */
        break;
    case NODE_ATTRIBUTE:
        if (!n || !*n) { hr = E_FAIL; break; }
        if (u && !xmlStrEqual(n, BAD_CAST "xmlns") && xmlStrncmp(n, BAD_CAST "xmlns:", 6)) {
            /* a namespaced attribute waiting for an element: its xmlNs lives on a holder element */
            xmlNodePtr holder = xmlNewDocNode(d->x, 0, BAD_CAST "ns", 0);
            xmlNsPtr ns = holder ? xmlNewNs(holder, u, pre) : 0;
            if (ns) {
                doc_orphan(d, holder);
                x = (xmlNodePtr)xmlNewDocProp(d->x, local, 0);
                if (x) x->ns = ns;
            } else if (holder) xmlFreeNode(holder);
        } else
            x = (xmlNodePtr)xmlNewDocProp(d->x, n, 0);
        break;
    case NODE_TEXT: x = xmlNewDocText(d->x, BAD_CAST ""); break;
    case NODE_CDATA_SECTION: x = xmlNewCDataBlock(d->x, BAD_CAST "", 0); break;
    case NODE_COMMENT: x = xmlNewDocComment(d->x, BAD_CAST ""); break;
    case NODE_PROCESSING_INSTRUCTION:
        if (!n || !*n) { hr = E_FAIL; break; }
        x = xmlNewDocPI(d->x, n, BAD_CAST "");
        break;
    case NODE_DOCUMENT_FRAGMENT: x = xmlNewDocFragment(d->x); break;
    case NODE_ENTITY_REFERENCE:
        if (!n || !*n) { hr = E_FAIL; break; }
        x = xmlNewReference(d->x, n);
        break;
    default: hr = E_INVALIDARG; break;
    }
    xmlFree(pre);
    xmlFree(n);
    xmlFree(u);
    if (FAILED(hr)) return hr;
    if (!x) return E_OUTOFMEMORY;
    doc_orphan(d, x);
    return wrap_node(d, x, out);
}

static HRESULT STDMETHODCALLTYPE doc_nodeFromID(void *This, BSTR id, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    xmlChar *i = utf8_wide(id, -1);
    xmlAttrPtr a = i ? xmlGetID(DOC(This)->x, i) : 0;
    xmlFree(i);
    return a && a->parent ? wrap_node(DOC(This), a->parent, out) : S_FALSE;
}

static HRESULT load_data(Doc *d, char *data, DWORD len, VARIANT_BOOL *ok)
{
    HRESULT hr = doc_load_bytes(d, data, (int)len, FALSE);
    mem_free(data);
    if (ok) *ok = hr == S_OK ? VARIANT_TRUE : VARIANT_FALSE;
    ready(d);
    return hr;
}

static HRESULT STDMETHODCALLTYPE doc_load(void *This, VARIANT src, VARIANT_BOOL *ok)
{
    Doc *d = DOC(This);
    if (ok) *ok = VARIANT_FALSE;
    const VARIANT *s = src.vt == (VT_BYREF | VT_VARIANT) ? src.pvarVal : &src;
    char *data = 0;
    DWORD len = 0;
    HRESULT hr;
    SysFreeString(d->url);
    d->url = 0;
    switch (s->vt) {
    case VT_BSTR:
    case VT_BSTR | VT_BYREF: {
        BSTR u = s->vt == VT_BSTR ? s->bstrVal : *s->pbstrVal;
        if (!u) return E_INVALIDARG;
        d->url = SysAllocString(u);
        hr = read_url(u, &data, &len);
        if (FAILED(hr)) {
            doc_load_bytes(d, "", 0, TRUE);
            err_set(d, hr, "The system cannot locate the object specified.", 0, 0, 0, 0, 0);
            ready(d);
            return S_FALSE;
        }
        return load_data(d, data, len, ok);
    }
    case VT_ARRAY | VT_UI1: {
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(s->parray, 1, &lo);
        SafeArrayGetUBound(s->parray, 1, &hi);
        len = (DWORD)(hi - lo + 1);
        data = mem_alloc(len + 1);
        void *p;
        if (!data || FAILED(SafeArrayAccessData(s->parray, &p))) { mem_free(data); return E_OUTOFMEMORY; }
        memcpy(data, p, len);
        SafeArrayUnaccessData(s->parray);
        return load_data(d, data, len, ok);
    }
    case VT_UNKNOWN:
    case VT_DISPATCH: {
        IUnknown *unk = s->punkVal;
        if (!unk) return E_INVALIDARG;
        Node *other = node_from_iface(unk);
        if (other && is_doc_node(other)) {               /* another DOMDocument: its text */
            xmlChar *t;
            hr = node_xml(other->doc, (xmlNodePtr)other->doc->x, FALSE, &t);
            if (FAILED(hr)) return hr;
            hr = doc_load_bytes(d, (const char *)t, xmlStrlen(t), TRUE);
            xmlFree(t);
            if (ok) *ok = hr == S_OK ? VARIANT_TRUE : VARIANT_FALSE;
            return hr;
        }
        hr = read_stream(unk, &data, &len);
        if (FAILED(hr)) return hr;
        return load_data(d, data, len, ok);
    }
    default:
        return E_INVALIDARG;
    }
}

static HRESULT STDMETHODCALLTYPE doc_get_readyState(void *This, LONG *out)
{
    (void)This;
    if (!out) return E_INVALIDARG;
    *out = 4;                                            /* READYSTATE_COMPLETE: loads are synchronous */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE doc_get_parseError(void *This, IXMLDOMParseError **out)
{
    if (!out) return E_INVALIDARG;
    return parseerr_create(&DOC(This)->err, out);
}

static HRESULT STDMETHODCALLTYPE doc_get_url(void *This, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    Doc *d = DOC(This);
    if (!d->url) { *out = 0; return S_FALSE; }
    *out = SysAllocString(d->url);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE doc_get_async(void *This, VARIANT_BOOL *out)
{
    if (!out) return E_INVALIDARG;
    *out = DOC(This)->async ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE doc_put_async(void *This, VARIANT_BOOL b) { DOC(This)->async = b != 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE doc_abort(void *This) { (void)This; return S_OK; }

static HRESULT STDMETHODCALLTYPE doc_loadXML(void *This, BSTR xml, VARIANT_BOOL *ok)
{
    Doc *d = DOC(This);
    if (ok) *ok = VARIANT_FALSE;
    SysFreeString(d->url);
    d->url = 0;
    const WCHAR *w = xml ? xml : L"";
    int n = (int)SysStringLen(xml);
    if (n && w[0] == 0xFEFF) { w++; n--; }
    xmlChar *u = utf8_wide(w, n);
    if (!u) return E_OUTOFMEMORY;
    HRESULT hr = doc_load_bytes(d, (const char *)u, xmlStrlen(u), TRUE);
    xmlFree(u);
    if (ok) *ok = hr == S_OK ? VARIANT_TRUE : VARIANT_FALSE;
    return hr;
}

/* the bytes save() writes: the declaration's encoding (UTF-8 when there is none) */
static HRESULT save_bytes(Doc *d, char **out, DWORD *len)
{
    xmlChar *s;
    HRESULT hr = node_xml(d, (xmlNodePtr)d->x, TRUE, &s);
    if (FAILED(hr)) return hr;
    xmlNodePtr decl = d->x->children;
    xmlChar *enc = 0;
    if (decl && decl->type == XML_PI_NODE && xmlStrEqual(decl->name, BAD_CAST "xml") && decl->content) {
        const xmlChar *e = xmlStrstr(decl->content, BAD_CAST "encoding");
        if (e) {
            e += 8;
            while (*e && *e != '"' && *e != '\'') e++;
            if (*e) {
                xmlChar q = *e++;
                const xmlChar *end = e;
                while (*end && *end != q) end++;
                enc = xmlStrndup(e, (int)(end - e));
            }
        }
    }
    int n = xmlStrlen(s);
    if (enc && (!xmlStrcasecmp(enc, BAD_CAST "UTF-16") || !xmlStrcasecmp(enc, BAD_CAST "UCS-2"))) {
        int w;
        WCHAR *ws = wide_utf8(s, n, &w);
        char *buf = ws ? mem_alloc((SIZE_T)w * 2 + 2) : 0;
        if (buf) {
            buf[0] = (char)0xFF; buf[1] = (char)0xFE;
            memcpy(buf + 2, ws, (size_t)w * 2);
            *out = buf;
            *len = (DWORD)w * 2 + 2;
        }
        mem_free(ws);
        hr = buf ? S_OK : E_OUTOFMEMORY;
    } else if (enc && (!xmlStrcasecmp(enc, BAD_CAST "windows-1252") || !xmlStrcasecmp(enc, BAD_CAST "ISO-8859-1") ||
                       !xmlStrcasecmp(enc, BAD_CAST "US-ASCII"))) {
        int w;
        WCHAR *ws = wide_utf8(s, n, &w);
        char *buf = ws ? mem_alloc((SIZE_T)w * 8 + 1) : 0;
        int o = 0;
        for (int i = 0; buf && i < w; i++) {
            if (ws[i] < 0x80 || (ws[i] < 0x100 && enc[0] != 'U' && enc[0] != 'u')) buf[o++] = (char)ws[i];
            else o += sprintf(buf + o, "&#%u;", (unsigned)ws[i]);   /* (outside the character set: a reference) */
        }
        mem_free(ws);
        if (buf) { *out = buf; *len = (DWORD)o; }
        hr = buf ? S_OK : E_OUTOFMEMORY;
    } else {
        char *buf = mem_alloc((SIZE_T)n + 1);
        if (buf) { memcpy(buf, s, (size_t)n); *out = buf; *len = (DWORD)n; }
        hr = buf ? S_OK : E_OUTOFMEMORY;
    }
    xmlFree(enc);
    xmlFree(s);
    return hr;
}

static HRESULT STDMETHODCALLTYPE doc_save(void *This, VARIANT dest)
{
    Doc *d = DOC(This);
    const VARIANT *s = dest.vt == (VT_BYREF | VT_VARIANT) ? dest.pvarVal : &dest;
    char *data;
    DWORD len;
    HRESULT hr;
    switch (s->vt) {
    case VT_BSTR: {
        if (!s->bstrVal) return E_INVALIDARG;
        if (FAILED(hr = save_bytes(d, &data, &len))) return hr;
        HANDLE f = CreateFileW(s->bstrVal, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (f == INVALID_HANDLE_VALUE) { mem_free(data); return HRESULT_FROM_WIN32(GetLastError()); }
        DWORD put = 0;
        BOOL ok = WriteFile(f, data, len, &put, 0);
        CloseHandle(f);
        mem_free(data);
        return ok && put == len ? S_OK : E_FAIL;
    }
    case VT_UNKNOWN:
    case VT_DISPATCH: {
        IUnknown *unk = s->punkVal;
        if (!unk) return E_INVALIDARG;
        Node *other = node_from_iface(unk);
        if (other && is_doc_node(other)) {
            xmlChar *t;
            if (FAILED(hr = node_xml(d, (xmlNodePtr)d->x, FALSE, &t))) return hr;
            hr = doc_load_bytes(other->doc, (const char *)t, xmlStrlen(t), TRUE);
            xmlFree(t);
            return SUCCEEDED(hr) ? S_OK : hr;
        }
        ISequentialStream *st;
        if (FAILED(unk->lpVtbl->QueryInterface(unk, &IID_IStream, (void **)&st)) &&
            FAILED(unk->lpVtbl->QueryInterface(unk, &IID_ISequentialStream, (void **)&st)))
            return E_INVALIDARG;
        if (SUCCEEDED(hr = save_bytes(d, &data, &len))) {
            ULONG put = 0;
            hr = st->lpVtbl->Write(st, data, len, &put);
            mem_free(data);
        }
        st->lpVtbl->Release(st);
        return SUCCEEDED(hr) ? S_OK : hr;
    }
    default:
        return E_INVALIDARG;
    }
}

#define BOOL_PROP(name, field) \
    static HRESULT STDMETHODCALLTYPE doc_get_##name(void *This, VARIANT_BOOL *out) \
    { if (!out) return E_INVALIDARG; *out = DOC(This)->field ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; } \
    static HRESULT STDMETHODCALLTYPE doc_put_##name(void *This, VARIANT_BOOL b) { DOC(This)->field = b != 0; return S_OK; }
BOOL_PROP(validateOnParse, validate)
BOOL_PROP(resolveExternals, resolve)
BOOL_PROP(preserveWhiteSpace, preserve)

static HRESULT STDMETHODCALLTYPE doc_put_onreadystatechange(void *This, VARIANT v)
{
    Doc *d = DOC(This);
    if (d->onready) { d->onready->lpVtbl->Release(d->onready); d->onready = 0; }
    if ((v.vt == VT_DISPATCH || v.vt == VT_UNKNOWN) && v.punkVal)
        v.punkVal->lpVtbl->QueryInterface(v.punkVal, &IID_IDispatch, (void **)&d->onready);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE doc_put_ignored(void *This, VARIANT v) { (void)This; (void)v; return S_OK; }

static HRESULT STDMETHODCALLTYPE doc_get_namespaces(void *This, IXMLDOMSchemaCollection **out)
{
    (void)This;
    if (out) *out = 0;
    OutputDebugStringA("msxml6: namespaces: no schema collection\n");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE doc_get_schemas(void *This, VARIANT *v)
{
    (void)This;
    if (!v) return E_INVALIDARG;
    VariantInit(v);
    v->vt = VT_NULL;
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE doc_putref_schemas(void *This, VARIANT v) { (void)This; (void)v; return S_OK; }

static HRESULT STDMETHODCALLTYPE doc_validate(void *This, IXMLDOMParseError **out)
{
    /* no schemas: a document is valid when it is well formed (it is, once loaded) */
    ParseErr none = { 0 };
    (void)This;
    return out ? parseerr_create(&none, out) : S_OK;
}

static BOOL prop_is(BSTR name, const WCHAR *what) { return name && !wcscmp(name, what); }

static HRESULT STDMETHODCALLTYPE doc_setProperty(void *This, BSTR name, VARIANT value)
{
    Doc *d = DOC(This);
    if (prop_is(name, L"SelectionLanguage")) {
        BSTR s;
        HRESULT hr = variant_string(&value, &s);
        if (FAILED(hr)) return hr;
        if (!_wcsicmp(s, L"XPath")) d->xslpattern = FALSE;
        else if (!_wcsicmp(s, L"XSLPattern") && d->version < 6) d->xslpattern = TRUE;
        else hr = E_FAIL;
        SysFreeString(s);
        return hr;
    }
    if (prop_is(name, L"SelectionNamespaces")) {
        BSTR s;
        HRESULT hr = variant_string(&value, &s);
        if (FAILED(hr)) return hr;
        xmlFree(d->sel_ns);
        d->sel_ns = utf8_wide(s, (int)SysStringLen(s));
        SysFreeString(s);
        return S_OK;
    }
    VARIANT b;
    VariantInit(&b);
    BOOL flag = SUCCEEDED(VariantChangeType(&b, &value, 0, VT_BOOL)) && b.boolVal;
    if (prop_is(name, L"ProhibitDTD")) { d->prohibit_dtd = flag; return S_OK; }
    if (prop_is(name, L"ResolveExternals")) { d->resolve = flag; return S_OK; }
    if (prop_is(name, L"ValidateOnParse")) { d->validate = flag; return S_OK; }
    if (prop_is(name, L"MaxElementDepth")) {
        VARIANT i;
        VariantInit(&i);
        if (SUCCEEDED(VariantChangeType(&i, &value, 0, VT_I4))) d->max_depth = i.lVal;
        return S_OK;
    }
    static const WCHAR *const accepted[] = {        /* (understood, no effect here) */
        L"NewParser", L"ServerHTTPRequest", L"AllowDocumentFunction", L"AllowXsltScript", L"UseInlineSchema",
        L"MultipleErrorMessages", L"MaxXMLSize", L"ForcedResync", L"NormalizeAttributeValues", L"UseInlineSchema",
        L"Async", L"ResolveExternals",
    };
    for (unsigned i = 0; i < sizeof accepted / sizeof accepted[0]; i++)
        if (prop_is(name, accepted[i])) return S_OK;
    return E_FAIL;
}

static HRESULT STDMETHODCALLTYPE doc_getProperty(void *This, BSTR name, VARIANT *v)
{
    Doc *d = DOC(This);
    if (!v) return E_INVALIDARG;
    VariantInit(v);
    if (prop_is(name, L"SelectionLanguage")) {
        v->vt = VT_BSTR;
        v->bstrVal = SysAllocString(d->xslpattern ? L"XSLPattern" : L"XPath");
        return S_OK;
    }
    if (prop_is(name, L"SelectionNamespaces")) {
        v->vt = VT_BSTR;
        v->bstrVal = bstr_utf8(d->sel_ns);
        return S_OK;
    }
    v->vt = VT_BOOL;
    if (prop_is(name, L"ProhibitDTD")) { v->boolVal = d->prohibit_dtd ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
    if (prop_is(name, L"ResolveExternals")) { v->boolVal = d->resolve ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
    if (prop_is(name, L"ValidateOnParse")) { v->boolVal = d->validate ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
    if (prop_is(name, L"NewParser") || prop_is(name, L"AllowDocumentFunction") || prop_is(name, L"AllowXsltScript") ||
        prop_is(name, L"UseInlineSchema") || prop_is(name, L"MultipleErrorMessages") ||
        prop_is(name, L"ServerHTTPRequest") || prop_is(name, L"NormalizeAttributeValues")) {
        v->boolVal = VARIANT_FALSE;
        return S_OK;
    }
    if (prop_is(name, L"MaxElementDepth") || prop_is(name, L"MaxXMLSize")) {
        v->vt = VT_I4;
        v->lVal = prop_is(name, L"MaxElementDepth") ? d->max_depth : 0;
        return S_OK;
    }
    v->vt = VT_EMPTY;
    return E_FAIL;
}

static HRESULT STDMETHODCALLTYPE doc_validateNode(void *This, IXMLDOMNode *node, IXMLDOMParseError **out)
{
    (void)node;
    return doc_validate(This, out);
}

static HRESULT STDMETHODCALLTYPE doc_importNode(void *This, IXMLDOMNode *node, VARIANT_BOOL deep, IXMLDOMNode **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    Doc *d = DOC(This);
    Node *n = node_from_iface(node);
    if (!n || is_doc_node(n)) return E_INVALIDARG;
    xmlNodePtr c = n->x->type == XML_ATTRIBUTE_NODE ? (xmlNodePtr)xmlCopyProp(0, (xmlAttrPtr)n->x)
                                                    : xmlDocCopyNode(n->x, d->x, deep ? 1 : 2);
    if (!c) return E_OUTOFMEMORY;
    if (c->doc != d->x) xmlSetTreeDoc(c, d->x);
    doc_orphan(d, c);
    return wrap_node(d, c, out);
}

static const IXMLDOMDocument3Vtbl doc_vtbl = {
    NODE_SLOTS,
    .get_doctype = (void *)doc_get_doctype, .get_implementation = (void *)doc_get_implementation,
    .get_documentElement = (void *)doc_get_documentElement, .putref_documentElement = (void *)doc_putref_documentElement,
    .createElement = (void *)doc_createElement, .createDocumentFragment = (void *)doc_createDocumentFragment,
    .createTextNode = (void *)doc_createTextNode, .createComment = (void *)doc_createComment,
    .createCDATASection = (void *)doc_createCDATASection,
    .createProcessingInstruction = (void *)doc_createProcessingInstruction,
    .createAttribute = (void *)doc_createAttribute, .createEntityReference = (void *)doc_createEntityReference,
    .getElementsByTagName = (void *)doc_getElementsByTagName, .createNode = (void *)doc_createNode,
    .nodeFromID = (void *)doc_nodeFromID, .load = (void *)doc_load, .get_readyState = (void *)doc_get_readyState,
    .get_parseError = (void *)doc_get_parseError, .get_url = (void *)doc_get_url, .get_async = (void *)doc_get_async,
    .put_async = (void *)doc_put_async, .abort = (void *)doc_abort, .loadXML = (void *)doc_loadXML,
    .save = (void *)doc_save, .get_validateOnParse = (void *)doc_get_validateOnParse,
    .put_validateOnParse = (void *)doc_put_validateOnParse, .get_resolveExternals = (void *)doc_get_resolveExternals,
    .put_resolveExternals = (void *)doc_put_resolveExternals,
    .get_preserveWhiteSpace = (void *)doc_get_preserveWhiteSpace,
    .put_preserveWhiteSpace = (void *)doc_put_preserveWhiteSpace,
    .put_onreadystatechange = (void *)doc_put_onreadystatechange, .put_ondataavailable = (void *)doc_put_ignored,
    .put_ontransformnode = (void *)doc_put_ignored,
    .get_namespaces = (void *)doc_get_namespaces, .get_schemas = (void *)doc_get_schemas,
    .putref_schemas = (void *)doc_putref_schemas, .validate = (void *)doc_validate,
    .setProperty = (void *)doc_setProperty, .getProperty = (void *)doc_getProperty,
    .validateNode = (void *)doc_validateNode, .importNode = (void *)doc_importNode,
};

HRESULT doc_create(int version, BOOL free_threaded, REFIID riid, void **out)
{
    *out = 0;
    msxml_init();
    Doc *d = mem_alloc(sizeof *d);
    if (!d) return E_OUTOFMEMORY;
    d->self.lpVtbl = &doc_vtbl;
    d->self.doc = d;
    d->refs = 1;
    d->version = version;
    d->free_threaded = free_threaded;
    d->async = TRUE;
    d->validate = TRUE;
    d->resolve = version < 6;                            /* MSXML 6 is safe by default */
    d->prohibit_dtd = version >= 6;
    d->xslpattern = version < 4;                         /* MSXML 3's default (NovaOS evaluates both as XPath) */
    d->x = xmlNewDoc(BAD_CAST "1.0");
    if (!d->x) { mem_free(d); return E_OUTOFMEMORY; }
    InterlockedIncrement(&g_objects);
    HRESULT hr = nd_qi(&d->self, riid, out);
    nd_release(&d->self);
    return hr;
}
