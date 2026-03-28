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
#include "../mm/vmm.h"

/* -----------------------------------------------------------------------
 * MSR addresses for SYSCALL/SYSRET
 * ----------------------------------------------------------------------- */
#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_CSTAR       0xC0000083   /* compat mode — unused */
#define MSR_SFMASK      0xC0000084

#define EFER_SCE        (1UL << 0)   /* SYSCALL/SYSRET enable */

/* -----------------------------------------------------------------------
 * Syscall handler typedef
 * ----------------------------------------------------------------------- */
typedef NTSTATUS (*SYSCALL_HANDLER)(UINT64 a1, UINT64 a2,
                                     UINT64 a3, UINT64 a4);

/* -----------------------------------------------------------------------
 * Individual syscall handler implementations
 * ----------------------------------------------------------------------- */

/* --- NtClose (0x000F) --- */
static NTSTATUS sys_NtClose(UINT64 Handle, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    return ObCloseHandle((HANDLE)Handle, NULL);
}

/* --- NtYieldExecution (0x0046) --- */
static NTSTATUS sys_NtYieldExecution(UINT64 a1, UINT64 a2,
                                      UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    sched_yield();
    return STATUS_SUCCESS;
}

/* --- NtDelayExecution (0x0034) --- */
static NTSTATUS sys_NtDelayExecution(UINT64 Alertable,
                                      UINT64 DelayIntervalPtr,
                                      UINT64 a3, UINT64 a4)
{
    (void)Alertable; (void)DelayIntervalPtr; (void)a3; (void)a4;
    /* Phase 2 stub: yield once */
    sched_yield();
    return STATUS_SUCCESS;
}

/* --- NtQuerySystemTime (0x0052) --- */
static NTSTATUS sys_NtQuerySystemTime(UINT64 SystemTimePtr,
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
static NTSTATUS sys_NtQueryPerformanceCounter(UINT64 CounterPtr,
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
static NTSTATUS sys_NtQuerySystemInformation(UINT64 InfoClass,
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
static NTSTATUS sys_NtQueryInformationProcess(UINT64 ProcessHandle,
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
static NTSTATUS sys_NtQueryInformationThread(UINT64 ThreadHandle,
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
static NTSTATUS sys_NtOpenProcess(UINT64 ProcessHandlePtr,
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
static NTSTATUS sys_NtAllocateVirtualMemory(UINT64 ProcessHandle,
                                             UINT64 BaseAddressPtr,
                                             UINT64 RegionSizePtr,
                                             UINT64 AllocationType)
{
    (void)ProcessHandle; (void)AllocationType;
    /* Phase 2 stub: allocate pages and return their address */
    if (!RegionSizePtr) return STATUS_INVALID_PARAMETER;
    UINT64 size = *(UINT64 *)(uintptr_t)RegionSizePtr;
    if (!size) return STATUS_INVALID_PARAMETER;

    UINT64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *mem = kernel_alloc_pages((size_t)pages);
    if (!mem) return STATUS_NO_MEMORY;

    __builtin_memset(mem, 0, (size_t)(pages * PAGE_SIZE));

    if (BaseAddressPtr)
        *(void **)(uintptr_t)BaseAddressPtr = mem;
    *(UINT64 *)(uintptr_t)RegionSizePtr = pages * PAGE_SIZE;
    return STATUS_SUCCESS;
}

/* --- NtFreeVirtualMemory (0x001E) --- */
static NTSTATUS sys_NtFreeVirtualMemory(UINT64 ProcessHandle,
                                         UINT64 BaseAddressPtr,
                                         UINT64 RegionSizePtr,
                                         UINT64 FreeType)
{
    (void)ProcessHandle; (void)FreeType;
    if (!BaseAddressPtr) return STATUS_INVALID_PARAMETER;

    void *base = *(void **)(uintptr_t)BaseAddressPtr;
    UINT64 size = RegionSizePtr ? *(UINT64 *)(uintptr_t)RegionSizePtr : 0;
    if (!base) return STATUS_INVALID_PARAMETER;

    UINT64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!pages) pages = 1;
    kernel_free_pages(base, (size_t)pages);
    return STATUS_SUCCESS;
}

/* --- NtOpenKey (0x0012) --- */
static NTSTATUS sys_NtOpenKey(UINT64 KeyHandlePtr, UINT64 DesiredAccess,
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
static NTSTATUS sys_NtCreateKey(UINT64 KeyHandlePtr, UINT64 DesiredAccess,
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
static NTSTATUS sys_NtQueryValueKey(UINT64 KeyHandle, UINT64 ValueNamePtr,
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
static NTSTATUS sys_NtSetValueKey(UINT64 KeyHandle, UINT64 ValueNamePtr,
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
static NTSTATUS sys_NtTerminateProcess(UINT64 ProcessHandle,
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
static NTSTATUS sys_NotImplemented(UINT64 a1, UINT64 a2,
                                    UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return STATUS_NOT_IMPLEMENTED;
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
}

/* -----------------------------------------------------------------------
 * KiSystemCallDispatch — C entry point from ASM stub
 * ----------------------------------------------------------------------- */
NTSTATUS KiSystemCallDispatch(UINT64 num, UINT64 arg1, UINT64 arg2,
                               UINT64 arg3, UINT64 arg4)
{
    if (num >= SYSCALL_MAX) return STATUS_INVALID_SYSTEM_SERVICE;

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
