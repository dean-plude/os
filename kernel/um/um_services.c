/*
 * um_services.c — Windows services that used to be answered inside ntdll
 *
 * ntdll lays out a stub for every Windows 10 1903 service, at its number
 * and in number order (userland/ntdll/ntdll.c), as programs that make
 * system calls themselves expect.  The services here were ntdll C code
 * before; now that their names are stubs, the kernel answers them, with
 * the same results: system information, timer resolution, locally unique
 * ids, the token calls that only pretend, the object types NovaOS has none
 * of (transactions, job objects), byte-range locks, quotas, extended
 * attributes, memory locking, device I/O controls, NtSignalAndWait and
 * NtTestAlert.  NtRaiseHardError reports the error (a program's own error
 * box, STATUS_SERVICE_NOTIFICATION) on the serial log.
 *
 * 32-bit programs keep ntdll's C versions (ntdll_rtl.c).
 */
#include "um_internal.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../ke/kpcr.h"
#include "../hal/firmware.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../lib/string.h"
#include "../fs/persist.h"

#define ST_SUCCESS                0x00000000u
#define ST_USER_APC               0x000000C0u
#define ST_PROCESS_NOT_IN_JOB     0x00000124u
#define ST_INVALID_INFO_CLASS     0xC0000003u
#define ST_INFO_LENGTH_MISMATCH   0xC0000004u
#define ST_ACCESS_VIOLATION       0xC0000005u
#define ST_INVALID_DEVICE_REQUEST 0xC0000010u
#define ST_OBJECT_NAME_NOT_FOUND  0xC0000034u
#define ST_EAS_NOT_SUPPORTED      0xC000004Fu
#define ST_NOT_SUPPORTED          0xC00000BBu
#define ST_INVALID_PARAMETER      0xC000000Du
#define ST_PRIVILEGE_NOT_HELD     0xC0000061u
#define ST_SERVICE_NOTIFICATION   0x50000018u     /* (with HARDERROR_OVERRIDE_ERRORMODE) */

static bool put(UINT64 va, const void *v, UINT64 n)
{
    return !va || NT_SUCCESS(CopyToUser((void *)(uintptr_t)va, v, n));
}

/* IO_STATUS_BLOCK { Status (padded to 8), Information } */
static UINT64 io_done(UINT64 iosb, UINT32 st)
{
    UINT64 io[2] = { st, 0 };
    return put(iosb, io, sizeof(io)) ? st : ST_ACCESS_VIOLATION;
}

static UINT64 call(UINT32 num, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    return um_service(num)(a1, a2, a3, a4);
}

/* -----------------------------------------------------------------------
 * NtQuerySystemInformation(ULONG Class, PVOID Buffer, ULONG Length, PULONG ReturnLength)
 * ----------------------------------------------------------------------- */
static UINT32 cpus(void)
{
    return g_cpu_count ? g_cpu_count : 1;
}

