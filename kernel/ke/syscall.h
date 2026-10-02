/*
 * syscall.h — NT System Call Dispatcher
 *
 * NT provides two system call mechanisms:
 *
 *   1. INT 0x2E — the classic NT syscall gate (used by ntdll.dll on
 *      Windows NT 3.x/4.x and still supported today for compatibility).
 *      The IDT vector 0x2E is already wired up in idt.c.
 *
 *   2. SYSCALL/SYSRET — the fast path used by ntdll.dll on all modern
 *      x86_64 Windows versions (since Windows XP SP2 / x64 edition).
 *      Uses MSR_LSTAR (0xC0000082) for the kernel entry point and
 *      MSR_STAR (0xC0000081) for the CS/SS selectors.
 *
 * Syscall numbers:
 *   Windows 10 1903 (build 18362) NT syscall numbers are used.
 *   The table is defined in syscall_table.h (generated from Windows
 *   ntdll.dll pdb symbols / reactos / j00ru's syscall table).
 *   Phase 2 implements a small subset sufficient for Phase 4 work.
 *
 * Calling convention (Windows x64 ABI for syscalls):
 *   RAX = syscall number
 *   R10 = arg1 (RCX is clobbered by SYSCALL, so ntdll moves RCX to R10)
 *   RDX = arg2
 *   R8  = arg3
 *   R9  = arg4
 *   Stack[0..n] = arg5..argN (user mode stack)
 *
 * Kernel entry (KiSystemCall64):
 *   - SWAPGS  → kernel GS base (points to KPCR / current KTHREAD)
 *   - Save user RSP, load kernel RSP (from TSS.RSP0 or KPCR)
 *   - Save RCX (user RIP), R11 (user RFLAGS)
 *   - Build trap frame on stack
 *   - Dispatch to C handler KiSystemCallDispatch(syscall_num, args...)
 *   - SYSRET back to user mode
 *
 * INT 0x2E convention:
 *   - IDT handler saves full frame (InterruptFrame)
 *   - Calls interrupt_dispatch() which calls KiInt2EDispatch()
 *   - Same C dispatch table as SYSCALL path
 */

#pragma once

#include "../include/types.h"

/* -----------------------------------------------------------------------
 * NTSTATUS codes used by syscall handlers
 * (duplicates of types.h but stated here for documentation)
 * ----------------------------------------------------------------------- */
/* Already defined in types.h:
 *   STATUS_SUCCESS               0x00000000
 *   STATUS_UNSUCCESSFUL          0xC0000001
 *   STATUS_NOT_IMPLEMENTED       0xC0000002
 *   STATUS_INVALID_HANDLE        0xC0000008
 *   STATUS_INVALID_PARAMETER     0xC000000D
 *   STATUS_NO_MEMORY             0xC0000017
 *   STATUS_ACCESS_DENIED         0xC0000022
 *   STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034
 *   STATUS_NOT_FOUND             0xC0000225
 */

/* -----------------------------------------------------------------------
 * NT Syscall numbers — Windows 10 1903 (x64)
 * Source: j00ru's Windows NT syscall tables
 * https://j00ru.vexillium.org/syscalls/nt/64/
 *
 * Only the subset needed for Phase 2-4 is listed here.
 * ----------------------------------------------------------------------- */
