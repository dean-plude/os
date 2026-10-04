/* msxmltest.exe — MSXML (msxml6.dll) as installers use it, 64- and 32-bit.
 *
 * The classes and ProgIDs are registered (Msxml2.DOMDocument[.3.0/.6.0],
 * Microsoft.XMLDOM, the free-threaded documents, XMLHTTP, SAXXMLReader);
 * DOMDocument60 loads a WiX Burn-style manifest and answers XPath with
 * SelectionNamespaces (selectNodes, selectSingleNode, predicates), the
 * nodes' names, namespaces, attributes and text; a document built node by
 * node (Omaha's request, createNode with a namespace) serializes as
 * MSXML writes it; parse errors are reported; save/load round-trip through
 * a file and a file:// URL; UTF-16 and windows-1252 bytes load; bin.base64
 * typed values decode; MSXML 3 and 6 defaults differ (SelectionLanguage,
 * ProhibitDTD); whitespace is dropped unless preserved; SAX2 reports
 * elements, attributes, characters, prefix mappings and fatal errors; and
 * msxml6.dll carries version 6.30. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <oleauto.h>
#include <msxml6.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

typedef struct {                       /* (version.dll's fixed block) */
    DWORD dwSignature, dwStrucVersion, dwFileVersionMS, dwFileVersionLS, dwProductVersionMS, dwProductVersionLS;
    DWORD dwFileFlagsMask, dwFileFlags, dwFileOS, dwFileType, dwFileSubtype, dwFileDateMS, dwFileDateLS;
} VS_FIXEDFILEINFO;
__declspec(dllimport) DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR name, LPDWORD handle);
__declspec(dllimport) BOOL WINAPI GetFileVersionInfoW(LPCWSTR name, DWORD handle, DWORD n, LPVOID data);
__declspec(dllimport) BOOL WINAPI VerQueryValueW(LPCVOID block, LPCWSTR sub, LPVOID *out, PUINT n);

static BOOL bstr_is(BSTR b, const WCHAR *s)
{
    BOOL r = b && !wcscmp(b, s);
    if (!r) printf("  got \"%ls\", want \"%ls\"\n", b ? b : L"(null)", s);
    return r;
}

static IXMLDOMDocument2 *new_doc(REFCLSID clsid)
{
    IXMLDOMDocument2 *d = 0;
    if (FAILED(CoCreateInstance(clsid, 0, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument2, (void **)&d))) return 0;
    d->lpVtbl->put_async(d, VARIANT_FALSE);
    return d;
}

static BOOL load_xml(IXMLDOMDocument2 *d, const WCHAR *xml)
{
    BSTR b = SysAllocString(xml);
    VARIANT_BOOL ok = VARIANT_FALSE;
    d->lpVtbl->loadXML(d, b, &ok);
    SysFreeString(b);
    return ok == VARIANT_TRUE;
}

static BSTR node_xml(void *n)
{
    BSTR b = 0;
    ((IXMLDOMNode *)n)->lpVtbl->get_xml((IXMLDOMNode *)n, &b);
    return b;
}

static void set_prop(IXMLDOMDocument2 *d, const WCHAR *name, const WCHAR *value)
{
    BSTR n = SysAllocString(name);
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(value);
    d->lpVtbl->setProperty(d, n, v);
    SysFreeString(n);
    SysFreeString(v.bstrVal);
}

static LONG count_nodes(void *ctx, const WCHAR *query)
{
    IXMLDOMNodeList *l = 0;
    BSTR q = SysAllocString(query);
    HRESULT hr = ((IXMLDOMNode *)ctx)->lpVtbl->selectNodes((IXMLDOMNode *)ctx, q, &l);
    SysFreeString(q);
    if (FAILED(hr) || !l) return -1;
    LONG n = -1;
    l->lpVtbl->get_length(l, &n);
    l->lpVtbl->Release(l);
    return n;
}

static BSTR attr_of(void *node, const WCHAR *name)
{
    IXMLDOMElement *el;
    if (FAILED(((IXMLDOMNode *)node)->lpVtbl->QueryInterface((IXMLDOMNode *)node, &IID_IXMLDOMElement, (void **)&el))) return 0;
    BSTR n = SysAllocString(name);
    VARIANT v;
    VariantInit(&v);
    el->lpVtbl->getAttribute(el, n, &v);
    SysFreeString(n);
    el->lpVtbl->Release(el);
    return v.vt == VT_BSTR ? v.bstrVal : 0;
}

static IXMLDOMNode *select1(void *ctx, const WCHAR *query)
{
    IXMLDOMNode *n = 0;
    BSTR q = SysAllocString(query);
    ((IXMLDOMNode *)ctx)->lpVtbl->selectSingleNode((IXMLDOMNode *)ctx, q, &n);
    SysFreeString(q);
    return n;
}

