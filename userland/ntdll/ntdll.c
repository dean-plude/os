/*
 * ntdll.dll — NovaOS native API: system-call stubs, heap, runtime helpers
 *
 * Each Nt* export is the classic x64 stub: mov r10, rcx; mov eax, N;
 * syscall; ret — numbers generated from the kernel's ke/syscall.h.
 *
 * The heap (Rtl*Heap) serves one process-wide heap: small blocks come
 * from segregated free lists carved out of 1 MiB arenas committed from a
 * 1 GiB reservation; blocks larger than 256 KiB get their own
 * NtAllocateVirtualMemory region.  Programs are single-threaded, so the
 * heap takes no lock.
 */

#define NOVA_BUILD_NTDLL
#include <winternl.h>
#include "syscall_numbers.h"

/* -----------------------------------------------------------------------
 * System-call stubs
 * ----------------------------------------------------------------------- */
#ifdef _WIN64                 /* 32-bit programs: ntdll_wow.c */
#define STUB(name, num)                                                     \
    __asm__(".globl " #name "\n"                                            \
            ".section .text$" #name ",\"xr\"\n"                             \
            #name ":\n\t"                                                   \
            "movq %rcx, %r10\n\t"                                           \
            "movl $" #num ", %eax\n\t"                                      \
            "syscall\n\t"                                                   \
            "retq\n\t"                                                      \
            ".section .drectve,\"yn\"\n\t"                                  \
            ".ascii \" /EXPORT:" #name "\"\n\t"                             \
            ".text\n");