#define SYSCALL_NtAccessCheck                     0x0000
#define SYSCALL_NtWorkerFactoryWorkerReady        0x0001
#define SYSCALL_NtAcceptConnectPort               0x0002
#define SYSCALL_NtMapUserPhysicalPagesScatter     0x0003
#define SYSCALL_NtWaitForSingleObject             0x0004
#define SYSCALL_NtCallbackReturn                  0x0005
#define SYSCALL_NtReadFile                        0x0006
#define SYSCALL_NtDeviceIoControlFile             0x0007
#define SYSCALL_NtWriteFile                       0x0008
#define SYSCALL_NtRemoveIoCompletion              0x0009
#define SYSCALL_NtReleaseSemaphore                0x000A
#define SYSCALL_NtReplyWaitReceivePort            0x000B
#define SYSCALL_NtReplyPort                       0x000C
#define SYSCALL_NtSetInformationThread            0x000D
#define SYSCALL_NtSetEvent                        0x000E
#define SYSCALL_NtClose                           0x000F
#define SYSCALL_NtQueryObject                     0x0010
#define SYSCALL_NtQueryInformationFile            0x0011
#define SYSCALL_NtOpenKey                         0x0012
#define SYSCALL_NtEnumerateValueKey               0x0013
#define SYSCALL_NtFindAtom                        0x0014
#define SYSCALL_NtQueryDefaultLocale              0x0015
#define SYSCALL_NtQueryKey                        0x0016
#define SYSCALL_NtQueryValueKey                   0x0017
#define SYSCALL_NtAllocateVirtualMemory           0x0018
#define SYSCALL_NtQueryInformationProcess         0x0019
#define SYSCALL_NtWaitForMultipleObjects32        0x001A
#define SYSCALL_NtWriteFileGather                 0x001B
#define SYSCALL_NtSetInformationProcess           0x001C
#define SYSCALL_NtCreateKey                       0x001D
#define SYSCALL_NtFreeVirtualMemory               0x001E
#define SYSCALL_NtImpersonateClientOfPort         0x001F
#define SYSCALL_NtReleaseMutant                   0x0020
#define SYSCALL_NtQueryInformationToken           0x0021
#define SYSCALL_NtRequestWaitReplyPort            0x0022
#define SYSCALL_NtQueryVirtualMemory              0x0023
#define SYSCALL_NtOpenThreadToken                 0x0024
#define SYSCALL_NtQueryInformationThread          0x0025
#define SYSCALL_NtOpenProcess                     0x0026
#define SYSCALL_NtSetInformationFile              0x0027
#define SYSCALL_NtSetValueKey                     0x0060
#define SYSCALL_NtMapViewOfSection                0x0028
#define SYSCALL_NtAccessCheckAndAuditAlarm        0x0029
#define SYSCALL_NtUnmapViewOfSection              0x002A
#define SYSCALL_NtReplyWaitReceivePortEx          0x002B
#define SYSCALL_NtTerminateProcess                0x002C
#define SYSCALL_NtSetEventBoostPriority           0x002D
#define SYSCALL_NtReadFileScatter                 0x002E
#define SYSCALL_NtOpenThreadTokenEx               0x002F
#define SYSCALL_NtOpenProcessTokenEx              0x0030
#define SYSCALL_NtQueryPerformanceCounter         0x0031
#define SYSCALL_NtEnumerateKey                    0x0032
#define SYSCALL_NtOpenFile                        0x0033
#define SYSCALL_NtDelayExecution                  0x0034
#define SYSCALL_NtQueryDirectoryFile              0x0035
#define SYSCALL_NtQuerySystemInformation          0x0036
#define SYSCALL_NtOpenSection                     0x0037
#define SYSCALL_NtQueryTimer                      0x0038
#define SYSCALL_NtFsControlFile                   0x0039
#define SYSCALL_NtWriteVirtualMemory              0x003A
#define SYSCALL_NtCloseObjectAuditAlarm           0x003B
#define SYSCALL_NtDuplicateObject                 0x003C
#define SYSCALL_NtQueryAttributesFile             0x003D
#define SYSCALL_NtClearEvent                      0x003E
#define SYSCALL_NtReadVirtualMemory               0x003F
#define SYSCALL_NtOpenEvent                       0x0040
#define SYSCALL_NtAdjustPrivilegesToken           0x0041
#define SYSCALL_NtDuplicateToken                  0x0042
#define SYSCALL_NtContinue                        0x0043
#define SYSCALL_NtQueryDefaultUILanguage          0x0044
#define SYSCALL_NtQueueApcThread                  0x0045
#define SYSCALL_NtYieldExecution                  0x0046
#define SYSCALL_NtAddAtom                         0x0047
#define SYSCALL_NtCreateEvent                     0x0048
#define SYSCALL_NtQueryVolumeInformationFile      0x0049
#define SYSCALL_NtCreateSection                   0x004A
#define SYSCALL_NtFlushBuffersFile                0x004B
#define SYSCALL_NtApphelpCacheControl             0x004C
#define SYSCALL_NtCreateProcessEx                 0x004D
#define SYSCALL_NtCreateThread                    0x004E
#define SYSCALL_NtIsProcessInJob                  0x004F
#define SYSCALL_NtProtectVirtualMemory            0x0050
#define SYSCALL_NtQuerySectionImageName           0x0051
#define SYSCALL_NtQuerySystemTime                 0x0052
#define SYSCALL_NtOpenSemaphore                   0x0053
#define SYSCALL_NtCreateMutant                    0x0054
#define SYSCALL_NtCreateFile                      0x0055
#define SYSCALL_NtWaitForMultipleObjects          0x005B
#define SYSCALL_NtCreateThreadEx                  0x00BD
#define SYSCALL_NtAllocateVirtualMemoryEx         0x00C4  /* Win10 1803+ */
#define SYSCALL_NtFlushInstructionCache           0x00CC  /* Win10 1903 */
#define SYSCALL_NtSetInformationThread            0x000D
#define SYSCALL_NtCreateProcessEx                 0x004D
#define SYSCALL_NtCreateThread                    0x004E
#define SYSCALL_NtQueryInformationFile            0x0011