/* ---- registration ---- */
static void test_registry(void)
{
    CLSID c;
    CHECK("Msxml2.DOMDocument ProgID", SUCCEEDED(CLSIDFromProgID(L"Msxml2.DOMDocument", &c)) && IsEqualCLSID(&c, &CLSID_DOMDocument2));
    CHECK("Msxml2.DOMDocument.6.0 ProgID", SUCCEEDED(CLSIDFromProgID(L"Msxml2.DOMDocument.6.0", &c)) && IsEqualCLSID(&c, &CLSID_DOMDocument60));
    CHECK("Msxml2.DOMDocument.3.0 ProgID", SUCCEEDED(CLSIDFromProgID(L"Msxml2.DOMDocument.3.0", &c)) && IsEqualCLSID(&c, &CLSID_DOMDocument30));
    CHECK("Microsoft.XMLDOM ProgID", SUCCEEDED(CLSIDFromProgID(L"Microsoft.XMLDOM", &c)) && IsEqualCLSID(&c, &CLSID_DOMDocument));
    CHECK("Msxml2.XMLHTTP.6.0 ProgID", SUCCEEDED(CLSIDFromProgID(L"Msxml2.XMLHTTP.6.0", &c)) && IsEqualCLSID(&c, &CLSID_XMLHTTP60));
    CHECK("Msxml2.SAXXMLReader.6.0 ProgID", SUCCEEDED(CLSIDFromProgID(L"Msxml2.SAXXMLReader.6.0", &c)) && IsEqualCLSID(&c, &CLSID_SAXXMLReader60));
    static const CLSID *const docs[] = { &CLSID_DOMDocument, &CLSID_DOMDocument2, &CLSID_DOMDocument26, &CLSID_DOMDocument30,
                                         &CLSID_DOMDocument60, &CLSID_FreeThreadedDOMDocument, &CLSID_FreeThreadedDOMDocument30,
                                         &CLSID_FreeThreadedDOMDocument60, &CLSID_DOMFreeThreadedDocument };
    int made = 0;
    for (unsigned i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        IXMLDOMDocument *d = 0;
        if (SUCCEEDED(CoCreateInstance(docs[i], 0, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument, (void **)&d)) && d) {
            made++;
            d->lpVtbl->Release(d);
        }
    }
    CHECK("every DOMDocument class creates", made == (int)(sizeof docs / sizeof docs[0]));
    /* Edge Update's check: DOMDocument60 as IXMLDOMDocument, then put_async */
    IXMLDOMDocument *d = 0;
    HRESULT hr = CoCreateInstance(&CLSID_DOMDocument60, 0, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument, (void **)&d);
    CHECK("CoCreateInstance(DOMDocument60, IXMLDOMDocument)", hr == S_OK && d);
    if (d) {
        CHECK("put_async(FALSE)", d->lpVtbl->put_async(d, VARIANT_FALSE) == S_OK);
        CHECK("put_resolveExternals(FALSE)", d->lpVtbl->put_resolveExternals(d, VARIANT_FALSE) == S_OK);
        IXMLDOMDocument3 *d3 = 0;
        CHECK("QI IXMLDOMDocument3", SUCCEEDED(d->lpVtbl->QueryInterface(d, &IID_IXMLDOMDocument3, (void **)&d3)) && d3);
        if (d3) d3->lpVtbl->Release(d3);
        d->lpVtbl->Release(d);
    }
    IUnknown *u = 0;
    CHECK("XMLHTTP60 creates", SUCCEEDED(CoCreateInstance(&CLSID_XMLHTTP60, 0, CLSCTX_INPROC_SERVER, &IID_IXMLHTTPRequest, (void **)&u)) && u);
    if (u) {
        IXMLHTTPRequest *x = (IXMLHTTPRequest *)u;
        BSTR m = SysAllocString(L"GET"), url = SysAllocString(L"http://10.0.2.2/none.xml");
        VARIANT f, e;
        f.vt = VT_BOOL; f.boolVal = VARIANT_FALSE;
        VariantInit(&e);
        LONG st = -1;
        CHECK("XMLHTTP open", x->lpVtbl->open(x, m, url, f, e, e) == S_OK && x->lpVtbl->get_readyState(x, &st) == S_OK && st == 1);
        SysFreeString(m); SysFreeString(url);
        u->lpVtbl->Release(u);
    }
    u = 0;
    CHECK("ServerXMLHTTP60 creates", SUCCEEDED(CoCreateInstance(&CLSID_ServerXMLHTTP60, 0, CLSCTX_INPROC_SERVER, &IID_IServerXMLHTTPRequest, (void **)&u)) && u);
    if (u) u->lpVtbl->Release(u);
}

/* ---- a WiX Burn manifest and XPath ---- */
static const WCHAR manifest[] =
    L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    L"<BurnManifest xmlns=\"http://schemas.microsoft.com/wix/2008/Burn\">\r\n"
    L"  <Log PathVariable=\"WixBundleLog\" Prefix=\"dd_vcredist\" Extension=\".txt\" />\r\n"
    L"  <Payload Id=\"a\" FilePath=\"packages\\vcRuntimeMinimum_x86\\vc_runtimeMinimum_x86.msi\" Packaging=\"embedded\" />\r\n"
    L"  <Payload Id=\"b\" FilePath=\"packages\\vcRuntimeAdditional_x86\\cab1.cab\" Packaging=\"embedded\" />\r\n"
    L"  <Chain>\r\n"
    L"    <MsiPackage Id=\"vcRuntimeMinimum_x86\" Vital=\"yes\" Cache=\"yes\"><PayloadRef Id=\"a\" /></MsiPackage>\r\n"
    L"    <MsiPackage Id=\"vcRuntimeAdditional_x86\" Vital=\"no\"><PayloadRef Id=\"b\" /></MsiPackage>\r\n"
    L"  </Chain>\r\n"
    L"</BurnManifest>\r\n";