#define XSTUB(name, num) STUB(name, num)
XSTUB(NtClose,                      SYS_NtClose)
XSTUB(NtCreateFile,                 SYS_NtCreateFile)
XSTUB(NtOpenFile,                   SYS_NtOpenFile)
XSTUB(NtQuerySecurityObject,        SYS_NtQuerySecurityObject)
XSTUB(NtSetSecurityObject,          SYS_NtSetSecurityObject)
XSTUB(NtReadFile,                   SYS_NtReadFile)
XSTUB(NtWriteFile,                  SYS_NtWriteFile)
XSTUB(NtQueryInformationFile,       SYS_NtQueryInformationFile)
XSTUB(NtSetInformationFile,         SYS_NtSetInformationFile)
XSTUB(NtQueryAttributesFile,        SYS_NtQueryAttributesFile)
XSTUB(NtQueryDirectoryFile,         SYS_NtQueryDirectoryFile)
XSTUB(NtQueryVolumeInformationFile, SYS_NtQueryVolumeInformationFile)
XSTUB(NtAllocateVirtualMemory,      SYS_NtAllocateVirtualMemory)
XSTUB(NtFreeVirtualMemory,          SYS_NtFreeVirtualMemory)
XSTUB(NtProtectVirtualMemory,       SYS_NtProtectVirtualMemory)
XSTUB(NtQueryVirtualMemory,         SYS_NtQueryVirtualMemory)
XSTUB(NtGetContextThread,           SYS_NtGetContextThread)
XSTUB(NtSetContextThread,           SYS_NtSetContextThread)
XSTUB(NtNovaCreateProcess,          SYS_NtNovaCreateProcess)
XSTUB(NtNovaProcessInfo,            SYS_NtNovaProcessInfo)
XSTUB(NtNovaProcessList,            SYS_NtNovaProcessList)
XSTUB(NtCreateKey,                  SYS_NtCreateKey)
XSTUB(NtOpenKey,                    SYS_NtOpenKey)
XSTUB(NtOpenKeyEx,                  SYS_NtOpenKeyEx)
XSTUB(NtDeleteKey,                  SYS_NtDeleteKey)
XSTUB(NtSetValueKey,                SYS_NtSetValueKey)
XSTUB(NtQueryValueKey,              SYS_NtQueryValueKey)
XSTUB(NtEnumerateValueKey,          SYS_NtEnumerateValueKey)
XSTUB(NtDeleteValueKey,             SYS_NtDeleteValueKey)
XSTUB(NtEnumerateKey,               SYS_NtEnumerateKey)
XSTUB(NtQueryKey,                   SYS_NtQueryKey)
XSTUB(NtFlushKey,                   SYS_NtFlushKey)
XSTUB(NtShutdownSystem,             SYS_NtShutdownSystem)
XSTUB(NtSetSystemPowerState,        SYS_NtSetSystemPowerState)
XSTUB(NtInitiatePowerAction,        SYS_NtInitiatePowerAction)
XSTUB(NtPowerInformation,           SYS_NtPowerInformation)
XSTUB(NtRenameKey,                  SYS_NtRenameKey)
XSTUB(NtTerminateProcess,           SYS_NtTerminateProcess)
XSTUB(NtQuerySystemTime,            SYS_NtQuerySystemTime)
XSTUB(NtQueryPerformanceCounter,    SYS_NtQueryPerformanceCounter)
XSTUB(NtDelayExecution,             SYS_NtDelayExecution)
XSTUB(NtYieldExecution,             SYS_NtYieldExecution)
XSTUB(NtCreateThreadEx,             SYS_NtCreateThreadEx)
XSTUB(NtTerminateThread,            SYS_NtTerminateThread)
XSTUB(NtResumeThread,               SYS_NtResumeThread)
XSTUB(NtSuspendThread,              SYS_NtSuspendThread)
XSTUB(NtQueryInformationThread,     SYS_NtQueryInformationThread)
XSTUB(NtSetInformationThread,       SYS_NtSetInformationThread)
XSTUB(NtQueryInformationProcess,    SYS_NtQueryInformationProcess)
XSTUB(NtCreateEvent,                SYS_NtCreateEvent)
XSTUB(NtOpenEvent,                  SYS_NtOpenEvent)
XSTUB(NtCreateSection,              SYS_NtCreateSection)
XSTUB(NtOpenSection,                SYS_NtOpenSection)
XSTUB(NtMapViewOfSection,           SYS_NtMapViewOfSection)
XSTUB(NtUnmapViewOfSection,         SYS_NtUnmapViewOfSection)
XSTUB(NtOpenMutant,                 SYS_NtOpenMutant)
XSTUB(NtOpenSemaphore,              SYS_NtOpenSemaphore)
XSTUB(NtSetEvent,                   SYS_NtSetEvent)
XSTUB(NtResetEvent,                 SYS_NtResetEvent)
XSTUB(NtClearEvent,                 SYS_NtClearEvent)
XSTUB(NtCreateMutant,               SYS_NtCreateMutant)
XSTUB(NtReleaseMutant,              SYS_NtReleaseMutant)
XSTUB(NtCreateSemaphore,            SYS_NtCreateSemaphore)
XSTUB(NtReleaseSemaphore,           SYS_NtReleaseSemaphore)
XSTUB(NtWaitForSingleObject,        SYS_NtWaitForSingleObject)
XSTUB(NtWaitForMultipleObjects,     SYS_NtWaitForMultipleObjects)
XSTUB(NtDuplicateObject,            SYS_NtDuplicateObject)
XSTUB(NtCreateNamedPipeFile,        SYS_NtCreateNamedPipeFile)
XSTUB(NtFsControlFile,              SYS_NtFsControlFile)
XSTUB(NtCancelIoFile,               SYS_NtCancelIoFile)
XSTUB(NtCancelIoFileEx,             SYS_NtCancelIoFileEx)
XSTUB(NtSetInformationObject,       SYS_NtSetInformationObject)
XSTUB(NtQueryObject,                SYS_NtQueryObject)
XSTUB(NtNovaClipboard,              SYS_NtNovaClipboard)
XSTUB(NtCreateDirectoryObject,      SYS_NtCreateDirectoryObject)
XSTUB(NtOpenDirectoryObject,        SYS_NtOpenDirectoryObject)
XSTUB(NtQueryDirectoryObject,       SYS_NtQueryDirectoryObject)
XSTUB(NtCreateSymbolicLinkObject,   SYS_NtCreateSymbolicLinkObject)
XSTUB(NtOpenSymbolicLinkObject,     SYS_NtOpenSymbolicLinkObject)
XSTUB(NtQuerySymbolicLinkObject,    SYS_NtQuerySymbolicLinkObject)
XSTUB(NtCreateTimer,                SYS_NtCreateTimer)
XSTUB(NtOpenTimer,                  SYS_NtOpenTimer)
XSTUB(NtSetTimer,                   SYS_NtSetTimer)
XSTUB(NtCancelTimer,                SYS_NtCancelTimer)
XSTUB(NtQueryTimer,                 SYS_NtQueryTimer)
XSTUB(NtQueryEvent,                 SYS_NtQueryEvent)
XSTUB(NtQuerySemaphore,             SYS_NtQuerySemaphore)
XSTUB(NtOpenThread,                 SYS_NtOpenThread)
XSTUB(NtMapViewOfSectionEx,         SYS_NtMapViewOfSectionEx)
XSTUB(NtCompareObjects,             SYS_NtCompareObjects)
XSTUB(NtAllocateVirtualMemoryEx,    SYS_NtAllocateVirtualMemoryEx)
XSTUB(NtReadVirtualMemory,          SYS_NtReadVirtualMemory)
XSTUB(NtWriteVirtualMemory,         SYS_NtWriteVirtualMemory)
XSTUB(NtOpenProcess,                SYS_NtOpenProcess)
XSTUB(NtContinue,                   SYS_NtContinue)
XSTUB(NtRaiseException,             SYS_NtRaiseException)
XSTUB(NtNovaLoadDll,                SYS_NtNovaLoadDll)
XSTUB(NtNovaDebugPrint,             SYS_NtNovaDebugPrint)
XSTUB(NtNovaWatchDirectory,         SYS_NtNovaWatchDirectory)
XSTUB(NtNovaFlushView,              SYS_NtNovaFlushView)
XSTUB(NtNovaGetRandom,              SYS_NtNovaGetRandom)
XSTUB(NtNovaSocket,                    SYS_NtNovaSocket)
XSTUB(NtNovaSockConnect,               SYS_NtNovaSockConnect)
XSTUB(NtNovaSockSend,                  SYS_NtNovaSockSend)
XSTUB(NtNovaSockRecv,                  SYS_NtNovaSockRecv)
XSTUB(NtNovaSockBind,                  SYS_NtNovaSockBind)
XSTUB(NtNovaSockListen,                SYS_NtNovaSockListen)
XSTUB(NtNovaSockAccept,                SYS_NtNovaSockAccept)
XSTUB(NtNovaSockCtl,                   SYS_NtNovaSockCtl)
XSTUB(NtNovaSockSendTo,                SYS_NtNovaSockSendTo)
XSTUB(NtNovaSockRecvFrom,              SYS_NtNovaSockRecvFrom)
XSTUB(NtNovaResolve,                   SYS_NtNovaResolve)
XSTUB(NtNovaAudioOpen,                 SYS_NtNovaAudioOpen)
XSTUB(NtNovaAudioWrite,                SYS_NtNovaAudioWrite)
XSTUB(NtNovaAudioCtl,                  SYS_NtNovaAudioCtl)
XSTUB(NtNovaGuiCreate,                 SYS_NtNovaGuiCreate)
XSTUB(NtNovaGuiGetMessage,             SYS_NtNovaGuiGetMessage)
XSTUB(NtNovaGuiInvalidate,             SYS_NtNovaGuiInvalidate)
XSTUB(NtNovaGuiSetText,                SYS_NtNovaGuiSetText)
XSTUB(NtNovaGuiShow,                   SYS_NtNovaGuiShow)
XSTUB(NtNovaGuiDestroy,                SYS_NtNovaGuiDestroy)
XSTUB(NtNovaGuiSetTimer,               SYS_NtNovaGuiSetTimer)
XSTUB(NtNovaGuiKillTimer,              SYS_NtNovaGuiKillTimer)
XSTUB(NtNovaGuiMessageBox,             SYS_NtNovaGuiMessageBox)
XSTUB(NtNovaGuiScreenSize,             SYS_NtNovaGuiScreenSize)
XSTUB(NtNovaGuiPostMessage,            SYS_NtNovaGuiPostMessage)
XSTUB(NtNovaGuiCtl,                    SYS_NtNovaGuiCtl)
#endif

