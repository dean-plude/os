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
#include "prof.h"
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
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "probe.h"
#include "smp.h"
#include "kpcr.h"
#include "../um/um.h"

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


/* -----------------------------------------------------------------------
 * Individual syscall handler implementations
 *
 * Every pointer argument comes from ring 3 and is untrusted.  Handlers
 * never dereference one directly: inputs are copied into kernel locals
 * with CopyFromUser()/Capture*(), and results are copied back with
 * CopyToUser().  A bad pointer yields STATUS_ACCESS_VIOLATION.
 * ----------------------------------------------------------------------- */

#define UPTR(p)  ((void *)(uintptr_t)(p))

/* Store a newly created handle in the caller's HANDLE*.  If the pointer
 * turns out to be bad, close the handle so it does not leak. */
static NTSTATUS return_handle(UINT64 HandlePtr, HANDLE h)
{
    NTSTATUS s = CopyToUser(UPTR(HandlePtr), &h, sizeof(h));
    if (!NT_SUCCESS(s)) ObCloseHandle(h, NULL);
    return s;
}

/* Copy an IO_STATUS_BLOCK out (if requested).  A failed copy only
 * overrides the result when the operation itself succeeded. */
static NTSTATUS return_iosb(UINT64 IoStatusPtr, const IO_STATUS_BLOCK *isb,
                            NTSTATUS s)
{
    if (!IoStatusPtr) return s;
    NTSTATUS cs = CopyToUser(UPTR(IoStatusPtr), isb, sizeof(*isb));
    return (NT_SUCCESS(s) && !NT_SUCCESS(cs)) ? cs : s;
}

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
        UINT64 t = 0;
        NTSTATUS s = CopyToUser(UPTR(SystemTimePtr), &t, sizeof(t));
        if (!NT_SUCCESS(s)) return s;
    }
    return STATUS_SUCCESS;
}

/* --- NtQueryPerformanceCounter (0x0031) --- */
static UINT64 sys_NtQueryPerformanceCounter(UINT64 CounterPtr,
                                               UINT64 FreqPtr,
                                               UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UINT64 tsc  = rdtsc();
    UINT64 freq = 2000000000ULL; /* ~2GHz */
    NTSTATUS s;
    if (CounterPtr && !NT_SUCCESS(s = CopyToUser(UPTR(CounterPtr), &tsc, sizeof(tsc))))
        return s;
    if (FreqPtr && !NT_SUCCESS(s = CopyToUser(UPTR(FreqPtr), &freq, sizeof(freq))))
        return s;
    return STATUS_SUCCESS;
}

