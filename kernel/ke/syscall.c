/*
 * syscall.c — NT System Call Dispatcher
 *
 * Implements the C-level dispatch table and SYSCALL MSR setup.
 *
 * Assembly entry (KiSystemCall64) is in arch/x86_64/syscall_entry.asm.
 * It saves the trap frame, calls KiSystemCallDispatch(), then SYSRET.
 *
 * Each handler receives up to 4 arguments.  For syscalls with more
 * than 4 arguments (e.g. NtCreateFile takes 11), additional arguments
 * are on the user-mode stack — we access them via the saved user RSP
 * in the trap frame.  Phase 2 handlers only use ≤4 arguments.
 */

#include "syscall.h"
#include "scheduler.h"
#include "printf.h"
#include "../include/types.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../ob/ob.h"
#include "../ps/ps.h"
#include "../cm/cm.h"
#include "../mm/vma.h"
#include "../mm/section.h"
#include "../io/io.h"
#include "../ldr/user_stubs.h"  /* KH_xxx kernel-helper numbers */

/* -----------------------------------------------------------------------
 * MSR addresses for SYSCALL/SYSRET
 * ----------------------------------------------------------------------- */
#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_CSTAR       0xC0000083   /* compat mode — unused */
#define MSR_SFMASK      0xC0000084

/* EFER_SCE is defined in cpu.h — no need to redefine here */

/* -----------------------------------------------------------------------
 * Syscall handler typedef
 * Returns UINT64 so that kernel-helper handlers can return 64-bit pointers
 * (e.g., RtlAllocateHeap returns a user-mode VA).  NT syscall handlers cast
 * their NTSTATUS return to UINT64 (zero-extends the 32-bit code into RAX).
 * ----------------------------------------------------------------------- */
typedef UINT64 (*SYSCALL_HANDLER)(UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4);

/* -----------------------------------------------------------------------
 * Individual syscall handler implementations
 * ----------------------------------------------------------------------- */

/* --- NtClose (0x000F) --- */
static UINT64 sys_NtClose(UINT64 Handle, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    return ObCloseHandle((HANDLE)Handle, NULL);
}

/* --- NtYieldExecution (0x0046) --- */
static UINT64 sys_NtYieldExecution(UINT64 a1, UINT64 a2,
                                      UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    sched_yield();
    return STATUS_SUCCESS;
}

/* --- NtDelayExecution (0x0034) --- */
static UINT64 sys_NtDelayExecution(UINT64 Alertable,
                                      UINT64 DelayIntervalPtr,
                                      UINT64 a3, UINT64 a4)
{
    (void)Alertable; (void)DelayIntervalPtr; (void)a3; (void)a4;
    /* Phase 2 stub: yield once */
    sched_yield();
    return STATUS_SUCCESS;
}

/* --- NtQuerySystemTime (0x0052) --- */
static UINT64 sys_NtQuerySystemTime(UINT64 SystemTimePtr,
                                       UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    /* Return 0 (epoch) — Phase 3 will hook the RTC/HPET */
    if (SystemTimePtr) {
        UINT64 *p = (UINT64 *)(uintptr_t)SystemTimePtr;
        *p = 0;
    }
    return STATUS_SUCCESS;
}

/* --- NtQueryPerformanceCounter (0x0031) --- */
static UINT64 sys_NtQueryPerformanceCounter(UINT64 CounterPtr,
                                               UINT64 FreqPtr,
                                               UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    uint64_t tsc = rdtsc();
    if (CounterPtr) *(UINT64 *)(uintptr_t)CounterPtr = tsc;
    if (FreqPtr)    *(UINT64 *)(uintptr_t)FreqPtr    = 2000000000ULL; /* ~2GHz */
    return STATUS_SUCCESS;
}

