/*
 * ps.h — NT Process and Thread Manager (PsXxx)
 *
 * This header defines the NT executive process and thread structures.
 * The naming and field layout follow the Windows NT kernel conventions
 * so that Phase 4 code (PE loader, Win32 API) can use them directly.
 *
 * Structure hierarchy:
 *
 *   EPROCESS (Executive Process) — full process descriptor
 *     └─ KPROCESS (Kernel Process) embedded at offset 0
 *          ├─ PageDirectoryBase (CR3 physical address)
 *          ├─ KernelTime, UserTime
 *          └─ ThreadListHead (list of KTHREADs)
 *
 *   ETHREAD (Executive Thread) — full thread descriptor
 *     └─ KTHREAD (Kernel Thread) embedded at offset 0
 *          ├─ TrapFrame
 *          ├─ KernelStack
 *          └─ ThreadListEntry (in owning EPROCESS)
 *
 * User-mode data structures:
 *
 *   PEB (Process Environment Block) — lives in user VA at 0x60 (64-bit)
 *     ├─ ImageBaseAddress
 *     ├─ Ldr (PEB_LDR_DATA — loaded module list)
 *     ├─ ProcessParameters (RTL_USER_PROCESS_PARAMETERS)
 *     └─ ...
 *
 *   TEB (Thread Environment Block) — lives at GS:[0] in user mode
 *     ├─ NtTib (NT_TIB — exception chain, stack limits, self pointer)
 *     ├─ ProcessEnvironmentBlock (pointer to PEB)
 *     ├─ ClientId (PID/TID)
 *     └─ LastErrorValue
 *
 * NT compatibility note:
 *   Real NT PEB/TEB offsets are exactly as documented in
 *   ntdll!RtlpQueryProcessDebugInformationRemote and the public
 *   PDB symbols.  We must match them for ntdll.dll to work correctly.
 */

#pragma once

#include "../include/types.h"
#include "../ob/ob.h"
#include "../ke/scheduler.h"
#include "../mm/vma.h"

/* -----------------------------------------------------------------------
 * NT_TIB — Thread Information Block (the first field of TEB)
 * Offset 0x00 in TEB.
 * ----------------------------------------------------------------------- */
typedef struct _NT_TIB {
    void        *ExceptionList;    /* +0x00: SEH chain head (ring 3) */
    void        *StackBase;        /* +0x08: top of stack (highest address) */
    void        *StackLimit;       /* +0x10: stack guard page */
    void        *SubSystemTib;     /* +0x18 */
    union {
        void    *FiberData;        /* +0x20 */
        UINT32   Version;
    };
    void        *ArbitraryUserPointer; /* +0x28 */
    struct _NT_TIB *Self;          /* +0x30: self-pointer (GS:[0x30]) */
} NT_TIB;

/* -----------------------------------------------------------------------
 * TEB — Thread Environment Block (64-bit layout)
 * Accessed via GS segment base (MSR_IA32_GSBASE for kernel,
 * MSR_IA32_KERNEL_GSBASE for user-mode via SWAPGS).
 * ----------------------------------------------------------------------- */
typedef struct _TEB {
    NT_TIB   NtTib;                   /* +0x000 */

    void    *EnvironmentPointer;       /* +0x038 */
    struct { UINT64 UniqueProcess; UINT64 UniqueThread; }
             ClientId;                 /* +0x040 */
    void    *ActiveRpcHandle;          /* +0x050 */
    void    *ThreadLocalStoragePointer;/* +0x058 */
    struct _PEB *ProcessEnvironmentBlock; /* +0x060 */
    UINT32   LastErrorValue;           /* +0x068 */
    UINT32   CountOfOwnedCriticalSections; /* +0x06C */
    void    *CsrClientThread;          /* +0x070 */
    void    *Win32ThreadInfo;          /* +0x078 */
    UINT32   Win32ClientInfo[31];      /* +0x080 */
    void    *WOW32Reserved;            /* +0x100 */
    UINT32   CurrentLocale;            /* +0x108 */
    UINT32   FpSoftwareStatusRegister; /* +0x10C */
    /* ... many more fields; we only define what Phase 2/4 needs ... */
    uint8_t  _pad[0x1000 - 0x110];    /* pad to 4KB page */
} TEB;

