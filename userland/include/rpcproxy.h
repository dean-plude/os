/* rpcproxy.h — COM proxy/stub DLLs: the tables a MIDL- or widl-generated
 * proxy file (IFoo_p.c) and its dlldata.c describe their interfaces with,
 * and the rpcrt4 functions that bring them to life.  Layouts follow the
 * Windows SDK's, since compiled proxy DLLs hand these tables to rpcrt4. */
#pragma once
#ifndef __RPCPROXY_H_VERSION__
#define __RPCPROXY_H_VERSION__ 475
#endif
#include <rpc.h>
#include <rpcndr.h>
#include <objbase.h>

_NOVA_BEGIN

#ifndef DECLSPEC_HIDDEN
#define DECLSPEC_HIDDEN
#endif

typedef struct tagCInterfaceProxyHeader {
#ifdef USE_STUBLESS_PROXY
    const void *pStublessProxyInfo;
#endif
    const IID *piid;
} CInterfaceProxyHeader;
#define CINTERFACE_PROXY_VTABLE(n) struct { CInterfaceProxyHeader header; void *Vtbl[n]; }
typedef struct tagCInterfaceProxyVtbl { CInterfaceProxyHeader header; void *Vtbl[1]; } CInterfaceProxyVtbl;

typedef void (__RPC_STUB *PRPC_STUB_FUNCTION)(IRpcStubBuffer *This, IRpcChannelBuffer *chan, PRPC_MESSAGE msg, DWORD *phase);
typedef struct tagCInterfaceStubHeader {
    const IID *piid;
    const MIDL_SERVER_INFO *pServerInfo;
    unsigned long DispatchTableCount;
    const PRPC_STUB_FUNCTION *pDispatchTable;
} CInterfaceStubHeader;
typedef struct tagCInterfaceStubVtbl { CInterfaceStubHeader header; IRpcStubBufferVtbl Vtbl; } CInterfaceStubVtbl;

typedef struct tagCStdStubBuffer {
    const IRpcStubBufferVtbl *lpVtbl;
    LONG RefCount;
    IUnknown *pvServerObject;
    const void *pCallFactoryVtbl;
    const IID *pAsyncIID;
    IPSFactoryBuffer *pPSFactory;
    const void *pRMBVtbl;
} CStdStubBuffer;

typedef struct tagCStdPSFactoryBuffer {
    const IPSFactoryBufferVtbl *lpVtbl;
    LONG RefCount;
    const struct tagProxyFileInfo **pProxyFileList;
    LONG Filler1;
} CStdPSFactoryBuffer;

typedef const CInterfaceProxyVtbl *PCInterfaceProxyVtblList;
typedef const CInterfaceStubVtbl *PCInterfaceStubVtblList;
typedef const char *PCInterfaceName;
typedef int __stdcall IIDLookupRtn(const IID *iid, int *index);
typedef IIDLookupRtn *PIIDLookup;
typedef struct tagProxyFileInfo {
    const PCInterfaceProxyVtblList *pProxyVtblList;
    const PCInterfaceStubVtblList *pStubVtblList;
    const PCInterfaceName *pNamesArray;
    const IID **pDelegatedIIDs;
    const PIIDLookup pIIDLookupRtn;
    unsigned short TableSize;
    unsigned short TableVersion;
    const IID **pAsyncIIDLookup;
    LONG_PTR Filler2, Filler3, Filler4;
} ProxyFileInfo;
typedef ProxyFileInfo ExtendedProxyFileInfo;

