/* rpcndr.h — NDR (Network Data Representation): the structures MIDL- and
 * widl-generated stubs fill in and the rpcrt4 entry points they call.
 * Layouts follow the documented Windows SDK ones, since compiled proxy/stub
 * DLLs (Microsoft's among them) hand these structures to rpcrt4. */
#pragma once
#include <rpc.h>
#include <string.h>

_NOVA_BEGIN

struct IRpcStubBuffer;
struct IRpcChannelBuffer;

#define __RPCNDR_H_VERSION__ 500
#define NDR_CHAR_REP_MASK     0x0000000FUL
#define NDR_INT_REP_MASK      0x000000F0UL
#define NDR_FLOAT_REP_MASK    0x0000FF00UL
#define NDR_LITTLE_ENDIAN     0x00000010UL
#define NDR_BIG_ENDIAN        0x00000000UL
#define NDR_IEEE_FLOAT        0x00000000UL
#define NDR_ASCII_CHAR        0x00000000UL
#define NDR_LOCAL_DATA_REPRESENTATION 0x00000010UL
#define NDR_LOCAL_ENDIAN      NDR_LITTLE_ENDIAN

#ifndef __MIDL_user_allocate_free_DEFINED__
#define __MIDL_user_allocate_free_DEFINED__
void *__RPC_USER MIDL_user_allocate(size_t n);
void __RPC_USER MIDL_user_free(void *p);
#endif

typedef unsigned char byte;
typedef byte cs_byte;
typedef unsigned char boolean;
#ifndef _HYPER_DEFINED
#define _HYPER_DEFINED
typedef __int64 hyper;
typedef unsigned __int64 MIDL_uhyper;
#endif
typedef char small;
typedef void *NDR_CCONTEXT;
typedef struct { void *pad[2]; void *userContext; } *NDR_SCONTEXT;
typedef void (__RPC_USER *NDR_RUNDOWN)(void *context);
typedef void (__RPC_USER *NDR_NOTIFY_ROUTINE)(void);
typedef void (__RPC_USER *NDR_NOTIFY2_ROUTINE)(boolean flag);
typedef const unsigned char *PFORMAT_STRING;

#define NdrFcShort(s) (unsigned char)((s) & 0xff), (unsigned char)((unsigned short)(s) >> 8)
#define NdrFcLong(s)  (unsigned char)((s) & 0xff), (unsigned char)(((s) >> 8) & 0xff), \
                      (unsigned char)(((s) >> 16) & 0xff), (unsigned char)((unsigned long)(s) >> 24)

typedef union _CLIENT_CALL_RETURN { void *Pointer; LONG_PTR Simple; } CLIENT_CALL_RETURN;

typedef struct _FULL_PTR_XLAT_TABLES FULL_PTR_XLAT_TABLES, *PFULL_PTR_XLAT_TABLES;
typedef struct _MIDL_STUB_DESC MIDL_STUB_DESC, *PMIDL_STUB_DESC;
typedef const MIDL_STUB_DESC *PCMIDL_STUB_DESC;
typedef struct _MIDL_SYNTAX_INFO MIDL_SYNTAX_INFO, *PMIDL_SYNTAX_INFO;
typedef struct _ARRAY_INFO { LONG Dimension; ULONG *BufferConformanceMark, *BufferVarianceMark, *MaxCountArray, *OffsetArray, *ActualCountArray; } ARRAY_INFO, *PARRAY_INFO;
typedef struct _NDR_ASYNC_MESSAGE *PNDR_ASYNC_MESSAGE;
typedef struct _NDR_CORRELATION_INFO *PNDR_CORRELATION_INFO;

