/* winnt.h — NT base definitions: exceptions, CONTEXT, PE images, locks,
 * interlocked operations (included by windows.h after the base types) */
#pragma once
_NOVA_BEGIN

typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;

/* -----------------------------------------------------------------------
 * Exceptions
 * ----------------------------------------------------------------------- */
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct __declspec(align(16)) _M128A { ULONGLONG Low; LONGLONG High; } M128A, *PM128A;

typedef struct __declspec(align(16)) _XSAVE_FORMAT {
    WORD ControlWord, StatusWord;
    BYTE TagWord, Reserved1;
    WORD ErrorOpcode;
    DWORD ErrorOffset;
    WORD ErrorSelector, Reserved2;
    DWORD DataOffset;
    WORD DataSelector, Reserved3;
    DWORD MxCsr, MxCsr_Mask;
    M128A FloatRegisters[8];
    M128A XmmRegisters[16];
    BYTE Reserved4[96];
} XSAVE_FORMAT, XMM_SAVE_AREA32;

typedef struct __declspec(align(16)) _CONTEXT {
    DWORD64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    DWORD ContextFlags;
    DWORD MxCsr;
    WORD SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD EFlags;
    DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    DWORD64 R8, R9, R10, R11, R12, R13, R14, R15;
    DWORD64 Rip;
    union {
        XMM_SAVE_AREA32 FltSave;
        struct {
            M128A Header[2];
            M128A Legacy[8];
            M128A Xmm0, Xmm1, Xmm2, Xmm3, Xmm4, Xmm5, Xmm6, Xmm7;
            M128A Xmm8, Xmm9, Xmm10, Xmm11, Xmm12, Xmm13, Xmm14, Xmm15;
        };
    };
    M128A VectorRegister[26];
    DWORD64 VectorControl;
    DWORD64 DebugControl, LastBranchToRip, LastBranchFromRip, LastExceptionToRip, LastExceptionFromRip;
} CONTEXT, *PCONTEXT, *LPCONTEXT;

#define CONTEXT_AMD64            0x00100000L
#define CONTEXT_CONTROL          (CONTEXT_AMD64 | 0x1L)
#define CONTEXT_INTEGER          (CONTEXT_AMD64 | 0x2L)
#define CONTEXT_SEGMENTS         (CONTEXT_AMD64 | 0x4L)
#define CONTEXT_FLOATING_POINT   (CONTEXT_AMD64 | 0x8L)
#define CONTEXT_DEBUG_REGISTERS  (CONTEXT_AMD64 | 0x10L)
#define CONTEXT_FULL             (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)
#define CONTEXT_ALL              (CONTEXT_FULL | CONTEXT_SEGMENTS | CONTEXT_DEBUG_REGISTERS)

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;

#define EXCEPTION_NONCONTINUABLE     0x01
#define EXCEPTION_UNWINDING          0x02
#define EXCEPTION_EXIT_UNWIND        0x04
#define EXCEPTION_STACK_INVALID      0x08
#define EXCEPTION_NESTED_CALL        0x10
#define EXCEPTION_TARGET_UNWIND      0x20
#define EXCEPTION_COLLIDED_UNWIND    0x40
#define EXCEPTION_UNWIND             (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND | EXCEPTION_TARGET_UNWIND | EXCEPTION_COLLIDED_UNWIND)