static void test_xpath(void)
{
    IXMLDOMDocument2 *d = new_doc(&CLSID_DOMDocument2);       /* Burn's "Msxml2.DOMDocument" */
    CHECK("Msxml2.DOMDocument creates", d != 0);
    if (!d) return;
    CHECK("loadXML manifest", load_xml(d, manifest));
    set_prop(d, L"SelectionLanguage", L"XPath");
    set_prop(d, L"SelectionNamespaces", L"xmlns:burn='http://schemas.microsoft.com/wix/2008/Burn'");
    BSTR n = SysAllocString(L"SelectionNamespaces");
    VARIANT v;
    VariantInit(&v);
    CHECK("getProperty SelectionNamespaces", d->lpVtbl->getProperty(d, n, &v) == S_OK && v.vt == VT_BSTR &&
          bstr_is(v.bstrVal, L"xmlns:burn='http://schemas.microsoft.com/wix/2008/Burn'"));
    VariantClear(&v);
    SysFreeString(n);

    IXMLDOMElement *root = 0;
    CHECK("documentElement", d->lpVtbl->get_documentElement(d, &root) == S_OK && root);
    if (!root) { d->lpVtbl->Release(d); return; }
    BSTR s = 0;
    root->lpVtbl->get_nodeName(root, &s);
    CHECK("root nodeName", bstr_is(s, L"BurnManifest"));
    SysFreeString(s);
    root->lpVtbl->get_namespaceURI(root, &s);
    CHECK("root namespaceURI", bstr_is(s, L"http://schemas.microsoft.com/wix/2008/Burn"));
    SysFreeString(s);
    root->lpVtbl->get_baseName(root, &s);
    CHECK("root baseName", bstr_is(s, L"BurnManifest"));
    SysFreeString(s);
    root->lpVtbl->get_prefix(root, &s);
    CHECK("root prefix empty", bstr_is(s, L""));
    SysFreeString(s);

    CHECK("selectNodes /burn:BurnManifest/burn:Payload = 2", count_nodes(d, L"/burn:BurnManifest/burn:Payload") == 2);
    CHECK("selectNodes burn:Chain/burn:MsiPackage = 2", count_nodes(root, L"burn:Chain/burn:MsiPackage") == 2);
    CHECK("selectNodes //burn:PayloadRef = 2", count_nodes(d, L"//burn:PayloadRef") == 2);
    CHECK("unprefixed name matches no namespace", count_nodes(d, L"/BurnManifest") == 0);
    IXMLDOMNode *pkg = select1(root, L"burn:Chain/burn:MsiPackage[@Vital='yes']");
    CHECK("selectSingleNode with a predicate", pkg != 0);
    if (pkg) {
        BSTR id = attr_of(pkg, L"Id");
        CHECK("its Id", bstr_is(id, L"vcRuntimeMinimum_x86"));
        SysFreeString(id);
        IXMLDOMNode *ref = select1(pkg, L"burn:PayloadRef/@Id");
        CHECK("attribute by XPath", ref != 0);
        if (ref) {
            ref->lpVtbl->get_text(ref, &s);
            CHECK("attribute text", bstr_is(s, L"a"));
            SysFreeString(s);
            DOMNodeType t;
            ref->lpVtbl->get_nodeType(ref, &t);
            CHECK("attribute nodeType", t == NODE_ATTRIBUTE);
            ref->lpVtbl->Release(ref);
        }
        pkg->lpVtbl->Release(pkg);
    }
    IXMLDOMNode *none = (IXMLDOMNode *)(INT_PTR)1;
    BSTR q = SysAllocString(L"burn:Nothing");
    CHECK("selectSingleNode miss: S_FALSE, NULL", root->lpVtbl->selectSingleNode(root, q, &none) == S_FALSE && !none);
    SysFreeString(q);
    IXMLDOMNodeList *l = 0;
    q = SysAllocString(L"burn:[bad");
    CHECK("bad XPath fails", FAILED(root->lpVtbl->selectNodes(root, q, &l)) && !l);
    SysFreeString(q);

    /* the node list and the attribute map */
    q = SysAllocString(L"/burn:BurnManifest/burn:Payload");
    d->lpVtbl->selectNodes(d, q, &l);
    SysFreeString(q);
    if (l) {
        IXMLDOMNode *item = 0, *next;
        l->lpVtbl->get_item(l, 1, &item);
        BSTR id = item ? attr_of(item, L"Id") : 0;
        CHECK("item(1)'s Id", bstr_is(id, L"b"));
        SysFreeString(id);
        if (item) item->lpVtbl->Release(item);
        int walked = 0;
        while (l->lpVtbl->nextNode(l, &next) == S_OK && next) { walked++; next->lpVtbl->Release(next); }
        CHECK("nextNode walks the list", walked == 2);
        l->lpVtbl->Release(l);
    }
    IXMLDOMNode *payload = select1(d, L"//burn:Payload");
    if (payload) {
        IXMLDOMNamedNodeMap *m = 0;
        LONG len = 0;
        CHECK("attributes map", payload->lpVtbl->get_attributes(payload, &m) == S_OK && m && m->lpVtbl->get_length(m, &len) == S_OK && len == 3);
        if (m) {
            IXMLDOMNode *a = 0;
            BSTR nm = SysAllocString(L"FilePath");
            CHECK("getNamedItem", m->lpVtbl->getNamedItem(m, nm, &a) == S_OK && a);
            SysFreeString(nm);
            if (a) {
                a->lpVtbl->get_text(a, &s);
                CHECK("its value", bstr_is(s, L"packages\\vcRuntimeMinimum_x86\\vc_runtimeMinimum_x86.msi"));
                SysFreeString(s);
                a->lpVtbl->Release(a);
            }
            m->lpVtbl->get_item(m, 0, &a);
            if (a) { a->lpVtbl->get_nodeName(a, &s); CHECK("item(0) name", bstr_is(s, L"Id")); SysFreeString(s); a->lpVtbl->Release(a); }
            m->lpVtbl->Release(m);
        }
        payload->lpVtbl->Release(payload);
    }
    /* the root's own declaration is an attribute */
    IXMLDOMNamedNodeMap *rm = 0;
    root->lpVtbl->get_attributes(root, &rm);
    if (rm) {
        LONG len = 0;
        rm->lpVtbl->get_length(rm, &len);
        BSTR x = SysAllocString(L"xmlns");
        IXMLDOMNode *a = 0;
        rm->lpVtbl->getNamedItem(rm, x, &a);
        SysFreeString(x);
        CHECK("xmlns is in the attribute map", len == 1 && a);
        if (a) a->lpVtbl->Release(a);
        rm->lpVtbl->Release(rm);
    }
    BSTR xmlns = attr_of(root, L"xmlns");
    CHECK("getAttribute(xmlns)", bstr_is(xmlns, L"http://schemas.microsoft.com/wix/2008/Burn"));
    SysFreeString(xmlns);

    IXMLDOMNodeList *byname = 0;
    q = SysAllocString(L"MsiPackage");
    LONG len = 0;
    CHECK("getElementsByTagName", d->lpVtbl->getElementsByTagName(d, q, &byname) == S_OK && byname &&
          byname->lpVtbl->get_length(byname, &len) == S_OK && len == 2);
    SysFreeString(q);
    if (byname) byname->lpVtbl->Release(byname);

    /* whitespace was dropped: Chain's children are the two packages */
    IXMLDOMNode *chain = select1(root, L"burn:Chain");
    if (chain) {
        IXMLDOMNodeList *kids = 0;
        chain->lpVtbl->get_childNodes(chain, &kids);
        len = 0;
        if (kids) { kids->lpVtbl->get_length(kids, &len); kids->lpVtbl->Release(kids); }
        CHECK("whitespace text dropped", len == 2);
        chain->lpVtbl->Release(chain);
    }
    /* the declaration is the first child, as a processing instruction */
    IXMLDOMNode *first = 0;
    d->lpVtbl->get_firstChild(d, &first);
    if (first) {
        DOMNodeType t;
        first->lpVtbl->get_nodeType(first, &t);
        first->lpVtbl->get_nodeName(first, &s);
        CHECK("declaration node", t == NODE_PROCESSING_INSTRUCTION && bstr_is(s, L"xml"));
        SysFreeString(s);
        first->lpVtbl->Release(first);
    }
    root->lpVtbl->Release(root);
    ULONG left = d->lpVtbl->Release(d);
    CHECK("document released", left == 0);
}

