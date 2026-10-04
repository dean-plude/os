/* msxml6.dll's shared declarations: the document state every node
 * wrapper points at, the node wrappers, and the string helpers */
#pragma once
#include <windows.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <objbase.h>
#include <oleauto.h>
#include <msxml6.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlsave.h>
#include <libxml/parserInternals.h>

#define XML_E_MISSINGROOT  ((HRESULT)0xC00CE558L)   /* "XML document must have a top level element." */
#define XML_E_BADXML       ((HRESULT)0xC00CE550L)   /* (other well-formedness errors) */
#ifndef INET_E_RESOURCE_NOT_FOUND
#define INET_E_RESOURCE_NOT_FOUND ((HRESULT)0x800C0005L)
#endif

/* one xmlNs a namespace declaration ("xmlns:p") is shown through, as an attribute node */
typedef struct NsAttr { struct NsAttr *next; xmlNsPtr ns; xmlAttrPtr attr; } NsAttr;

typedef struct ParseErr {
    LONG code, line, linepos, filepos;
    BSTR reason, src, url;
} ParseErr;

typedef struct Doc Doc;

/* a COM object over one libxml2 node (or, embedded in Doc, over the document) */
typedef struct Node {
    const void *lpVtbl;
    LONG refs;
    Doc *doc;
    xmlNodePtr x;                 /* (the document's own Node: NULL, doc->x is used) */
} Node;

struct Doc {
    Node self;                    /* the DOMDocument object */
    LONG refs;                    /* the document object's references + one per live wrapper */
    xmlDocPtr x;
    int version;                  /* 3 (msxml3's classes) or 6 */
    BOOL async, validate, resolve, preserve, prohibit_dtd, xslpattern, free_threaded;
    LONG max_depth;
    xmlChar *sel_ns;              /* SelectionNamespaces, UTF-8 */
    BSTR url;
    ParseErr err;
    IDispatch *onready;
    xmlNodePtr *orphans;          /* nodes made or removed: freed with the document when still parentless */
    int norphans, corphans;
    xmlDocPtr *old;               /* trees replaced by load/loadXML (wrappers may still point into them) */
    int nold, cold;
    NsAttr *nsattrs;
};

/* main.c */
extern LONG g_objects;
void msxml_init(void);

/* text.c: strings */
BSTR bstr_utf8(const xmlChar *s);                       /* NULL -> "" */
BSTR bstr_utf8n(const xmlChar *s, int n);
xmlChar *utf8_wide(const WCHAR *s, int n);              /* n < 0: to the terminator; xmlFree it */
WCHAR *wide_utf8(const xmlChar *s, int n, int *outlen); /* HeapFree it */
HRESULT variant_string(const VARIANT *v, BSTR *out);    /* any VARIANT -> BSTR (caller frees) */
HRESULT ret_bstr(BSTR *out, const xmlChar *s);
void encodings_init(void);
HRESULT read_url(const WCHAR *url, char **data, DWORD *len);   /* a path, file:// or http(s):// URL */
HRESULT read_stream(IUnknown *unk, char **data, DWORD *len);  /* IStream / ISequentialStream */
HRESULT http_get(const WCHAR *url, char **data, DWORD *len);  /* httpreq.c */

/* dom.c */
HRESULT doc_create(int version, BOOL free_threaded, REFIID riid, void **out);
HRESULT node_wrap(Doc *doc, xmlNodePtr x, REFIID riid, void **out);   /* x NULL: *out NULL, S_FALSE */
Node *node_from_iface(void *iface);                     /* our node, or NULL */
xmlNodePtr node_x(Node *n);
void doc_addref(Doc *d);
void doc_release(Doc *d);
void doc_orphan(Doc *d, xmlNodePtr x);
HRESULT node_xml(Doc *d, xmlNodePtr x, BOOL decl_encoding, xmlChar **out); /* xmlFree it */
HRESULT doc_load_bytes(Doc *d, const char *data, int len, BOOL utf8);  /* parse into d; S_FALSE on a parse error */
xmlAttrPtr ns_attr(Doc *d, xmlNodePtr el, xmlNsPtr ns);

/* lists.c */
HRESULT list_children(Doc *d, xmlNodePtr parent, IXMLDOMNodeList **out);
HRESULT list_static(Doc *d, xmlNodePtr *items, int n, IXMLDOMNodeList **out); /* takes items (HeapAlloc'd) */
HRESULT attrmap_create(Doc *d, xmlNodePtr el, IXMLDOMNamedNodeMap **out);
HRESULT parseerr_create(const ParseErr *e, IXMLDOMParseError **out);
HRESULT impl_create(IXMLDOMImplementation **out);
HRESULT empty_map(IXMLDOMNamedNodeMap **out);

/* httpreq.c, saxreader.c */
HRESULT httpreq_create(BOOL server, REFIID riid, void **out);
HRESULT saxreader_create(int version, REFIID riid, void **out);

/* the IDispatch part every object has: no type information (C++ callers use the vtables) */
HRESULT STDMETHODCALLTYPE disp_count(void *This, UINT *n);
HRESULT STDMETHODCALLTYPE disp_info(void *This, UINT i, LCID lcid, ITypeInfo **out);
HRESULT STDMETHODCALLTYPE disp_ids(void *This, REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids);
HRESULT STDMETHODCALLTYPE disp_invoke(void *This, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *p,
                                      VARIANT *res, EXCEPINFO *ei, UINT *argerr);
#define DISPATCH_SLOTS \
    .GetTypeInfoCount = (void *)disp_count, .GetTypeInfo = (void *)disp_info, \
    .GetIDsOfNames = (void *)disp_ids, .Invoke = (void *)disp_invoke

void *mem_alloc(SIZE_T n);
void *mem_realloc(void *p, SIZE_T n);
void mem_free(void *p);