/* Services for NovaOS user-mode programs whose Windows 10 1903 numbers
 * collide with entries above: numbered from 0x0180 (ntdll is built from
 * this header, so the stubs always match). */
#define SYSCALL_NtTerminateThread                 0x0180
#define SYSCALL_NtResumeThread                    0x0181
#define SYSCALL_NtSuspendThread                   0x0182
#define SYSCALL_NtCreateSemaphore                 0x0183
#define SYSCALL_NtResetEvent                      0x0184
#define SYSCALL_NtRaiseException                  0x0185
#define SYSCALL_NtNovaLoadDll                     0x0186  /* NovaOS: LdrLoadDll's kernel half */
#define SYSCALL_NtNovaDebugPrint                  0x0187  /* NovaOS: OutputDebugString */
#define SYSCALL_NtNovaGetRandom                   0x0188  /* NovaOS: RtlGenRandom (the kernel entropy pool) */
#define SYSCALL_NtNovaUnimplemented               0x0189  /* NovaOS: a program called an import NovaOS lacks */
#define SYSCALL_NtGetContextThread                0x018A
#define SYSCALL_NtSetContextThread                0x018B
#define SYSCALL_NtNovaCreateProcess               0x018C  /* NovaOS: CreateProcess's kernel half */
#define SYSCALL_NtNovaProcessInfo                 0x018D  /* NovaOS: exit code / pid of a process handle */
#define SYSCALL_NtNovaProcessList                 0x018E  /* NovaOS: the running programs (tasklist) */
#define SYSCALL_NtNovaWatchDirectory              0x018F  /* NovaOS: FindFirstChangeNotification's kernel half */
#define SYSCALL_NtNovaFlushView                   0x019B  /* NovaOS: FlushViewOfFile's kernel half */
#define SYSCALL_NtNovaConsole                     0x019C  /* NovaOS: console modes and input records */
/* NovaOS sockets (ws2_32's kernel half) */
#define SYSCALL_NtNovaSocket                      0x0190
#define SYSCALL_NtNovaSockConnect                 0x0191
#define SYSCALL_NtNovaSockSend                    0x0192
#define SYSCALL_NtNovaSockRecv                    0x0193
#define SYSCALL_NtNovaSockBind                    0x0194
#define SYSCALL_NtNovaSockListen                  0x0195
#define SYSCALL_NtNovaSockAccept                  0x0196
#define SYSCALL_NtNovaSockCtl                     0x0197
#define SYSCALL_NtNovaSockSendTo                  0x0198
#define SYSCALL_NtNovaSockRecvFrom                0x0199
#define SYSCALL_NtNovaResolve                     0x019A
/* NovaOS sound (winmm and mmdevapi's kernel half) */
#define SYSCALL_NtNovaAudioOpen                   0x01E0
#define SYSCALL_NtNovaAudioWrite                  0x01E1
#define SYSCALL_NtNovaAudioCtl                    0x01E2
/* Registry services Windows 10 numbers elsewhere */
#define SYSCALL_NtDeleteKey                       0x01B0
#define SYSCALL_NtDeleteValueKey                  0x01B1
#define SYSCALL_NtFlushKey                        0x01B2
#define SYSCALL_NtOpenKeyEx                       0x01B3
#define SYSCALL_NtRenameKey                       0x01B4
#define SYSCALL_NtOpenMutant                      0x01B8
/* Pipes and I/O Windows 10 numbers elsewhere */
#define SYSCALL_NtCreateNamedPipeFile             0x01C0
#define SYSCALL_NtCancelIoFile                    0x01C1
#define SYSCALL_NtCancelIoFileEx                  0x01C2
#define SYSCALL_NtSetInformationObject            0x01C3
#define SYSCALL_NtNovaClipboard                   0x01C4  /* NovaOS: the system clipboard */
#define SYSCALL_NtCreateDirectoryObject           0x01C5
#define SYSCALL_NtOpenDirectoryObject             0x01C6
#define SYSCALL_NtQueryDirectoryObject            0x01C7
#define SYSCALL_NtCreateSymbolicLinkObject        0x01C8
#define SYSCALL_NtOpenSymbolicLinkObject          0x01C9
#define SYSCALL_NtQuerySymbolicLinkObject         0x01CA
#define SYSCALL_NtCreateTimer                     0x01CB
#define SYSCALL_NtOpenTimer                       0x01CC
#define SYSCALL_NtSetTimer                        0x01CD
#define SYSCALL_NtCancelTimer                     0x01CE
#define SYSCALL_NtQueryEvent                      0x01CF
#define SYSCALL_NtQuerySemaphore                  0x01D0
#define SYSCALL_NtOpenThread                      0x01D1
#define SYSCALL_NtMapViewOfSectionEx              0x01D2
#define SYSCALL_NtCompareObjects                  0x01D3
#define SYSCALL_NtShutdownSystem                  0x01D4
#define SYSCALL_NtSetSystemPowerState             0x01D5
#define SYSCALL_NtInitiatePowerAction             0x01D6
#define SYSCALL_NtPowerInformation                0x01D7
/* NovaOS GUI (user32/gdi32's kernel half) */
#define SYSCALL_NtNovaGuiCreate                   0x01A0
#define SYSCALL_NtNovaGuiGetMessage               0x01A1
#define SYSCALL_NtNovaGuiInvalidate               0x01A2
#define SYSCALL_NtNovaGuiSetText                  0x01A3
#define SYSCALL_NtNovaGuiShow                     0x01A4
#define SYSCALL_NtNovaGuiDestroy                  0x01A5
#define SYSCALL_NtNovaGuiSetTimer                 0x01A6
#define SYSCALL_NtNovaGuiKillTimer                0x01A7
#define SYSCALL_NtNovaGuiMessageBox               0x01A8
#define SYSCALL_NtNovaGuiScreenSize               0x01A9
#define SYSCALL_NtNovaGuiPostMessage              0x01AA
#define SYSCALL_NtNovaGuiCtl                      0x01AB

