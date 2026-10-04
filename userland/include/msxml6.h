/* msxml6.h — MSXML 6.0 (and the MSXML 3.0 classes msxml6.dll also
 * answers on NovaOS): the XML DOM, SAX2 and XMLHTTP interfaces, CLSIDs
 * and IIDs (NovaOS).  C only: each interface lists its inherited methods
 * in order, as Windows' vtables have them. */
#pragma once
#include <objbase.h>
#include <oleauto.h>

/* ---- classes ---- */
DEFINE_GUID(CLSID_DOMDocument,                0x2933BF90, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(CLSID_DOMFreeThreadedDocument,    0x2933BF91, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(CLSID_DOMDocument2,               0xF6D90F11, 0x9C73, 0x11D3, 0xB3, 0x2E, 0x00, 0xC0, 0x4F, 0x99, 0x0B, 0xB4); /* Msxml2.DOMDocument */
DEFINE_GUID(CLSID_FreeThreadedDOMDocument,    0xF6D90F12, 0x9C73, 0x11D3, 0xB3, 0x2E, 0x00, 0xC0, 0x4F, 0x99, 0x0B, 0xB4);
DEFINE_GUID(CLSID_DOMDocument26,              0xF5078F1B, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_GUID(CLSID_FreeThreadedDOMDocument26,  0xF5078F1C, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_GUID(CLSID_DOMDocument30,              0xF5078F32, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_GUID(CLSID_FreeThreadedDOMDocument30,  0xF5078F33, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_GUID(CLSID_DOMDocument60,              0x88D96A05, 0xF192, 0x11D4, 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5);
DEFINE_GUID(CLSID_FreeThreadedDOMDocument60,  0x88D96A06, 0xF192, 0x11D4, 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5);
DEFINE_GUID(CLSID_XMLHTTPRequest,             0xED8C108E, 0x4349, 0x11D2, 0x91, 0xA4, 0x00, 0xC0, 0x4F, 0x79, 0x69, 0xE8); /* Microsoft.XMLHTTP */
DEFINE_GUID(CLSID_XMLHTTP,                    0xF6D90F16, 0x9C73, 0x11D3, 0xB3, 0x2E, 0x00, 0xC0, 0x4F, 0x99, 0x0B, 0xB4);
DEFINE_GUID(CLSID_XMLHTTP26,                  0xF5078F1E, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_GUID(CLSID_XMLHTTP30,                  0xF5078F35, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_GUID(CLSID_XMLHTTP60,                  0x88D96A0A, 0xF192, 0x11D4, 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5);
DEFINE_GUID(CLSID_FreeThreadedXMLHTTP60,      0x88D96A09, 0xF192, 0x11D4, 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5);
DEFINE_GUID(CLSID_ServerXMLHTTP,              0xAFBA6B42, 0x5692, 0x48EA, 0x81, 0x41, 0xDC, 0x51, 0x7D, 0xCF, 0x0E, 0xF1);
DEFINE_GUID(CLSID_ServerXMLHTTP30,            0xAFB40FFD, 0xB609, 0x40A3, 0x98, 0x28, 0xF8, 0x8B, 0xBE, 0x11, 0xE4, 0xE3);
DEFINE_GUID(CLSID_ServerXMLHTTP60,            0x88D96A0B, 0xF192, 0x11D4, 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5);
DEFINE_GUID(CLSID_SAXXMLReader,               0x079AA557, 0x4A18, 0x424A, 0x8E, 0xEE, 0xE3, 0x9F, 0x0A, 0x8D, 0x41, 0xB9);
DEFINE_GUID(CLSID_SAXXMLReader30,             0x3124C396, 0xFB13, 0x4836, 0xA6, 0xAD, 0x13, 0x17, 0xF1, 0x71, 0x36, 0x88);
DEFINE_GUID(CLSID_SAXXMLReader60,             0x88D96A0C, 0xF192, 0x11D4, 0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5);

/* ---- interfaces ---- */
DEFINE_GUID(IID_IXMLDOMNode,                  0x2933BF80, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMDocument,              0x2933BF81, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMNodeList,              0x2933BF82, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMNamedNodeMap,          0x2933BF83, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMCharacterData,         0x2933BF84, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMAttribute,             0x2933BF85, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMElement,               0x2933BF86, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMText,                  0x2933BF87, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMComment,               0x2933BF88, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMProcessingInstruction, 0x2933BF89, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMCDATASection,          0x2933BF8A, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMDocumentType,          0x2933BF8B, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMEntityReference,       0x2933BF8E, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMImplementation,        0x2933BF8F, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMDocument2,             0x2933BF95, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMDocument3,             0x2933BF96, 0x7B36, 0x11D2, 0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60);
DEFINE_GUID(IID_IXMLDOMDocumentFragment,      0x3EFAA413, 0x272F, 0x11D2, 0x83, 0x6F, 0x00, 0x00, 0xF8, 0x7A, 0x77, 0x82);
DEFINE_GUID(IID_IXMLDOMParseError,            0x3EFAA426, 0x272F, 0x11D2, 0x83, 0x6F, 0x00, 0x00, 0xF8, 0x7A, 0x77, 0x82);
DEFINE_GUID(IID_IXMLDOMSelection,             0xAA634FC7, 0x5888, 0x44A7, 0xA2, 0x57, 0x3A, 0x47, 0x15, 0x0D, 0x3A, 0x0E);
DEFINE_GUID(IID_IXMLHTTPRequest,              0xED8C108D, 0x4349, 0x11D2, 0x91, 0xA4, 0x00, 0xC0, 0x4F, 0x79, 0x69, 0xE8);
DEFINE_GUID(IID_IServerXMLHTTPRequest,        0x2E9196BF, 0x13BA, 0x4DD4, 0x91, 0xCA, 0x6C, 0x57, 0x1F, 0x28, 0x14, 0x95);
DEFINE_GUID(IID_ISAXXMLReader,                0xA4F96ED0, 0xF829, 0x476E, 0x81, 0xC0, 0xCD, 0xC7, 0xBD, 0x2A, 0x08, 0x02);
DEFINE_GUID(IID_ISAXContentHandler,           0x1545CDFA, 0x9E4E, 0x4497, 0xA8, 0xA4, 0x2B, 0xF7, 0xD0, 0x11, 0x2C, 0x44);
DEFINE_GUID(IID_ISAXErrorHandler,             0xA60511C4, 0xCCF5, 0x479E, 0x98, 0xA3, 0xDC, 0x8D, 0xC5, 0x45, 0xB7, 0xD0);
DEFINE_GUID(IID_ISAXLocator,                  0x9B7E472A, 0x0DE4, 0x4640, 0xBF, 0xF3, 0x84, 0xD3, 0x8A, 0x05, 0x1C, 0x31);
DEFINE_GUID(IID_ISAXAttributes,               0xF078ABE1, 0x45D2, 0x4832, 0x91, 0xEA, 0x44, 0x66, 0xCE, 0x2F, 0x25, 0xC9);

typedef enum tagDOMNodeType {
    NODE_INVALID, NODE_ELEMENT, NODE_ATTRIBUTE, NODE_TEXT, NODE_CDATA_SECTION, NODE_ENTITY_REFERENCE,
    NODE_ENTITY, NODE_PROCESSING_INSTRUCTION, NODE_COMMENT, NODE_DOCUMENT, NODE_DOCUMENT_TYPE,
    NODE_DOCUMENT_FRAGMENT, NODE_NOTATION
} DOMNodeType;

typedef interface IXMLDOMNode IXMLDOMNode;
typedef interface IXMLDOMNodeList IXMLDOMNodeList;
typedef interface IXMLDOMNamedNodeMap IXMLDOMNamedNodeMap;
typedef interface IXMLDOMDocument IXMLDOMDocument;
typedef interface IXMLDOMDocumentType IXMLDOMDocumentType;
typedef interface IXMLDOMImplementation IXMLDOMImplementation;
typedef interface IXMLDOMElement IXMLDOMElement;
typedef interface IXMLDOMAttribute IXMLDOMAttribute;
typedef interface IXMLDOMDocumentFragment IXMLDOMDocumentFragment;
typedef interface IXMLDOMText IXMLDOMText;
typedef interface IXMLDOMComment IXMLDOMComment;
typedef interface IXMLDOMCDATASection IXMLDOMCDATASection;
typedef interface IXMLDOMProcessingInstruction IXMLDOMProcessingInstruction;
typedef interface IXMLDOMEntityReference IXMLDOMEntityReference;
typedef interface IXMLDOMParseError IXMLDOMParseError;
typedef interface IXMLDOMSchemaCollection IXMLDOMSchemaCollection;
typedef interface ISAXContentHandler ISAXContentHandler;
typedef interface ISAXErrorHandler ISAXErrorHandler;
typedef interface ISAXLocator ISAXLocator;
typedef interface ISAXAttributes ISAXAttributes;

/* the inherited method blocks (INTERFACE is the interface being declared) */
#define MSXML_IUNKNOWN \
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE; \
    STDMETHOD_(ULONG, AddRef)(THIS) PURE; \
    STDMETHOD_(ULONG, Release)(THIS) PURE;
#define MSXML_IDISPATCH MSXML_IUNKNOWN \
    STDMETHOD(GetTypeInfoCount)(THIS_ UINT *n) PURE; \
    STDMETHOD(GetTypeInfo)(THIS_ UINT i, LCID lcid, ITypeInfo **out) PURE; \
    STDMETHOD(GetIDsOfNames)(THIS_ REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids) PURE; \
    STDMETHOD(Invoke)(THIS_ DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, \
                      VARIANT *result, EXCEPINFO *ei, UINT *argerr) PURE;
#define MSXML_IXMLDOMNODE MSXML_IDISPATCH \
    STDMETHOD(get_nodeName)(THIS_ BSTR *name) PURE; \
    STDMETHOD(get_nodeValue)(THIS_ VARIANT *value) PURE; \
    STDMETHOD(put_nodeValue)(THIS_ VARIANT value) PURE; \
    STDMETHOD(get_nodeType)(THIS_ DOMNodeType *type) PURE; \
    STDMETHOD(get_parentNode)(THIS_ IXMLDOMNode **parent) PURE; \
    STDMETHOD(get_childNodes)(THIS_ IXMLDOMNodeList **list) PURE; \
    STDMETHOD(get_firstChild)(THIS_ IXMLDOMNode **child) PURE; \
    STDMETHOD(get_lastChild)(THIS_ IXMLDOMNode **child) PURE; \
    STDMETHOD(get_previousSibling)(THIS_ IXMLDOMNode **node) PURE; \
    STDMETHOD(get_nextSibling)(THIS_ IXMLDOMNode **node) PURE; \
    STDMETHOD(get_attributes)(THIS_ IXMLDOMNamedNodeMap **map) PURE; \
    STDMETHOD(insertBefore)(THIS_ IXMLDOMNode *child, VARIANT ref, IXMLDOMNode **out) PURE; \
    STDMETHOD(replaceChild)(THIS_ IXMLDOMNode *child, IXMLDOMNode *old, IXMLDOMNode **out) PURE; \
    STDMETHOD(removeChild)(THIS_ IXMLDOMNode *child, IXMLDOMNode **out) PURE; \
    STDMETHOD(appendChild)(THIS_ IXMLDOMNode *child, IXMLDOMNode **out) PURE; \
    STDMETHOD(hasChildNodes)(THIS_ VARIANT_BOOL *has) PURE; \
    STDMETHOD(get_ownerDocument)(THIS_ IXMLDOMDocument **doc) PURE; \
    STDMETHOD(cloneNode)(THIS_ VARIANT_BOOL deep, IXMLDOMNode **out) PURE; \
    STDMETHOD(get_nodeTypeString)(THIS_ BSTR *s) PURE; \
    STDMETHOD(get_text)(THIS_ BSTR *text) PURE; \
    STDMETHOD(put_text)(THIS_ BSTR text) PURE; \
    STDMETHOD(get_specified)(THIS_ VARIANT_BOOL *b) PURE; \
    STDMETHOD(get_definition)(THIS_ IXMLDOMNode **node) PURE; \
    STDMETHOD(get_nodeTypedValue)(THIS_ VARIANT *v) PURE; \
    STDMETHOD(put_nodeTypedValue)(THIS_ VARIANT v) PURE; \
    STDMETHOD(get_dataType)(THIS_ VARIANT *v) PURE; \
    STDMETHOD(put_dataType)(THIS_ BSTR type) PURE; \
    STDMETHOD(get_xml)(THIS_ BSTR *xml) PURE; \
    STDMETHOD(transformNode)(THIS_ IXMLDOMNode *style, BSTR *out) PURE; \
    STDMETHOD(selectNodes)(THIS_ BSTR query, IXMLDOMNodeList **list) PURE; \
    STDMETHOD(selectSingleNode)(THIS_ BSTR query, IXMLDOMNode **node) PURE; \
    STDMETHOD(get_parsed)(THIS_ VARIANT_BOOL *b) PURE; \
    STDMETHOD(get_namespaceURI)(THIS_ BSTR *uri) PURE; \
    STDMETHOD(get_prefix)(THIS_ BSTR *prefix) PURE; \
    STDMETHOD(get_baseName)(THIS_ BSTR *name) PURE; \
    STDMETHOD(transformNodeToObject)(THIS_ IXMLDOMNode *style, VARIANT out) PURE;
#define MSXML_IXMLDOMDOCUMENT MSXML_IXMLDOMNODE \
    STDMETHOD(get_doctype)(THIS_ IXMLDOMDocumentType **dt) PURE; \
    STDMETHOD(get_implementation)(THIS_ IXMLDOMImplementation **impl) PURE; \
    STDMETHOD(get_documentElement)(THIS_ IXMLDOMElement **el) PURE; \
    STDMETHOD(putref_documentElement)(THIS_ IXMLDOMElement *el) PURE; \
    STDMETHOD(createElement)(THIS_ BSTR name, IXMLDOMElement **el) PURE; \
    STDMETHOD(createDocumentFragment)(THIS_ IXMLDOMDocumentFragment **frag) PURE; \
    STDMETHOD(createTextNode)(THIS_ BSTR data, IXMLDOMText **text) PURE; \
    STDMETHOD(createComment)(THIS_ BSTR data, IXMLDOMComment **comment) PURE; \
    STDMETHOD(createCDATASection)(THIS_ BSTR data, IXMLDOMCDATASection **cdata) PURE; \
    STDMETHOD(createProcessingInstruction)(THIS_ BSTR target, BSTR data, IXMLDOMProcessingInstruction **pi) PURE; \
    STDMETHOD(createAttribute)(THIS_ BSTR name, IXMLDOMAttribute **attr) PURE; \
    STDMETHOD(createEntityReference)(THIS_ BSTR name, IXMLDOMEntityReference **ref) PURE; \
    STDMETHOD(getElementsByTagName)(THIS_ BSTR name, IXMLDOMNodeList **list) PURE; \
    STDMETHOD(createNode)(THIS_ VARIANT type, BSTR name, BSTR uri, IXMLDOMNode **node) PURE; \
    STDMETHOD(nodeFromID)(THIS_ BSTR id, IXMLDOMNode **node) PURE; \
    STDMETHOD(load)(THIS_ VARIANT src, VARIANT_BOOL *ok) PURE; \
    STDMETHOD(get_readyState)(THIS_ LONG *state) PURE; \
    STDMETHOD(get_parseError)(THIS_ IXMLDOMParseError **err) PURE; \
    STDMETHOD(get_url)(THIS_ BSTR *url) PURE; \
    STDMETHOD(get_async)(THIS_ VARIANT_BOOL *async) PURE; \
    STDMETHOD(put_async)(THIS_ VARIANT_BOOL async) PURE; \
    STDMETHOD(abort)(THIS) PURE; \
    STDMETHOD(loadXML)(THIS_ BSTR xml, VARIANT_BOOL *ok) PURE; \
    STDMETHOD(save)(THIS_ VARIANT dest) PURE; \
    STDMETHOD(get_validateOnParse)(THIS_ VARIANT_BOOL *b) PURE; \
    STDMETHOD(put_validateOnParse)(THIS_ VARIANT_BOOL b) PURE; \
    STDMETHOD(get_resolveExternals)(THIS_ VARIANT_BOOL *b) PURE; \
    STDMETHOD(put_resolveExternals)(THIS_ VARIANT_BOOL b) PURE; \
    STDMETHOD(get_preserveWhiteSpace)(THIS_ VARIANT_BOOL *b) PURE; \
    STDMETHOD(put_preserveWhiteSpace)(THIS_ VARIANT_BOOL b) PURE; \
    STDMETHOD(put_onreadystatechange)(THIS_ VARIANT v) PURE; \
    STDMETHOD(put_ondataavailable)(THIS_ VARIANT v) PURE; \
    STDMETHOD(put_ontransformnode)(THIS_ VARIANT v) PURE;
#define MSXML_IXMLDOMDOCUMENT2 MSXML_IXMLDOMDOCUMENT \
    STDMETHOD(get_namespaces)(THIS_ IXMLDOMSchemaCollection **ns) PURE; \
    STDMETHOD(get_schemas)(THIS_ VARIANT *v) PURE; \
    STDMETHOD(putref_schemas)(THIS_ VARIANT v) PURE; \
    STDMETHOD(validate)(THIS_ IXMLDOMParseError **err) PURE; \
    STDMETHOD(setProperty)(THIS_ BSTR name, VARIANT value) PURE; \
    STDMETHOD(getProperty)(THIS_ BSTR name, VARIANT *value) PURE;
#define MSXML_IXMLDOMNODELIST MSXML_IDISPATCH \
    STDMETHOD(get_item)(THIS_ LONG index, IXMLDOMNode **node) PURE; \
    STDMETHOD(get_length)(THIS_ LONG *n) PURE; \
    STDMETHOD(nextNode)(THIS_ IXMLDOMNode **node) PURE; \
    STDMETHOD(reset)(THIS) PURE; \
    STDMETHOD(get__newEnum)(THIS_ IUnknown **e) PURE;
#define MSXML_IXMLDOMCHARACTERDATA MSXML_IXMLDOMNODE \
    STDMETHOD(get_data)(THIS_ BSTR *data) PURE; \
    STDMETHOD(put_data)(THIS_ BSTR data) PURE; \
    STDMETHOD(get_length)(THIS_ LONG *n) PURE; \
    STDMETHOD(substringData)(THIS_ LONG offset, LONG count, BSTR *data) PURE; \
    STDMETHOD(appendData)(THIS_ BSTR data) PURE; \
    STDMETHOD(insertData)(THIS_ LONG offset, BSTR data) PURE; \
    STDMETHOD(deleteData)(THIS_ LONG offset, LONG count) PURE; \
    STDMETHOD(replaceData)(THIS_ LONG offset, LONG count, BSTR data) PURE;
#define MSXML_IXMLHTTPREQUEST MSXML_IDISPATCH \
    STDMETHOD(open)(THIS_ BSTR method, BSTR url, VARIANT async, VARIANT user, VARIANT password) PURE; \
    STDMETHOD(setRequestHeader)(THIS_ BSTR header, BSTR value) PURE; \
    STDMETHOD(getResponseHeader)(THIS_ BSTR header, BSTR *value) PURE; \
    STDMETHOD(getAllResponseHeaders)(THIS_ BSTR *headers) PURE; \
    STDMETHOD(send)(THIS_ VARIANT body) PURE; \
    STDMETHOD(abort)(THIS) PURE; \
    STDMETHOD(get_status)(THIS_ LONG *status) PURE; \
    STDMETHOD(get_statusText)(THIS_ BSTR *text) PURE; \
    STDMETHOD(get_responseXML)(THIS_ IDispatch **doc) PURE; \
    STDMETHOD(get_responseText)(THIS_ BSTR *text) PURE; \
    STDMETHOD(get_responseBody)(THIS_ VARIANT *body) PURE; \
    STDMETHOD(get_responseStream)(THIS_ VARIANT *body) PURE; \
    STDMETHOD(get_readyState)(THIS_ LONG *state) PURE; \
    STDMETHOD(put_onreadystatechange)(THIS_ IDispatch *handler) PURE;

#undef INTERFACE
#define INTERFACE IXMLDOMNode
DECLARE_INTERFACE_(IXMLDOMNode, IDispatch) { BEGIN_INTERFACE MSXML_IXMLDOMNODE END_INTERFACE };
#undef INTERFACE
#define INTERFACE IXMLDOMDocument
DECLARE_INTERFACE_(IXMLDOMDocument, IXMLDOMNode) { BEGIN_INTERFACE MSXML_IXMLDOMDOCUMENT END_INTERFACE };
#undef INTERFACE
#define INTERFACE IXMLDOMDocument2
DECLARE_INTERFACE_(IXMLDOMDocument2, IXMLDOMDocument) { BEGIN_INTERFACE MSXML_IXMLDOMDOCUMENT2 END_INTERFACE };
#undef INTERFACE
#define INTERFACE IXMLDOMDocument3
DECLARE_INTERFACE_(IXMLDOMDocument3, IXMLDOMDocument2)
{
    BEGIN_INTERFACE
    MSXML_IXMLDOMDOCUMENT2
    STDMETHOD(validateNode)(THIS_ IXMLDOMNode *node, IXMLDOMParseError **err) PURE;
    STDMETHOD(importNode)(THIS_ IXMLDOMNode *node, VARIANT_BOOL deep, IXMLDOMNode **out) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMNodeList
DECLARE_INTERFACE_(IXMLDOMNodeList, IDispatch) { BEGIN_INTERFACE MSXML_IXMLDOMNODELIST END_INTERFACE };
#undef INTERFACE
#define INTERFACE IXMLDOMSelection
DECLARE_INTERFACE_(IXMLDOMSelection, IXMLDOMNodeList)
{
    BEGIN_INTERFACE
    MSXML_IXMLDOMNODELIST
    STDMETHOD(get_expr)(THIS_ BSTR *expr) PURE;
    STDMETHOD(put_expr)(THIS_ BSTR expr) PURE;
    STDMETHOD(get_context)(THIS_ IXMLDOMNode **node) PURE;
    STDMETHOD(putref_context)(THIS_ IXMLDOMNode *node) PURE;
    STDMETHOD(peekNode)(THIS_ IXMLDOMNode **node) PURE;
    STDMETHOD(matches)(THIS_ IXMLDOMNode *node, IXMLDOMNode **out) PURE;
    STDMETHOD(removeNext)(THIS_ IXMLDOMNode **node) PURE;
    STDMETHOD(removeAll)(THIS) PURE;
    STDMETHOD(clone)(THIS_ IXMLDOMSelection **out) PURE;
    STDMETHOD(getProperty)(THIS_ BSTR name, VARIANT *value) PURE;
    STDMETHOD(setProperty)(THIS_ BSTR name, VARIANT value) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMNamedNodeMap
DECLARE_INTERFACE_(IXMLDOMNamedNodeMap, IDispatch)
{
    BEGIN_INTERFACE
    MSXML_IDISPATCH
    STDMETHOD(getNamedItem)(THIS_ BSTR name, IXMLDOMNode **node) PURE;
    STDMETHOD(setNamedItem)(THIS_ IXMLDOMNode *node, IXMLDOMNode **out) PURE;
    STDMETHOD(removeNamedItem)(THIS_ BSTR name, IXMLDOMNode **out) PURE;
    STDMETHOD(get_item)(THIS_ LONG index, IXMLDOMNode **node) PURE;
    STDMETHOD(get_length)(THIS_ LONG *n) PURE;
    STDMETHOD(getQualifiedItem)(THIS_ BSTR name, BSTR uri, IXMLDOMNode **node) PURE;
    STDMETHOD(removeQualifiedItem)(THIS_ BSTR name, BSTR uri, IXMLDOMNode **out) PURE;
    STDMETHOD(nextNode)(THIS_ IXMLDOMNode **node) PURE;
    STDMETHOD(reset)(THIS) PURE;
    STDMETHOD(get__newEnum)(THIS_ IUnknown **e) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMParseError
DECLARE_INTERFACE_(IXMLDOMParseError, IDispatch)
{
    BEGIN_INTERFACE
    MSXML_IDISPATCH
    STDMETHOD(get_errorCode)(THIS_ LONG *code) PURE;
    STDMETHOD(get_url)(THIS_ BSTR *url) PURE;
    STDMETHOD(get_reason)(THIS_ BSTR *reason) PURE;
    STDMETHOD(get_srcText)(THIS_ BSTR *text) PURE;
    STDMETHOD(get_line)(THIS_ LONG *line) PURE;
    STDMETHOD(get_linepos)(THIS_ LONG *pos) PURE;
    STDMETHOD(get_filepos)(THIS_ LONG *pos) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMElement
DECLARE_INTERFACE_(IXMLDOMElement, IXMLDOMNode)
{
    BEGIN_INTERFACE
    MSXML_IXMLDOMNODE
    STDMETHOD(get_tagName)(THIS_ BSTR *name) PURE;
    STDMETHOD(getAttribute)(THIS_ BSTR name, VARIANT *value) PURE;
    STDMETHOD(setAttribute)(THIS_ BSTR name, VARIANT value) PURE;
    STDMETHOD(removeAttribute)(THIS_ BSTR name) PURE;
    STDMETHOD(getAttributeNode)(THIS_ BSTR name, IXMLDOMAttribute **attr) PURE;
    STDMETHOD(setAttributeNode)(THIS_ IXMLDOMAttribute *attr, IXMLDOMAttribute **old) PURE;
    STDMETHOD(removeAttributeNode)(THIS_ IXMLDOMAttribute *attr, IXMLDOMAttribute **out) PURE;
    STDMETHOD(getElementsByTagName)(THIS_ BSTR name, IXMLDOMNodeList **list) PURE;
    STDMETHOD(normalize)(THIS) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMAttribute
DECLARE_INTERFACE_(IXMLDOMAttribute, IXMLDOMNode)
{
    BEGIN_INTERFACE
    MSXML_IXMLDOMNODE
    STDMETHOD(get_name)(THIS_ BSTR *name) PURE;
    STDMETHOD(get_value)(THIS_ VARIANT *value) PURE;
    STDMETHOD(put_value)(THIS_ VARIANT value) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMCharacterData
DECLARE_INTERFACE_(IXMLDOMCharacterData, IXMLDOMNode) { BEGIN_INTERFACE MSXML_IXMLDOMCHARACTERDATA END_INTERFACE };
#undef INTERFACE
#define INTERFACE IXMLDOMComment
DECLARE_INTERFACE_(IXMLDOMComment, IXMLDOMCharacterData) { BEGIN_INTERFACE MSXML_IXMLDOMCHARACTERDATA END_INTERFACE };
#undef INTERFACE
#define INTERFACE IXMLDOMText
DECLARE_INTERFACE_(IXMLDOMText, IXMLDOMCharacterData)
{
    BEGIN_INTERFACE
    MSXML_IXMLDOMCHARACTERDATA
    STDMETHOD(splitText)(THIS_ LONG offset, IXMLDOMText **out) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMCDATASection
DECLARE_INTERFACE_(IXMLDOMCDATASection, IXMLDOMText)
{
    BEGIN_INTERFACE
    MSXML_IXMLDOMCHARACTERDATA
    STDMETHOD(splitText)(THIS_ LONG offset, IXMLDOMText **out) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMProcessingInstruction
DECLARE_INTERFACE_(IXMLDOMProcessingInstruction, IXMLDOMNode)
{
    BEGIN_INTERFACE
    MSXML_IXMLDOMNODE
    STDMETHOD(get_target)(THIS_ BSTR *target) PURE;
    STDMETHOD(get_data)(THIS_ BSTR *data) PURE;
    STDMETHOD(put_data)(THIS_ BSTR data) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMDocumentFragment
DECLARE_INTERFACE_(IXMLDOMDocumentFragment, IXMLDOMNode) { BEGIN_INTERFACE MSXML_IXMLDOMNODE END_INTERFACE };
#undef INTERFACE
#define INTERFACE IXMLDOMEntityReference
DECLARE_INTERFACE_(IXMLDOMEntityReference, IXMLDOMNode) { BEGIN_INTERFACE MSXML_IXMLDOMNODE END_INTERFACE };
#undef INTERFACE
#define INTERFACE IXMLDOMDocumentType
DECLARE_INTERFACE_(IXMLDOMDocumentType, IXMLDOMNode)
{
    BEGIN_INTERFACE
    MSXML_IXMLDOMNODE
    STDMETHOD(get_name)(THIS_ BSTR *name) PURE;
    STDMETHOD(get_entities)(THIS_ IXMLDOMNamedNodeMap **map) PURE;
    STDMETHOD(get_notations)(THIS_ IXMLDOMNamedNodeMap **map) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLDOMImplementation
DECLARE_INTERFACE_(IXMLDOMImplementation, IDispatch)
{
    BEGIN_INTERFACE
    MSXML_IDISPATCH
    STDMETHOD(hasFeature)(THIS_ BSTR feature, BSTR version, VARIANT_BOOL *has) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE IXMLHTTPRequest
DECLARE_INTERFACE_(IXMLHTTPRequest, IDispatch) { BEGIN_INTERFACE MSXML_IXMLHTTPREQUEST END_INTERFACE };
#undef INTERFACE
#define INTERFACE IServerXMLHTTPRequest
DECLARE_INTERFACE_(IServerXMLHTTPRequest, IXMLHTTPRequest)
{
    BEGIN_INTERFACE
    MSXML_IXMLHTTPREQUEST
    STDMETHOD(setTimeouts)(THIS_ LONG resolve, LONG connect, LONG send, LONG receive) PURE;
    STDMETHOD(waitForResponse)(THIS_ VARIANT timeout, VARIANT_BOOL *done) PURE;
    STDMETHOD(getOption)(THIS_ int option, VARIANT *value) PURE;
    STDMETHOD(setOption)(THIS_ int option, VARIANT value) PURE;
    END_INTERFACE
};

/* ---- SAX2 (strings are counted: pointer and length) ---- */
#undef INTERFACE
#define INTERFACE ISAXXMLReader
DECLARE_INTERFACE_(ISAXXMLReader, IUnknown)
{
    BEGIN_INTERFACE
    MSXML_IUNKNOWN
    STDMETHOD(getFeature)(THIS_ const WCHAR *name, VARIANT_BOOL *value) PURE;
    STDMETHOD(putFeature)(THIS_ const WCHAR *name, VARIANT_BOOL value) PURE;
    STDMETHOD(getProperty)(THIS_ const WCHAR *name, VARIANT *value) PURE;
    STDMETHOD(putProperty)(THIS_ const WCHAR *name, VARIANT value) PURE;
    STDMETHOD(getEntityResolver)(THIS_ IUnknown **resolver) PURE;
    STDMETHOD(putEntityResolver)(THIS_ IUnknown *resolver) PURE;
    STDMETHOD(getContentHandler)(THIS_ ISAXContentHandler **handler) PURE;
    STDMETHOD(putContentHandler)(THIS_ ISAXContentHandler *handler) PURE;
    STDMETHOD(getDTDHandler)(THIS_ IUnknown **handler) PURE;
    STDMETHOD(putDTDHandler)(THIS_ IUnknown *handler) PURE;
    STDMETHOD(getErrorHandler)(THIS_ ISAXErrorHandler **handler) PURE;
    STDMETHOD(putErrorHandler)(THIS_ ISAXErrorHandler *handler) PURE;
    STDMETHOD(getBaseURL)(THIS_ const WCHAR **url) PURE;
    STDMETHOD(putBaseURL)(THIS_ const WCHAR *url) PURE;
    STDMETHOD(getSecureBaseURL)(THIS_ const WCHAR **url) PURE;
    STDMETHOD(putSecureBaseURL)(THIS_ const WCHAR *url) PURE;
    STDMETHOD(parse)(THIS_ VARIANT input) PURE;
    STDMETHOD(parseURL)(THIS_ const WCHAR *url) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE ISAXContentHandler
DECLARE_INTERFACE_(ISAXContentHandler, IUnknown)
{
    BEGIN_INTERFACE
    MSXML_IUNKNOWN
    STDMETHOD(putDocumentLocator)(THIS_ ISAXLocator *locator) PURE;
    STDMETHOD(startDocument)(THIS) PURE;
    STDMETHOD(endDocument)(THIS) PURE;
    STDMETHOD(startPrefixMapping)(THIS_ const WCHAR *prefix, int nprefix, const WCHAR *uri, int nuri) PURE;
    STDMETHOD(endPrefixMapping)(THIS_ const WCHAR *prefix, int nprefix) PURE;
    STDMETHOD(startElement)(THIS_ const WCHAR *uri, int nuri, const WCHAR *local, int nlocal,
                            const WCHAR *qname, int nqname, ISAXAttributes *attrs) PURE;
    STDMETHOD(endElement)(THIS_ const WCHAR *uri, int nuri, const WCHAR *local, int nlocal,
                          const WCHAR *qname, int nqname) PURE;
    STDMETHOD(characters)(THIS_ const WCHAR *chars, int n) PURE;
    STDMETHOD(ignorableWhitespace)(THIS_ const WCHAR *chars, int n) PURE;
    STDMETHOD(processingInstruction)(THIS_ const WCHAR *target, int ntarget, const WCHAR *data, int ndata) PURE;
    STDMETHOD(skippedEntity)(THIS_ const WCHAR *name, int nname) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE ISAXErrorHandler
DECLARE_INTERFACE_(ISAXErrorHandler, IUnknown)
{
    BEGIN_INTERFACE
    MSXML_IUNKNOWN
    STDMETHOD(error)(THIS_ ISAXLocator *locator, const WCHAR *message, HRESULT code) PURE;
    STDMETHOD(fatalError)(THIS_ ISAXLocator *locator, const WCHAR *message, HRESULT code) PURE;
    STDMETHOD(ignorableWarning)(THIS_ ISAXLocator *locator, const WCHAR *message, HRESULT code) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE ISAXLocator
DECLARE_INTERFACE_(ISAXLocator, IUnknown)
{
    BEGIN_INTERFACE
    MSXML_IUNKNOWN
    STDMETHOD(getColumnNumber)(THIS_ int *column) PURE;
    STDMETHOD(getLineNumber)(THIS_ int *line) PURE;
    STDMETHOD(getPublicId)(THIS_ const WCHAR **id) PURE;
    STDMETHOD(getSystemId)(THIS_ const WCHAR **id) PURE;
    END_INTERFACE
};
#undef INTERFACE
#define INTERFACE ISAXAttributes
DECLARE_INTERFACE_(ISAXAttributes, IUnknown)
{
    BEGIN_INTERFACE
    MSXML_IUNKNOWN
    STDMETHOD(getLength)(THIS_ int *n) PURE;
    STDMETHOD(getURI)(THIS_ int i, const WCHAR **uri, int *n) PURE;
    STDMETHOD(getLocalName)(THIS_ int i, const WCHAR **name, int *n) PURE;
    STDMETHOD(getQName)(THIS_ int i, const WCHAR **name, int *n) PURE;
    STDMETHOD(getName)(THIS_ int i, const WCHAR **uri, int *nuri, const WCHAR **local, int *nlocal,
                       const WCHAR **qname, int *nqname) PURE;
    STDMETHOD(getIndexFromName)(THIS_ const WCHAR *uri, int nuri, const WCHAR *local, int nlocal, int *i) PURE;
    STDMETHOD(getIndexFromQName)(THIS_ const WCHAR *qname, int nqname, int *i) PURE;
    STDMETHOD(getType)(THIS_ int i, const WCHAR **type, int *n) PURE;
    STDMETHOD(getTypeFromName)(THIS_ const WCHAR *uri, int nuri, const WCHAR *local, int nlocal,
                               const WCHAR **type, int *n) PURE;
    STDMETHOD(getTypeFromQName)(THIS_ const WCHAR *qname, int nqname, const WCHAR **type, int *n) PURE;
    STDMETHOD(getValue)(THIS_ int i, const WCHAR **value, int *n) PURE;
    STDMETHOD(getValueFromName)(THIS_ const WCHAR *uri, int nuri, const WCHAR *local, int nlocal,
                                const WCHAR **value, int *n) PURE;
    STDMETHOD(getValueFromQName)(THIS_ const WCHAR *qname, int nqname, const WCHAR **value, int *n) PURE;
    END_INTERFACE
};
#undef INTERFACE