#define STATUS_WAIT_0                    ((DWORD)0x00000000L)
#define STATUS_ABANDONED_WAIT_0          ((DWORD)0x00000080L)
#define STATUS_TIMEOUT                   ((DWORD)0x00000102L)
#define STATUS_GUARD_PAGE_VIOLATION      ((DWORD)0x80000001L)
#define STATUS_DATATYPE_MISALIGNMENT     ((DWORD)0x80000002L)
#define STATUS_BREAKPOINT                ((DWORD)0x80000003L)
#define STATUS_SINGLE_STEP               ((DWORD)0x80000004L)
#define STATUS_IN_PAGE_ERROR             ((DWORD)0xC0000006L)
#define STATUS_INVALID_HANDLE_EXC        ((DWORD)0xC0000008L)
#define STATUS_NONCONTINUABLE_EXCEPTION  ((DWORD)0xC0000025L)
#define STATUS_INVALID_DISPOSITION       ((DWORD)0xC0000026L)
#define STATUS_UNWIND_CONSOLIDATE        ((DWORD)0x80000029L)
#define STATUS_ILLEGAL_INSTRUCTION       ((DWORD)0xC000001DL)
#define STATUS_ARRAY_BOUNDS_EXCEEDED     ((DWORD)0xC000008CL)
#define STATUS_FLOAT_DENORMAL_OPERAND    ((DWORD)0xC000008DL)
#define STATUS_FLOAT_DIVIDE_BY_ZERO      ((DWORD)0xC000008EL)
#define STATUS_FLOAT_INEXACT_RESULT      ((DWORD)0xC000008FL)
#define STATUS_FLOAT_INVALID_OPERATION   ((DWORD)0xC0000090L)
#define STATUS_FLOAT_OVERFLOW            ((DWORD)0xC0000091L)
#define STATUS_FLOAT_STACK_CHECK         ((DWORD)0xC0000092L)
#define STATUS_FLOAT_UNDERFLOW           ((DWORD)0xC0000093L)
#define STATUS_INTEGER_DIVIDE_BY_ZERO    ((DWORD)0xC0000094L)
#define STATUS_INTEGER_OVERFLOW          ((DWORD)0xC0000095L)
#define STATUS_PRIVILEGED_INSTRUCTION    ((DWORD)0xC0000096L)
#define STATUS_STACK_OVERFLOW            ((DWORD)0xC00000FDL)
#define STATUS_CONTROL_C_EXIT            ((DWORD)0xC000013AL)
#define STATUS_FLOAT_MULTIPLE_FAULTS     ((DWORD)0xC00002B4L)
#define STATUS_FLOAT_MULTIPLE_TRAPS      ((DWORD)0xC00002B5L)
#define STATUS_STACK_BUFFER_OVERRUN      ((DWORD)0xC0000409L)
#define STATUS_ASSERTION_FAILURE         ((DWORD)0xC0000420L)

#define EXCEPTION_ACCESS_VIOLATION          ((DWORD)0xC0000005L)
#define EXCEPTION_DATATYPE_MISALIGNMENT     STATUS_DATATYPE_MISALIGNMENT
#define EXCEPTION_BREAKPOINT                STATUS_BREAKPOINT
#define EXCEPTION_SINGLE_STEP               STATUS_SINGLE_STEP
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED     STATUS_ARRAY_BOUNDS_EXCEEDED
#define EXCEPTION_FLT_DENORMAL_OPERAND      STATUS_FLOAT_DENORMAL_OPERAND
#define EXCEPTION_FLT_DIVIDE_BY_ZERO        STATUS_FLOAT_DIVIDE_BY_ZERO
#define EXCEPTION_FLT_INEXACT_RESULT        STATUS_FLOAT_INEXACT_RESULT
#define EXCEPTION_FLT_INVALID_OPERATION     STATUS_FLOAT_INVALID_OPERATION
#define EXCEPTION_FLT_OVERFLOW              STATUS_FLOAT_OVERFLOW
#define EXCEPTION_FLT_STACK_CHECK           STATUS_FLOAT_STACK_CHECK
#define EXCEPTION_FLT_UNDERFLOW             STATUS_FLOAT_UNDERFLOW
#define EXCEPTION_INT_DIVIDE_BY_ZERO        STATUS_INTEGER_DIVIDE_BY_ZERO
#define EXCEPTION_INT_OVERFLOW              STATUS_INTEGER_OVERFLOW
#define EXCEPTION_PRIV_INSTRUCTION          STATUS_PRIVILEGED_INSTRUCTION
#define EXCEPTION_IN_PAGE_ERROR             STATUS_IN_PAGE_ERROR
#define EXCEPTION_ILLEGAL_INSTRUCTION       STATUS_ILLEGAL_INSTRUCTION
#define EXCEPTION_NONCONTINUABLE_EXCEPTION  STATUS_NONCONTINUABLE_EXCEPTION
#define EXCEPTION_STACK_OVERFLOW            STATUS_STACK_OVERFLOW
#define EXCEPTION_INVALID_DISPOSITION       STATUS_INVALID_DISPOSITION
#define EXCEPTION_GUARD_PAGE                STATUS_GUARD_PAGE_VIOLATION
#define EXCEPTION_INVALID_HANDLE            STATUS_INVALID_HANDLE_EXC

typedef enum _EXCEPTION_DISPOSITION {
    ExceptionContinueExecution, ExceptionContinueSearch, ExceptionNestedException, ExceptionCollidedUnwind
} EXCEPTION_DISPOSITION;