/* ---- building a document (Omaha's request) ---- */
static void test_build(void)
{
    IXMLDOMDocument2 *d = new_doc(&CLSID_DOMDocument60);
    if (!d) { CHECK("DOMDocument60", 0); return; }
    BSTR t = SysAllocString(L"xml"), data = SysAllocString(L"version=\"1.0\" encoding=\"UTF-8\"");
    IXMLDOMProcessingInstruction *pi = 0;
    CHECK("createProcessingInstruction", d->lpVtbl->createProcessingInstruction(d, t, data, &pi) == S_OK && pi);
    SysFreeString(t); SysFreeString(data);
    IXMLDOMNode *out = 0;
    if (pi) { d->lpVtbl->appendChild(d, (IXMLDOMNode *)pi, &out); if (out) out->lpVtbl->Release(out); pi->lpVtbl->Release(pi); }
    BSTR name = SysAllocString(L"request");
    IXMLDOMElement *req = 0;
    CHECK("createElement", d->lpVtbl->createElement(d, name, &req) == S_OK && req);
    SysFreeString(name);
    if (!req) { d->lpVtbl->Release(d); return; }
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"urn:test");
    name = SysAllocString(L"xmlns:x");
    CHECK("setAttribute declares a namespace", req->lpVtbl->setAttribute(req, name, v) == S_OK);
    SysFreeString(name);
    VariantClear(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"3.0");
    name = SysAllocString(L"protocol");
    CHECK("setAttribute", req->lpVtbl->setAttribute(req, name, v) == S_OK);
    SysFreeString(name);
    VariantClear(&v);
    v.vt = VT_I4;
    v.lVal = 1;
    name = SysAllocString(L"ismachine");
    CHECK("setAttribute with a number", req->lpVtbl->setAttribute(req, name, v) == S_OK);
    SysFreeString(name);
    CHECK("appendChild to the document", d->lpVtbl->appendChild(d, (IXMLDOMNode *)req, 0) == S_OK);
    name = SysAllocString(L"app");
    IXMLDOMElement *app = 0;
    d->lpVtbl->createElement(d, name, &app);
    SysFreeString(name);
    BSTR txt = SysAllocString(L"a<b&c");
    IXMLDOMText *tn = 0;
    d->lpVtbl->createTextNode(d, txt, &tn);
    SysFreeString(txt);
    if (app && tn) {
        app->lpVtbl->appendChild(app, (IXMLDOMNode *)tn, 0);
        req->lpVtbl->appendChild(req, (IXMLDOMNode *)app, 0);
    }
    BSTR x = node_xml(d);
    CHECK("document xml", bstr_is(x, L"<?xml version=\"1.0\"?>\r\n<request xmlns:x=\"urn:test\" protocol=\"3.0\" ismachine=\"1\"><app>a&lt;b&amp;c</app></request>\r\n"));
    SysFreeString(x);
    IXMLDOMElement *second = 0;
    name = SysAllocString(L"second");
    d->lpVtbl->createElement(d, name, &second);
    SysFreeString(name);
    CHECK("a second document element is refused", second && FAILED(d->lpVtbl->appendChild(d, (IXMLDOMNode *)second, 0)));
    if (second) second->lpVtbl->Release(second);

    /* createNode with a namespace the parent already declares */
    VARIANT type;
    type.vt = VT_I4;
    type.lVal = NODE_ELEMENT;
    BSTR uri = SysAllocString(L"urn:test");
    name = SysAllocString(L"x:item");
    IXMLDOMNode *item = 0;
    CHECK("createNode", d->lpVtbl->createNode(d, type, name, uri, &item) == S_OK && item);
    SysFreeString(name);
    if (item) {
        req->lpVtbl->appendChild(req, item, 0);
        BSTR ns = 0;
        item->lpVtbl->get_namespaceURI(item, &ns);
        CHECK("createNode's namespace", bstr_is(ns, L"urn:test"));
        SysFreeString(ns);
        item->lpVtbl->Release(item);
    }
    x = node_xml(req);
    CHECK("no repeated declaration", bstr_is(x, L"<request xmlns:x=\"urn:test\" protocol=\"3.0\" ismachine=\"1\"><app>a&lt;b&amp;c</app><x:item/></request>"));
    SysFreeString(x);
    SysFreeString(uri);

    /* changing the tree */
    if (app && tn) {
        IXMLDOMNode *removed = 0;
        CHECK("removeChild", req->lpVtbl->removeChild(req, (IXMLDOMNode *)app, &removed) == S_OK && removed);
        if (removed) removed->lpVtbl->Release(removed);
        IXMLDOMNode *fc = 0;
        req->lpVtbl->get_firstChild(req, &fc);
        VARIANT ref;
        ref.vt = VT_DISPATCH;
        ref.pdispVal = (IDispatch *)fc;
        CHECK("insertBefore", fc && req->lpVtbl->insertBefore(req, (IXMLDOMNode *)app, ref, 0) == S_OK);
        IXMLDOMNode *clone = 0;
        CHECK("cloneNode deep", app->lpVtbl->cloneNode(app, VARIANT_TRUE, &clone) == S_OK && clone);
        if (clone && fc) {
            CHECK("replaceChild", req->lpVtbl->replaceChild(req, clone, fc, 0) == S_OK);
            x = node_xml(req);
            CHECK("after the changes", bstr_is(x, L"<request xmlns:x=\"urn:test\" protocol=\"3.0\" ismachine=\"1\"><app>a&lt;b&amp;c</app><app>a&lt;b&amp;c</app></request>"));
            SysFreeString(x);
        }
        if (clone) clone->lpVtbl->Release(clone);
        if (fc) fc->lpVtbl->Release(fc);
        VARIANT_BOOL has;
        tn->lpVtbl->hasChildNodes(tn, &has);
        CHECK("a text node has no children", has == VARIANT_FALSE);
        LONG n = 0;
        tn->lpVtbl->get_length(tn, &n);
        CHECK("text length", n == 5);
        BSTR more = SysAllocString(L"!");
        tn->lpVtbl->appendData(tn, more);
        SysFreeString(more);
        tn->lpVtbl->get_data(tn, &x);
        CHECK("appendData", bstr_is(x, L"a<b&c!"));
        SysFreeString(x);
    }
    if (tn) tn->lpVtbl->Release(tn);
    if (app) app->lpVtbl->Release(app);

    /* put_text replaces the children; typed binary values */
    BSTR b64 = SysAllocString(L"bin.base64");
    req->lpVtbl->put_dataType(req, b64);
    SysFreeString(b64);
    BSTR content = SysAllocString(L"AQID/w==");
    req->lpVtbl->put_text(req, content);
    SysFreeString(content);
    VariantInit(&v);
    CHECK("nodeTypedValue bin.base64", req->lpVtbl->get_nodeTypedValue(req, &v) == S_OK && v.vt == (VT_ARRAY | VT_UI1));
    if (v.vt == (VT_ARRAY | VT_UI1)) {
        BYTE *p;
        LONG hi = -1;
        SafeArrayGetUBound(v.parray, 1, &hi);
        SafeArrayAccessData(v.parray, (void **)&p);
        CHECK("decoded bytes", hi == 3 && p[0] == 1 && p[1] == 2 && p[2] == 3 && p[3] == 0xFF);
        SafeArrayUnaccessData(v.parray);
    }
    VariantClear(&v);
    req->lpVtbl->Release(req);
    CHECK("built document released", d->lpVtbl->Release(d) == 0);
}