/* -----------------------------------------------------------------------
 * Memory/string primitives (real ntdll exports these too)
 * ----------------------------------------------------------------------- */
/* rep movsb/stosb: fast, and the compiler can't turn them back into calls */
__declspec(dllexport) void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

__declspec(dllexport) void *memmove(void *d, const void *s, size_t n)
{
    void *r = d;
    if ((char *)d <= (const char *)s || (char *)d >= (const char *)s + n) {
        __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        d = (char *)d + n - 1; s = (const char *)s + n - 1;
        __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return r;
}

__declspec(dllexport) void *memset(void *d, int c, size_t n)
{
    void *r = d;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

__declspec(dllexport) int memcmp(const void *x, const void *y, size_t n)
{
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}

__declspec(dllexport) size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

/* -----------------------------------------------------------------------
 * PEB, strings, errors, exit
 * ----------------------------------------------------------------------- */
NTSYSAPI PPEB NTAPI RtlGetCurrentPeb(void)
{
    PPEB peb;
#ifdef _WIN64
    __asm__("movq %%gs:0x60, %0" : "=r"(peb));
#else
    __asm__("movl %%fs:0x30, %0" : "=r"(peb));
#endif
    return peb;
}

/* Windows 10 22H2 (build 19045), the version NovaOS reports everywhere */
NTSYSAPI VOID NTAPI RtlGetNtVersionNumbers(ULONG *major, ULONG *minor, ULONG *build)
{
    if (major) *major = 10;
    if (minor) *minor = 0;
    if (build) *build = 0xF0000000u | 19045;       /* high nibble: a free (retail) build */
}

NTSYSAPI NTSTATUS NTAPI RtlGetVersion(PVOID info)
{
    ULONG *v = info;                               /* RTL_OSVERSIONINFOW(EX) */
    ULONG size = v[0];
    if (size < 276) return 0xC000000D;             /* STATUS_INVALID_PARAMETER */
    for (ULONG i = 1; i < size / 4; i++) v[i] = 0;
    v[1] = 10;                                     /* major */
    v[2] = 0;                                      /* minor */
    v[3] = 19045;                                  /* build */
    v[4] = 2;                                      /* VER_PLATFORM_WIN32_NT */
    if (size >= 284) ((UCHAR *)v)[282] = 1;        /* wProductType: VER_NT_WORKSTATION */
    return 0;
}

NTSYSAPI VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING us, const WCHAR *s)
{
    USHORT n = 0;
    if (s) while (s[n]) n++;
    us->Length = (USHORT)(n * 2);
    us->MaximumLength = (USHORT)(s ? n * 2 + 2 : 0);
    us->Buffer = (WCHAR *)s;
}

/* Extended attributes: the file system has none */
NTSYSAPI NTSTATUS NTAPI NtSetEaFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID buf, ULONG len)
{
    (void)h; (void)buf; (void)len;
    if (io) { io->Status = (NTSTATUS)0xC000004F; io->Information = 0; }
    return (NTSTATUS)0xC000004F;                      /* STATUS_EAS_NOT_SUPPORTED */
}
NTSYSAPI NTSTATUS NTAPI NtQueryEaFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID buf, ULONG len, BOOLEAN single, PVOID list,
                                      ULONG list_len, PULONG index, BOOLEAN restart)
{
    (void)h; (void)buf; (void)len; (void)single; (void)list; (void)list_len; (void)index; (void)restart;
    if (io) { io->Status = (NTSTATUS)0xC000004F; io->Information = 0; }
    return (NTSTATUS)0xC000004F;
}