/* --- NtQuerySystemInformation (0x0036) --- */
static UINT64 sys_NtQuerySystemInformation(UINT64 InfoClass,
                                              UINT64 InfoPtr,
                                              UINT64 InfoLen,
                                              UINT64 ReturnLenPtr)
{
    UINT32 *ReturnLength = (UINT32 *)(uintptr_t)ReturnLenPtr;

    switch ((UINT32)InfoClass) {
    case SystemBasicInformation: {
        if (ReturnLength) *ReturnLength = sizeof(SYSTEM_BASIC_INFORMATION);
        if (InfoLen < sizeof(SYSTEM_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        if (!InfoPtr) return STATUS_INVALID_PARAMETER;

        SYSTEM_BASIC_INFORMATION *sbi = (SYSTEM_BASIC_INFORMATION *)(uintptr_t)InfoPtr;
        __builtin_memset(sbi, 0, sizeof(*sbi));
        sbi->PageSize              = (UINT32)PAGE_SIZE;
        sbi->AllocationGranularity = 65536;  /* 64KB, same as Windows */
        sbi->MinimumUserModeAddress = 0x10000;
        sbi->MaximumUserModeAddress = 0x7FFFFFFFFFFEFFFF;
        sbi->ActiveProcessorsAffinityMask = 1;
        sbi->NumberOfProcessors    = 1;
        /* PMM stats for physical page counts */
        uint64_t total_pages, free_pages, used_pages;
        extern void pmm_stats(uint64_t *, uint64_t *, uint64_t *);
        pmm_stats(&total_pages, &free_pages, &used_pages);
        sbi->NumberOfPhysicalPages = (UINT32)total_pages;
        return STATUS_SUCCESS;
    }
    default:
        if (ReturnLength) *ReturnLength = 0;
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* --- NtQueryInformationProcess (0x0019) --- */
static UINT64 sys_NtQueryInformationProcess(UINT64 ProcessHandle,
                                               UINT64 ProcInfoClass,
                                               UINT64 ProcInfoPtr,
                                               UINT64 ProcInfoLen)
{
    void *proc_obj;
    NTSTATUS s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                            PROCESS_ALL_ACCESS,
                                            ObpProcessType, NULL,
                                            &proc_obj, NULL);
    /* Allow NtCurrentProcess() pseudo-handle = -1 */
    PEPROCESS proc = NULL;
    if (!NT_SUCCESS(s)) {
        if ((INT64)ProcessHandle == -1) {
            proc = PsGetCurrentProcess();
        } else {
            return s;
        }
    } else {
        proc = (PEPROCESS)proc_obj;
    }

    switch ((UINT32)ProcInfoClass) {
    case ProcessBasicInformation: {
        if (ProcInfoLen < sizeof(PROCESS_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        if (!ProcInfoPtr) return STATUS_INVALID_PARAMETER;

        PROCESS_BASIC_INFORMATION *pbi =
            (PROCESS_BASIC_INFORMATION *)(uintptr_t)ProcInfoPtr;
        __builtin_memset(pbi, 0, sizeof(*pbi));
        pbi->ExitStatus            = proc->ExitStatus;
        pbi->PebBaseAddress        = proc->Peb;
        pbi->AffinityMask          = 1;
        pbi->BasePriority          = 8;
        pbi->UniqueProcessId       = proc->UniqueProcessId;
        pbi->InheritedFromUniqueProcessId =
            proc->InheritedFromUniqueProcessId;
        if (proc_obj) ObDereferenceObject(proc_obj);
        return STATUS_SUCCESS;
    }
    default:
        if (proc_obj) ObDereferenceObject(proc_obj);
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* --- NtQueryInformationThread (0x0025) --- */
static UINT64 sys_NtQueryInformationThread(UINT64 ThreadHandle,
                                              UINT64 ThreadInfoClass,
                                              UINT64 ThreadInfoPtr,
                                              UINT64 ThreadInfoLen)
{
    PETHREAD et = NULL;
    if ((INT64)ThreadHandle == -2) {
        et = PsGetCurrentThread();
    } else {
        void *obj;
        NTSTATUS s = ObReferenceObjectByHandle((HANDLE)ThreadHandle,
                                                THREAD_ALL_ACCESS,
                                                ObpThreadType, NULL,
                                                &obj, NULL);
        if (!NT_SUCCESS(s)) return s;
        et = (PETHREAD)obj;
    }
    if (!et) return STATUS_INVALID_HANDLE;

    switch ((UINT32)ThreadInfoClass) {
    case ThreadBasicInformation: {
        if (ThreadInfoLen < sizeof(THREAD_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        if (!ThreadInfoPtr) return STATUS_INVALID_PARAMETER;

        THREAD_BASIC_INFORMATION *tbi =
            (THREAD_BASIC_INFORMATION *)(uintptr_t)ThreadInfoPtr;
        __builtin_memset(tbi, 0, sizeof(*tbi));
        tbi->ExitStatus          = et->ExitStatus;
        tbi->TebBaseAddress      = et->Teb;
        tbi->ClientId.UniqueProcess = et->Cid.UniqueProcess;
        tbi->ClientId.UniqueThread  = et->Cid.UniqueThread;
        tbi->AffinityMask        = 1;
        tbi->Priority            = 8;
        tbi->BasePriority        = 8;
        if ((INT64)ThreadHandle != -2) ObDereferenceObject((void *)et);
        return STATUS_SUCCESS;
    }
    default:
        if ((INT64)ThreadHandle != -2) ObDereferenceObject((void *)et);
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* --- NtOpenProcess (0x0026) --- */
static UINT64 sys_NtOpenProcess(UINT64 ProcessHandlePtr,
                                   UINT64 DesiredAccess,
                                   UINT64 ObjAttrPtr,
                                   UINT64 ClientIdPtr)
{
    (void)ObjAttrPtr;

    struct { UINT64 UniqueProcess; UINT64 UniqueThread; } *cid =
        (void *)(uintptr_t)ClientIdPtr;
    if (!cid) return STATUS_INVALID_PARAMETER;

    PEPROCESS proc = NULL;
    NTSTATUS s = PsLookupProcessByProcessId(cid->UniqueProcess, &proc);
    if (!NT_SUCCESS(s)) return s;

    HANDLE h = 0;
    s = ObInsertObject(proc, NULL, (ACCESS_MASK)DesiredAccess, 0, NULL, &h);
    ObDereferenceObject(proc);   /* InsertObject added its own reference */
    if (!NT_SUCCESS(s)) return s;

    if (ProcessHandlePtr)
        *(HANDLE *)(uintptr_t)ProcessHandlePtr = h;
    return STATUS_SUCCESS;
}

/* --- NtAllocateVirtualMemory (0x0018) --- */
static UINT64 sys_NtAllocateVirtualMemory(UINT64 ProcessHandle,
                                             UINT64 BaseAddressPtr,
                                             UINT64 RegionSizePtr,
                                             UINT64 AllocationType)
{
    if (!RegionSizePtr) return STATUS_INVALID_PARAMETER;

    PEPROCESS proc;
    if ((INT64)ProcessHandle == -1) proc = PsGetCurrentProcess();
    else {
        void *obj;
        NTSTATUS s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                               PROCESS_ALL_ACCESS, ObpProcessType,
                                               NULL, &obj, NULL);
        if (!NT_SUCCESS(s)) return s;
        proc = (PEPROCESS)obj;
    }
    if (!proc) return STATUS_INVALID_HANDLE;

    UINT64 base = BaseAddressPtr ? *(UINT64 *)(uintptr_t)BaseAddressPtr : 0;
    UINT64 size = *(UINT64 *)(uintptr_t)RegionSizePtr;
    if (!size) {
        if ((INT64)ProcessHandle != -1) ObDereferenceObject(proc);
        return STATUS_INVALID_PARAMETER;
    }

    UINT32 protect = PAGE_READWRITE;
    NTSTATUS s = VmaAllocate(&proc->VmaSpace, &base, &size,
                              (UINT32)AllocationType, protect);
    if ((INT64)ProcessHandle != -1) ObDereferenceObject(proc);
    if (!NT_SUCCESS(s)) return s;

    if (BaseAddressPtr) *(UINT64 *)(uintptr_t)BaseAddressPtr = base;
    *(UINT64 *)(uintptr_t)RegionSizePtr = size;
    return STATUS_SUCCESS;
}

/* --- NtFreeVirtualMemory (0x001E) --- */
static UINT64 sys_NtFreeVirtualMemory(UINT64 ProcessHandle,
                                         UINT64 BaseAddressPtr,
                                         UINT64 RegionSizePtr,
                                         UINT64 FreeType)
{
    if (!BaseAddressPtr) return STATUS_INVALID_PARAMETER;

    PEPROCESS proc;
    if ((INT64)ProcessHandle == -1) proc = PsGetCurrentProcess();
    else {
        void *obj;
        NTSTATUS s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                               PROCESS_ALL_ACCESS, ObpProcessType,
                                               NULL, &obj, NULL);
        if (!NT_SUCCESS(s)) return s;
        proc = (PEPROCESS)obj;
    }
    if (!proc) return STATUS_INVALID_HANDLE;

    UINT64 base = *(UINT64 *)(uintptr_t)BaseAddressPtr;
    UINT64 size = RegionSizePtr ? *(UINT64 *)(uintptr_t)RegionSizePtr : 0;

    NTSTATUS s = VmaFree(&proc->VmaSpace, &base, &size, (UINT32)FreeType);
    if ((INT64)ProcessHandle != -1) ObDereferenceObject(proc);
    if (NT_SUCCESS(s)) {
        *(UINT64 *)(uintptr_t)BaseAddressPtr = base;
        if (RegionSizePtr) *(UINT64 *)(uintptr_t)RegionSizePtr = size;
    }
    return s;
}

/* --- NtOpenKey (0x0012) --- */
static UINT64 sys_NtOpenKey(UINT64 KeyHandlePtr, UINT64 DesiredAccess,
                               UINT64 ObjAttrPtr, UINT64 a4)
{
    (void)a4;
    if (!KeyHandlePtr || !ObjAttrPtr) return STATUS_INVALID_PARAMETER;
    HANDLE h = 0;
    NTSTATUS s = NtOpenKey(&h, (ACCESS_MASK)DesiredAccess,
                            (POBJECT_ATTRIBUTES)(uintptr_t)ObjAttrPtr);
    if (NT_SUCCESS(s))
        *(HANDLE *)(uintptr_t)KeyHandlePtr = h;
    return s;
}

/* --- NtCreateKey (0x001D) --- */
static UINT64 sys_NtCreateKey(UINT64 KeyHandlePtr, UINT64 DesiredAccess,
                                  UINT64 ObjAttrPtr, UINT64 TitleIndex)
{
    if (!KeyHandlePtr || !ObjAttrPtr) return STATUS_INVALID_PARAMETER;
    HANDLE h = 0;
    UINT32 disp = 0;
    NTSTATUS s = NtCreateKey(&h, (ACCESS_MASK)DesiredAccess,
                              (POBJECT_ATTRIBUTES)(uintptr_t)ObjAttrPtr,
                              (UINT32)TitleIndex, NULL, 0, &disp);
    if (NT_SUCCESS(s))
        *(HANDLE *)(uintptr_t)KeyHandlePtr = h;
    return s;
}

/* --- NtQueryValueKey (0x0017) --- */
static UINT64 sys_NtQueryValueKey(UINT64 KeyHandle, UINT64 ValueNamePtr,
                                     UINT64 InfoClass, UINT64 InfoPtr)
{
    /* Note: InfoLen and ResultLength are on user stack (args 5/6) —
     * Phase 2 stub uses fixed length / ignores ResultLength */
    UINT32 resultlen = 0;
    return NtQueryValueKey((HANDLE)KeyHandle,
                            (UNICODE_STRING *)(uintptr_t)ValueNamePtr,
                            (KEY_VALUE_INFORMATION_CLASS)(UINT32)InfoClass,
                            (void *)(uintptr_t)InfoPtr,
                            4096,
                            &resultlen);
}

/* --- NtSetValueKey (0x0027) --- */
static UINT64 sys_NtSetValueKey(UINT64 KeyHandle, UINT64 ValueNamePtr,
                                   UINT64 TitleIndex, UINT64 Type)
{
    /* Data and DataSize are on user stack — Phase 2 stub ignores them */
    (void)TitleIndex;
    return NtSetValueKey((HANDLE)KeyHandle,
                          (UNICODE_STRING *)(uintptr_t)ValueNamePtr,
                          0,
                          (UINT32)Type,
                          NULL, 0);
}

/* --- NtTerminateProcess (0x002C) --- */
static UINT64 sys_NtTerminateProcess(UINT64 ProcessHandle,
                                        UINT64 ExitStatus,
                                        UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    if ((INT64)ProcessHandle == -1 || !ProcessHandle) {
        PEPROCESS p = PsGetCurrentProcess();
        return p ? PsTerminateProcess(p, (NTSTATUS)ExitStatus)
                 : STATUS_INVALID_HANDLE;
    }
    void *obj;
    NTSTATUS s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                            PROCESS_ALL_ACCESS,
                                            ObpProcessType, NULL,
                                            &obj, NULL);
    if (!NT_SUCCESS(s)) return s;
    s = PsTerminateProcess((PEPROCESS)obj, (NTSTATUS)ExitStatus);
    ObDereferenceObject(obj);
    return s;
}

/* -----------------------------------------------------------------------
 * Not-implemented stub
 * ----------------------------------------------------------------------- */
static UINT64 sys_NotImplemented(UINT64 a1, UINT64 a2,
                                  UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return (UINT64)(UINT32)STATUS_NOT_IMPLEMENTED;
}

/* -----------------------------------------------------------------------
 * Phase 6: NtSetInformationThread (0x000D)
 * ----------------------------------------------------------------------- */
static UINT64 sys_NtSetInformationThread(UINT64 ThreadHandle,
                                          UINT64 ThreadInfoClass,
                                          UINT64 ThreadInfoPtr,
                                          UINT64 ThreadInfoLen)
{
    (void)ThreadHandle; (void)ThreadInfoPtr; (void)ThreadInfoLen;
    /* Phase 6: accept ThreadHideFromDebugger (17) as no-op */
    if ((UINT32)ThreadInfoClass == 17 /* ThreadHideFromDebugger */)
        return (UINT64)(UINT32)STATUS_SUCCESS;
    return (UINT64)(UINT32)STATUS_NOT_IMPLEMENTED;
}

/* -----------------------------------------------------------------------
 * Phase 6: NtFlushInstructionCache (0x00CC)
 * ----------------------------------------------------------------------- */
static UINT64 sys_NtFlushInstructionCache(UINT64 ProcessHandle,
                                           UINT64 BaseAddress,
                                           UINT64 Length,
                                           UINT64 a4)
{
    (void)ProcessHandle; (void)BaseAddress; (void)Length; (void)a4;
    /* On x86-64, instruction cache is coherent — nothing to flush */
    return (UINT64)(UINT32)STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Phase 6: NtCreateProcessEx (0x004D) — minimal stub
 * ----------------------------------------------------------------------- */
static UINT64 sys_NtCreateProcessEx(UINT64 ProcessHandlePtr,
                                     UINT64 DesiredAccess,
                                     UINT64 ObjAttrPtr,
                                     UINT64 ParentProcessHandle)
{
    (void)DesiredAccess; (void)ObjAttrPtr; (void)ParentProcessHandle;
    /* Phase 6: stub — full implementation deferred to Phase 7 */
    if (ProcessHandlePtr)
        *(HANDLE *)(uintptr_t)ProcessHandlePtr = 0;
    return (UINT64)(UINT32)STATUS_NOT_IMPLEMENTED;
}

/* -----------------------------------------------------------------------
 * Phase 6: NtCreateThread (0x004E) — minimal stub
 * ----------------------------------------------------------------------- */
static UINT64 sys_NtCreateThread(UINT64 ThreadHandlePtr,
                                  UINT64 DesiredAccess,
                                  UINT64 ObjAttrPtr,
                                  UINT64 ProcessHandle)
{
    (void)DesiredAccess; (void)ObjAttrPtr; (void)ProcessHandle;
    /* Phase 6: stub */
    if (ThreadHandlePtr)
        *(HANDLE *)(uintptr_t)ThreadHandlePtr = 0;
    return (UINT64)(UINT32)STATUS_NOT_IMPLEMENTED;
}

/* -----------------------------------------------------------------------
 * Phase 6: Kernel-helper handlers (0x01F0–0x01FF)
 * These handle Win32/RTL operations that have no direct NT syscall number.
 * ----------------------------------------------------------------------- */

/* KH 0x01F0: RtlAllocateHeap(HeapHandle, Flags, Size) → user-mode VA */
static UINT64 sys_KhRtlAllocateHeap(UINT64 HeapHandle, UINT64 Flags,
                                      UINT64 Size, UINT64 a4)
{
    (void)HeapHandle; (void)Flags; (void)a4;
    if (!Size) return 0;

    PEPROCESS proc = PsGetCurrentProcess();
    if (!proc) return 0;

    UINT64 base = 0;
    UINT64 alloc_size = (Size + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);
    NTSTATUS s = VmaAllocate(&proc->VmaSpace, &base, &alloc_size,
                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return NT_SUCCESS(s) ? base : 0;
}

/* KH 0x01F1: RtlFreeHeap(HeapHandle, Flags, Ptr) → BOOL */
static UINT64 sys_KhRtlFreeHeap(UINT64 HeapHandle, UINT64 Flags,
                                  UINT64 Ptr, UINT64 a4)
{
    (void)HeapHandle; (void)Flags; (void)a4;
    if (!Ptr) return 1; /* success */

    PEPROCESS proc = PsGetCurrentProcess();
    if (!proc) return 0;

    UINT64 base = Ptr;
    UINT64 size = PAGE_SIZE;
    VmaFree(&proc->VmaSpace, &base, &size, MEM_RELEASE);
    return 1;
}

/* KH 0x01F2: RtlReAllocateHeap(HeapHandle, Flags, Ptr, Size) → user-mode VA */
static UINT64 sys_KhRtlReAllocateHeap(UINT64 HeapHandle, UINT64 Flags,
                                        UINT64 Ptr, UINT64 Size)
{
    /* Allocate new, copy old (size unknown → copy one page), free old */
    UINT64 nw = sys_KhRtlAllocateHeap(HeapHandle, Flags, Size, 0);
    if (!nw) return 0;
    if (Ptr) {
        /* Copy min(Size, PAGE_SIZE) bytes via physmap — best effort */
        UINT64 copy_len = Size < PAGE_SIZE ? Size : PAGE_SIZE;
        __builtin_memcpy((void *)(uintptr_t)nw,
                         (const void *)(uintptr_t)Ptr,
                         (size_t)copy_len);
        sys_KhRtlFreeHeap(HeapHandle, Flags, Ptr, 0);
    }
    return nw;
}

/* KH 0x01F3: GetCurrentProcessId() → UINT32 PID */
static UINT64 sys_KhGetCurrentProcessId(UINT64 a1, UINT64 a2,
                                          UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    PEPROCESS proc = PsGetCurrentProcess();
    return proc ? proc->UniqueProcessId : 0;
}

/* KH 0x01F4: GetCurrentThreadId() → UINT32 TID */
static UINT64 sys_KhGetCurrentThreadId(UINT64 a1, UINT64 a2,
                                         UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    PETHREAD et = PsGetCurrentThread();
    return et ? et->UniqueThread : 0;
}

/* KH 0x01F5: GetLastError() → UINT32 */
static UINT64 sys_KhGetLastError(UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    PETHREAD et = PsGetCurrentThread();
    if (!et || !et->Teb) return 0;
    /* TEB.LastErrorValue is at offset 0x68 — access via physmap not possible
     * (TEB is in user VA).  Kernel tracks LastErrorValue in ETHREAD instead. */
    return (UINT64)et->ExitStatus; /* Phase 6: repurpose ExitStatus as a placeholder */
}

/* KH 0x01F6: SetLastError(ErrorCode) */
static UINT64 sys_KhSetLastError(UINT64 ErrorCode, UINT64 a2,
                                   UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    /* Phase 6: silently accept — no TEB write without physmap lookup */
    (void)ErrorCode;
    return 0;
}

/* KH 0x01F7: DbgPrint(FormatStr, ...) */
static UINT64 sys_KhDbgPrint(UINT64 FmtPtr, UINT64 a2,
                               UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    if (FmtPtr)
        kprintf("[DbgPrint] %s", (const char *)(uintptr_t)FmtPtr);
    return 0;
}

/* KH 0x01F8: GetStdHandle(nStdHandle) → HANDLE */
static UINT64 sys_KhGetStdHandle(UINT64 StdHandle, UINT64 a2,
                                   UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    /* STD_INPUT_HANDLE = -10, STD_OUTPUT_HANDLE = -11, STD_ERROR_HANDLE = -12 */
    /* Return pseudo-handles; real console I/O is Phase 8 */
    switch ((INT32)StdHandle) {
    case -10: return (UINT64)(INT64)-10; /* STD_INPUT  */
    case -11: return (UINT64)(INT64)-11; /* STD_OUTPUT */
    case -12: return (UINT64)(INT64)-12; /* STD_ERROR  */
    default:  return (UINT64)(INT64)-1;  /* INVALID_HANDLE_VALUE */
    }
}

/* KH 0x01F9: RtlInitUnicodeString(UNICODE_STRING*, PCWSTR) */
static UINT64 sys_KhRtlInitUnicodeString(UINT64 DestPtr, UINT64 SrcPtr,
                                           UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UNICODE_STRING *us = (UNICODE_STRING *)(uintptr_t)DestPtr;
    const WCHAR    *s  = (const WCHAR *)(uintptr_t)SrcPtr;
    if (!us) return 0;
    if (!s) {
        us->Length = us->MaximumLength = 0;
        us->Buffer = NULL;
        return 0;
    }
    USHORT len = 0;
    while (s[len]) len++;
    us->Buffer        = (WCHAR *)(uintptr_t)SrcPtr;
    us->Length        = (USHORT)(len * sizeof(WCHAR));
    us->MaximumLength = (USHORT)((len + 1) * sizeof(WCHAR));
    return 0;
}

/* KH 0x01FA: GetProcessHeap() → PEB.ProcessHeap VA */
static UINT64 sys_KhGetProcessHeap(UINT64 a1, UINT64 a2,
                                     UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    PEPROCESS proc = PsGetCurrentProcess();
    if (!proc || !proc->Peb) return 0;
    /* PEB.ProcessHeap is at offset 0x30 in 64-bit PEB */
    /* We can't easily read user-mode PEB here; return USER_HEAP_VA directly */
    return USER_HEAP_VA;
}

/* KH 0x01FB: IsDebuggerPresent() → BOOL */
static UINT64 sys_KhIsDebuggerPresent(UINT64 a1, UINT64 a2,
                                        UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return 0; /* FALSE — no debugger */
}

/* KH 0x01FC: RtlZeroMemory(Dst, Len) */
static UINT64 sys_KhRtlZeroMemory(UINT64 Dst, UINT64 Len,
                                    UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    if (Dst && Len)
        __builtin_memset((void *)(uintptr_t)Dst, 0, (size_t)Len);
    return 0;
}

/* KH 0x01FD: RtlMoveMemory(Dst, Src, Len) */
static UINT64 sys_KhRtlMoveMemory(UINT64 Dst, UINT64 Src,
                                    UINT64 Len, UINT64 a4)
{
    (void)a4;
    if (Dst && Src && Len)
        __builtin_memmove((void *)(uintptr_t)Dst,
                          (const void *)(uintptr_t)Src,
                          (size_t)Len);
    return 0;
}

/* KH 0x01FE: GetCurrentProcess() → (HANDLE)-1 */
static UINT64 sys_KhGetCurrentProcess(UINT64 a1, UINT64 a2,
                                        UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return (UINT64)(INT64)-1;
}

/* KH 0x01FF: GetCurrentThread() → (HANDLE)-2 */
static UINT64 sys_KhGetCurrentThread(UINT64 a1, UINT64 a2,
                                       UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return (UINT64)(INT64)-2;
}

/* -----------------------------------------------------------------------
 * Phase 3 syscall handlers
 * ----------------------------------------------------------------------- */

/* --- NtCreateFile (0x0055 in Win10 1903 — actually varies) --- */
/* NtCreateFile: 11 args, we handle via IoCreateFile with first 4 */
static UINT64 sys_NtCreateFile(UINT64 FileHandlePtr, UINT64 DesiredAccess,
                                   UINT64 ObjAttrPtr, UINT64 IoStatusPtr)
{
    if (!FileHandlePtr || !ObjAttrPtr) return STATUS_INVALID_PARAMETER;
    IO_STATUS_BLOCK  isb;
    HANDLE h = 0;
    NTSTATUS s = IoCreateFile(&h,
                               (ACCESS_MASK)DesiredAccess,
                               (POBJECT_ATTRIBUTES)(uintptr_t)ObjAttrPtr,
                               &isb, NULL, 0, 0,
                               FILE_OPEN_IF, 0, NULL, 0);
    if (IoStatusPtr) *(IO_STATUS_BLOCK *)(uintptr_t)IoStatusPtr = isb;
    if (NT_SUCCESS(s)) *(HANDLE *)(uintptr_t)FileHandlePtr = h;
    return s;
}

/* --- NtReadFile (0x0006) ---
 * Args: FileHandle, Event, ApcRoutine, ApcContext,
 *       IoStatusBlock (5th), Buffer (6th), Length (7th),
 *       ByteOffset (8th), Key (9th)
 * Phase 4: dispatch first 4 args via registers; remaining via pointer args
 * encoded in a3/a4 as IoStatusBlock and Buffer ptr for simple callers. */
static UINT64 sys_NtReadFile(UINT64 FileHandle, UINT64 IoStatusPtr,
                                 UINT64 BufferPtr, UINT64 Length)
{
    IO_STATUS_BLOCK isb;
    __builtin_memset(&isb, 0, sizeof(isb));

    void   *buf = (void *)(uintptr_t)BufferPtr;
    UINT32  len = (UINT32)Length;

    NTSTATUS s = IoReadFile((HANDLE)FileHandle,
                             (HANDLE)0, NULL, NULL,
                             &isb,
                             buf, len,
                             NULL, NULL);

    if (IoStatusPtr) *(IO_STATUS_BLOCK *)(uintptr_t)IoStatusPtr = isb;
    return s;
}

/* --- NtWriteFile (0x0008) --- */
static UINT64 sys_NtWriteFile(UINT64 FileHandle, UINT64 IoStatusPtr,
                                  UINT64 BufferPtr, UINT64 Length)
{
    IO_STATUS_BLOCK isb;
    __builtin_memset(&isb, 0, sizeof(isb));

    void   *buf = (void *)(uintptr_t)BufferPtr;
    UINT32  len = (UINT32)Length;

    NTSTATUS s = IoWriteFile((HANDLE)FileHandle,
                             (HANDLE)0, NULL, NULL,
                             &isb,
                             buf, len,
                             NULL, NULL);

    if (IoStatusPtr) *(IO_STATUS_BLOCK *)(uintptr_t)IoStatusPtr = isb;
    return s;
}

/* --- NtQueryInformationFile (0x0011) --- */
static UINT64 sys_NtQueryInformationFile(UINT64 FileHandle,
                                             UINT64 IoStatusPtr,
                                             UINT64 FileInfoPtr,
                                             UINT64 Length)
{
    /* FileInformationClass is the 5th argument — stub to STATUS_SUCCESS */
    IO_STATUS_BLOCK isb;
    NTSTATUS s = IoQueryInformationFile((HANDLE)FileHandle,
                                         &isb,
                                         (void *)(uintptr_t)FileInfoPtr,
                                         (UINT32)Length,
                                         FileBasicInformation);
    if (IoStatusPtr) *(IO_STATUS_BLOCK *)(uintptr_t)IoStatusPtr = isb;
    return s;
}

/* --- NtCreateSection (0x004A) --- */
static UINT64 sys_NtCreateSection(UINT64 SectionHandlePtr,
                                      UINT64 DesiredAccess,
                                      UINT64 ObjAttrPtr,
                                      UINT64 MaximumSizePtr)
{
    HANDLE h = 0;
    UINT64 maxsz = MaximumSizePtr ? *(UINT64 *)(uintptr_t)MaximumSizePtr : 0;
    NTSTATUS s = NtCreateSection(&h,
                                  (ACCESS_MASK)DesiredAccess,
                                  (POBJECT_ATTRIBUTES)(uintptr_t)ObjAttrPtr,
                                  MaximumSizePtr ? &maxsz : NULL,
                                  PAGE_READWRITE,
                                  SEC_COMMIT,
                                  0);
    if (NT_SUCCESS(s) && SectionHandlePtr)
        *(HANDLE *)(uintptr_t)SectionHandlePtr = h;
    return s;
}

/* --- NtMapViewOfSection (0x0028) --- */
static UINT64 sys_NtMapViewOfSection(UINT64 SectionHandle,
                                         UINT64 ProcessHandle,
                                         UINT64 BaseAddressPtr,
                                         UINT64 ZeroBits)
{
    void  *base = BaseAddressPtr ? *(void **)(uintptr_t)BaseAddressPtr : NULL;
    UINT64 view_size = 0;
    NTSTATUS s = NtMapViewOfSection((HANDLE)SectionHandle,
                                     (HANDLE)ProcessHandle,
                                     &base,
                                     (ULONG_PTR)ZeroBits,
                                     0, NULL, &view_size,
                                     ViewShare, 0,
                                     PAGE_READWRITE);
    if (NT_SUCCESS(s) && BaseAddressPtr)
        *(void **)(uintptr_t)BaseAddressPtr = base;
    return s;
}

/* --- NtUnmapViewOfSection (0x002A) --- */
static UINT64 sys_NtUnmapViewOfSection(UINT64 ProcessHandle,
                                           UINT64 BaseAddress,
                                           UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    return NtUnmapViewOfSection((HANDLE)ProcessHandle,
                                 (void *)(uintptr_t)BaseAddress);
}

/* --- NtQueryVirtualMemory (0x0023) --- */
static UINT64 sys_NtQueryVirtualMemory(UINT64 ProcessHandle,
                                           UINT64 BaseAddress,
                                           UINT64 MemInfoClass,
                                           UINT64 MemInfoPtr)
{
    if (MemInfoClass != 0 /* MemoryBasicInformation */) return STATUS_INVALID_INFO_CLASS;

    PEPROCESS proc;
    if ((INT64)ProcessHandle == -1) proc = PsGetCurrentProcess();
    else {
        void *obj;
        NTSTATUS s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                               PROCESS_ALL_ACCESS, ObpProcessType,
                                               NULL, &obj, NULL);
        if (!NT_SUCCESS(s)) return s;
        proc = (PEPROCESS)obj;
    }
    if (!proc) return STATUS_INVALID_HANDLE;

    MEMORY_BASIC_INFORMATION mbi;
    UINT64 rlen = 0;
    NTSTATUS s = VmaQuery(&proc->VmaSpace, BaseAddress, &mbi, &rlen);
    if (NT_SUCCESS(s) && MemInfoPtr)
        __builtin_memcpy((void *)(uintptr_t)MemInfoPtr, &mbi, sizeof(mbi));
    if ((INT64)ProcessHandle != -1) ObDereferenceObject(proc);
    return s;
}

/* --- NtProtectVirtualMemory (0x0050) --- */
static UINT64 sys_NtProtectVirtualMemory(UINT64 ProcessHandle,
                                             UINT64 BaseAddrPtr,
                                             UINT64 RegionSizePtr,
                                             UINT64 NewProtect)
{
    PEPROCESS proc;
    if ((INT64)ProcessHandle == -1) proc = PsGetCurrentProcess();
    else {
        void *obj;
        NTSTATUS s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                               PROCESS_ALL_ACCESS, ObpProcessType,
                                               NULL, &obj, NULL);
        if (!NT_SUCCESS(s)) return s;
        proc = (PEPROCESS)obj;
    }
    if (!proc) return STATUS_INVALID_HANDLE;

    UINT64 base = BaseAddrPtr ? *(UINT64 *)(uintptr_t)BaseAddrPtr : 0;
    UINT64 size = RegionSizePtr ? *(UINT64 *)(uintptr_t)RegionSizePtr : 0;
    UINT32 old  = 0;
    NTSTATUS s = VmaProtect(&proc->VmaSpace, &base, &size, (UINT32)NewProtect, &old);
    if ((INT64)ProcessHandle != -1) ObDereferenceObject(proc);
    return s;
}

/* -----------------------------------------------------------------------
 * Syscall dispatch table
 * ----------------------------------------------------------------------- */
static SYSCALL_HANDLER syscall_table[SYSCALL_MAX];

static void build_syscall_table(void)
{
    /* Default all entries to not-implemented */
    for (int i = 0; i < SYSCALL_MAX; i++)
        syscall_table[i] = sys_NotImplemented;

    /* Wire in implemented handlers */
    syscall_table[SYSCALL_NtClose]                    = sys_NtClose;
    syscall_table[SYSCALL_NtYieldExecution]           = sys_NtYieldExecution;
    syscall_table[SYSCALL_NtDelayExecution]           = sys_NtDelayExecution;
    syscall_table[SYSCALL_NtQuerySystemTime]          = sys_NtQuerySystemTime;
    syscall_table[SYSCALL_NtQueryPerformanceCounter]  = sys_NtQueryPerformanceCounter;
    syscall_table[SYSCALL_NtQuerySystemInformation]   = sys_NtQuerySystemInformation;
    syscall_table[SYSCALL_NtQueryInformationProcess]  = sys_NtQueryInformationProcess;
    syscall_table[SYSCALL_NtQueryInformationThread]   = sys_NtQueryInformationThread;
    syscall_table[SYSCALL_NtOpenProcess]              = sys_NtOpenProcess;
    syscall_table[SYSCALL_NtAllocateVirtualMemory]    = sys_NtAllocateVirtualMemory;
    syscall_table[SYSCALL_NtFreeVirtualMemory]        = sys_NtFreeVirtualMemory;
    syscall_table[SYSCALL_NtOpenKey]                  = sys_NtOpenKey;
    syscall_table[SYSCALL_NtCreateKey]                = sys_NtCreateKey;
    syscall_table[SYSCALL_NtQueryValueKey]            = sys_NtQueryValueKey;
    syscall_table[SYSCALL_NtSetValueKey]              = sys_NtSetValueKey;
    syscall_table[SYSCALL_NtTerminateProcess]         = sys_NtTerminateProcess;
    syscall_table[SYSCALL_NtEnumerateKey]             = (SYSCALL_HANDLER)(void *)sys_NotImplemented;
    syscall_table[SYSCALL_NtEnumerateValueKey]        = (SYSCALL_HANDLER)(void *)sys_NotImplemented;

    /* Phase 3: I/O Manager + Section + VMA */
    syscall_table[SYSCALL_NtCreateFile]               = sys_NtCreateFile;
    syscall_table[SYSCALL_NtReadFile]                 = sys_NtReadFile;
    syscall_table[SYSCALL_NtWriteFile]                = sys_NtWriteFile;
    syscall_table[SYSCALL_NtQueryInformationFile]     = sys_NtQueryInformationFile;
    syscall_table[SYSCALL_NtCreateSection]            = sys_NtCreateSection;
    syscall_table[SYSCALL_NtMapViewOfSection]         = sys_NtMapViewOfSection;
    syscall_table[SYSCALL_NtUnmapViewOfSection]       = sys_NtUnmapViewOfSection;
    syscall_table[SYSCALL_NtQueryVirtualMemory]       = sys_NtQueryVirtualMemory;
    syscall_table[SYSCALL_NtProtectVirtualMemory]     = sys_NtProtectVirtualMemory;

    /* Phase 6: new NT syscalls */
    syscall_table[SYSCALL_NtSetInformationThread]     = sys_NtSetInformationThread;
    syscall_table[SYSCALL_NtFlushInstructionCache]    = sys_NtFlushInstructionCache;
    syscall_table[SYSCALL_NtCreateProcessEx]          = sys_NtCreateProcessEx;
    syscall_table[SYSCALL_NtCreateThread]             = sys_NtCreateThread;

    /* Phase 6: kernel-helper handlers (0x01F0–0x01FF) */
    syscall_table[KH_RtlAllocateHeap]       = sys_KhRtlAllocateHeap;
    syscall_table[KH_RtlFreeHeap]           = sys_KhRtlFreeHeap;
    syscall_table[KH_RtlReAllocateHeap]     = sys_KhRtlReAllocateHeap;
    syscall_table[KH_GetCurrentProcessId]   = sys_KhGetCurrentProcessId;
    syscall_table[KH_GetCurrentThreadId]    = sys_KhGetCurrentThreadId;
    syscall_table[KH_GetLastError]          = sys_KhGetLastError;
    syscall_table[KH_SetLastError]          = sys_KhSetLastError;
    syscall_table[KH_DbgPrint]              = sys_KhDbgPrint;
    syscall_table[KH_GetStdHandle]          = sys_KhGetStdHandle;
    syscall_table[KH_RtlInitUnicodeString]  = sys_KhRtlInitUnicodeString;
    syscall_table[KH_GetProcessHeap]        = sys_KhGetProcessHeap;
    syscall_table[KH_IsDebuggerPresent]     = sys_KhIsDebuggerPresent;
    syscall_table[KH_RtlZeroMemory]         = sys_KhRtlZeroMemory;
    syscall_table[KH_RtlMoveMemory]         = sys_KhRtlMoveMemory;
    syscall_table[KH_GetCurrentProcess]     = sys_KhGetCurrentProcess;
    syscall_table[KH_GetCurrentThread]      = sys_KhGetCurrentThread;
}

/* -----------------------------------------------------------------------
 * KiSystemCallDispatch — C entry point from ASM stub
 * ----------------------------------------------------------------------- */
UINT64 KiSystemCallDispatch(UINT64 num, UINT64 arg1, UINT64 arg2,
                             UINT64 arg3, UINT64 arg4)
{
    if (num >= SYSCALL_MAX)
        return (UINT64)(UINT32)STATUS_INVALID_SYSTEM_SERVICE;

    SYSCALL_HANDLER handler = syscall_table[num];
    return handler(arg1, arg2, arg3, arg4);
}

/* -----------------------------------------------------------------------
 * SyscallInitialize — set up MSRs for SYSCALL/SYSRET
 * ----------------------------------------------------------------------- */
void SyscallInitialize(void)
{
    build_syscall_table();

    /* STAR MSR:
     *   Bits 47:32 = kernel CS (for SYSCALL: CS = STAR[47:32], SS = STAR[47:32]+8)
     *   Bits 63:48 = user CS-16 (for SYSRET: CS = STAR[63:48]+16, SS = STAR[63:48]+8)
     *
     * We use: kernel CS = GDT_KERNEL_CODE = 0x08, kernel SS = 0x10
     *         user CS = GDT_USER_CODE = 0x18, user SS = GDT_USER_DATA = 0x20
     * For SYSRET 64-bit: CS = STAR[63:48]+16, SS = STAR[63:48]+8
     *   => STAR[63:48] = 0x18 - 16 = 0x08 — but use 0x10 so user CS = 0x18+16=0x28
     *   Actually Windows uses: STAR[63:48] = 0x0023 (user CS = 0x33, user SS = 0x2B)
     *   We replicate that layout:
     *     STAR[47:32] = 0x0010  (kernel: CS=0x10 is wrong — fix: use 0x08)
     *
     * Correct STAR layout for our GDT:
     *   kernel SS = kernel CS + 8 = 0x08 + 0x08 = 0x10 ✓ (GDT_KERNEL_DATA)
     *   STAR[47:32] = GDT_KERNEL_CODE = 0x08
     *   user CS (SYSRET 64-bit) = STAR[63:48] + 16
     *   user SS (SYSRET)        = STAR[63:48] + 8
     *   We want user CS = 0x18 (GDT_USER_CODE | 3 = 0x1B with RPL)
     *           user SS = 0x20 (GDT_USER_DATA | 3 = 0x23 with RPL)
     *   => STAR[63:48] = 0x18 - 16 = 0x08, giving user CS = 0x18, SS = 0x10 (wrong)
     *
     *   Windows NT GDT order: 0x08=kcode, 0x10=kdata, 0x18=ucode32, 0x20=udata, 0x28=ucode64
     *   For SYSRET to 64-bit: CS = STAR[63:48]+16, SS = STAR[63:48]+8
     *   We want CS=0x28|3=0x2B (user code 64-bit), SS=0x20|3=0x23 (user data)
     *   => STAR[63:48] = 0x28 - 16 = 0x18
     *      then SS = 0x18 + 8 = 0x20 ✓, CS = 0x18 + 16 = 0x28 ✓
     *
     *   But our GDT has GDT_USER_CODE=0x18, GDT_USER_DATA=0x20 (see gdt.h).
     *   For Phase 2 (kernel-only), user mode segments don't matter yet.
     *   Use same layout as Windows: STAR[63:48]=0x0018, STAR[47:32]=0x0008
     */
    UINT64 star = ((UINT64)0x0018 << 48) | ((UINT64)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);

    /* LSTAR = kernel entry for 64-bit SYSCALL */
    wrmsr(MSR_LSTAR, (UINT64)(uintptr_t)KiSystemCall64);

    /* CSTAR = kernel entry for compat-mode SYSCALL (we don't support it) */
    wrmsr(MSR_CSTAR, 0);

    /* SFMASK = clear IF (bit 9) and DF (bit 10) on syscall entry */
    wrmsr(MSR_SFMASK, 0x300);

    /* Enable SCE in EFER */
    UINT64 efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);

    kprintf("[SYSCALL] Initialized: LSTAR=%p STAR=0x%llx\n",
            (void *)KiSystemCall64,
            (unsigned long long)star);
    kprintf("[SYSCALL] Dispatch table: %u entries wired\n", SYSCALL_MAX);
}
