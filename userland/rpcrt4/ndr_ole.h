/* ndr_ole.h — rpcrt4's proxy and stub objects (ndr_ole.c), as ndr_proc.c sees them */
#pragma once
#include "ndr.h"

#define NDR_MAX_METHODS 1024        /* stubless thunks: methods 0..1023 */
#define NDR_MAX_FORWARD 256         /* methods a delegated base interface may have */
#define NDR_STR_(x) #x
#define NDR_STR(x) NDR_STR_(x)

/* an interface proxy; the pointer handed out is &pProxyVtbl.  The first
 * fields are Windows' CStdProxyBuffer's */
typedef struct StdProxy {
    const IRpcProxyBufferVtbl *lpVtbl;
    const void *pProxyVtbl;
    LONG refs;
    IUnknown *outer;
    IRpcChannelBuffer *chan;
    IPSFactoryBuffer *factory;
    IUnknown *base_proxy;                   /* the delegated base interface's proxy (its interface) */
    IRpcProxyBuffer *base_buf;
    const IID *iid;
    const MIDL_STUBLESS_PROXY_INFO *info;
} StdProxy;
#define PROXY_FROM_IFACE(p) ((StdProxy *)((char *)(p) - sizeof(void *)))
/* base_proxy's offset from the interface pointer (the forwarding thunks read it) */
#ifdef _WIN64
#define PROXY_BASE_OFFSET 40
#else
#define PROXY_BASE_OFFSET 20
#endif

/* a stub's CInterfaceStubHeader sits just before its vtable */
#define STUB_HEADER(stub) ((const CInterfaceStubHeader *)((const char *)((CStdStubBuffer *)(stub))->lpVtbl - sizeof(CInterfaceStubHeader)))
/* a delegating stub keeps its base interface's stub just before itself */
typedef struct { IRpcStubBuffer *base_stub; CStdStubBuffer sb; } DelegStub;
#define DELEGATED_BASE_STUB(stub) (((DelegStub *)((char *)(stub) - sizeof(void *)))->base_stub)

HRESULT ndr_invoke_stub(IRpcStubBuffer *This, RPCOLEMESSAGE *msg, IRpcChannelBuffer *chan);
