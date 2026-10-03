/*
 * NovaOS: the MSVC C++ exception data (what `throw` emits and the EH
 * runtime reads), as the VC runtime's private <ehdata.h> names it, for
 * the STL's exception_ptr (excptptr.cpp).  The same layout NovaOS's own
 * vcruntime140 (userland/vcruntime140/eh.c) uses.
 */
#pragma once
#include <windows.h>

#if defined(_M_X64) || defined(_M_ARM64)
#define _EH_RELATIVE_TYPEINFO 1
#define _EH_PTR(t) int
#else
#define _EH_RELATIVE_TYPEINFO 0
#define _EH_PTR(t) t
#endif

#define EH_EXCEPTION_NUMBER    ('msc' | 0xE0000000)
#define EH_MAGIC_NUMBER1       0x19930520
#define EH_MAGIC_NUMBER2       0x19930521
#define EH_MAGIC_NUMBER3       0x19930522
#define EH_PURE_MAGIC_NUMBER1  0x01994000
#if _EH_RELATIVE_TYPEINFO
#define EH_EXCEPTION_PARAMETERS 4
#else
#define EH_EXCEPTION_PARAMETERS 3
#endif

#define CT_IsSimpleType    0x00000001
#define CT_ByReferenceOnly 0x00000002
#define CT_HasVirtualBase  0x00000004
#define CT_IsWinRTHandle   0x00000008
#define CT_IsStdBadAlloc   0x00000010

#define TI_IsConst     0x00000001
#define TI_IsVolatile  0x00000002
#define TI_IsUnaligned 0x00000004
#define TI_IsPure      0x00000008
#define TI_IsWinRT     0x00000010

typedef struct PMD {
    int mdisp;
    int pdisp;
    int vdisp;
} PMD;

typedef struct TypeDescriptor {
    const void *pVFTable;
    void *spare;
    char name[1];
} TypeDescriptor;

typedef const struct _s_CatchableType {
    unsigned int properties;
    _EH_PTR(TypeDescriptor *) pType;
    PMD thisDisplacement;
    int sizeOrOffset;
    _EH_PTR(void *) copyFunction;
} CatchableType;

typedef const struct _s_CatchableTypeArray {
    int nCatchableTypes;
    _EH_PTR(CatchableType *) arrayOfCatchableTypes[1];
} CatchableTypeArray;

typedef const struct _s_ThrowInfo {
    unsigned int attributes;
    _EH_PTR(void *) pmfnUnwind;
    _EH_PTR(void *) pForwardCompat;
    _EH_PTR(CatchableTypeArray *) pCatchableTypeArray;
} ThrowInfo;

/* a Windows Runtime exception's information (never thrown on NovaOS) */
typedef struct WINRTEXCEPTIONINFO {
    void *description;
    void *restrictedErrorString;
    void *restrictedErrorReference;
    void *capabilitySid;
    long hr;
    void *restrictedInfo;
    ThrowInfo *throwInfo;
    unsigned int size;
    void *PrepareThrow;
} WINRTEXCEPTIONINFO;

typedef struct EHExceptionRecord {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    struct EHParameters {
        DWORD magicNumber;
        PVOID pExceptionObject;
        ThrowInfo *pThrowInfo;
#if _EH_RELATIVE_TYPEINFO
        PVOID pThrowImageBase;
#endif
    } params;
} EHExceptionRecord;

#define PER_IS_MSVC_EH(p) ((p)->ExceptionCode == EH_EXCEPTION_NUMBER && (p)->NumberParameters == EH_EXCEPTION_PARAMETERS && \
    ((p)->params.magicNumber == EH_MAGIC_NUMBER1 || (p)->params.magicNumber == EH_MAGIC_NUMBER2 || \
     (p)->params.magicNumber == EH_MAGIC_NUMBER3))
#define PER_IS_MSVC_PURE_OR_NATIVE_EH(p) (PER_IS_MSVC_EH(p) || ((p)->ExceptionCode == EH_EXCEPTION_NUMBER && \
    (p)->NumberParameters == EH_EXCEPTION_PARAMETERS && (p)->params.magicNumber == EH_PURE_MAGIC_NUMBER1))