/* -----------------------------------------------------------------------
 * PEB_LDR_DATA — loaded module list
 * ----------------------------------------------------------------------- */
typedef struct _LDR_DATA_TABLE_ENTRY {
    struct { void *Flink, *Blink; } InLoadOrderLinks;        /* +0x000 */
    struct { void *Flink, *Blink; } InMemoryOrderLinks;      /* +0x010 */
    struct { void *Flink, *Blink; } InInitializationOrderLinks; /* +0x020 */
    void        *DllBase;           /* +0x030 */
    void        *EntryPoint;        /* +0x038 */
    UINT32       SizeOfImage;       /* +0x040 */
    UINT32       _pad1;
    UNICODE_STRING FullDllName;     /* +0x048 */
    UNICODE_STRING BaseDllName;     /* +0x058 */
    UINT32       Flags;             /* +0x068 */
    UINT16       LoadCount;         /* +0x06C */
    UINT16       TlsIndex;
    /* ... */
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;

typedef struct _PEB_LDR_DATA {
    UINT32  Length;
    UINT32  Initialized;
    void   *SsHandle;
    struct { void *Flink, *Blink; } InLoadOrderModuleList;
    struct { void *Flink, *Blink; } InMemoryOrderModuleList;
    struct { void *Flink, *Blink; } InInitializationOrderModuleList;
    void   *EntryInProgress;
    UINT32  ShutdownInProgress;
    void   *ShutdownThreadId;
} PEB_LDR_DATA, *PPEB_LDR_DATA;

/* -----------------------------------------------------------------------
 * RTL_USER_PROCESS_PARAMETERS
 * ----------------------------------------------------------------------- */
typedef struct _RTL_USER_PROCESS_PARAMETERS {
    UINT32         MaximumLength;
    UINT32         Length;
    UINT32         Flags;
    UINT32         DebugFlags;
    void          *ConsoleHandle;
    UINT32         ConsoleFlags;
    UINT32         _pad0;
    HANDLE         StandardInput;
    HANDLE         StandardOutput;
    HANDLE         StandardError;
    UNICODE_STRING CurrentDirectory;
    HANDLE         CurrentDirectoryHandle;
    UNICODE_STRING DllPath;
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
    void          *Environment;
    /* ... */
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;

/* -----------------------------------------------------------------------
 * PEB — Process Environment Block (64-bit layout)
 * Pointed to by TEB.ProcessEnvironmentBlock (TEB+0x60).
 * Also accessible via NtQueryInformationProcess(ProcessBasicInformation).
 * ----------------------------------------------------------------------- */
typedef struct _PEB {
    UINT8    InheritedAddressSpace;    /* +0x000 */
    UINT8    ReadImageFileExecOptions; /* +0x001 */
    UINT8    BeingDebugged;            /* +0x002 */
    UINT8    BitField;                 /* +0x003: SpareBits etc. */
    UINT32   _pad0;
    void    *Mutant;                   /* +0x008 */
    void    *ImageBaseAddress;         /* +0x010 */
    PPEB_LDR_DATA Ldr;                 /* +0x018 */
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters; /* +0x020 */
    void    *SubSystemData;            /* +0x028 */
    void    *ProcessHeap;              /* +0x030 */
    void    *FastPebLock;              /* +0x038 */
    void    *AtlThunkSListPtr;         /* +0x040 */
    void    *IFEOKey;                  /* +0x048 */
    UINT32   CrossProcessFlags;        /* +0x050 */
    UINT32   _pad1;
    union {
        void *KernelCallbackTable;     /* +0x058 */
        void *UserSharedInfoPtr;
    };
    UINT32   SystemReserved;           /* +0x060 */
    UINT32   AtlThunkSListPtr32;       /* +0x064 */
    void    *ApiSetMap;                /* +0x068 */
    UINT32   TlsExpansionCounter;      /* +0x070 */
    UINT32   _pad2;
    void    *TlsBitmap;                /* +0x078 */
    UINT32   TlsBitmapBits[2];         /* +0x080 */
    void    *ReadOnlySharedMemoryBase; /* +0x088 */
    void    *SharedData;               /* +0x090 */
    void   **ReadOnlyStaticServerData; /* +0x098 */
    void    *AnsiCodePageData;         /* +0x0A0 */
    void    *OemCodePageData;          /* +0x0A8 */
    void    *UnicodeCaseTableData;     /* +0x0B0 */
    UINT32   NumberOfProcessors;       /* +0x0B8 */
    UINT32   NtGlobalFlag;             /* +0x0BC */
    UINT64   CriticalSectionTimeout;   /* +0x0C0 */
    UINT64   HeapSegmentReserve;       /* +0x0C8 */
    UINT64   HeapSegmentCommit;        /* +0x0D0 */
    UINT64   HeapDeCommitTotalFreeThreshold; /* +0x0D8 */
    UINT64   HeapDeCommitFreeBlockThreshold; /* +0x0E0 */
    UINT32   NumberOfHeaps;            /* +0x0E8 */
    UINT32   MaximumNumberOfHeaps;     /* +0x0EC */
    void   **ProcessHeaps;             /* +0x0F0 */
    /* ... many more fields ... pad to common size */
} PEB;

/* -----------------------------------------------------------------------
 * KPROCESS — kernel-level process (embedded at start of EPROCESS)
 * ----------------------------------------------------------------------- */
typedef struct _KPROCESS {
    /* Page table root for this process */
    UINT64   DirectoryTableBase;   /* CR3 physical address */

    /* CPU time accounting */
    UINT64   KernelTime;
    UINT64   UserTime;

    /* Unique IDs */
    UINT64   UniqueProcessId;

    /* Doubly-linked list of KTHREADs in this process */
    struct { void *Flink, *Blink; } ThreadListHead;
    UINT32   ThreadCount;
} KPROCESS;

/* -----------------------------------------------------------------------
 * EPROCESS — executive-level process (full process object body)
 * ----------------------------------------------------------------------- */
#define EPROCESS_IMAGE_NAME_MAX 16

typedef struct _EPROCESS {
    KPROCESS   Pcb;                  /* Must be first — kernel thread list */

    /* Process identity */
    UINT64     UniqueProcessId;      /* PID */
    UINT64     InheritedFromUniqueProcessId;

    /* Object manager linkage */
    struct { void *Flink, *Blink; } ActiveProcessLinks;

    /* Image */
    char       ImageFileName[EPROCESS_IMAGE_NAME_MAX];  /* Short name (ASCII) */
    void      *SectionObject;        /* Points to the mapped image section */

    /* Memory */
    UINT64     VirtualSize;
    UINT64     WorkingSetSize;
    PHANDLE_TABLE ObjectTable;       /* Per-process handle table */

    /* Address space — user VA descriptor tree (Phase 3) */
    VMA_SPACE  VmaSpace;

    /* User-mode data (in the process's VA space) */
    PEB       *Peb;                  /* Virtual address in process VA space */

    /* Security */
    void      *Token;                /* Primary token (ETOKEN pointer) */

    /* Exit status */
    NTSTATUS   ExitStatus;
    bool       HasExited;

    /* Spin for process-level operations */
    volatile UINT32 SpinNext, SpinOwner;
} EPROCESS, *PEPROCESS;

/* -----------------------------------------------------------------------
 * KTHREAD — kernel-level thread (embedded at start of ETHREAD)
 * ----------------------------------------------------------------------- */
typedef struct _KTHREAD {
    /* Must be first — scheduler uses this directly via Thread cast */
    Thread     SchedulerThread;     /* Phase 1 scheduler thread object */

    /* Execution state */
    UINT64     KernelTime;
    UINT64     UserTime;

    /* Context save on syscall/interrupt */
    UINT64     KernelStack;         /* Virtual address of kernel stack top */
    UINT64     KernelStackBase;

    /* Wait state */
    UINT32     WaitReason;
    UINT32     WaitBlockCount;

    /* APC (Asynchronous Procedure Call) — Phase 3 */
    struct { void *Flink, *Blink; } ApcListHead[2]; /* 0=Kernel, 1=User */
    UINT32     ApcQueueable;

    /* Thread list entry (in owning EPROCESS) */
    struct { void *Flink, *Blink; } ThreadListEntry;
} KTHREAD;

/* -----------------------------------------------------------------------
 * ETHREAD — executive-level thread (full thread object body)
 * ----------------------------------------------------------------------- */
typedef struct _ETHREAD {
    KTHREAD    Tcb;                  /* Must be first */

    /* Thread identity */
    UINT64     UniqueThread;         /* TID */

    /* Owning process */
    PEPROCESS  Process;

    /* User-mode data */
    TEB       *Teb;                  /* Virtual address in process VA space */

    /* Client ID (PID/TID pair — what Win32 sees) */
    struct { UINT64 UniqueProcess; UINT64 UniqueThread; } Cid;

    /* Exit status */
    NTSTATUS   ExitStatus;
    bool       HasExited;
} ETHREAD, *PETHREAD;

/* -----------------------------------------------------------------------
 * Process/Thread priority levels (NT-compatible)
 * ----------------------------------------------------------------------- */
#define THREAD_PRIORITY_IDLE          -15
#define THREAD_PRIORITY_LOWEST        -2
#define THREAD_PRIORITY_BELOW_NORMAL  -1
#define THREAD_PRIORITY_NORMAL         0
#define THREAD_PRIORITY_ABOVE_NORMAL   1
#define THREAD_PRIORITY_HIGHEST        2
#define THREAD_PRIORITY_TIME_CRITICAL  15

/* Process priority classes */
#define PROCESS_PRIORITY_CLASS_IDLE            1
#define PROCESS_PRIORITY_CLASS_NORMAL          2
#define PROCESS_PRIORITY_CLASS_HIGH            3
#define PROCESS_PRIORITY_CLASS_REALTIME        4
#define PROCESS_PRIORITY_CLASS_BELOW_NORMAL    5
#define PROCESS_PRIORITY_CLASS_ABOVE_NORMAL    6

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Initialize the process manager.  Creates the System process and
 * System thread (wraps the kernel's boot execution context). */
void PsInitialize(void);

/* Create a new kernel-mode process (no user-mode address space yet). */
NTSTATUS PsCreateSystemProcess(
    PEPROCESS      *ProcessOut,
    const char     *ImageFileName,
    void           *Token);          /* NULL = inherit from System */

/* Create a kernel-mode thread in the given process. */
NTSTATUS PsCreateSystemThread(
    HANDLE         *ThreadHandle,
    ACCESS_MASK     DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    HANDLE          ProcessHandle,
    void           *ClientId,
    ThreadEntry     StartRoutine,
    void           *StartContext);

/* Terminate a process (sets ExitStatus, terminates all threads). */
NTSTATUS PsTerminateProcess(PEPROCESS Process, NTSTATUS ExitStatus);

/* Terminate current thread. */
NTSTATUS PsTerminateSystemThread(NTSTATUS ExitStatus);

/* Lookup functions (caller must dereference returned pointer). */
NTSTATUS PsLookupProcessByProcessId(UINT64 Pid, PEPROCESS *Process);
NTSTATUS PsLookupThreadByThreadId(UINT64 Tid, PETHREAD *Thread);

/* Current context accessors (uses scheduler's current thread). */
PEPROCESS PsGetCurrentProcess(void);
PETHREAD  PsGetCurrentThread(void);
PHANDLE_TABLE PsGetCurrentProcessHandleTable(void);

/* The System process (PID 4, like Windows). */
extern PEPROCESS PsInitialSystemProcess;

/*
 * Create a user-mode process from a PE image buffer.
 * Loads the image, maps it into a fresh address space, resolves imports,
 * and creates a user-mode thread at the entry point.
 *
 * @PeBuffer:     Pointer to raw PE32+ image data (in kernel memory).
 * @PeSize:       Size of the PE buffer.
 * @ImageName:    Short name for the process (e.g. "test.exe").
 * @ProcessOut:   Receives the EPROCESS pointer (optional).
 * @ThreadHandle: Receives a handle to the initial thread (optional).
 */
NTSTATUS PsCreateUserProcess(
    void           *PeBuffer,
    UINT64          PeSize,
    const char     *ImageName,
    PEPROCESS      *ProcessOut,
    HANDLE         *ThreadHandle);

/*
 * The user-mode thread entry trampoline.
 * Sets up the user-mode context (iretq) and jumps to the image entry point.
 * Called as the kernel thread entry for user-mode threads.
 */
void PsUserThreadEntry(void *arg);