/* x64 unwind data (.pdata / .xdata) */
typedef struct _RUNTIME_FUNCTION { DWORD BeginAddress, EndAddress, UnwindData; } RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;

#define UNW_FLAG_NHANDLER   0x0
#define UNW_FLAG_EHANDLER   0x1
#define UNW_FLAG_UHANDLER   0x2
#define UNW_FLAG_CHAININFO  0x4

#define UNWIND_HISTORY_TABLE_SIZE 12
typedef struct _UNWIND_HISTORY_TABLE_ENTRY { DWORD64 ImageBase; PRUNTIME_FUNCTION FunctionEntry; } UNWIND_HISTORY_TABLE_ENTRY;
typedef struct _UNWIND_HISTORY_TABLE {
    DWORD Count; BYTE LocalHint, GlobalHint, Search, Once;
    DWORD64 LowAddress, HighAddress;
    UNWIND_HISTORY_TABLE_ENTRY Entry[UNWIND_HISTORY_TABLE_SIZE];
} UNWIND_HISTORY_TABLE, *PUNWIND_HISTORY_TABLE;

typedef struct _KNONVOLATILE_CONTEXT_POINTERS {
    PM128A FloatingContext[16];
    PDWORD64 IntegerContext[16];
} KNONVOLATILE_CONTEXT_POINTERS, *PKNONVOLATILE_CONTEXT_POINTERS;

struct _DISPATCHER_CONTEXT;
typedef EXCEPTION_DISPOSITION (*PEXCEPTION_ROUTINE)(PEXCEPTION_RECORD rec, PVOID frame, PCONTEXT ctx,
                                                    struct _DISPATCHER_CONTEXT *dc);
typedef struct _DISPATCHER_CONTEXT {
    DWORD64 ControlPc;
    DWORD64 ImageBase;
    PRUNTIME_FUNCTION FunctionEntry;
    DWORD64 EstablisherFrame;
    DWORD64 TargetIp;
    PCONTEXT ContextRecord;
    PEXCEPTION_ROUTINE LanguageHandler;
    PVOID HandlerData;
    PUNWIND_HISTORY_TABLE HistoryTable;
    DWORD ScopeIndex;
    DWORD Fill0;
} DISPATCHER_CONTEXT, *PDISPATCHER_CONTEXT;

/* __C_specific_handler's scope table (clang/MSVC __try) */
typedef struct _SCOPE_TABLE_AMD64 {
    DWORD Count;
    struct { DWORD BeginAddress, EndAddress, HandlerAddress, JumpTarget; } ScopeRecord[1];
} SCOPE_TABLE_AMD64, *PSCOPE_TABLE_AMD64;

typedef LONG (WINAPI *PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS info);
typedef LONG (WINAPI *PTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS info);
typedef PTOP_LEVEL_EXCEPTION_FILTER LPTOP_LEVEL_EXCEPTION_FILTER;

