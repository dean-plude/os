/*
 * ndr.h — rpcrt4's NDR engine, shared between its files.
 *
 * The format strings MIDL and widl emit (the "Oicf" interpreted mode) are
 * read here: ndr_types.c marshals, unmarshals, sizes and frees one type,
 * ndr_proc.c runs whole calls (stubless proxies on the client, NdrStubCall2
 * in the server), ndr_ole.c holds the COM side (proxies, stubs, the
 * proxy/stub class factory, registration).
 */
#pragma once
#define NOVA_BUILD_RPCRT4
#define USE_STUBLESS_PROXY
#include <rpcproxy.h>

/* format characters */
enum {
    FC_BYTE = 0x01, FC_CHAR, FC_SMALL, FC_USMALL, FC_WCHAR, FC_SHORT, FC_USHORT, FC_LONG, FC_ULONG,
    FC_FLOAT, FC_HYPER, FC_DOUBLE, FC_ENUM16, FC_ENUM32, FC_IGNORE, FC_ERROR_STATUS_T,
    FC_RP = 0x11, FC_UP, FC_OP, FC_FP,
    FC_STRUCT = 0x15, FC_PSTRUCT, FC_CSTRUCT, FC_CPSTRUCT, FC_CVSTRUCT, FC_BOGUS_STRUCT,
    FC_CARRAY = 0x1b, FC_CVARRAY, FC_SMFARRAY, FC_LGFARRAY, FC_SMVARRAY, FC_LGVARRAY, FC_BOGUS_ARRAY,
    FC_C_CSTRING = 0x22, FC_C_BSTRING, FC_C_SSTRING, FC_C_WSTRING, FC_CSTRING, FC_BSTRING, FC_SSTRING, FC_WSTRING,
    FC_ENCAPSULATED_UNION = 0x2a, FC_NON_ENCAPSULATED_UNION, FC_BYTE_COUNT_POINTER, FC_TRANSMIT_AS, FC_REPRESENT_AS,
    FC_IP = 0x2f,
    FC_BIND_CONTEXT = 0x30, FC_BIND_GENERIC, FC_BIND_PRIMITIVE, FC_AUTO_HANDLE, FC_CALLBACK_HANDLE,
    FC_POINTER = 0x36, FC_ALIGNM2, FC_ALIGNM4, FC_ALIGNM8,
    FC_STRUCTPAD1 = 0x3d, FC_STRUCTPAD2, FC_STRUCTPAD3, FC_STRUCTPAD4, FC_STRUCTPAD5, FC_STRUCTPAD6, FC_STRUCTPAD7,
    FC_STRING_SIZED = 0x44,
    FC_NO_REPEAT = 0x46, FC_FIXED_REPEAT, FC_VARIABLE_REPEAT, FC_FIXED_OFFSET, FC_VARIABLE_OFFSET, FC_PP,
    FC_EMBEDDED_COMPLEX = 0x4c,
    FC_DEREFERENCE = 0x54, FC_DIV_2, FC_MULT_2, FC_ADD_1, FC_SUB_1, FC_CALLBACK, FC_CONSTANT_IID,
    FC_END = 0x5b, FC_PAD = 0x5c,
    FC_USER_MARSHAL = 0xb4, FC_RANGE = 0xb7, FC_INT3264 = 0xb8, FC_UINT3264 = 0xb9,
};
/* pointer attributes (the byte after FC_RP/FC_UP/...) */
#define FC_ALLOCATE_ALL_NODES 0x01
#define FC_DONT_FREE          0x02
#define FC_ALLOCED_ON_STACK   0x04
#define FC_SIMPLE_POINTER     0x08
#define FC_POINTER_DEREF      0x10
/* correlation descriptor kinds (high nibble of its first byte) */
#define FC_NORMAL_CONFORMANCE    0x00
#define FC_POINTER_CONFORMANCE   0x10
#define FC_TOP_LEVEL_CONFORMANCE 0x20
#define FC_CONSTANT_CONFORMANCE  0x40
/* Oi procedure header flags */
#define Oi_HAS_RPCFLAGS 0x08
#define Oi_OBJECT_PROC  0x04
/* parameter attributes (PARAM_ATTRIBUTES) */
#define PA_MUST_SIZE   0x0001
#define PA_MUST_FREE   0x0002
#define PA_IN          0x0008
#define PA_OUT         0x0010
#define PA_RETURN      0x0020
#define PA_BASETYPE    0x0040
#define PA_BYVALUE     0x0080
#define PA_SIMPLEREF   0x0100
#define PA_DONT_FREE_INST 0x0200
/* user marshal flags (high nibble of the byte after FC_USER_MARSHAL) */
#define USER_MARSHAL_UNIQUE  0x80
#define USER_MARSHAL_REF     0x40
#define USER_MARSHAL_POINTER 0xc0
#define USER_MARSHAL_PTR_PREFIX 0x72657355            /* "User" */