typedef struct _MIDL_STUB_MESSAGE {
    PRPC_MESSAGE RpcMsg;
    unsigned char *Buffer;
    unsigned char *BufferStart;
    unsigned char *BufferEnd;
    unsigned char *BufferMark;
    unsigned long BufferLength;
    unsigned long MemorySize;
    unsigned char *Memory;
    unsigned char IsClient;
    unsigned char Pad;
    unsigned short uFlags2;
    int ReuseBuffer;
    struct NDR_ALLOC_ALL_NODES_CONTEXT *pAllocAllNodesContext;
    struct NDR_POINTER_QUEUE_STATE *pPointerQueueState;
    int IgnoreEmbeddedPointers;
    unsigned char *PointerBufferMark;
    unsigned char CorrDespIncrement;
    unsigned char uFlags;
    unsigned short UniquePtrCount;
    ULONG_PTR MaxCount;
    unsigned long Offset;
    unsigned long ActualCount;
    void *(__RPC_API *pfnAllocate)(size_t);
    void (__RPC_API *pfnFree)(void *);
    unsigned char *StackTop;
    unsigned char *pPresentedType;
    unsigned char *pTransmitType;
    handle_t SavedHandle;
    const struct _MIDL_STUB_DESC *StubDesc;
    struct _FULL_PTR_XLAT_TABLES *FullPtrXlatTables;
    unsigned long FullPtrRefId;
    unsigned long PointerLength;
    unsigned int fInDontFree : 1;
    unsigned int fDontCallFreeInst : 1;
    unsigned int fUnused1 : 1;
    unsigned int fHasReturn : 1;
    unsigned int fHasExtensions : 1;
    unsigned int fHasNewCorrDesc : 1;
    unsigned int fIsIn : 1;
    unsigned int fIsOut : 1;
    unsigned int fIsOicf : 1;
    unsigned int fBufferValid : 1;
    unsigned int fHasMemoryValidateCallback : 1;
    unsigned int fInFree : 1;
    unsigned int fNeedMCCP : 1;
    unsigned int fUnused2 : 3;
    unsigned int fUnused3 : 16;
    unsigned long dwDestContext;
    void *pvDestContext;
    NDR_SCONTEXT *SavedContextHandles;
    long ParamNumber;
    struct IRpcChannelBuffer *pRpcChannelBuffer;
    PARRAY_INFO pArrayInfo;
    unsigned long *SizePtrCountArray;
    unsigned long *SizePtrOffsetArray;
    unsigned long *SizePtrLengthArray;
    void *pArgQueue;
    unsigned long dwStubPhase;
    void *LowStackMark;
    PNDR_ASYNC_MESSAGE pAsyncMsg;
    PNDR_CORRELATION_INFO pCorrInfo;
    unsigned char *pCorrMemory;
    void *pMemoryList;
    INT_PTR pCSInfo;
    unsigned char *ConformanceMark;
    unsigned char *VarianceMark;
    INT_PTR Unused;
    struct _NDR_PROC_CONTEXT *pContext;
    void *ContextHandleHash;
    void *pUserMarshalList;
    INT_PTR Reserved51_3;
    INT_PTR Reserved51_4;
    INT_PTR Reserved51_5;
} MIDL_STUB_MESSAGE, *PMIDL_STUB_MESSAGE;