NTSYSAPI ULONG NTAPI RtlNtStatusToDosError(NTSTATUS s)
{
    switch ((ULONG)s) {
    case 0x00000000: return ERROR_SUCCESS;
    case 0x00000103: return 997;                          /* ERROR_IO_PENDING */
    case 0x80000005: return ERROR_MORE_DATA;
    case 0xC0000010: return 1;                            /* ERROR_INVALID_FUNCTION */
    case 0xC0000023: return ERROR_INSUFFICIENT_BUFFER;
    case 0xC00000AB: case 0xC00000AC: case 0xC00000AE: return 231;   /* ERROR_PIPE_BUSY */
    case 0xC00000AD: return 230;                          /* ERROR_BAD_PIPE */
    case 0xC00000B0: return 233;                          /* ERROR_PIPE_NOT_CONNECTED */
    case 0xC00000B1: case 0xC00000D9: return 232;         /* ERROR_NO_DATA */
    case 0xC00000B2: return 535;                          /* ERROR_PIPE_CONNECTED */
    case 0xC00000B3: return 536;                          /* ERROR_PIPE_LISTENING */
    case 0xC00000B5: return 121;                          /* ERROR_SEM_TIMEOUT */
    case 0xC000014B: return 109;                          /* ERROR_BROKEN_PIPE */
    case 0xC0000120: return 995;                          /* ERROR_OPERATION_ABORTED */
    case 0xC0000225: return 1168;                         /* ERROR_NOT_FOUND */
    case 0xC000004F: return 282;                          /* ERROR_EAS_NOT_SUPPORTED */
    case 0xC0000135: return 126;                          /* ERROR_MOD_NOT_FOUND */
    case 0xC000007B: return 193;                          /* ERROR_BAD_EXE_FORMAT */
    case 0x80000006: return ERROR_NO_MORE_FILES;
    case 0xC0000002: return ERROR_CALL_NOT_IMPLEMENTED;
    case 0xC0000003: case 0xC000000D: return ERROR_INVALID_PARAMETER;
    case 0xC0000004: return ERROR_INSUFFICIENT_BUFFER;
    case 0xC0000008: return ERROR_INVALID_HANDLE;
    case 0xC0000011: return ERROR_HANDLE_EOF;
    case 0xC0000017: case 0xC000009A: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xC0000018: case 0xC00000A0: return ERROR_INVALID_ADDRESS;
    case 0xC0000022: return ERROR_ACCESS_DENIED;
    case 0xC0000033: return ERROR_INVALID_NAME;
    case 0xC0000034: return ERROR_FILE_NOT_FOUND;
    case 0xC0000035: return ERROR_ALREADY_EXISTS;
    case 0xC000003A: return ERROR_PATH_NOT_FOUND;
    case 0xC000007F: return ERROR_DISK_FULL;
    case 0xC00000A2: return 19;                           /* ERROR_WRITE_PROTECT: a read-only volume */
    case 0xC0000032: return 1392;                         /* ERROR_FILE_CORRUPT */
    case 0xC00000BA: return ERROR_ACCESS_DENIED;          /* file is a directory */
    case 0xC0000101: return ERROR_DIR_NOT_EMPTY;
    case 0xC0000103: return ERROR_INVALID_NAME;           /* not a directory */
    case 0xC000011F: return ERROR_TOO_MANY_OPEN_FILES;
    case 0xC0000121: return ERROR_ACCESS_DENIED;
    }
    return ERROR_INVALID_FUNCTION;
}