#define IID_GENERIC_CHECK_IID(name, iid, index) memcmp(iid, name##_ProxyVtblList[index]->header.piid, 16)
#define IID_BS_LOOKUP_SETUP int result, low = -1;
#define IID_BS_LOOKUP_INITIAL_TEST(name, sz, split) \
    if ((result = name##_CHECK_IID(split)) > 0) { low = sz - split; } else if (!result) { low = split; goto found_label; }
#define IID_BS_LOOKUP_NEXT_TEST(name, split) \
    if ((result = name##_CHECK_IID(low + split)) >= 0) { low = low + split; if (!result) goto found_label; }
#define IID_BS_LOOKUP_RETURN_RESULT(name, sz, index) \
    low = low + 1; if (low >= sz) goto not_found_label; if (name##_CHECK_IID(low)) goto not_found_label; goto found_label; \
    not_found_label: return 0; found_label: (index) = low; return 1;
#define IID_LOOKUP_SETUP
#define IID_LOOKUP_RETURN_RESULT(name, sz, index) return 0;

/* the stub methods every interface stub shares (CStdStubBuffer_Release and
 * CStdStubBuffer2_Release come from the DLL's dlldata.c) */
RPCRTAPI HRESULT STDMETHODCALLTYPE CStdStubBuffer_QueryInterface(IRpcStubBuffer *This, REFIID riid, void **ppv);
RPCRTAPI ULONG STDMETHODCALLTYPE CStdStubBuffer_AddRef(IRpcStubBuffer *This);
RPCRTAPI HRESULT STDMETHODCALLTYPE CStdStubBuffer_Connect(IRpcStubBuffer *This, IUnknown *server);
RPCRTAPI void STDMETHODCALLTYPE CStdStubBuffer_Disconnect(IRpcStubBuffer *This);
RPCRTAPI HRESULT STDMETHODCALLTYPE CStdStubBuffer_Invoke(IRpcStubBuffer *This, RPCOLEMESSAGE *msg, IRpcChannelBuffer *chan);
RPCRTAPI IRpcStubBuffer *STDMETHODCALLTYPE CStdStubBuffer_IsIIDSupported(IRpcStubBuffer *This, REFIID riid);
RPCRTAPI ULONG STDMETHODCALLTYPE CStdStubBuffer_CountRefs(IRpcStubBuffer *This);
RPCRTAPI HRESULT STDMETHODCALLTYPE CStdStubBuffer_DebugServerQueryInterface(IRpcStubBuffer *This, void **ppv);
RPCRTAPI void STDMETHODCALLTYPE CStdStubBuffer_DebugServerRelease(IRpcStubBuffer *This, void *pv);
ULONG STDMETHODCALLTYPE CStdStubBuffer_Release(IRpcStubBuffer *This);
ULONG STDMETHODCALLTYPE CStdStubBuffer2_Release(IRpcStubBuffer *This);
RPCRTAPI ULONG STDMETHODCALLTYPE NdrCStdStubBuffer_Release(IRpcStubBuffer *This, IPSFactoryBuffer *factory);
RPCRTAPI ULONG STDMETHODCALLTYPE NdrCStdStubBuffer2_Release(IRpcStubBuffer *This, IPSFactoryBuffer *factory);
#define CStdStubBuffer_METHODS \
    CStdStubBuffer_QueryInterface, CStdStubBuffer_AddRef, CStdStubBuffer_Release, CStdStubBuffer_Connect, \
    CStdStubBuffer_Disconnect, CStdStubBuffer_Invoke, CStdStubBuffer_IsIIDSupported, CStdStubBuffer_CountRefs, \
    CStdStubBuffer_DebugServerQueryInterface, CStdStubBuffer_DebugServerRelease
#define CStdStubBuffer_DELEGATING_METHODS 0, 0, CStdStubBuffer2_Release, 0, 0, 0, 0, 0, 0, 0

RPCRTAPI HRESULT STDMETHODCALLTYPE IUnknown_QueryInterface_Proxy(IUnknown *This, REFIID riid, void **ppv);
RPCRTAPI ULONG STDMETHODCALLTYPE IUnknown_AddRef_Proxy(IUnknown *This);
RPCRTAPI ULONG STDMETHODCALLTYPE IUnknown_Release_Proxy(IUnknown *This);
RPCRTAPI void __RPC_STUB NdrStubForwardingFunction(IRpcStubBuffer *This, IRpcChannelBuffer *chan, PRPC_MESSAGE msg, DWORD *phase);

/* the older (/Os) proxies and stubs that marshal by code instead of by format string */
RPCRTAPI void RPC_ENTRY NdrProxyInitialize(void *This, PRPC_MESSAGE msg, PMIDL_STUB_MESSAGE sm, PMIDL_STUB_DESC desc, unsigned int proc);
RPCRTAPI void RPC_ENTRY NdrProxyGetBuffer(void *This, PMIDL_STUB_MESSAGE sm);
RPCRTAPI void RPC_ENTRY NdrProxySendReceive(void *This, PMIDL_STUB_MESSAGE sm);
RPCRTAPI void RPC_ENTRY NdrProxyFreeBuffer(void *This, PMIDL_STUB_MESSAGE sm);
RPCRTAPI HRESULT RPC_ENTRY NdrProxyErrorHandler(DWORD code);
RPCRTAPI void RPC_ENTRY NdrStubInitialize(PRPC_MESSAGE msg, PMIDL_STUB_MESSAGE sm, PMIDL_STUB_DESC desc, IRpcChannelBuffer *chan);
RPCRTAPI void RPC_ENTRY NdrStubGetBuffer(IRpcStubBuffer *This, IRpcChannelBuffer *chan, PMIDL_STUB_MESSAGE sm);

RPCRTAPI HRESULT RPC_ENTRY NdrDllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv, const ProxyFileInfo **list,
                                                const CLSID *psclsid, CStdPSFactoryBuffer *factory);
RPCRTAPI HRESULT RPC_ENTRY NdrDllCanUnloadNow(CStdPSFactoryBuffer *factory);
RPCRTAPI HRESULT RPC_ENTRY NdrDllRegisterProxy(HMODULE dll, const ProxyFileInfo **list, const CLSID *psclsid);
RPCRTAPI HRESULT RPC_ENTRY NdrDllUnregisterProxy(HMODULE dll, const ProxyFileInfo **list, const CLSID *psclsid);

/* dlldata.c */
#define EXTERN_PROXY_FILE(name)    EXTERN_C const ProxyFileInfo name##_ProxyFileInfo;
#define PROXYFILE_LIST_START       const ProxyFileInfo *aProxyFileList[] = {
#define REFERENCE_PROXY_FILE(name) &name##_ProxyFileInfo
#define PROXYFILE_LIST_END         0 };
#define GET_DLL_CLSID (aProxyFileList[0]->pStubVtblList[0] ? aProxyFileList[0]->pStubVtblList[0]->header.piid : 0)
#define CLSID_PSFACTORYBUFFER
#ifdef PROXY_CLSID
#define DLLDATA_GETPROXYDLLCLSID(list, clsid)
#define DLLDATA_CLSID_ARG(clsid) &PROXY_CLSID
#elif defined(PROXY_CLSID_IS)
#define DLLDATA_GETPROXYDLLCLSID(list, clsid) static const CLSID nova_proxy_clsid = PROXY_CLSID_IS;
#define DLLDATA_CLSID_ARG(clsid) &nova_proxy_clsid
#else
#define DLLDATA_GETPROXYDLLCLSID(list, clsid)
#define DLLDATA_CLSID_ARG(clsid) (clsid)
#endif
#ifdef REGISTER_PROXY_DLL
#define DLLREGISTRY_ROUTINES(list, clsid) \
    HINSTANCE hProxyDll = 0; \
    BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved) \
    { (void)reserved; if (reason == DLL_PROCESS_ATTACH) hProxyDll = dll; return TRUE; } \
    __declspec(dllexport) HRESULT STDAPICALLTYPE DllRegisterServer(void) { return NdrDllRegisterProxy(hProxyDll, list, DLLDATA_CLSID_ARG(clsid)); } \
    __declspec(dllexport) HRESULT STDAPICALLTYPE DllUnregisterServer(void) { return NdrDllUnregisterProxy(hProxyDll, list, DLLDATA_CLSID_ARG(clsid)); }
#else
#define DLLREGISTRY_ROUTINES(list, clsid)
#endif
#define DLLDATA_ROUTINES(list, clsid) \
    DLLDATA_GETPROXYDLLCLSID(list, clsid) \
    CStdPSFactoryBuffer gPFactory = { 0, 0, 0, 0 }; \
    __declspec(dllexport) HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) \
    { return NdrDllGetClassObject(rclsid, riid, ppv, list, DLLDATA_CLSID_ARG(clsid), &gPFactory); } \
    __declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow(void) { return NdrDllCanUnloadNow(&gPFactory); } \
    ULONG STDMETHODCALLTYPE CStdStubBuffer_Release(IRpcStubBuffer *This) { return NdrCStdStubBuffer_Release(This, (IPSFactoryBuffer *)&gPFactory); } \
    ULONG STDMETHODCALLTYPE CStdStubBuffer2_Release(IRpcStubBuffer *This) { return NdrCStdStubBuffer2_Release(This, (IPSFactoryBuffer *)&gPFactory); } \
    DLLREGISTRY_ROUTINES(list, clsid)

_NOVA_END