typedef void *(__RPC_API *GENERIC_BINDING_ROUTINE)(void *);
typedef void (__RPC_API *GENERIC_UNBIND_ROUTINE)(void *, unsigned char *);
typedef struct _GENERIC_BINDING_ROUTINE_PAIR { GENERIC_BINDING_ROUTINE pfnBind; GENERIC_UNBIND_ROUTINE pfnUnbind; } GENERIC_BINDING_ROUTINE_PAIR;
typedef struct __GENERIC_BINDING_INFO { void *pObj; unsigned int Size; GENERIC_BINDING_ROUTINE pfnBind; GENERIC_UNBIND_ROUTINE pfnUnbind; } GENERIC_BINDING_INFO, *PGENERIC_BINDING_INFO;
typedef void (__RPC_USER *EXPR_EVAL)(struct _MIDL_STUB_MESSAGE *);
typedef void (__RPC_USER *XMIT_HELPER_ROUTINE)(PMIDL_STUB_MESSAGE);
typedef struct _XMIT_ROUTINE_QUINTUPLE { XMIT_HELPER_ROUTINE pfnTranslateToXmit, pfnTranslateFromXmit, pfnFreeXmit, pfnFreeInst; } XMIT_ROUTINE_QUINTUPLE;
typedef unsigned long (__RPC_USER *USER_MARSHAL_SIZING_ROUTINE)(unsigned long *, unsigned long, void *);
typedef unsigned char *(__RPC_USER *USER_MARSHAL_MARSHALLING_ROUTINE)(unsigned long *, unsigned char *, void *);
typedef unsigned char *(__RPC_USER *USER_MARSHAL_UNMARSHALLING_ROUTINE)(unsigned long *, unsigned char *, void *);
typedef void (__RPC_USER *USER_MARSHAL_FREEING_ROUTINE)(unsigned long *, void *);
typedef struct _USER_MARSHAL_ROUTINE_QUADRUPLE {
    USER_MARSHAL_SIZING_ROUTINE pfnBufferSize;
    USER_MARSHAL_MARSHALLING_ROUTINE pfnMarshall;
    USER_MARSHAL_UNMARSHALLING_ROUTINE pfnUnmarshall;
    USER_MARSHAL_FREEING_ROUTINE pfnFree;
} USER_MARSHAL_ROUTINE_QUADRUPLE;
typedef enum _USER_MARSHAL_CB_TYPE { USER_MARSHAL_CB_BUFFER_SIZE, USER_MARSHAL_CB_MARSHALL, USER_MARSHAL_CB_UNMARSHALL, USER_MARSHAL_CB_FREE } USER_MARSHAL_CB_TYPE;
typedef struct _USER_MARSHAL_CB {
    unsigned long Flags;
    PMIDL_STUB_MESSAGE pStubMsg;
    PFORMAT_STRING pReserve;
    unsigned long Signature;
    USER_MARSHAL_CB_TYPE CBType;
    PFORMAT_STRING pFormat;
    PFORMAT_STRING pTypeFormat;
} USER_MARSHAL_CB;
#define USER_MARSHAL_CB_SIGNATURE 'USRC'
#define USER_CALL_CTXT_MASK(f)    ((f) & 0x00ff)
#define USER_CALL_AUX_MASK(f)     ((f) & 0xff00)
#define GET_USER_DATA_REP(f)      ((f) >> 16)
#define USER_CALL_IS_ASYNC        0x0100
#define USER_CALL_NEW_CORRELATION_DESC 0x0200
typedef struct _MALLOC_FREE_STRUCT { void *(__RPC_USER *pfnAllocate)(size_t); void (__RPC_USER *pfnFree)(void *); } MALLOC_FREE_STRUCT;
typedef struct _COMM_FAULT_OFFSETS { short CommOffset, FaultOffset; } COMM_FAULT_OFFSETS;
typedef struct _NDR_CS_ROUTINES NDR_CS_ROUTINES;
typedef struct _NDR_EXPR_DESC NDR_EXPR_DESC;

struct _MIDL_STUB_DESC {
    void *RpcInterfaceInformation;
    void *(__RPC_API *pfnAllocate)(size_t);
    void (__RPC_API *pfnFree)(void *);
    union { handle_t *pAutoHandle; handle_t *pPrimitiveHandle; PGENERIC_BINDING_INFO pGenericBindingInfo; } IMPLICIT_HANDLE_INFO;
    const NDR_RUNDOWN *apfnNdrRundownRoutines;
    const GENERIC_BINDING_ROUTINE_PAIR *aGenericBindingRoutinePairs;
    const EXPR_EVAL *apfnExprEval;
    const XMIT_ROUTINE_QUINTUPLE *aXmitQuintuple;
    const unsigned char *pFormatTypes;
    int fCheckBounds;
    unsigned long Version;
    MALLOC_FREE_STRUCT *pMallocFreeStruct;
    long MIDLVersion;
    const COMM_FAULT_OFFSETS *CommFaultOffsets;
    const USER_MARSHAL_ROUTINE_QUADRUPLE *aUserMarshalQuadruple;
    const NDR_NOTIFY_ROUTINE *NotifyRoutineTable;
    ULONG_PTR mFlags;
    const NDR_CS_ROUTINES *CsRoutineTables;
    void *ProxyServerInfo;
    const NDR_EXPR_DESC *pExprInfo;
};

