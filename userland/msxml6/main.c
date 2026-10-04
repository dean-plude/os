/*
 * msxml6.dll — Microsoft XML Core Services 6.0 for NovaOS: the XML DOM
 * (DOMDocument, with XPath selectNodes/selectSingleNode), SAX2
 * (SAXXMLReader) and XMLHTTP, on libxml2 (third_party/libxml2, MIT).
 * Windows answers the MSXML 3.0 and version-independent classes
 * ("Msxml2.DOMDocument", "Microsoft.XMLDOM") from msxml3.dll; NovaOS
 * registers them to this DLL, which gives them MSXML 3's defaults.
 * The kernel's registry defaults (kernel/um/um_registry.c) list the
 * classes and ProgIDs.
 */
#include "msxml_private.h"

LONG g_objects;
int _fltused = 0x9875;                  /* floating point in use (XPath numbers; the compiler references it) */
static LONG g_locks;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_once(INIT_ONCE *o, void *p, void **ctx)
{
    (void)o; (void)p; (void)ctx;
    xmlInitParser();
    encodings_init();
    return TRUE;
}

void msxml_init(void) { InitOnceExecuteOnce(&g_once, init_once, 0, 0); }

enum { K_DOC3, K_DOC6, K_FTDOC3, K_FTDOC6, K_HTTP, K_SERVERHTTP, K_SAX3, K_SAX6 };

static const struct { const CLSID *clsid; int kind; } classes[] = {
    { &CLSID_DOMDocument, K_DOC3 },               { &CLSID_DOMDocument2, K_DOC3 },
    { &CLSID_DOMDocument26, K_DOC3 },             { &CLSID_DOMDocument30, K_DOC3 },
    { &CLSID_DOMDocument60, K_DOC6 },
    { &CLSID_DOMFreeThreadedDocument, K_FTDOC3 }, { &CLSID_FreeThreadedDOMDocument, K_FTDOC3 },
    { &CLSID_FreeThreadedDOMDocument26, K_FTDOC3 }, { &CLSID_FreeThreadedDOMDocument30, K_FTDOC3 },
    { &CLSID_FreeThreadedDOMDocument60, K_FTDOC6 },
    { &CLSID_XMLHTTPRequest, K_HTTP },            { &CLSID_XMLHTTP, K_HTTP },
    { &CLSID_XMLHTTP26, K_HTTP },                 { &CLSID_XMLHTTP30, K_HTTP },
    { &CLSID_XMLHTTP60, K_HTTP },                 { &CLSID_FreeThreadedXMLHTTP60, K_HTTP },
    { &CLSID_ServerXMLHTTP, K_SERVERHTTP },       { &CLSID_ServerXMLHTTP30, K_SERVERHTTP },
    { &CLSID_ServerXMLHTTP60, K_SERVERHTTP },
    { &CLSID_SAXXMLReader, K_SAX3 },              { &CLSID_SAXXMLReader30, K_SAX3 },
    { &CLSID_SAXXMLReader60, K_SAX6 },
};

typedef struct Factory { const IClassFactoryVtbl *lpVtbl; int kind; } Factory;

static HRESULT STDMETHODCALLTYPE cf_qi(IClassFactory *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *out = This;
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_addref(IClassFactory *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE cf_release(IClassFactory *This) { (void)This; return 1; }

static HRESULT STDMETHODCALLTYPE cf_create(IClassFactory *This, IUnknown *outer, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    if (outer) return CLASS_E_NOAGGREGATION;
    switch (((Factory *)This)->kind) {
    case K_DOC3: return doc_create(3, FALSE, riid, out);
    case K_DOC6: return doc_create(6, FALSE, riid, out);
    case K_FTDOC3: return doc_create(3, TRUE, riid, out);
    case K_FTDOC6: return doc_create(6, TRUE, riid, out);
    case K_HTTP: return httpreq_create(FALSE, riid, out);
    case K_SERVERHTTP: return httpreq_create(TRUE, riid, out);
    case K_SAX3: return saxreader_create(3, riid, out);
    case K_SAX6: return saxreader_create(6, riid, out);
    }
    return E_FAIL;
}

static HRESULT STDMETHODCALLTYPE cf_lock(IClassFactory *This, BOOL lock)
{
    (void)This;
    if (lock) InterlockedIncrement(&g_locks);
    else InterlockedDecrement(&g_locks);
    return S_OK;
}

static const IClassFactoryVtbl factory_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static Factory factories[] = {
    { &factory_vtbl, K_DOC3 }, { &factory_vtbl, K_DOC6 }, { &factory_vtbl, K_FTDOC3 }, { &factory_vtbl, K_FTDOC6 },
    { &factory_vtbl, K_HTTP }, { &factory_vtbl, K_SERVERHTTP }, { &factory_vtbl, K_SAX3 }, { &factory_vtbl, K_SAX6 },
};

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    for (unsigned i = 0; i < sizeof classes / sizeof classes[0]; i++)
        if (IsEqualCLSID(clsid, classes[i].clsid))
            return cf_qi((IClassFactory *)&factories[classes[i].kind], riid, out);
    return CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return g_objects || g_locks ? S_FALSE : S_OK; }

/* the classes are in the registry from the start (the kernel's defaults) */
__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }

int xmlDllMain(void *inst, unsigned long reason, void *reserved);  /* libxml2's per-thread state */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    xmlDllMain(inst, reason, reserved);
    return TRUE;
}