static UINT64 sysinfo_basic(UINT64 buf, UINT32 len, UINT64 ret)
{
    UINT8 b[64];                                  /* SYSTEM_BASIC_INFORMATION (x64) */
    memset(b, 0, sizeof(b));
    UINT32 n = cpus(), pages = (UINT32)pmm_ram_pages();   /* the machine's RAM */
    UINT32 v32[] = { 0, 156250, 4096, pages, 1, pages, 65536 };
    memcpy(b, v32, sizeof(v32));                  /* Reserved .. AllocationGranularity */
    UINT64 lo = 0x10000, hi = UINT64_C(0x7FFFFFFEFFFF), mask = n >= 64 ? ~0ULL : (1ULL << n) - 1;
    memcpy(b + 0x20, &lo, 8);
    memcpy(b + 0x28, &hi, 8);
    memcpy(b + 0x30, &mask, 8);
    b[0x38] = (UINT8)n;                           /* NumberOfProcessors */
    UINT32 size = 0x40;
    if (!put(ret, &size, 4)) return ST_ACCESS_VIOLATION;
    if (len < size) return ST_INFO_LENGTH_MISMATCH;
    return put(buf, b, size) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

static UINT64 sysinfo_processes(UINT64 buf, UINT32 len, UINT64 ret)
{
    /* SYSTEM_PROCESS_INFORMATION (0x100 bytes on x64), then its name */
    static UmProcInfo list[UM_MAX_PROCS];        /* under the big lock */
    int n = UmList(list, UM_MAX_PROCS);
    UINT32 need = 0;
    for (int i = 0; i < n; i++) if (!list[i].exited) need += (0x100 + 2 * (UINT32)strlen(list[i].name) + 2 + 7) & ~7u;
    if (!need) need = 0x100;
    if (!put(ret, &need, 4)) return ST_ACCESS_VIOLATION;
    if (len < need) return ST_INFO_LENGTH_MISMATCH;
    UINT8 *k = kzalloc(need);
    if (!k) return 0xC0000017u;                   /* STATUS_NO_MEMORY */
    UINT32 at = 0, last = 0;
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (list[i].exited) continue;
        UINT8 *p = k + at;
        UINT32 nl = (UINT32)strlen(list[i].name), size = (0x100 + 2 * nl + 2 + 7) & ~7u;
        memcpy(p + 4, &list[i].threads, 4);                            /* NumberOfThreads */
        UINT16 l16[2] = { (UINT16)(2 * nl), (UINT16)(2 * nl + 2) };
        UINT64 name = buf + at + 0x100;
        memcpy(p + 0x38, l16, 4);                                      /* ImageName */
        memcpy(p + 0x40, &name, 8);
        for (UINT32 c = 0; c < nl; c++) p[0x100 + 2 * c] = (UINT8)list[i].name[c];
        UINT64 pid = list[i].pid, mem = (UINT64)list[i].mem_kb * 1024;
        memcpy(p + 0x50, &pid, 8);                                     /* UniqueProcessId */
        memcpy(p + 0x68, &mem, 8);                                     /* PeakVirtualSize */
        memcpy(p + 0x90, &mem, 8);                                     /* WorkingSetSize */
        if (!first) { UINT32 off = at - last; memcpy(k + last, &off, 4); }   /* NextEntryOffset */
        first = false;
        last = at;
        at += size;
    }
    UINT64 r = put(buf, k, need) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
    kfree(k);
    return r;
}

/* SystemModuleInformation (11) and SystemModuleInformationEx (77): the
 * kernel's modules, which on NovaOS is one, the kernel itself, listed as
 * Windows lists its own (\SystemRoot\system32\ntoskrnl.exe).  An
 * RTL_PROCESS_MODULE_INFORMATION is 0x128 bytes; class 11 puts a count
 * (padded to 8) before the records, class 77 chains 0x140-byte records
 * { NextOffset (padded to 8), the record, ImageChecksum, TimeDateStamp,
 * DefaultBase }, the last with NextOffset 0. */
extern char __text_start[], __kernel_end[];