/* ---- errors, files, encodings ---- */
static void test_errors_and_files(void)
{
    IXMLDOMDocument2 *d = new_doc(&CLSID_DOMDocument60);
    if (!d) { CHECK("DOMDocument60", 0); return; }
    BSTR b = SysAllocString(L"<a>\n<b></a>");
    VARIANT_BOOL ok = VARIANT_TRUE;
    CHECK("loadXML malformed: S_FALSE", d->lpVtbl->loadXML(d, b, &ok) == S_FALSE && ok == VARIANT_FALSE);
    SysFreeString(b);
    IXMLDOMParseError *e = 0;
    d->lpVtbl->get_parseError(d, &e);
    if (e) {
        LONG code = 0, line = 0;
        BSTR reason = 0;
        e->lpVtbl->get_errorCode(e, &code);
        e->lpVtbl->get_line(e, &line);
        e->lpVtbl->get_reason(e, &reason);
        CHECK("parseError code, line and reason", code != 0 && line == 2 && reason && reason[0]);
        SysFreeString(reason);
        e->lpVtbl->Release(e);
    } else CHECK("parseError", 0);
    IXMLDOMElement *none = 0;
    CHECK("empty after a failed load", d->lpVtbl->get_documentElement(d, &none) == S_FALSE && !none);
    b = SysAllocString(L"");
    d->lpVtbl->loadXML(d, b, &ok);
    SysFreeString(b);
    e = 0;
    d->lpVtbl->get_parseError(d, &e);
    LONG code = 0;
    if (e) { e->lpVtbl->get_errorCode(e, &code); e->lpVtbl->Release(e); }
    CHECK("empty document: XML_E_MISSINGROOT", (unsigned long)code == 0xC00CE558UL);
    CHECK("MSXML 6 prohibits a DTD", !load_xml(d, L"<!DOCTYPE a [<!ENTITY e \"x\">]><a>&e;</a>"));
    VARIANT v;
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_FALSE;
    BSTR pn = SysAllocString(L"ProhibitDTD");
    d->lpVtbl->setProperty(d, pn, v);
    SysFreeString(pn);
    CHECK("ProhibitDTD false allows it", load_xml(d, L"<!DOCTYPE a [<!ENTITY e \"x\">]><a>&e;</a>"));

    /* save and load back, from a path and a file:// URL */
    CHECK("load the manifest", load_xml(d, manifest));
    CreateDirectoryW(L"C:\\Temp", 0);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"C:\\Temp\\msxmltest.xml");
    CHECK("save to a file", d->lpVtbl->save(d, v) == S_OK);
    char head[64] = { 0 };
    HANDLE f = CreateFileW(v.bstrVal, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    DWORD got = 0;
    if (f != INVALID_HANDLE_VALUE) { ReadFile(f, head, 38, &got, 0); CloseHandle(f); }
    CHECK("the file keeps the declaration's encoding", !strncmp(head, "<?xml version=\"1.0\" encoding=\"utf-8\"?>", 38));
    IXMLDOMDocument2 *d2 = new_doc(&CLSID_DOMDocument60);
    ok = VARIANT_FALSE;
    CHECK("load from a path", d2 && d2->lpVtbl->load(d2, v, &ok) == S_OK && ok == VARIANT_TRUE);
    VariantClear(&v);
    if (d2) {
        CHECK("its Payloads", count_nodes(d2, L"//*[local-name()='Payload']") == 2);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"file:///C:/Temp/msxmltest.xml");
        ok = VARIANT_FALSE;
        CHECK("load from a file:// URL", d2->lpVtbl->load(d2, v, &ok) == S_OK && ok == VARIANT_TRUE);
        BSTR url = 0;
        d2->lpVtbl->get_url(d2, &url);
        CHECK("url", bstr_is(url, L"file:///C:/Temp/msxmltest.xml"));
        SysFreeString(url);
        VariantClear(&v);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"C:\\Temp\\no-such-file.xml");
        CHECK("load a missing file: S_FALSE", d2->lpVtbl->load(d2, v, &ok) == S_FALSE && ok == VARIANT_FALSE);
        VariantClear(&v);

        /* UTF-16 bytes with a mark, and windows-1252 */
        static const WCHAR u16[] = L"\xFEFF<?xml version=\"1.0\" encoding=\"UTF-16\"?><t>caf\x00E9</t>";
        SAFEARRAY *a = SafeArrayCreateVector(VT_UI1, 0, sizeof u16 - sizeof(WCHAR));
        void *p;
        SafeArrayAccessData(a, &p);
        memcpy(p, u16, sizeof u16 - sizeof(WCHAR));
        SafeArrayUnaccessData(a);
        v.vt = VT_ARRAY | VT_UI1;
        v.parray = a;
        ok = VARIANT_FALSE;
        d2->lpVtbl->load(d2, v, &ok);
        IXMLDOMElement *el = 0;
        BSTR s = 0;
        if (ok) { d2->lpVtbl->get_documentElement(d2, &el); if (el) { el->lpVtbl->get_text(el, &s); el->lpVtbl->Release(el); } }
        CHECK("UTF-16 bytes", ok == VARIANT_TRUE && bstr_is(s, L"caf\x00E9"));
        SysFreeString(s);
        VariantClear(&v);
        static const char cp[] = "<?xml version=\"1.0\" encoding=\"windows-1252\"?><t>caf\xE9 \x80</t>";
        a = SafeArrayCreateVector(VT_UI1, 0, sizeof cp - 1);
        SafeArrayAccessData(a, &p);
        memcpy(p, cp, sizeof cp - 1);
        SafeArrayUnaccessData(a);
        v.vt = VT_ARRAY | VT_UI1;
        v.parray = a;
        ok = VARIANT_FALSE;
        d2->lpVtbl->load(d2, v, &ok);
        s = 0;
        el = 0;
        if (ok) { d2->lpVtbl->get_documentElement(d2, &el); if (el) { el->lpVtbl->get_text(el, &s); el->lpVtbl->Release(el); } }
        CHECK("windows-1252 bytes", ok == VARIANT_TRUE && bstr_is(s, L"caf\x00E9 \x20AC"));
        SysFreeString(s);
        VariantClear(&v);
        d2->lpVtbl->Release(d2);
    }
    d->lpVtbl->Release(d);
}

