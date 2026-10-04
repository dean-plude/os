/* rpc.h — the RPC runtime's basic types (what MIDL- and widl-generated code includes first) */
#pragma once
#include <windows.h>

_NOVA_BEGIN

#ifndef RPC_ENTRY
#define RPC_ENTRY     __stdcall
#define RPC_VAR_ENTRY __cdecl
#define RPC_USER
#define __RPC_API     __stdcall
#define __RPC_USER    __stdcall
#define __RPC_STUB    __stdcall
#define __RPC_CALLEE  __stdcall
#define __RPC_FAR
#define RPC_FAR
#endif
#if defined(NOVA_BUILD_RPCRT4)
#define RPCRTAPI __declspec(dllexport)
#else
#define RPCRTAPI __declspec(dllimport)
#endif
#define RPCNSAPI RPCRTAPI

#ifdef _WIN64
#define __RPC_WIN64__
#else
#define __RPC_WIN32__
#endif

typedef long RPC_STATUS;
typedef void *I_RPC_HANDLE;
typedef I_RPC_HANDLE RPC_BINDING_HANDLE;
typedef RPC_BINDING_HANDLE handle_t;
typedef void *RPC_IF_HANDLE;
typedef unsigned char *RPC_CSTR;
typedef unsigned short *RPC_WSTR;

typedef struct _RPC_VERSION { unsigned short MajorVersion, MinorVersion; } RPC_VERSION;
typedef struct _RPC_SYNTAX_IDENTIFIER { GUID SyntaxGUID; RPC_VERSION SyntaxVersion; } RPC_SYNTAX_IDENTIFIER, *PRPC_SYNTAX_IDENTIFIER;

typedef struct _RPC_MESSAGE {
    RPC_BINDING_HANDLE Handle;
    unsigned long DataRepresentation;
    void *Buffer;
    unsigned int BufferLength;
    unsigned int ProcNum;
    PRPC_SYNTAX_IDENTIFIER TransferSyntax;
    void *RpcInterfaceInformation;
    void *ReservedForRuntime;
    void *ManagerEpv;
    void *ImportContext;
    unsigned long RpcFlags;
} RPC_MESSAGE, *PRPC_MESSAGE;

#define RPC_S_OK                    0
#define RPC_S_INVALID_ARG           87
#define RPC_S_OUT_OF_MEMORY         14
#define RPC_S_INVALID_BOUND         1734
#define RPC_S_SERVER_UNAVAILABLE    1722
#define RPC_S_CALL_FAILED           1726
#define RPC_S_PROCNUM_OUT_OF_RANGE  1745
#define RPC_X_NULL_REF_POINTER      1780
#define RPC_X_BAD_STUB_DATA         1783
#define RPC_X_INVALID_BOUND         1734
#define RPC_X_ENUM_VALUE_OUT_OF_RANGE 1781
#define RPC_X_BYTE_COUNT_TOO_SMALL  1782
#define RPC_S_INTERNAL_ERROR        1766
#define RPC_X_SS_IN_NULL_CONTEXT    1775

RPCRTAPI void RPC_ENTRY RpcRaiseException(RPC_STATUS status);

_NOVA_END