/* -----------------------------------------------------------------------
 * Syscall table size
 * ----------------------------------------------------------------------- */
#define SYSCALL_MAX  0x0200   /* 512 entries — covers all Win10 1903 syscalls + KH helpers */

/* -----------------------------------------------------------------------
 * NtQuerySystemInformation system information classes
 * ----------------------------------------------------------------------- */
typedef enum _SYSTEM_INFORMATION_CLASS {
    SystemBasicInformation           = 0,
    SystemProcessorInformation       = 1,
    SystemPerformanceInformation     = 2,
    SystemTimeOfDayInformation       = 3,
    SystemPathInformation            = 4,
    SystemProcessInformation         = 5,
    SystemCallCountInformation       = 6,
    SystemDeviceInformation          = 7,
    SystemProcessorPerformanceInformation = 8,
    SystemFlagsInformation           = 9,
    SystemCallTimeInformation        = 10,
    SystemModuleInformation          = 11,
    SystemKernelDebuggerInformation  = 35,
    SystemCodeIntegrityInformation   = 103,
} SYSTEM_INFORMATION_CLASS;

typedef struct _SYSTEM_BASIC_INFORMATION {
    UINT32  Reserved;
    UINT32  TimerResolution;
    UINT32  PageSize;
    UINT32  NumberOfPhysicalPages;
    UINT32  LowestPhysicalPageNumber;
    UINT32  HighestPhysicalPageNumber;
    UINT32  AllocationGranularity;
    UINT64  MinimumUserModeAddress;
    UINT64  MaximumUserModeAddress;
    UINT64  ActiveProcessorsAffinityMask;
    UINT8   NumberOfProcessors;
} SYSTEM_BASIC_INFORMATION;