/* ---- MSXML 3's and 6's defaults, whitespace, node kinds ---- */
static void test_versions(void)
{
    IXMLDOMDocument2 *d3 = new_doc(&CLSID_DOMDocument30), *d6 = new_doc(&CLSID_DOMDocument60);
    if (!d3 || !d6) { CHECK("3.0 and 6.0 documents", 0); return; }
    BSTR n = SysAllocString(L"SelectionLanguage");
    VARIANT v3, v6;
    VariantInit(&v3);
    VariantInit(&v6);
    d3->lpVtbl->getProperty(d3, n, &v3);
    d6->lpVtbl->getProperty(d6, n, &v6);
    CHECK("MSXML 3 SelectionLanguage XSLPattern", v3.vt == VT_BSTR && bstr_is(v3.bstrVal, L"XSLPattern"));
    CHECK("MSXML 6 SelectionLanguage XPath", v6.vt == VT_BSTR && bstr_is(v6.bstrVal, L"XPath"));
    VariantClear(&v3);
    VariantClear(&v6);
    SysFreeString(n);
    CHECK("MSXML 3 allows a DTD", load_xml(d3, L"<!DOCTYPE a [<!ELEMENT a EMPTY>]><a/>"));
    CHECK("XSLPattern-style path", count_nodes(d3, L"a") == 1);

    CHECK("whitespace load", load_xml(d6, L"<a>\n  <b> x </b>\n  <!--c--><![CDATA[<d>]]>\n</a>"));
    IXMLDOMElement *a = 0;
    d6->lpVtbl->get_documentElement(d6, &a);
    if (a) {
        IXMLDOMNodeList *k = 0;
        LONG len = 0;
        a->lpVtbl->get_childNodes(a, &k);
        if (k) { k->lpVtbl->get_length(k, &len); }
        CHECK("children without whitespace", len == 3);
        BSTR s = 0;
        a->lpVtbl->get_text(a, &s);
        CHECK("element text (comments left out)", bstr_is(s, L"x <d>"));
        SysFreeString(s);
        if (k) {
            IXMLDOMNode *c = 0;
            k->lpVtbl->get_item(k, 1, &c);
            if (c) { c->lpVtbl->get_nodeName(c, &s); CHECK("comment nodeName", bstr_is(s, L"#comment")); SysFreeString(s); c->lpVtbl->Release(c); }
            k->lpVtbl->get_item(k, 2, &c);
            if (c) {
                c->lpVtbl->get_nodeTypeString(c, &s);
                CHECK("CDATA nodeTypeString", bstr_is(s, L"cdatasection"));
                SysFreeString(s);
                s = node_xml(c);
                CHECK("CDATA xml", bstr_is(s, L"<![CDATA[<d>]]>"));
                SysFreeString(s);
                c->lpVtbl->Release(c);
            }
            k->lpVtbl->Release(k);
        }
        a->lpVtbl->Release(a);
    }
    d6->lpVtbl->put_preserveWhiteSpace(d6, VARIANT_TRUE);
    load_xml(d6, L"<a>\n  <b/>\n</a>");
    CHECK("preserveWhiteSpace keeps it", count_nodes(d6, L"/a/node()") == 3);

    /* importNode across documents */
    IXMLDOMDocument3 *d63 = 0;
    d6->lpVtbl->QueryInterface(d6, &IID_IXMLDOMDocument3, (void **)&d63);
    load_xml(d3, L"<x><y z=\"1\"/></x>");
    IXMLDOMNode *y = select1(d3, L"//y"), *imp = 0;
    CHECK("importNode", d63 && y && d63->lpVtbl->importNode(d63, y, VARIANT_TRUE, &imp) == S_OK && imp);
    IXMLDOMDocument *owner = 0;
    if (imp) {
        imp->lpVtbl->get_ownerDocument(imp, &owner);
        CHECK("imported node's owner", owner == (IXMLDOMDocument *)d63);
        if (owner) owner->lpVtbl->Release(owner);
        imp->lpVtbl->Release(imp);
    }
    if (y) y->lpVtbl->Release(y);
    if (d63) d63->lpVtbl->Release(d63);
    d3->lpVtbl->Release(d3);
    d6->lpVtbl->Release(d6);
}