NTSYSAPI VOID NTAPI RtlExitUserProcess(NTSTATUS status)
{
    extern void nova_run_process_detach(void);
    nova_run_process_detach();
    NtTerminateProcess(NtCurrentProcess(), status);
    for (;;) NtYieldExecution();
}

NTSYSAPI VOID NTAPI RtlExitUserThread(NTSTATUS status)
{
    extern void nova_run_thread_detach(void);
    nova_run_thread_detach();
    NtTerminateThread(NtCurrentThread(), status);
    for (;;) NtYieldExecution();
}

__declspec(dllexport) int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* -----------------------------------------------------------------------
 * Heap
 * ----------------------------------------------------------------------- */
typedef struct Block {
    SIZE_T        size;        /* usable bytes (class size, or the large size) */
    SIZE_T        tag;         /* HEAP_MAGIC | class, or HEAP_LARGE */
#ifndef _WIN64
    SIZE_T        pad[2];      /* (32-bit: SSE code and JIT compilers such as
                                * Mesa's expect 16-byte-aligned blocks too) */
#endif
} Block;                       /* 16 bytes: user data stays 16-byte aligned */

#define HEAP_MAGIC   ((SIZE_T)0x4E4F564148454150ULL)   /* "NOVAHEAP" (its low half in 32-bit programs) */
#define HEAP_LARGE   ((SIZE_T)0x4E4F56414C415247ULL)   /* "NOVALARG" */
#define NCLASSES     48
#define LARGE_MIN    (256 * 1024)
#ifdef _WIN64
#define RESERVE_SIZE ((SIZE_T)1024 * 1024 * 1024)   /* 1 GiB of address space */
#else
#define RESERVE_SIZE ((SIZE_T)128 * 1024 * 1024)    /* 32-bit programs: 128 MiB at a time */
#endif
#define ARENA_SIZE   (1024 * 1024)

static SIZE_T  class_size[NCLASSES];
static void   *free_list[NCLASSES];
static char   *arena_base, *arena_cur, *arena_end, *arena_reserved_end;
static int     heap_ready;
static volatile long heap_lock;