static UINT64 sysinfo_modules(bool ex, UINT64 buf, UINT32 len, UINT64 ret)
{
    static const char path[] = "\\SystemRoot\\system32\\ntoskrnl.exe";
    UINT8 rec[0x140];
    memset(rec, 0, sizeof(rec));
    UINT8 *m = rec + 8;                           /* both: the record starts at 8 */
    UINT64 base = (UINT64)(uintptr_t)__text_start;
    UINT32 size = (UINT32)((uintptr_t)__kernel_end - (uintptr_t)__text_start), flags = 0x08804000u;
    UINT16 load = 1, off = 21;                    /* LoadCount; "ntoskrnl.exe" after "\SystemRoot\system32\" */
    memcpy(m + 0x10, &base, 8);                   /* ImageBase */
    memcpy(m + 0x18, &size, 4);                   /* ImageSize */
    memcpy(m + 0x1C, &flags, 4);                  /* Flags (as Windows reports its kernel) */
    memcpy(m + 0x24, &load, 2);
    memcpy(m + 0x26, &off, 2);                    /* OffsetToFileName */
    memcpy(m + 0x28, path, sizeof(path));         /* FullPathName */
    UINT32 need;
    if (ex) {
        memcpy(rec + 0x138, &base, 8);            /* DefaultBase; NextOffset 0: the last */
        need = 0x140;
    } else {
        UINT32 one = 1;
        memcpy(rec, &one, 4);                     /* NumberOfModules */
        need = 8 + 0x128;
    }
    if (!put(ret, &need, 4)) return ST_ACCESS_VIOLATION;
    if (len < need) return ST_INFO_LENGTH_MISMATCH;
    return put(buf, rec, need) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

static UINT64 sys_query_system_information(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT32 len = (UINT32)a3;
    switch ((UINT32)a1) {
    case 0:                                                         /* SystemBasicInformation */
        return sysinfo_basic(a2, len, a4);
    case 2: {                                                       /* SystemPerformanceInformation */
        UINT8 p[344];
        memset(p, 0, sizeof(p));
        UINT32 limit = (UINT32)pmm_ram_pages(), avail = (UINT32)pmm_free_now(), size = sizeof(p);
        if (avail > limit) avail = limit;
        UINT32 committed = limit - avail;                           /* (no page file) */
        memcpy(p + 0x3C, &avail, 4);
        memcpy(p + 0x40, &committed, 4);
        memcpy(p + 0x44, &limit, 4);
        if (!put(a4, &size, 4)) return ST_ACCESS_VIOLATION;
        if (len < size) return ST_INFO_LENGTH_MISMATCH;
        return put(a2, p, size) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
    }
    case 3: {                                                       /* SystemTimeOfDayInformation */
        UINT64 t[6] = { um_boot_time_100ns(), um_now_100ns(), 0, 0, 0, 0 };   /* Boot, Current, Bias, Id, Boot/SleepTimeBias */
        UINT32 n = len < sizeof(t) ? len : (UINT32)sizeof(t);
        if (!put(a2, t, n) || !put(a4, &n, 4)) return ST_ACCESS_VIOLATION;
        return ST_SUCCESS;
    }
    case 5:                                                         /* SystemProcessInformation */
        return sysinfo_processes(a2, len, a4);
    case 8: {                                                       /* SystemProcessorPerformanceInformation */
        UINT32 need = 48 * cpus();
        if (!put(a4, &need, 4)) return ST_ACCESS_VIOLATION;
        if (len < need) return ST_INFO_LENGTH_MISMATCH;
        UINT8 z[48];
        memset(z, 0, sizeof(z));
        for (UINT32 i = 0; i < need / 48; i++) if (!put(a2 + 48 * (UINT64)i, z, 48)) return ST_ACCESS_VIOLATION;
        return ST_SUCCESS;
    }
    case 11:                                                        /* SystemModuleInformation */
    case 77:                                                        /* SystemModuleInformationEx */
        return sysinfo_modules((UINT32)a1 == 77, a2, len, a4);
    case 76:                                                        /* SystemFirmwareTableInformation */
        return call(SYSCALL_NtNovaFirmwareTable, a2, a3, a4, 0);
    default: {
        UINT32 z = 0;
        return put(a4, &z, 4) ? ST_INVALID_INFO_CLASS : ST_ACCESS_VIOLATION;
    }
    }
}

/* NtQueryTimerResolution(PULONG Maximum, PULONG Minimum, PULONG Current) */
static UINT64 sys_query_timer_resolution(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UINT32 coarse = 156250, fine = 5000;
    return put(a1, &coarse, 4) && put(a2, &fine, 4) && put(a3, &coarse, 4) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* NtSetTimerResolution(ULONG Desired, BOOLEAN Set, PULONG Current) */
static UINT64 sys_set_timer_resolution(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a4;
    UINT32 cur = 156250;
    return put(a3, &cur, 4) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* NtAllocateLocallyUniqueId(PLUID Luid) */
static UINT64 sys_allocate_luid(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    static volatile UINT32 next = 0x10000;
    UINT32 luid[2] = { __atomic_add_fetch(&next, 1, __ATOMIC_SEQ_CST), 0 };
    return put(a1, luid, sizeof(luid)) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* -----------------------------------------------------------------------
 * Tokens: these few only pretend (the rest are um_security.c's)
 * ----------------------------------------------------------------------- */
/* NtSetInformationToken(HANDLE, TOKEN_INFORMATION_CLASS, PVOID, ULONG) */
static UINT64 sys_set_information_token(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return ST_SUCCESS;
}

/* NtAdjustPrivilegesToken(HANDLE, BOOLEAN DisableAll, PTOKEN_PRIVILEGES New,
 * ULONG Length, PTOKEN_PRIVILEGES Previous, PULONG ReturnLength): every
 * privilege asked for is granted (nothing is checked) */
static UINT64 sys_adjust_privileges_token(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3;
    UINT64 prev = um_stack_arg(5), ret = um_stack_arg(6);
    UINT32 need = 4, zero = 0;
    if (!put(ret, &need, 4)) return ST_ACCESS_VIOLATION;
    if (prev) {
        if ((UINT32)a4 < need) return 0xC0000023u;                  /* STATUS_BUFFER_TOO_SMALL */
        if (!put(prev, &zero, 4)) return ST_ACCESS_VIOLATION;
    }
    return ST_SUCCESS;
}

/* NtPrivilegeCheck(HANDLE, PPRIVILEGE_SET, PBOOLEAN Result): held, and
 * each is marked SE_PRIVILEGE_USED_FOR_ACCESS */
static UINT64 sys_privilege_check(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a4;
    UINT32 count = 0;
    if (a2 && !NT_SUCCESS(CopyFromUser(&count, (const void *)(uintptr_t)a2, 4))) return ST_ACCESS_VIOLATION;
    for (UINT32 i = 0; i < count && i < 64; i++) {                  /* { Count, Control, { LUID, Attributes }[] } */
        UINT64 at = a2 + 8 + 12 * (UINT64)i + 8;
        UINT32 attr;
        if (!NT_SUCCESS(CopyFromUser(&attr, (const void *)(uintptr_t)at, 4))) return ST_ACCESS_VIOLATION;
        attr |= 0x80000000u;
        if (!put(at, &attr, 4)) return ST_ACCESS_VIOLATION;
    }
    UINT8 yes = 1;
    return put(a3, &yes, 1) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

/* -----------------------------------------------------------------------
 * What NovaOS has none of: transactions (TxF) and job objects
 * ----------------------------------------------------------------------- */
static UINT64 no_handle(UINT64 h, UINT32 st)
{
    UINT64 z = 0;
    return put(h, &z, 8) ? st : ST_ACCESS_VIOLATION;
}

/* NtCreateTransaction(PHANDLE, ...), NtCreateJobObject(PHANDLE, ...) */
static UINT64 sys_create_none(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    return no_handle(a1, ST_NOT_SUPPORTED);
}

/* NtOpenJobObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES) */
static UINT64 sys_open_job(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    return no_handle(a1, ST_OBJECT_NAME_NOT_FOUND);
}

/* NtCommit/RollbackTransaction, NtAssignProcessToJobObject,
 * NtSetInformationJobObject */
static UINT64 sys_not_supported(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return ST_NOT_SUPPORTED;
}

/* NtSetSystemInformation(Class, PVOID, ULONG): SystemMemoryListInformation
 * (80) only, a SYSTEM_MEMORY_LIST_COMMAND, with SeProfileSingleProcess-
 * Privilege, as RAMMap and EmptyStandbyList use it: MemoryFlushModifiedList
 * (3) writes drive C:'s changed files to the data disk; MemoryPurgeStandbyList
 * (4) and MemoryPurgeLowPriorityStandbyList (5) let go of the contents of
 * every saved file nothing holds (fs/persist.c), as Windows drops the
 * standby list's cached file pages; MemoryEmptyWorkingSets (2) has nothing
 * to do (no page file: a working set is all of a process). */
static UINT64 sys_set_system_information(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    if ((UINT32)a1 != 80) return ST_INVALID_INFO_CLASS;
    if ((UINT32)a3 != 4) return ST_INFO_LENGTH_MISMATCH;
    UINT32 cmd;
    if (!a2 || !NT_SUCCESS(CopyFromUser(&cmd, (const void *)(uintptr_t)a2, 4))) return ST_ACCESS_VIOLATION;
    if (cmd < 2 || cmd > 5) return ST_INVALID_PARAMETER;
    if (!um_privilege_held(13)) return ST_PRIVILEGE_NOT_HELD;   /* SeProfileSingleProcessPrivilege */
    if (cmd == 3) PersistSync();
    if (cmd == 4 || cmd == 5) PersistLetGoAll();
    return ST_SUCCESS;
}

/* NtQueryInformationJobObject(HANDLE, Class, PVOID, ULONG, PULONG ReturnLength) */
static UINT64 sys_query_job(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    UINT32 z = 0;
    return put(um_stack_arg(5), &z, 4) ? ST_NOT_SUPPORTED : ST_ACCESS_VIOLATION;
}

/* NtIsProcessInJob(HANDLE Process, HANDLE Job) */
static UINT64 sys_is_process_in_job(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return ST_PROCESS_NOT_IN_JOB;
}

/* -----------------------------------------------------------------------
 * Files: every write reaches the disk anyway; byte-range locks always
 * succeed (no one else holds one); no quotas, no volume labels to set,
 * no extended attributes, and no driver that answers device I/O controls
 * (pipes and the file system use NtFsControlFile)
 * ----------------------------------------------------------------------- */
/* NtFlushBuffersFile(HANDLE, PIO_STATUS_BLOCK) */
static UINT64 sys_flush_buffers(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a3; (void)a4;
    return io_done(a2, ST_SUCCESS);
}

/* NtLockFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK,
 * PLARGE_INTEGER Offset, PLARGE_INTEGER Length, ULONG Key, BOOLEAN FailImmediately,
 * BOOLEAN Exclusive) */
static UINT64 sys_lock_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a3; (void)a4;
    UINT64 r = io_done(um_stack_arg(5), ST_SUCCESS);
    if (r == ST_SUCCESS && a2) call(SYSCALL_NtSetEvent, a2, 0, 0, 0);
    return r;
}

/* NtUnlockFile(HANDLE, PIO_STATUS_BLOCK, PLARGE_INTEGER, PLARGE_INTEGER, ULONG Key) */
static UINT64 sys_unlock_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a3; (void)a4;
    return io_done(a2, ST_SUCCESS);
}

/* NtQuery/SetQuotaInformationFile, NtSetVolumeInformationFile */
static UINT64 sys_invalid_device_request(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return ST_INVALID_DEVICE_REQUEST;
}

/* NtSetEaFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG), NtQueryEaFile(HANDLE,
 * PIO_STATUS_BLOCK, ...) */
static UINT64 sys_ea_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a3; (void)a4;
    return io_done(a2, ST_EAS_NOT_SUPPORTED);
}

/* NtDeviceIoControlFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID,
 * PIO_STATUS_BLOCK, ULONG Code, PVOID In, ULONG InLength, PVOID Out, ULONG OutLength) */
static UINT64 sys_device_io_control(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return io_done(um_stack_arg(5), ST_INVALID_DEVICE_REQUEST);
}

/* NtLock/UnlockVirtualMemory(HANDLE, PVOID *, PSIZE_T, ULONG): memory is
 * never paged out, so locking it in is a no-op */
static UINT64 sys_lock_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Waits and alerts
 * ----------------------------------------------------------------------- */
/* NtSignalAndWaitForSingleObject(HANDLE Signal, HANDLE Wait, BOOLEAN Alertable,
 * PLARGE_INTEGER Timeout): Signal is an event, a mutant or a semaphore */
static UINT64 sys_signal_and_wait(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    if (!NT_SUCCESS((UINT32)call(SYSCALL_NtSetEvent, a1, 0, 0, 0)) &&
        !NT_SUCCESS((UINT32)call(SYSCALL_NtReleaseMutant, a1, 0, 0, 0))) {
        UINT64 s = call(SYSCALL_NtReleaseSemaphore, a1, 1, 0, 0);
        if (!NT_SUCCESS((UINT32)s)) return s;
    }
    return call(SYSCALL_NtWaitForSingleObject, a2, a3, a4, 0);
}

/* NtTestAlert(): the kernel holds no user APCs (kernel32 keeps them and
 * runs them itself), so there is never one to deliver */
static UINT64 sys_test_alert(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * NtRaiseHardError(NTSTATUS Status, ULONG NumberOfParameters,
 * ULONG UnicodeStringParameterMask, PULONG_PTR Parameters,
 * ULONG ValidResponseOptions, PULONG Response)
 *
 * The error goes to the serial log with its strings: a program's own
 * error box (STATUS_SERVICE_NOTIFICATION: text, caption, MB_ type) or a
 * system error.  No box is shown yet; the answer is IDOK for an error box
 * and ResponseReturnToCaller (0) otherwise.
 * ----------------------------------------------------------------------- */
static void ustr(UINT64 us, char *out, int cap)
{
    UINT64 u[2];                                  /* { Length, MaximumLength, (pad), Buffer } */
    out[0] = 0;
    if (!us || !NT_SUCCESS(CopyFromUser(u, (const void *)(uintptr_t)us, sizeof(u)))) return;
    UINT32 n = (UINT32)(u[0] & 0xFFFF) / 2;
    int o = 0;
    for (UINT32 i = 0; i < n && o < cap - 1; i++) {
        UINT16 c;
        if (!NT_SUCCESS(CopyFromUser(&c, (const void *)(uintptr_t)(u[1] + 2 * (UINT64)i), 2))) break;
        out[o++] = c >= 0x20 && c < 0x7F ? (char)c : c == '\n' || c == '\r' ? ' ' : '?';
    }
    out[o] = 0;
}

static UINT64 sys_raise_hard_error(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT32 status = (UINT32)a1, count = (UINT32)a2 < 4 ? (UINT32)a2 : 4;
    UINT64 par[4] = { 0, 0, 0, 0 };
    if (count && a4 && !NT_SUCCESS(CopyFromUser(par, (const void *)(uintptr_t)a4, 8 * (UINT64)count))) return ST_ACCESS_VIOLATION;
    char s[2][160];
    for (int i = 0; i < 2; i++) {
        s[i][0] = 0;
        if ((UINT32)i < count && (a3 & (1u << i))) ustr(par[i], s[i], sizeof(s[i]));
    }
    UmProcess *p = UmCurrent();
    const char *name = p ? UmName(p) : "?";
    UINT32 response;
    if (status == ST_SERVICE_NOTIFICATION) {
        kprintf("[UM] %s: error box \"%s\": %s (type 0x%llx)\n", name, s[1], s[0], (unsigned long long)par[2]);
        response = 1;                                               /* IDOK */
    } else {
        kprintf("[UM] %s: hard error 0x%08x (%u parameters: 0x%llx 0x%llx 0x%llx 0x%llx) %s %s\n", name, status, (UINT32)a2,
                (unsigned long long)par[0], (unsigned long long)par[1], (unsigned long long)par[2], (unsigned long long)par[3], s[0], s[1]);
        response = 0;                                               /* ResponseReturnToCaller */
    }
    return put(um_stack_arg(6), &response, 4) ? ST_SUCCESS : ST_ACCESS_VIOLATION;
}

void um_services_init(void)
{
    static const struct { UINT32 num; SYSCALL_HANDLER h; bool lock_free; } svc[] = {
        { SYSCALL_NtQuerySystemInformation,      sys_query_system_information, false },
        { SYSCALL_NtSetSystemInformation,        sys_set_system_information, false },
        { SYSCALL_NtQueryTimerResolution,        sys_query_timer_resolution, true },
        { SYSCALL_NtSetTimerResolution,          sys_set_timer_resolution, true },
        { SYSCALL_NtAllocateLocallyUniqueId,     sys_allocate_luid, true },
        { SYSCALL_NtSetInformationToken,         sys_set_information_token, true },
        { SYSCALL_NtAdjustPrivilegesToken,       sys_adjust_privileges_token, true },
        { SYSCALL_NtPrivilegeCheck,              sys_privilege_check, true },
        { SYSCALL_NtCreateTransaction,           sys_create_none, true },
        { SYSCALL_NtCommitTransaction,           sys_not_supported, true },
        { SYSCALL_NtRollbackTransaction,         sys_not_supported, true },
        { SYSCALL_NtCreateJobObject,             sys_create_none, true },
        { SYSCALL_NtOpenJobObject,               sys_open_job, true },
        { SYSCALL_NtAssignProcessToJobObject,    sys_not_supported, true },
        { SYSCALL_NtQueryInformationJobObject,   sys_query_job, true },
        { SYSCALL_NtSetInformationJobObject,     sys_not_supported, true },
        { SYSCALL_NtIsProcessInJob,              sys_is_process_in_job, true },
        { SYSCALL_NtFlushBuffersFile,            sys_flush_buffers, true },
        { SYSCALL_NtLockFile,                    sys_lock_file, true },
        { SYSCALL_NtUnlockFile,                  sys_unlock_file, true },
        { SYSCALL_NtQueryQuotaInformationFile,   sys_invalid_device_request, true },
        { SYSCALL_NtSetQuotaInformationFile,     sys_invalid_device_request, true },
        { SYSCALL_NtSetVolumeInformationFile,    sys_invalid_device_request, true },
        { SYSCALL_NtSetEaFile,                   sys_ea_file, true },
        { SYSCALL_NtQueryEaFile,                 sys_ea_file, true },
        { SYSCALL_NtDeviceIoControlFile,         sys_device_io_control, true },
        { SYSCALL_NtLockVirtualMemory,           sys_lock_vm, true },
        { SYSCALL_NtUnlockVirtualMemory,         sys_lock_vm, true },
        { SYSCALL_NtSignalAndWaitForSingleObject, sys_signal_and_wait, true },
        { SYSCALL_NtTestAlert,                   sys_test_alert, true },
        { SYSCALL_NtRaiseHardError,              sys_raise_hard_error, false },
    };
    for (unsigned i = 0; i < sizeof(svc) / sizeof(svc[0]); i++) {
        um_install(svc[i].num, svc[i].h);
        if (svc[i].lock_free) um_lock_free(svc[i].num);
    }
}