/* NtQueryInformationProcess — ProcessInformationClass values */
typedef enum _PROCESSINFOCLASS {
    ProcessBasicInformation         = 0,
    ProcessQuotaLimits              = 1,
    ProcessIoCounters               = 2,
    ProcessVmCounters               = 3,
    ProcessTimes                    = 4,
    ProcessBasePriority             = 5,
    ProcessRaisePriority            = 6,
    ProcessDebugPort                = 7,
    ProcessExceptionPort            = 8,
    ProcessAccessToken              = 9,
    ProcessImageFileName            = 27,
    ProcessBreakOnTermination       = 29,
    ProcessSubsystemInformation     = 75,
} PROCESSINFOCLASS;

typedef struct _PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    void    *PebBaseAddress;    /* PEB virtual address */
    UINT64   AffinityMask;
    INT32    BasePriority;
    UINT64   UniqueProcessId;
    UINT64   InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION;

/* NtQueryInformationThread */
typedef enum _THREADINFOCLASS {
    ThreadBasicInformation          = 0,
    ThreadTimes                     = 1,
    ThreadPriority                  = 2,
    ThreadBasePriority              = 3,
    ThreadAffinityMask              = 4,
    ThreadImpersonationToken        = 5,
    ThreadDescriptorTableEntry      = 6,
    ThreadEnableAlignmentFaultFixup = 7,
    ThreadEventPair                 = 8,
    ThreadQuerySetWin32StartAddress = 9,
    ThreadZeroTlsCell               = 10,
    ThreadPerformanceCount          = 11,
    ThreadAmILastThread             = 12,
    ThreadIdealProcessor            = 13,
    ThreadPriorityBoost             = 14,
    ThreadSetTlsArrayAddress        = 15,
    ThreadIsIoPending               = 16,
    ThreadHideFromDebugger          = 17,
} THREADINFOCLASS;

typedef struct _THREAD_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    void    *TebBaseAddress;
    struct { UINT64 UniqueProcess; UINT64 UniqueThread; } ClientId;
    UINT64   AffinityMask;
    INT32    Priority;
    INT32    BasePriority;
} THREAD_BASIC_INFORMATION;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/*
 * Initialize the syscall subsystem:
 *   - Programs MSR_LSTAR with KiSystemCall64 address
 *   - Programs MSR_STAR with kernel/user CS selectors
 *   - Programs MSR_SFMASK to clear IF on syscall entry
 *   - Enables SCE bit in EFER
 */
void SyscallInitialize(void);

/*
 * The main C-level syscall dispatcher.  Called by both the SYSCALL
 * fast-path handler and the INT 0x2E slow-path handler.
 *
 * @num:   syscall number (from RAX)
 * @arg1:  first argument (from R10 / RCX)
 * @arg2:  second argument (from RDX)
 * @arg3:  third argument (from R8)
 * @arg4:  fourth argument (from R9)
 *
 * Returns UINT64 (placed in RAX on SYSRET to user mode).
 * NT syscall handlers return NTSTATUS zero-extended to 64 bits.
 * Kernel-helper handlers (0x01F0-0x01FF) may return 64-bit pointers.
 */
void SyscallInitCpu(void);
UINT64 KiSystemCallDispatch(UINT64 num, UINT64 arg1, UINT64 arg2,
                             UINT64 arg3, UINT64 arg4, UINT64 user_rsp);

/* A system service: the first four arguments; the rest are on the user
 * stack (sched_current()->user_rsp + 0x28 + 8*(n-5)). */
typedef UINT64 (*SYSCALL_HANDLER)(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
/* Install a handler; returns the one it replaces. */
SYSCALL_HANDLER SyscallSetHandler(UINT32 num, SYSCALL_HANDLER h);

/*
 * Syscall entry point (assembly, installed in MSR_LSTAR).
 * Defined in syscall_entry.asm.
 */
extern void KiSystemCall64(void);
extern void KiSystemCall32(void);         /* compat-mode SYSCALL: ends the program */
void KiCompatSyscall(UINT64 rip);
/* A system call with the kernel lock handled as on the SYSCALL path */
UINT64 KiSystemCallEntry(UINT64 num, UINT64 arg1, UINT64 arg2, UINT64 arg3, UINT64 arg4, UINT64 user_rsp);