static inline unsigned short rd16(const unsigned char *p) { return (unsigned short)(p[0] | p[1] << 8); }
static inline short rds16(const unsigned char *p) { return (short)rd16(p); }
static inline unsigned long rd32(const unsigned char *p) { return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24; }

/* the engine's error, kept in the stub message (a field Windows leaves
 * unused): the first failure wins, later reads and writes do nothing */
#define NDR_ERR(sm) ((sm)->Reserved51_3)
void ndr_fail(PMIDL_STUB_MESSAGE sm, long status);
#define NDR_FAILED(sm) (NDR_ERR(sm) != 0)

/* buffer access, aligned relative to the buffer's start */
void ndr_align(PMIDL_STUB_MESSAGE sm, unsigned n);
void ndr_size_align(PMIDL_STUB_MESSAGE sm, unsigned n);
unsigned char *ndr_take(PMIDL_STUB_MESSAGE sm, unsigned long n);      /* NULL (and failed) if past the end */
void ndr_put(PMIDL_STUB_MESSAGE sm, const void *p, unsigned long n);
void ndr_put32(PMIDL_STUB_MESSAGE sm, unsigned long v);
unsigned long ndr_get32(PMIDL_STUB_MESSAGE sm);
void *ndr_alloc(PMIDL_STUB_MESSAGE sm, size_t n);                    /* zeroed, the stub descriptor's allocator */
void ndr_free(PMIDL_STUB_MESSAGE sm, void *p);

/* one type: @mem is what the MS Ndr*Marshall routines take (the pointer
 * value for pointers and interface pointers, the data's address otherwise);
 * @pmem is what the Unmarshall routines take (where that is stored) */
void ndr_size_type(PMIDL_STUB_MESSAGE sm, unsigned char *mem, PFORMAT_STRING f);
void ndr_marshal_type(PMIDL_STUB_MESSAGE sm, unsigned char *mem, PFORMAT_STRING f);
void ndr_unmarshal_type(PMIDL_STUB_MESSAGE sm, unsigned char **pmem, PFORMAT_STRING f, int alloc);
void ndr_free_type(PMIDL_STUB_MESSAGE sm, unsigned char *mem, PFORMAT_STRING f);
unsigned long ndr_memsize(PMIDL_STUB_MESSAGE sm, PFORMAT_STRING f);   /* 0: not fixed */
int ndr_is_pointer(unsigned char fc);
unsigned ndr_base_size(unsigned char fc);                            /* bytes on the wire (0: not a base type) */
unsigned ndr_base_memsize(unsigned char fc);
void ndr_base_size_type(PMIDL_STUB_MESSAGE sm, unsigned char fc);
void ndr_base_marshal(PMIDL_STUB_MESSAGE sm, const unsigned char *mem, unsigned char fc);
void ndr_base_unmarshal(PMIDL_STUB_MESSAGE sm, unsigned char *mem, unsigned char fc);
/* the size of an array or string a correlation descriptor gives;
 * returns the descriptor's end */
PFORMAT_STRING ndr_conformance(PMIDL_STUB_MESSAGE sm, unsigned char *base, PFORMAT_STRING desc, ULONG_PTR *out);

/* the COM pieces ole32 provides (loaded on first use: rpcrt4 does not
 * import ole32, which loads proxy DLLs that import rpcrt4) */
typedef struct {
    HRESULT (WINAPI *CoMarshalInterface)(IStream *, REFIID, IUnknown *, DWORD, void *, DWORD);
    HRESULT (WINAPI *CoUnmarshalInterface)(IStream *, REFIID, void **);
    HRESULT (WINAPI *CoGetMarshalSizeMax)(ULONG *, REFIID, IUnknown *, DWORD, void *, DWORD);
    HRESULT (WINAPI *CoReleaseMarshalData)(IStream *);
    HRESULT (WINAPI *CoGetPSClsid)(REFIID, CLSID *);
    HRESULT (WINAPI *CoGetClassObject)(REFCLSID, DWORD, void *, REFIID, void **);
    void *(WINAPI *CoTaskMemAlloc)(SIZE_T);
    void (WINAPI *CoTaskMemFree)(void *);
} Ole32Fns;
const Ole32Fns *ndr_ole32(void);
/* the PS factory that serves @iid (CoGetPSClsid, then that class's IPSFactoryBuffer) */
HRESULT ndr_ps_factory(REFIID iid, IPSFactoryBuffer **out);