/* --- NtQuerySystemInformation (0x0036) --- */
static UINT64 sys_NtQuerySystemInformation(UINT64 InfoClass,
                                              UINT64 InfoPtr,
                                              UINT64 InfoLen,
                                              UINT64 ReturnLenPtr)
{
    NTSTATUS s;
    UINT32   retlen;

    switch ((UINT32)InfoClass) {
    case SystemBasicInformation: {
        retlen = sizeof(SYSTEM_BASIC_INFORMATION);
        if (ReturnLenPtr &&
            !NT_SUCCESS(s = CopyToUser(UPTR(ReturnLenPtr), &retlen, sizeof(retlen))))
            return s;
        if (InfoLen < sizeof(SYSTEM_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        if (!InfoPtr) return STATUS_INVALID_PARAMETER;

        SYSTEM_BASIC_INFORMATION sbi;
        __builtin_memset(&sbi, 0, sizeof(sbi));
        sbi.PageSize              = (UINT32)PAGE_SIZE;
        sbi.AllocationGranularity = 65536;  /* 64KB, same as Windows */
        sbi.MinimumUserModeAddress = 0x10000;
        sbi.MaximumUserModeAddress = 0x7FFFFFFFFFFEFFFF;
        sbi.ActiveProcessorsAffinityMask = (1ULL << g_cpu_count) - 1;
        sbi.NumberOfProcessors    = (UINT8)g_cpu_count;
        extern size_t pmm_ram_pages(void);
        sbi.NumberOfPhysicalPages = (UINT32)pmm_ram_pages();     /* the machine's RAM */
        return CopyToUser(UPTR(InfoPtr), &sbi, sizeof(sbi));
    }
    default:
        retlen = 0;
        if (ReturnLenPtr &&
            !NT_SUCCESS(s = CopyToUser(UPTR(ReturnLenPtr), &retlen, sizeof(retlen))))
            return s;
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* --- NtQueryInformationProcess (0x0019) --- */
static UINT64 sys_NtQueryInformationProcess(UINT64 ProcessHandle,
                                               UINT64 ProcInfoClass,
                                               UINT64 ProcInfoPtr,
                                               UINT64 ProcInfoLen)
{
    /* ObReferenceObjectByHandle also resolves the NtCurrentProcess()
     * pseudo-handle (-1), taking a reference we must drop on every path. */
    void *proc_obj = NULL;
    NTSTATUS s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                            PROCESS_ALL_ACCESS,
                                            ObpProcessType, NULL,
                                            &proc_obj, NULL);
    if (!NT_SUCCESS(s)) return s;
    PEPROCESS proc = (PEPROCESS)proc_obj;
    if (!proc) return STATUS_INVALID_HANDLE;

    switch ((UINT32)ProcInfoClass) {
    case ProcessBasicInformation: {
        if (ProcInfoLen < sizeof(PROCESS_BASIC_INFORMATION) || !ProcInfoPtr) {
            ObDereferenceObject(proc_obj);
            return ProcInfoPtr ? STATUS_BUFFER_TOO_SMALL
                               : STATUS_INVALID_PARAMETER;
        }

        PROCESS_BASIC_INFORMATION pbi;
        __builtin_memset(&pbi, 0, sizeof(pbi));
        pbi.ExitStatus            = proc->ExitStatus;
        pbi.PebBaseAddress        = proc->Peb;
        pbi.AffinityMask          = 1;
        pbi.BasePriority          = 8;
        pbi.UniqueProcessId       = proc->UniqueProcessId;
        pbi.InheritedFromUniqueProcessId =
            proc->InheritedFromUniqueProcessId;
        ObDereferenceObject(proc_obj);
        return CopyToUser(UPTR(ProcInfoPtr), &pbi, sizeof(pbi));
    }
    default:
        ObDereferenceObject(proc_obj);
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
        if (ThreadInfoLen < sizeof(THREAD_BASIC_INFORMATION) || !ThreadInfoPtr) {
            if ((INT64)ThreadHandle != -2) ObDereferenceObject((void *)et);
            return ThreadInfoPtr ? STATUS_BUFFER_TOO_SMALL
                                 : STATUS_INVALID_PARAMETER;
        }

        THREAD_BASIC_INFORMATION tbi;
        __builtin_memset(&tbi, 0, sizeof(tbi));
        tbi.ExitStatus          = et->ExitStatus;
        tbi.TebBaseAddress      = et->Teb;
        tbi.ClientId.UniqueProcess = et->Cid.UniqueProcess;
        tbi.ClientId.UniqueThread  = et->Cid.UniqueThread;
        tbi.AffinityMask        = 1;
        tbi.Priority            = 8;
        tbi.BasePriority        = 8;
        if ((INT64)ThreadHandle != -2) ObDereferenceObject((void *)et);
        return CopyToUser(UPTR(ThreadInfoPtr), &tbi, sizeof(tbi));
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

    struct { UINT64 UniqueProcess; UINT64 UniqueThread; } cid;
    if (!ClientIdPtr) return STATUS_INVALID_PARAMETER;
    NTSTATUS s = CopyFromUser(&cid, UPTR(ClientIdPtr), sizeof(cid));
    if (!NT_SUCCESS(s)) return s;

    PEPROCESS proc = NULL;
    s = PsLookupProcessByProcessId(cid.UniqueProcess, &proc);
    if (!NT_SUCCESS(s)) return s;

    HANDLE h = 0;
    s = ObInsertObject(proc, NULL, (ACCESS_MASK)DesiredAccess, 0, NULL, &h);
    ObDereferenceObject(proc);   /* InsertObject added its own reference */
    if (!NT_SUCCESS(s)) return s;

    if (ProcessHandlePtr) return return_handle(ProcessHandlePtr, h);
    return STATUS_SUCCESS;
}

/* --- NtAllocateVirtualMemory (0x0018) --- */
static UINT64 sys_NtAllocateVirtualMemory(UINT64 ProcessHandle,
                                             UINT64 BaseAddressPtr,
                                             UINT64 RegionSizePtr,
                                             UINT64 AllocationType)
{
    if (!RegionSizePtr) return STATUS_INVALID_PARAMETER;

    /* Capture in/out parameters before touching the process */
    UINT64 base = 0, size = 0;
    NTSTATUS s;
    if (BaseAddressPtr &&
        !NT_SUCCESS(s = CopyFromUser(&base, UPTR(BaseAddressPtr), sizeof(base))))
        return s;
    if (!NT_SUCCESS(s = CopyFromUser(&size, UPTR(RegionSizePtr), sizeof(size))))
        return s;
    if (!size) return STATUS_INVALID_PARAMETER;

    PEPROCESS proc;
    if ((INT64)ProcessHandle == -1) proc = PsGetCurrentProcess();
    else {
        void *obj;
        s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                      PROCESS_ALL_ACCESS, ObpProcessType,
                                      NULL, &obj, NULL);
        if (!NT_SUCCESS(s)) return s;
        proc = (PEPROCESS)obj;
    }
    if (!proc) return STATUS_INVALID_HANDLE;

    UINT32 protect = PAGE_READWRITE;
    s = VmaAllocate(&proc->VmaSpace, &base, &size,
                    (UINT32)AllocationType, protect);
    if ((INT64)ProcessHandle != -1) ObDereferenceObject(proc);
    if (!NT_SUCCESS(s)) return s;

    if (BaseAddressPtr &&
        !NT_SUCCESS(s = CopyToUser(UPTR(BaseAddressPtr), &base, sizeof(base))))
        return s;
    return CopyToUser(UPTR(RegionSizePtr), &size, sizeof(size));
}

/* --- NtFreeVirtualMemory (0x001E) --- */
static UINT64 sys_NtFreeVirtualMemory(UINT64 ProcessHandle,
                                         UINT64 BaseAddressPtr,
                                         UINT64 RegionSizePtr,
                                         UINT64 FreeType)
{
    if (!BaseAddressPtr) return STATUS_INVALID_PARAMETER;

    UINT64 base = 0, size = 0;
    NTSTATUS s;
    if (!NT_SUCCESS(s = CopyFromUser(&base, UPTR(BaseAddressPtr), sizeof(base))))
        return s;
    if (RegionSizePtr &&
        !NT_SUCCESS(s = CopyFromUser(&size, UPTR(RegionSizePtr), sizeof(size))))
        return s;

    PEPROCESS proc;
    if ((INT64)ProcessHandle == -1) proc = PsGetCurrentProcess();
    else {
        void *obj;
        s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                      PROCESS_ALL_ACCESS, ObpProcessType,
                                      NULL, &obj, NULL);
        if (!NT_SUCCESS(s)) return s;
        proc = (PEPROCESS)obj;
    }
    if (!proc) return STATUS_INVALID_HANDLE;

    s = VmaFree(&proc->VmaSpace, &base, &size, (UINT32)FreeType);
    if ((INT64)ProcessHandle != -1) ObDereferenceObject(proc);
    if (!NT_SUCCESS(s)) return s;

    if (!NT_SUCCESS(s = CopyToUser(UPTR(BaseAddressPtr), &base, sizeof(base))))
        return s;
    if (RegionSizePtr)
        return CopyToUser(UPTR(RegionSizePtr), &size, sizeof(size));
    return STATUS_SUCCESS;
}

/* --- NtOpenKey (0x0012) --- */
static UINT64 sys_NtOpenKey(UINT64 KeyHandlePtr, UINT64 DesiredAccess,
                               UINT64 ObjAttrPtr, UINT64 a4)
{
    (void)a4;
    if (!KeyHandlePtr || !ObjAttrPtr) return STATUS_INVALID_PARAMETER;

    CAPTURED_OBJECT_ATTRIBUTES coa;
    NTSTATUS s = CaptureObjectAttributes(UPTR(ObjAttrPtr), &coa);
    if (!NT_SUCCESS(s)) return s;

    HANDLE h = 0;
    s = NtOpenKey(&h, (ACCESS_MASK)DesiredAccess, &coa.Attributes);
    ReleaseCapturedObjectAttributes(&coa);
    if (!NT_SUCCESS(s)) return s;
    return return_handle(KeyHandlePtr, h);
}

/* --- NtCreateKey (0x001D) --- */
static UINT64 sys_NtCreateKey(UINT64 KeyHandlePtr, UINT64 DesiredAccess,
                                  UINT64 ObjAttrPtr, UINT64 TitleIndex)
{
    if (!KeyHandlePtr || !ObjAttrPtr) return STATUS_INVALID_PARAMETER;

    CAPTURED_OBJECT_ATTRIBUTES coa;
    NTSTATUS s = CaptureObjectAttributes(UPTR(ObjAttrPtr), &coa);
    if (!NT_SUCCESS(s)) return s;

    HANDLE h = 0;
    UINT32 disp = 0;
    s = NtCreateKey(&h, (ACCESS_MASK)DesiredAccess, &coa.Attributes,
                    (UINT32)TitleIndex, NULL, 0, &disp);
    ReleaseCapturedObjectAttributes(&coa);
    if (!NT_SUCCESS(s)) return s;
    return return_handle(KeyHandlePtr, h);
}

/* --- NtQueryValueKey (0x0017) --- */
static UINT64 sys_NtQueryValueKey(UINT64 KeyHandle, UINT64 ValueNamePtr,
                                     UINT64 InfoClass, UINT64 InfoPtr)
{
    /* Note: InfoLen and ResultLength are on user stack (args 5/6) —
     * Phase 2 stub uses a fixed length and ignores ResultLength.  The
     * result is built in a kernel buffer and only the bytes actually
     * produced are copied back to the caller. */
    enum { QUERY_VALUE_LEN = 4096 };
    if (!ValueNamePtr) return STATUS_INVALID_PARAMETER;

    UNICODE_STRING name;
    NTSTATUS s = CaptureUnicodeString(UPTR(ValueNamePtr), &name);
    if (!NT_SUCCESS(s)) return s;

    void *kbuf = NULL;
    if (InfoPtr) {
        kbuf = kzalloc(QUERY_VALUE_LEN);
        if (!kbuf) { ReleaseCapturedUnicodeString(&name); return STATUS_NO_MEMORY; }
    }

    UINT32 resultlen = 0;
    s = NtQueryValueKey((HANDLE)KeyHandle, &name,
                        (KEY_VALUE_INFORMATION_CLASS)(UINT32)InfoClass,
                        kbuf, QUERY_VALUE_LEN, &resultlen);
    if (NT_SUCCESS(s) && kbuf) {
        if (resultlen > QUERY_VALUE_LEN) resultlen = QUERY_VALUE_LEN;
        s = CopyToUser(UPTR(InfoPtr), kbuf, resultlen);
    }

    kfree(kbuf);
    ReleaseCapturedUnicodeString(&name);
    return s;
}

/* --- NtSetValueKey (0x0027) --- */
static UINT64 sys_NtSetValueKey(UINT64 KeyHandle, UINT64 ValueNamePtr,
                                   UINT64 TitleIndex, UINT64 Type)
{
    /* Data and DataSize are on user stack — Phase 2 stub ignores them */
    (void)TitleIndex;
    if (!ValueNamePtr) return STATUS_INVALID_PARAMETER;

    UNICODE_STRING name;
    NTSTATUS s = CaptureUnicodeString(UPTR(ValueNamePtr), &name);
    if (!NT_SUCCESS(s)) return s;

    s = NtSetValueKey((HANDLE)KeyHandle, &name, 0, (UINT32)Type, NULL, 0);
    ReleaseCapturedUnicodeString(&name);
    return s;
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
    if (ProcessHandlePtr) {
        HANDLE none = 0;
        NTSTATUS s = CopyToUser(UPTR(ProcessHandlePtr), &none, sizeof(none));
        if (!NT_SUCCESS(s)) return s;
    }
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
    if (ThreadHandlePtr) {
        HANDLE none = 0;
        NTSTATUS s = CopyToUser(UPTR(ThreadHandlePtr), &none, sizeof(none));
        if (!NT_SUCCESS(s)) return s;
    }
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
        /* Both blocks are user memory: copy through the probed path so a
         * forged Ptr cannot be used to read kernel memory. */
        UINT64 copy_len = Size < PAGE_SIZE ? Size : PAGE_SIZE;
        if (!NT_SUCCESS(CopyUserToUser(UPTR(nw), UPTR(Ptr), (size_t)copy_len))) {
            sys_KhRtlFreeHeap(HeapHandle, Flags, nw, 0);
            return 0;
        }
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
    if (FmtPtr) {
        /* Captured (and truncated) copy; printed via %s, never as a format */
        char msg[256];
        if (NT_SUCCESS(CopyStringFromUser(msg, sizeof(msg), UPTR(FmtPtr))))
            kprintf("[DbgPrint] %s", msg);
    }
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
    if (!DestPtr) return 0;

    UNICODE_STRING us = { 0, 0, NULL };
    if (SrcPtr) {
        /* Largest string whose (len + 1) * 2 still fits a USHORT */
        UINT32 len = 0;
        if (!NT_SUCCESS(ProbeUserWideStringLength(UPTR(SrcPtr), 32766, &len)))
            return 0;
        us.Buffer        = (WCHAR *)(uintptr_t)SrcPtr;   /* stays a user pointer */
        us.Length        = (USHORT)(len * sizeof(WCHAR));
        us.MaximumLength = (USHORT)((len + 1) * sizeof(WCHAR));
    }
    CopyToUser(UPTR(DestPtr), &us, sizeof(us));
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
    if (Dst && Len) ZeroUser(UPTR(Dst), (size_t)Len);
    return 0;
}

/* KH 0x01FD: RtlMoveMemory(Dst, Src, Len) */
static UINT64 sys_KhRtlMoveMemory(UINT64 Dst, UINT64 Src,
                                    UINT64 Len, UINT64 a4)
{
    (void)a4;
    if (Dst && Src && Len) CopyUserToUser(UPTR(Dst), UPTR(Src), (size_t)Len);
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

    CAPTURED_OBJECT_ATTRIBUTES coa;
    NTSTATUS s = CaptureObjectAttributes(UPTR(ObjAttrPtr), &coa);
    if (!NT_SUCCESS(s)) return s;

    IO_STATUS_BLOCK  isb;
    __builtin_memset(&isb, 0, sizeof(isb));
    HANDLE h = 0;
    s = IoCreateFile(&h,
                     (ACCESS_MASK)DesiredAccess,
                     &coa.Attributes,
                     &isb, NULL, 0, 0,
                     FILE_OPEN_IF, 0, NULL, 0);
    ReleaseCapturedObjectAttributes(&coa);

    s = return_iosb(IoStatusPtr, &isb, s);
    if (NT_SUCCESS(s)) return return_handle(FileHandlePtr, h);
    if (h) ObCloseHandle(h, NULL);   /* IOSB copy-out failed after create */
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

    /* Read into a kernel bounce buffer, then copy out what was read.
     * Validate the destination first so a bad buffer consumes no data. */
    UINT32 len = (UINT32)Length;
    if (len > USER_MAX_BOUNCE) len = USER_MAX_BOUNCE;
    NTSTATUS s = ProbeForWrite(UPTR(BufferPtr), len, 1);
    if (!NT_SUCCESS(s)) return s;

    void *kbuf = NULL;
    if (len) {
        kbuf = kmalloc(len);
        if (!kbuf) return STATUS_NO_MEMORY;
    }

    s = IoReadFile((HANDLE)FileHandle,
                   (HANDLE)0, NULL, NULL,
                   &isb,
                   kbuf, len,
                   NULL, NULL);

    if (NT_SUCCESS(s) && kbuf) {
        UINT64 got = isb.Information;
        if (got > len) got = len;
        s = CopyToUser(UPTR(BufferPtr), kbuf, (size_t)got);
    }
    kfree(kbuf);
    return return_iosb(IoStatusPtr, &isb, s);
}

/* --- NtWriteFile (0x0008) --- */
static UINT64 sys_NtWriteFile(UINT64 FileHandle, UINT64 IoStatusPtr,
                                  UINT64 BufferPtr, UINT64 Length)
{
    IO_STATUS_BLOCK isb;
    __builtin_memset(&isb, 0, sizeof(isb));

    /* Copy the data into a kernel bounce buffer before the driver sees it */
    UINT32 len = (UINT32)Length;
    if (len > USER_MAX_BOUNCE) len = USER_MAX_BOUNCE;

    void *kbuf = NULL;
    if (len) {
        kbuf = kmalloc(len);
        if (!kbuf) return STATUS_NO_MEMORY;
    }
    NTSTATUS s = CopyFromUser(kbuf, UPTR(BufferPtr), len);
    if (!NT_SUCCESS(s)) { kfree(kbuf); return s; }

    s = IoWriteFile((HANDLE)FileHandle,
                    (HANDLE)0, NULL, NULL,
                    &isb,
                    kbuf, len,
                    NULL, NULL);
    kfree(kbuf);
    return return_iosb(IoStatusPtr, &isb, s);
}

/* --- NtQueryInformationFile (0x0011) --- */
static UINT64 sys_NtQueryInformationFile(UINT64 FileHandle,
                                             UINT64 IoStatusPtr,
                                             UINT64 FileInfoPtr,
                                             UINT64 Length)
{
    /* FileInformationClass is the 5th argument — stub to STATUS_SUCCESS */
    UINT32 len = (UINT32)Length;
    if (len > USER_MAX_BOUNCE) len = USER_MAX_BOUNCE;
    NTSTATUS s = ProbeForWrite(UPTR(FileInfoPtr), len, 1);
    if (!NT_SUCCESS(s)) return s;

    void *kbuf = NULL;
    if (len) {
        kbuf = kzalloc(len);
        if (!kbuf) return STATUS_NO_MEMORY;
    }

    IO_STATUS_BLOCK isb;
    __builtin_memset(&isb, 0, sizeof(isb));
    s = IoQueryInformationFile((HANDLE)FileHandle,
                               &isb,
                               kbuf,
                               len,
                               FileBasicInformation);
    if (NT_SUCCESS(s) && kbuf)
        s = CopyToUser(UPTR(FileInfoPtr), kbuf, len);
    kfree(kbuf);
    return return_iosb(IoStatusPtr, &isb, s);
}

/* --- NtCreateSection (0x004A) --- */
static UINT64 sys_NtCreateSection(UINT64 SectionHandlePtr,
                                      UINT64 DesiredAccess,
                                      UINT64 ObjAttrPtr,
                                      UINT64 MaximumSizePtr)
{
    NTSTATUS s;
    UINT64 maxsz = 0;
    if (MaximumSizePtr &&
        !NT_SUCCESS(s = CopyFromUser(&maxsz, UPTR(MaximumSizePtr), sizeof(maxsz))))
        return s;

    CAPTURED_OBJECT_ATTRIBUTES coa;
    if (ObjAttrPtr && !NT_SUCCESS(s = CaptureObjectAttributes(UPTR(ObjAttrPtr), &coa)))
        return s;

    HANDLE h = 0;
    s = NtCreateSection(&h,
                        (ACCESS_MASK)DesiredAccess,
                        ObjAttrPtr ? &coa.Attributes : NULL,
                        MaximumSizePtr ? &maxsz : NULL,
                        PAGE_READWRITE,
                        SEC_COMMIT,
                        0);
    if (ObjAttrPtr) ReleaseCapturedObjectAttributes(&coa);
    if (NT_SUCCESS(s) && SectionHandlePtr)
        return return_handle(SectionHandlePtr, h);
    return s;
}

/* --- NtMapViewOfSection (0x0028) --- */
static UINT64 sys_NtMapViewOfSection(UINT64 SectionHandle,
                                         UINT64 ProcessHandle,
                                         UINT64 BaseAddressPtr,
                                         UINT64 ZeroBits)
{
    NTSTATUS s;
    void  *base = NULL;
    if (BaseAddressPtr &&
        !NT_SUCCESS(s = CopyFromUser(&base, UPTR(BaseAddressPtr), sizeof(base))))
        return s;

    UINT64 view_size = 0;
    s = NtMapViewOfSection((HANDLE)SectionHandle,
                           (HANDLE)ProcessHandle,
                           &base,
                           (ULONG_PTR)ZeroBits,
                           0, NULL, &view_size,
                           ViewShare, 0,
                           PAGE_READWRITE);
    if (NT_SUCCESS(s) && BaseAddressPtr) {
        s = CopyToUser(UPTR(BaseAddressPtr), &base, sizeof(base));
        if (!NT_SUCCESS(s))   /* caller can't learn the address — undo */
            NtUnmapViewOfSection((HANDLE)ProcessHandle, base);
    }
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
    if ((INT64)ProcessHandle != -1) ObDereferenceObject(proc);
    if (NT_SUCCESS(s) && MemInfoPtr)
        s = CopyToUser(UPTR(MemInfoPtr), &mbi, sizeof(mbi));
    return s;
}

/* --- NtProtectVirtualMemory (0x0050) --- */
static UINT64 sys_NtProtectVirtualMemory(UINT64 ProcessHandle,
                                             UINT64 BaseAddrPtr,
                                             UINT64 RegionSizePtr,
                                             UINT64 NewProtect)
{
    UINT64 base = 0, size = 0;
    NTSTATUS s;
    if (BaseAddrPtr &&
        !NT_SUCCESS(s = CopyFromUser(&base, UPTR(BaseAddrPtr), sizeof(base))))
        return s;
    if (RegionSizePtr &&
        !NT_SUCCESS(s = CopyFromUser(&size, UPTR(RegionSizePtr), sizeof(size))))
        return s;

    PEPROCESS proc;
    if ((INT64)ProcessHandle == -1) proc = PsGetCurrentProcess();
    else {
        void *obj;
        s = ObReferenceObjectByHandle((HANDLE)ProcessHandle,
                                      PROCESS_ALL_ACCESS, ObpProcessType,
                                      NULL, &obj, NULL);
        if (!NT_SUCCESS(s)) return s;
        proc = (PEPROCESS)obj;
    }
    if (!proc) return STATUS_INVALID_HANDLE;

    UINT32 old  = 0;
    s = VmaProtect(&proc->VmaSpace, &base, &size, (UINT32)NewProtect, &old);
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

/* Install @h for syscall @num; returns the previous handler. */
SYSCALL_HANDLER SyscallSetHandler(UINT32 num, SYSCALL_HANDLER h)
{
    if (num >= SYSCALL_MAX) return NULL;
    SYSCALL_HANDLER old = syscall_table[num];
    syscall_table[num] = h;
    return old;
}

/* The service a system call names.  Windows reads only EAX: its low 12
 * bits are the service number and bit 12 picks the table (0: the kernel's,
 * 1: win32k's); every other bit is ignored, and programs that make
 * system calls themselves may leave them set.  NovaOS has no win32k numbers: a call into that
 * table gets SYSCALL_MAX, STATUS_INVALID_SYSTEM_SERVICE. */
static inline UINT64 service_of(UINT64 eax)
{
    return eax & 0x1000 ? SYSCALL_MAX : eax & 0xFFF;
}

/* -----------------------------------------------------------------------
 * KiSystemCallDispatch — C entry point from ASM stub
 * ----------------------------------------------------------------------- */
UINT64 KiSystemCallDispatch(UINT64 num, UINT64 arg1, UINT64 arg2,
                             UINT64 arg3, UINT64 arg4, UINT64 user_rsp)
{
    num = service_of(num);
    if (num >= SYSCALL_MAX)
        return (UINT64)(UINT32)STATUS_INVALID_SYSTEM_SERVICE;

    Thread *t = sched_current();
    t->user_rsp = user_rsp;                  /* arguments 5+ live on the user stack */

    if (!t->um) return syscall_table[num](arg1, arg2, arg3, arg4);   /* in-kernel callers */
    if (!UmSyscallAllowed(num))                /* legacy services expect Ps/Ob state */
        return (UINT64)(UINT32)STATUS_INVALID_SYSTEM_SERVICE;

    /* A program's service runs with interrupts on: it may be long (file
     * I/O) or wait (console input).  We're on this thread's kernel stack. */
    sti();
    UINT64 r = UmSyscall(num, arg1, arg2, arg3, arg4);
    cli();
    UmReturnToUser();                        /* killed meanwhile? never returns */
    return r;
}

/* SYSCALL instruction path (syscall_entry.asm): the big kernel lock is
 * taken here, on the way in from user mode, and dropped on the way back —
 * unless the service runs under its own locks (UmSyscallLockFree). */
/* SYSCALL executed by 32-bit code (KiSystemCall32): an illegal instruction
 * for a 32-bit program, which ends it */
void KiCompatSyscall(UINT64 rip)
{
    bkl_acquire();
    if (sched_current()->um) UmFault(0xC000001Du, (UINT32)rip - 2, 0);
    kprintf("[SYSCALL] compat-mode SYSCALL outside a program at 0x%llx\n", (unsigned long long)rip);
    for (;;) __asm__ volatile ("cli; hlt");
}

UINT64 KiSystemCallEntry(UINT64 num, UINT64 arg1, UINT64 arg2,
                         UINT64 arg3, UINT64 arg4, UINT64 user_rsp)
{
    /* Programs' services with locks of their own skip the big lock */
    num = service_of(num);
    ProfSyscall(num);
    bool big = !(sched_current()->um && num < SYSCALL_MAX && UmSyscallLockFree(num));
    if (big) bkl_acquire();
    UINT64 r = KiSystemCallDispatch(num, arg1, arg2, arg3, arg4, user_rsp);
    if (big) bkl_release();
    if (bkl_held()) {                                   /* never into user mode with it */
        kprintf("[SMP] Bug: system call %03llx returns holding the kernel lock\n",
                (unsigned long long)num);
        bkl_leave_kernel();
    }
    sched_resched_pending();
    return r;
}

/* -----------------------------------------------------------------------
 * SyscallInitialize — set up MSRs for SYSCALL/SYSRET
 * ----------------------------------------------------------------------- */
void SyscallInitialize(void)
{
    build_syscall_table();
    SyscallInitCpu();
    kprintf("[SYSCALL] Initialized: LSTAR=%p\n", (void *)KiSystemCall64);
    kprintf("[SYSCALL] Dispatch table: %u entries wired\n", SYSCALL_MAX);
}

/* The SYSCALL MSRs of the calling CPU */
void SyscallInitCpu(void)
{

    /* STAR MSR:
     *   Bits 47:32 — SYSCALL: CS = STAR[47:32],      SS = STAR[47:32] + 8
     *   Bits 63:48 — SYSRETQ: CS = STAR[63:48] + 16, SS = STAR[63:48] + 8
     *
     * Kernel: CS = 0x10, SS = 0x18.
     * User:   base = GDT_USER_CODE32 | 3 = 0x23 → SS = 0x2B, CS = 0x33,
     *         Windows' own STAR value.
     * The base carries RPL 3 itself: Intel CPUs force RPL 3 on both
     * selectors, but AMD ones load SS as STAR[63:48] + 8 unchanged, and
     * with 0x10 there programs ran with SS = 0x18 (RPL 0).  That works in
     * 64-bit mode until an interrupt's IRETQ back to the program checks
     * the SS it saved and raises #GP, after its SWAPGS (seen under KVM on
     * AMD hosts; QEMU's emulation forces RPL 3 like Intel).
     * See the selector layout in gdt.h.
     */
    UINT64 star = ((UINT64)SEL_USER_CODE32 << 48) |
                  ((UINT64)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);

    /* LSTAR = kernel entry for 64-bit SYSCALL */
    wrmsr(MSR_LSTAR, (UINT64)(uintptr_t)KiSystemCall64);

    /* CSTAR = kernel entry for compat-mode SYSCALL: 32-bit programs use
     * int 0x2E, so it ends the program (never a jump to address 0) */
    wrmsr(MSR_CSTAR, (UINT64)(uintptr_t)KiSystemCall32);

    /* SFMASK = clear IF (bit 9) and DF (bit 10) on syscall entry */
    wrmsr(MSR_SFMASK, 0x300);

    /* Enable SCE in EFER */
    UINT64 efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);
}