/* ---- SAX2 ---- */
typedef struct Handler {
    const ISAXContentHandlerVtbl *lpVtbl;
    int elements, ends, prefixes, docs;
    WCHAR chars[64];
    WCHAR lastattr[64];
} Handler;

static HRESULT STDMETHODCALLTYPE h_qi(Handler *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISAXContentHandler)) { *out = This; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_ref(Handler *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE h_loc(Handler *This, ISAXLocator *l) { (void)This; (void)l; return S_OK; }
static HRESULT STDMETHODCALLTYPE h_start(Handler *This) { This->docs++; return S_OK; }
static HRESULT STDMETHODCALLTYPE h_end(Handler *This) { This->docs += 10; return S_OK; }
static HRESULT STDMETHODCALLTYPE h_spm(Handler *This, const WCHAR *p, int np, const WCHAR *u, int nu)
{
    (void)p; (void)np; (void)u; (void)nu;
    This->prefixes++;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE h_epm(Handler *This, const WCHAR *p, int np) { (void)p; (void)np; This->prefixes += 10; return S_OK; }
static HRESULT STDMETHODCALLTYPE h_se(Handler *This, const WCHAR *u, int nu, const WCHAR *l, int nl, const WCHAR *q, int nq,
                                      ISAXAttributes *a)
{
    (void)u; (void)nu; (void)l; (void)nl; (void)q; (void)nq;
    This->elements++;
    const WCHAR *v;
    int n;
    if (a->lpVtbl->getValueFromQName(a, L"id", 2, &v, &n) == S_OK && n < 63) { memcpy(This->lastattr, v, n * sizeof(WCHAR)); This->lastattr[n] = 0; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE h_ee(Handler *This, const WCHAR *u, int nu, const WCHAR *l, int nl, const WCHAR *q, int nq)
{
    (void)u; (void)nu; (void)l; (void)nl; (void)q; (void)nq;
    This->ends++;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE h_ch(Handler *This, const WCHAR *c, int n)
{
    size_t have = wcslen(This->chars);
    if (have + n < 63) { memcpy(This->chars + have, c, n * sizeof(WCHAR)); This->chars[have + n] = 0; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE h_pi(Handler *This, const WCHAR *t, int nt, const WCHAR *d, int nd)
{
    (void)This; (void)t; (void)nt; (void)d; (void)nd;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE h_skip(Handler *This, const WCHAR *n, int nn) { (void)This; (void)n; (void)nn; return S_OK; }

static const ISAXContentHandlerVtbl handler_vtbl = {
    (void *)h_qi, (void *)h_ref, (void *)h_ref, (void *)h_loc, (void *)h_start, (void *)h_end, (void *)h_spm,
    (void *)h_epm, (void *)h_se, (void *)h_ee, (void *)h_ch, (void *)h_ch, (void *)h_pi, (void *)h_skip,
};

typedef struct ErrHandler { const ISAXErrorHandlerVtbl *lpVtbl; int fatal; } ErrHandler;
static HRESULT STDMETHODCALLTYPE eh_qi(ErrHandler *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISAXErrorHandler)) { *out = This; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE eh_ref(ErrHandler *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE eh_err(ErrHandler *This, ISAXLocator *l, const WCHAR *m, HRESULT hr)
{
    (void)l; (void)m; (void)hr;
    This->fatal++;
    return S_OK;
}
static const ISAXErrorHandlerVtbl errhandler_vtbl = { (void *)eh_qi, (void *)eh_ref, (void *)eh_ref, (void *)eh_err, (void *)eh_err, (void *)eh_err };

static void test_sax(void)
{
    ISAXXMLReader *r = 0;
    CHECK("SAXXMLReader60 creates", SUCCEEDED(CoCreateInstance(&CLSID_SAXXMLReader60, 0, CLSCTX_INPROC_SERVER, &IID_ISAXXMLReader, (void **)&r)) && r);
    if (!r) return;
    Handler h = { &handler_vtbl };
    ErrHandler eh = { &errhandler_vtbl };
    r->lpVtbl->putContentHandler(r, (ISAXContentHandler *)&h);
    r->lpVtbl->putErrorHandler(r, (ISAXErrorHandler *)&eh);
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"<r xmlns:p=\"urn:p\"><p:a id=\"one\">hi</p:a><b/></r>");
    CHECK("SAX parse", r->lpVtbl->parse(r, v) == S_OK);
    VariantClear(&v);
    CHECK("SAX elements", h.elements == 3 && h.ends == 3);
    CHECK("SAX document start and end", h.docs == 11);
    CHECK("SAX prefix mapping", h.prefixes == 11);
    CHECK("SAX characters", !wcscmp(h.chars, L"hi"));
    CHECK("SAX attribute", !wcscmp(h.lastattr, L"one"));
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"<r><a></r>");
    CHECK("SAX malformed fails", FAILED(r->lpVtbl->parse(r, v)) && eh.fatal == 1);
    VariantClear(&v);
    r->lpVtbl->Release(r);
}

static void test_version_resource(void)
{
    WCHAR path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\msxml6.dll");
    DWORD h, n = GetFileVersionInfoSizeW(path, &h);
    void *buf = n ? malloc(n) : 0;
    VS_FIXEDFILEINFO *fi = 0;
    UINT len = 0;
    BOOL ok = buf && GetFileVersionInfoW(path, 0, n, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &len) && fi;
    CHECK("msxml6.dll version 6.30", ok && HIWORD(fi->dwFileVersionMS) == 6 && LOWORD(fi->dwFileVersionMS) == 30);
    free(buf);
}

int main(void)
{
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    test_registry();
    test_xpath();
    test_build();
    test_errors_and_files();
    test_versions();
    test_sax();
    test_version_resource();
    CoUninitialize();
    printf("msxmltest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