/* -----------------------------------------------------------------------
 * PE images
 * ----------------------------------------------------------------------- */
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE  0x00004550
typedef struct _IMAGE_DOS_HEADER {
    WORD e_magic, e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc, e_ss, e_sp, e_csum, e_ip, e_cs,
         e_lfarlc, e_ovno, e_res[4], e_oemid, e_oeminfo, e_res2[10];
    LONG e_lfanew;
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;
typedef struct _IMAGE_FILE_HEADER {
    WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
    WORD SizeOfOptionalHeader, Characteristics;
} IMAGE_FILE_HEADER;
typedef struct _IMAGE_DATA_DIRECTORY { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct _IMAGE_OPTIONAL_HEADER64 {
    WORD Magic; BYTE MajorLinkerVersion, MinorLinkerVersion;
    DWORD SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode;
    ULONGLONG ImageBase; DWORD SectionAlignment, FileAlignment;
    WORD MajorOperatingSystemVersion, MinorOperatingSystemVersion, MajorImageVersion, MinorImageVersion,
         MajorSubsystemVersion, MinorSubsystemVersion;
    DWORD Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    WORD Subsystem, DllCharacteristics;
    ULONGLONG SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    DWORD LoaderFlags, NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[16];
} IMAGE_OPTIONAL_HEADER64;
typedef struct _IMAGE_NT_HEADERS64 { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader; } IMAGE_NT_HEADERS64, IMAGE_NT_HEADERS, *PIMAGE_NT_HEADERS;
#define IMAGE_DIRECTORY_ENTRY_EXPORT     0
#define IMAGE_DIRECTORY_ENTRY_IMPORT     1
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION  3
#define IMAGE_DIRECTORY_ENTRY_BASERELOC  5
#define IMAGE_DIRECTORY_ENTRY_TLS        9
#define IMAGE_FILE_DLL                   0x2000
typedef struct _IMAGE_EXPORT_DIRECTORY {
    DWORD Characteristics, TimeDateStamp; WORD MajorVersion, MinorVersion;
    DWORD Name, Base, NumberOfFunctions, NumberOfNames, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
} IMAGE_EXPORT_DIRECTORY, *PIMAGE_EXPORT_DIRECTORY;
typedef VOID (__stdcall *PIMAGE_TLS_CALLBACK)(PVOID DllHandle, DWORD Reason, PVOID Reserved);
typedef struct _IMAGE_TLS_DIRECTORY64 {
    ULONGLONG StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks;
    DWORD SizeOfZeroFill, Characteristics;
} IMAGE_TLS_DIRECTORY64, IMAGE_TLS_DIRECTORY, *PIMAGE_TLS_DIRECTORY;

#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3
#define DLL_PROCESS_DETACH 0

/* -----------------------------------------------------------------------
 * Locks
 * ----------------------------------------------------------------------- */
typedef struct _RTL_CRITICAL_SECTION {
    PVOID DebugInfo;
    LONG LockCount;             /* -1: free; else owner + waiters - 1 */
    LONG RecursionCount;
    HANDLE OwningThread;        /* thread ID */
    HANDLE LockSemaphore;       /* auto-reset event, created on first contention */
    ULONG_PTR SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION, CRITICAL_SECTION, *PCRITICAL_SECTION, *LPCRITICAL_SECTION;

typedef struct _RTL_SRWLOCK { PVOID Ptr; } RTL_SRWLOCK, *PRTL_SRWLOCK, SRWLOCK, *PSRWLOCK;
typedef struct _RTL_CONDITION_VARIABLE { PVOID Ptr; } RTL_CONDITION_VARIABLE, *PRTL_CONDITION_VARIABLE, CONDITION_VARIABLE, *PCONDITION_VARIABLE;
typedef union _RTL_RUN_ONCE { PVOID Ptr; } RTL_RUN_ONCE, *PRTL_RUN_ONCE, INIT_ONCE, *PINIT_ONCE, *LPINIT_ONCE;
#define SRWLOCK_INIT            { 0 }
#define CONDITION_VARIABLE_INIT { 0 }
#define INIT_ONCE_STATIC_INIT   { 0 }
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1

/* -----------------------------------------------------------------------
 * Interlocked operations (full barriers, like the MSVC intrinsics)
 * ----------------------------------------------------------------------- */
#define _NOVA_IL static __inline__ __attribute__((always_inline, unused))
_NOVA_IL LONG InterlockedIncrement(LONG volatile *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG InterlockedDecrement(LONG volatile *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG InterlockedExchange(LONG volatile *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG InterlockedExchangeAdd(LONG volatile *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG InterlockedAdd(LONG volatile *p, LONG v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG InterlockedOr(LONG volatile *p, LONG v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG InterlockedAnd(LONG volatile *p, LONG v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG InterlockedXor(LONG volatile *p, LONG v) { return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG InterlockedCompareExchange(LONG volatile *p, LONG x, LONG cmp)
{ __atomic_compare_exchange_n(p, &cmp, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp; }
_NOVA_IL LONG64 InterlockedIncrement64(LONG64 volatile *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG64 InterlockedDecrement64(LONG64 volatile *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG64 InterlockedExchange64(LONG64 volatile *p, LONG64 v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG64 InterlockedExchangeAdd64(LONG64 volatile *p, LONG64 v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
_NOVA_IL LONG64 InterlockedCompareExchange64(LONG64 volatile *p, LONG64 x, LONG64 cmp)
{ __atomic_compare_exchange_n(p, &cmp, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp; }
_NOVA_IL PVOID InterlockedExchangePointer(PVOID volatile *p, PVOID v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
_NOVA_IL PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID x, PVOID cmp)
{ __atomic_compare_exchange_n(p, &cmp, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp; }
#define MemoryBarrier()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor() __builtin_ia32_pause()

_NOVA_END