typedef void (__RPC_API *SERVER_ROUTINE)(void);
typedef void (__RPC_API *STUB_THUNK)(PMIDL_STUB_MESSAGE);
struct _MIDL_SYNTAX_INFO {
    RPC_SYNTAX_IDENTIFIER TransferSyntax;
    void *DispatchTable;
    PFORMAT_STRING ProcString;
    const unsigned short *FmtStringOffset;
    PFORMAT_STRING TypeString;
    const void *aUserMarshalQuadruple;
    const void *pMethodProperties;
    ULONG_PTR pReserved2;
};
typedef struct _MIDL_SERVER_INFO_ {
    PMIDL_STUB_DESC pStubDesc;
    const SERVER_ROUTINE *DispatchTable;
    PFORMAT_STRING ProcString;
    const unsigned short *FmtStringOffset;
    const STUB_THUNK *ThunkTable;
    PRPC_SYNTAX_IDENTIFIER pTransferSyntax;
    ULONG_PTR nCount;
    PMIDL_SYNTAX_INFO pSyntaxInfo;
} MIDL_SERVER_INFO, *PMIDL_SERVER_INFO;
typedef struct _MIDL_STUBLESS_PROXY_INFO {
    PMIDL_STUB_DESC pStubDesc;
    PFORMAT_STRING ProcFormatString;
    const unsigned short *FormatStringOffset;
    PRPC_SYNTAX_IDENTIFIER pTransferSyntax;
    ULONG_PTR nCount;
    PMIDL_SYNTAX_INFO pSyntaxInfo;
} MIDL_STUBLESS_PROXY_INFO, *PMIDL_STUBLESS_PROXY_INFO;

/* the interpreter */
RPCRTAPI CLIENT_CALL_RETURN RPC_VAR_ENTRY NdrClientCall2(PMIDL_STUB_DESC desc, PFORMAT_STRING fmt, ...);
RPCRTAPI long RPC_ENTRY NdrStubCall2(struct IRpcStubBuffer *stub, struct IRpcChannelBuffer *chan, PRPC_MESSAGE msg, unsigned long *phase);
RPCRTAPI void *RPC_ENTRY NdrOleAllocate(size_t n);
RPCRTAPI void RPC_ENTRY NdrOleFree(void *p);
RPCRTAPI void RPC_ENTRY NdrConvert(PMIDL_STUB_MESSAGE msg, PFORMAT_STRING fmt);
RPCRTAPI void RPC_ENTRY NdrConvert2(PMIDL_STUB_MESSAGE msg, PFORMAT_STRING fmt, long n);
RPCRTAPI void RPC_ENTRY NdrClearOutParameters(PMIDL_STUB_MESSAGE msg, PFORMAT_STRING fmt, void *arg);
RPCRTAPI void *RPC_ENTRY NdrAllocate(PMIDL_STUB_MESSAGE msg, size_t n);

#define NDR_TYPE_ROUTINES(name) \
    RPCRTAPI unsigned char *RPC_ENTRY Ndr##name##Marshall(PMIDL_STUB_MESSAGE m, unsigned char *mem, PFORMAT_STRING f); \
    RPCRTAPI unsigned char *RPC_ENTRY Ndr##name##Unmarshall(PMIDL_STUB_MESSAGE m, unsigned char **mem, PFORMAT_STRING f, unsigned char alloc); \
    RPCRTAPI void RPC_ENTRY Ndr##name##BufferSize(PMIDL_STUB_MESSAGE m, unsigned char *mem, PFORMAT_STRING f); \
    RPCRTAPI unsigned long RPC_ENTRY Ndr##name##MemorySize(PMIDL_STUB_MESSAGE m, PFORMAT_STRING f); \
    RPCRTAPI void RPC_ENTRY Ndr##name##Free(PMIDL_STUB_MESSAGE m, unsigned char *mem, PFORMAT_STRING f);
NDR_TYPE_ROUTINES(Pointer)
NDR_TYPE_ROUTINES(SimpleStruct)
NDR_TYPE_ROUTINES(ConformantStruct)
NDR_TYPE_ROUTINES(ComplexStruct)
NDR_TYPE_ROUTINES(FixedArray)
NDR_TYPE_ROUTINES(ConformantArray)
NDR_TYPE_ROUTINES(ConformantVaryingArray)
NDR_TYPE_ROUTINES(ComplexArray)
NDR_TYPE_ROUTINES(ConformantString)
NDR_TYPE_ROUTINES(InterfacePointer)
NDR_TYPE_ROUTINES(UserMarshal)
#undef NDR_TYPE_ROUTINES
RPCRTAPI void RPC_ENTRY NdrSimpleTypeMarshall(PMIDL_STUB_MESSAGE m, unsigned char *mem, unsigned char fc);
RPCRTAPI void RPC_ENTRY NdrSimpleTypeUnmarshall(PMIDL_STUB_MESSAGE m, unsigned char *mem, unsigned char fc);

_NOVA_END