static void hlock(void) { while (__atomic_exchange_n(&heap_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void hunlock(void) { __atomic_store_n(&heap_lock, 0, __ATOMIC_RELEASE); }

static void heap_init(void)
{
    /* 16..1024 in 16-byte steps (64 would be too many): 16, 32, 48 ... then x1.25 */
    SIZE_T s = 16;
    for (int i = 0; i < NCLASSES; i++) {
        class_size[i] = s;
        s = s < 256 ? s + 16 : (s + s / 4 + 15) & ~(SIZE_T)15;
        if (s > LARGE_MIN) s = LARGE_MIN;
    }
    PVOID base = 0;
    SIZE_T size = RESERVE_SIZE;
    if (NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_RESERVE, PAGE_READWRITE))) {
        arena_base = arena_cur = arena_end = base;
        arena_reserved_end = (char *)base + size;
    }
    heap_ready = 1;
    RtlGetCurrentPeb()->ProcessHeap = (PVOID)&heap_ready;
}

static int class_of(SIZE_T n)
{
    for (int i = 0; i < NCLASSES; i++) if (class_size[i] >= n) return i;
    return -1;
}

static void *carve(SIZE_T bytes)
{
    if (arena_cur + bytes > arena_end) {
        SIZE_T grow = bytes > ARENA_SIZE ? (bytes + ARENA_SIZE - 1) & ~(SIZE_T)(ARENA_SIZE - 1) : ARENA_SIZE;
        if (!arena_base || arena_end + grow > arena_reserved_end) {
            /* this reservation is used up: start another */
            PVOID base = 0;
            SIZE_T size = grow > RESERVE_SIZE ? grow : RESERVE_SIZE;
            if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_RESERVE, PAGE_READWRITE)))
                return 0;
            arena_base = arena_cur = arena_end = base;
            arena_reserved_end = (char *)base + size;
        }
        PVOID at = arena_end;
        SIZE_T sz = grow;
        if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &at, 0, &sz, MEM_COMMIT, PAGE_READWRITE)))
            return 0;
        arena_end += grow;
    }
    void *p = arena_cur;
    arena_cur += bytes;
    return p;
}

NTSYSAPI PVOID NTAPI RtlGetProcessHeap(void)
{
    if (!heap_ready) heap_init();
    return (PVOID)&heap_ready;
}

NTSYSAPI PVOID NTAPI RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T n)
{
    (void)heap;
    if (!heap_ready) heap_init();
    if (!n) n = 1;
    Block *b;
    if (n >= LARGE_MIN - sizeof(Block)) {
        PVOID base = 0;
        SIZE_T size = n + sizeof(Block);
        if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size,
                                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
            return 0;
        b = base;
        b->size = n;
        b->tag = HEAP_LARGE;
        return b + 1;                               /* fresh pages are zeroed */
    }
    int c = class_of(n);
    hlock();
    if (free_list[c]) {
        b = (Block *)free_list[c] - 1;
        free_list[c] = *(void **)free_list[c];
    } else {
        b = carve(class_size[c] + sizeof(Block));
    }
    hunlock();
    if (!b) return 0;
    b->size = class_size[c];
    b->tag = HEAP_MAGIC | (SIZE_T)c;
    if (flags & HEAP_ZERO_MEMORY) memset(b + 1, 0, class_size[c]);
    return b + 1;
}

NTSYSAPI BOOLEAN NTAPI RtlFreeHeap(PVOID heap, ULONG flags, PVOID p)
{
    (void)heap; (void)flags;
    if (!p) return TRUE;
    Block *b = (Block *)p - 1;
    if (b->tag == HEAP_LARGE) {
        PVOID base = b;
        SIZE_T size = 0;
        return NT_SUCCESS(NtFreeVirtualMemory(NtCurrentProcess(), &base, &size, MEM_RELEASE));
    }
    if ((b->tag & ~(SIZE_T)0xFF) != (HEAP_MAGIC & ~(SIZE_T)0xFF)) return FALSE;   /* not ours */
    int c = (int)(b->tag & 0xFF);
    if (c >= NCLASSES) return FALSE;
    hlock();
    *(void **)p = free_list[c];
    free_list[c] = p;
    b->tag = 0;                                     /* catches double frees */
    hunlock();
    return TRUE;
}

NTSYSAPI SIZE_T NTAPI RtlSizeHeap(PVOID heap, ULONG flags, const VOID *p)
{
    (void)heap; (void)flags;
    return p ? ((const Block *)p - 1)->size : (SIZE_T)-1;
}

NTSYSAPI PVOID NTAPI RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID p, SIZE_T n)
{
    if (!p) return RtlAllocateHeap(heap, flags, n);
    SIZE_T old = RtlSizeHeap(heap, 0, p);
    if (n <= old && ((Block *)p - 1)->tag != HEAP_LARGE) return p;
    PVOID q = RtlAllocateHeap(heap, flags, n);
    if (!q) return 0;
    memcpy(q, p, old < n ? old : n);
    RtlFreeHeap(heap, 0, p);
    return q;
}
