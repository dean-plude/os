/* winternl.h — NT native API subset (ntdll.dll) used by kernel32 */
#pragma once
#include <windows.h>
_NOVA_BEGIN
typedef LONG NTSTATUS;
#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS            ((NTSTATUS)0x00000000)
#define STATUS_PENDING            ((NTSTATUS)0x00000103)
#define STATUS_OBJECT_NAME_INVALID ((NTSTATUS)0xC0000033)
#define STATUS_BUFFER_OVERFLOW    ((NTSTATUS)0x80000005)
#define STATUS_NO_MORE_FILES      ((NTSTATUS)0x80000006)
#define STATUS_END_OF_FILE        ((NTSTATUS)0xC0000011)
#define STATUS_NO_MEMORY          ((NTSTATUS)0xC0000017)
#define STATUS_ACCESS_VIOLATION   ((NTSTATUS)0xC0000005)
#define STATUS_INVALID_PARAMETER  ((NTSTATUS)0xC000000D)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035)

typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct _OBJECT_ATTRIBUTES {
    ULONG Length; HANDLE RootDirectory; PUNICODE_STRING ObjectName;
    ULONG Attributes; PVOID SecurityDescriptor, SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
typedef struct _IO_STATUS_BLOCK { union { NTSTATUS Status; PVOID Pointer; }; ULONG_PTR Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

#define OBJ_CASE_INSENSITIVE 0x40
#define OBJ_INHERIT          0x02
#define FILE_SUPERSEDE        0
#define FILE_OPEN             1
#define FILE_CREATE           2
#define FILE_OPEN_IF          3
#define FILE_OVERWRITE        4
#define FILE_OVERWRITE_IF     5
#define FILE_DIRECTORY_FILE      0x00000001
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define FILE_NON_DIRECTORY_FILE  0x00000040
#define FILE_DELETE_ON_CLOSE     0x00001000
#define FILE_READ_DATA        0x0001
#define FILE_LIST_DIRECTORY   0x0001
#define FILE_WRITE_DATA       0x0002
#define FILE_APPEND_DATA      0x0004
#define SYNCHRONIZE           0x00100000
#define DELETE                0x00010000
#define FileBasicInformation        4
#define FileStandardInformation     5
#define FileDispositionInformation 13
#define FilePositionInformation    14
#define FileEndOfFileInformation   20
#define FileDirectoryInformation    1
#define FileFsDeviceInformation     4

typedef struct { LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime; ULONG FileAttributes; } FILE_BASIC_INFORMATION;
typedef struct { LARGE_INTEGER AllocationSize, EndOfFile; ULONG NumberOfLinks; BOOLEAN DeletePending, Directory; } FILE_STANDARD_INFORMATION;
typedef struct {
    ULONG NextEntryOffset, FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
    ULONG FileAttributes, FileNameLength;
    WCHAR FileName[1];
} FILE_DIRECTORY_INFORMATION;
typedef struct { ULONG DeviceType, Characteristics; } FILE_FS_DEVICE_INFORMATION;

/* PEB / process parameters (x64 layout, only the fields NovaOS fills) */
typedef struct _CURDIR { UNICODE_STRING DosPath; HANDLE Handle; } CURDIR;
typedef struct _RTL_USER_PROCESS_PARAMETERS {
    ULONG MaximumLength, Length, Flags, DebugFlags;
    HANDLE ConsoleHandle; ULONG ConsoleFlags;
    HANDLE StandardInput, StandardOutput, StandardError;
    CURDIR CurrentDirectory;
    UNICODE_STRING DllPath, ImagePathName, CommandLine;
    PVOID Environment;
    ULONG StartingX, StartingY, CountX, CountY, CountCharsX, CountCharsY, FillAttribute, WindowFlags, ShowWindowFlags;
    UNICODE_STRING WindowTitle, DesktopInfo, ShellInfo, RuntimeData;   /* RuntimeData: STARTUPINFO.lpReserved2 */
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;
/* Loader data (built by ntdll at process start) */
typedef struct _PEB_LDR_DATA {
    ULONG Length; BOOLEAN Initialized; PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList, InMemoryOrderModuleList, InInitializationOrderModuleList;
    PVOID EntryInProgress; BOOLEAN ShutdownInProgress; HANDLE ShutdownThreadId;
} PEB_LDR_DATA, *PPEB_LDR_DATA;
typedef struct _LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY InLoadOrderLinks, InMemoryOrderLinks, InInitializationOrderLinks;
    PVOID DllBase, EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName, BaseDllName;
    ULONG Flags;
    USHORT LoadCount, TlsIndex;
    LIST_ENTRY HashLinks;
    ULONG TimeDateStamp;
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;
#define LDRP_IMAGE_DLL               0x00000004
#define LDRP_ENTRY_PROCESSED         0x00004000
#define LDRP_DONT_CALL_FOR_THREADS   0x00040000
#define LDRP_PROCESS_ATTACH_CALLED   0x00080000

typedef struct _PEB {
    BYTE Reserved1[2]; BYTE BeingDebugged; BYTE Reserved2[1];   /* then pointer-aligned: 8 (x64) or 4 (x86) */
    PVOID Mutant, ImageBaseAddress;
    PPEB_LDR_DATA Ldr;
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters;
    PVOID SubSystemData, ProcessHeap;
    PVOID FastPebLock;              /* the RTL_CRITICAL_SECTION guarding the current directory */
} PEB, *PPEB;

/* The modules the kernel mapped, in initialization order (dependencies
 * first); read by ntdll's loader.  NovaOS-specific, at a fixed address. */
typedef struct _NOVA_LDR_MODULE {
    ULONGLONG Base, Size;
    ULONG EntryRva, Flags;          /* Flags: 1 = DLL */
    CHAR Name[64], Path[96];
} NOVA_LDR_MODULE;
typedef struct _NOVA_LDR_INFO {
    ULONG Count, Reserved;
    NOVA_LDR_MODULE Modules[64];
} NOVA_LDR_INFO;
#ifdef __x86_64__
#define NOVA_LDR_INFO_ADDRESS ((NOVA_LDR_INFO *)0x00007FFDF0001000ULL)
#else
#define NOVA_LDR_INFO_ADDRESS ((NOVA_LDR_INFO *)0x7FF01000UL)       /* 32-bit programs (see the kernel's UM32_PEB_VA) */
#endif

typedef struct _CLIENT_ID { HANDLE UniqueProcess, UniqueThread; } CLIENT_ID;
typedef struct _THREAD_BASIC_INFORMATION {
    NTSTATUS ExitStatus; PVOID TebBaseAddress; CLIENT_ID ClientId;
    ULONG_PTR AffinityMask; LONG Priority, BasePriority;
} THREAD_BASIC_INFORMATION;
typedef struct _PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus; PPEB PebBaseAddress; ULONG_PTR AffinityMask; LONG BasePriority;
    ULONG_PTR UniqueProcessId, InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION;

/* TEB fields NovaOS uses */
#ifdef __x86_64__
#define TEB_EXCEPTION_LIST      0x0000
#define TEB_STACK_BASE          0x0008
#define TEB_STACK_LIMIT         0x0010
#define TEB_SELF                0x0030
#define TEB_CLIENT_ID           0x0040
#define TEB_TLS_POINTER         0x0058      /* ThreadLocalStoragePointer (static TLS) */
#define TEB_PEB                 0x0060
#define TEB_LAST_ERROR          0x0068
#define TEB_DEALLOCATION_STACK  0x1478
#define TEB_TLS_SLOTS           0x1480      /* TlsSlots[64] */
#define TEB_TLS_EXPANSION       0x1780      /* TlsExpansionSlots (1024 more) */
#define TEB_FLS_DATA            0x17C8
#define TLS_MINIMUM_AVAILABLE   64
#define TLS_EXPANSION_SLOTS     1024
static __inline__ BYTE *NtCurrentTebBytes(void) { BYTE *t; __asm__("movq %%gs:0x30, %0" : "=r"(t)); return t; }
#else                                       /* x86: fs:0 is the TEB */
#define TEB_EXCEPTION_LIST      0x0000
#define TEB_STACK_BASE          0x0004
#define TEB_STACK_LIMIT         0x0008
#define TEB_SELF                0x0018
#define TEB_CLIENT_ID           0x0020
#define TEB_TLS_POINTER         0x002C
#define TEB_PEB                 0x0030
#define TEB_LAST_ERROR          0x0034
#define TEB_DEALLOCATION_STACK  0x0E0C
#define TEB_TLS_SLOTS           0x0E10
#define TEB_TLS_EXPANSION       0x0F94
#define TEB_FLS_DATA            0x0FB4
#define TLS_MINIMUM_AVAILABLE   64
#define TLS_EXPANSION_SLOTS     1024
static __inline__ BYTE *NtCurrentTebBytes(void) { BYTE *t; __asm__("movl %%fs:0x18, %0" : "=r"(t)); return t; }
#endif

#define NtCurrentProcess() ((HANDLE)(LONG_PTR)-1)
#define NtCurrentThread()  ((HANDLE)(LONG_PTR)-2)
#define STATUS_TIMEOUT_NT          ((NTSTATUS)0x00000102)
#define STATUS_DLL_NOT_FOUND       ((NTSTATUS)0xC0000135)
#define STATUS_ENTRYPOINT_NOT_FOUND ((NTSTATUS)0xC0000139)
#define STATUS_DLL_INIT_FAILED     ((NTSTATUS)0xC0000142)
#define STATUS_PROCEDURE_NOT_FOUND ((NTSTATUS)0xC000007A)
#define STATUS_INVALID_HANDLE      ((NTSTATUS)0xC0000008)
#define STATUS_UNHANDLED_EXCEPTION ((NTSTATUS)0xC0000144)
typedef enum _EVENT_TYPE { NotificationEvent, SynchronizationEvent } EVENT_TYPE;
typedef enum _WAIT_TYPE { WaitAll, WaitAny } WAIT_TYPE;
typedef ULONG (NTAPI *PUSER_THREAD_START_ROUTINE)(PVOID);

NTSYSAPI NTSTATUS NTAPI NtClose(HANDLE h);
NTSYSAPI NTSTATUS NTAPI NtCreateFile(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                                     PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disposition,
                                     ULONG options, PVOID ea, ULONG ealen);
NTSYSAPI NTSTATUS NTAPI NtOpenFile(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                                   ULONG share, ULONG options);
NTSYSAPI NTSTATUS NTAPI NtCreateNamedPipeFile(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                                              ULONG share, ULONG disposition, ULONG options, ULONG type, ULONG read_mode,
                                              ULONG completion, ULONG max_inst, ULONG in_quota, ULONG out_quota,
                                              PLARGE_INTEGER timeout);
NTSYSAPI NTSTATUS NTAPI NtFsControlFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io, ULONG code,
                                        PVOID in, ULONG in_len, PVOID out, ULONG out_len);
NTSYSAPI NTSTATUS NTAPI NtCancelIoFile(HANDLE h, PIO_STATUS_BLOCK io);
NTSYSAPI NTSTATUS NTAPI NtCancelIoFileEx(HANDLE h, PIO_STATUS_BLOCK req, PIO_STATUS_BLOCK io);
NTSYSAPI NTSTATUS NTAPI NtSetInformationObject(HANDLE h, ULONG cls, PVOID info, ULONG len);
NTSYSAPI NTSTATUS NTAPI NtQueryObject(HANDLE h, ULONG cls, PVOID info, ULONG len, PULONG ret);
/* The current directory (ntdll keeps it; ProcessParameters->CurrentDirectory shows it) */
NTSYSAPI NTSTATUS NTAPI RtlSetCurrentDirectory_U(PUNICODE_STRING dir);
/* User APCs: NtTestAlert runs the calling thread's (kernel32 queues them) */
NTSYSAPI NTSTATUS NTAPI NtTestAlert(void);
NTSYSAPI VOID     NTAPI RtlNovaSetApcRunner(BOOL (*fn)(void));
NTSYSAPI ULONG    NTAPI RtlGetCurrentDirectory_U(ULONG len, PWSTR buf);
/* NovaOS: the system clipboard (op 0 empty, 1 set, 2 get, 3 list, 4 sequence, 5 owner) */
typedef struct { ULONG Format; CHAR Name[60]; ULONG Size; } NOVA_CLIP_ENTRY;
NTSYSAPI NTSTATUS NTAPI NtOpenProcess(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, CLIENT_ID *cid);
NTSYSAPI NTSTATUS NTAPI NtOpenThread(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, CLIENT_ID *cid);
NTSYSAPI NTSTATUS NTAPI NtReadVirtualMemory(HANDLE p, PVOID base, PVOID buf, SIZE_T n, PSIZE_T done);
NTSYSAPI NTSTATUS NTAPI NtWriteVirtualMemory(HANDLE p, PVOID base, PVOID buf, SIZE_T n, PSIZE_T done);
NTSYSAPI LONG_PTR NTAPI NtNovaClipboard(ULONG op, ULONG_PTR a, PVOID b, ULONG_PTR c, const char *name);
NTSYSAPI NTSTATUS NTAPI NtReadFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io,
                                   PVOID buf, ULONG len, PLARGE_INTEGER off, PULONG key);
NTSYSAPI NTSTATUS NTAPI NtWriteFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io,
                                    const VOID *buf, ULONG len, PLARGE_INTEGER off, PULONG key);
NTSYSAPI NTSTATUS NTAPI NtQueryInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID info, ULONG len, ULONG cls);
NTSYSAPI NTSTATUS NTAPI NtSetInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID info, ULONG len, ULONG cls);
NTSYSAPI NTSTATUS NTAPI NtQueryAttributesFile(POBJECT_ATTRIBUTES oa, FILE_BASIC_INFORMATION *info);
NTSYSAPI NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io,
                                             PVOID info, ULONG len, ULONG cls, BOOLEAN single,
                                             PUNICODE_STRING name, BOOLEAN restart);
NTSYSAPI NTSTATUS NTAPI NtQueryVolumeInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID info, ULONG len, ULONG cls);
NTSYSAPI NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE p, PVOID *base, ULONG_PTR zero, PSIZE_T size, ULONG type, ULONG prot);
NTSYSAPI NTSTATUS NTAPI NtAllocateVirtualMemoryEx(HANDLE p, PVOID *base, PSIZE_T size, ULONG type, ULONG prot, PVOID params, ULONG n);
NTSYSAPI NTSTATUS NTAPI NtMapViewOfSectionEx(HANDLE sec, HANDLE p, PVOID *base, PLARGE_INTEGER off, PSIZE_T size, ULONG type,
                                             ULONG prot, PVOID params, ULONG n);
NTSYSAPI NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE p, PVOID *base, PSIZE_T size, ULONG type);
NTSYSAPI NTSTATUS NTAPI NtProtectVirtualMemory(HANDLE p, PVOID *base, PSIZE_T size, ULONG prot, PULONG old);
NTSYSAPI NTSTATUS NTAPI NtQueryVirtualMemory(HANDLE p, PVOID addr, int cls, PVOID buf, SIZE_T n, PSIZE_T ret);
NTSYSAPI NTSTATUS NTAPI NtGetContextThread(HANDLE t, PCONTEXT c);
NTSYSAPI NTSTATUS NTAPI NtSetContextThread(HANDLE t, const CONTEXT *c);
/* NovaOS: create a process sharing this one's console (UTF-8 full paths) */
/* RuntimeData: STARTUPINFO.lpReserved2 bytes for the new process (NULL: none)
 * Flags: 1 = inherit handles, 2 = no console; Environment: UTF-8
 * "NAME=value" strings each ended by NUL, then an empty one (NULL: default) */
typedef struct { HANDLE StdHandle[3]; HANDLE Process, Thread; ULONG64 ProcessId, ThreadId;
                 ULONG64 Flags; const char *Environment; ULONG64 EnvironmentSize;
                 const void *RuntimeData; ULONG64 RuntimeDataSize; } NOVA_CREATE_PROCESS;
NTSYSAPI NTSTATUS NTAPI NtNovaCreateProcess(const char *image, const char *cmdline, const char *dir, NOVA_CREATE_PROCESS *io);
/* NovaOS: Out = { process id, exit code (STILL_ACTIVE while running), exited } */
NTSYSAPI NTSTATUS NTAPI NtNovaProcessInfo(HANDLE p, ULONG64 out[3]);
/* NovaOS: the running programs */
typedef struct { ULONG Pid, MemoryKb, Threads, Exited; CHAR Name[32]; } NOVA_PROCESS_ENTRY;
NTSYSAPI NTSTATUS NTAPI NtNovaProcessList(NOVA_PROCESS_ENTRY *buf, ULONG max, PULONG count);
/* the registry */
NTSYSAPI NTSTATUS NTAPI NtCreateKey(PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, ULONG title, PUNICODE_STRING cls,
                                    ULONG options, PULONG disposition);
NTSYSAPI NTSTATUS NTAPI NtOpenKey(PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES oa);
NTSYSAPI NTSTATUS NTAPI NtOpenKeyEx(PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, ULONG options);
NTSYSAPI NTSTATUS NTAPI NtDeleteKey(HANDLE key);
NTSYSAPI NTSTATUS NTAPI NtSetValueKey(HANDLE key, PUNICODE_STRING name, ULONG title, ULONG type, PVOID data, ULONG size);
NTSYSAPI NTSTATUS NTAPI NtQueryValueKey(HANDLE key, PUNICODE_STRING name, int cls, PVOID info, ULONG len, PULONG ret);
NTSYSAPI NTSTATUS NTAPI NtEnumerateValueKey(HANDLE key, ULONG index, int cls, PVOID info, ULONG len, PULONG ret);
NTSYSAPI NTSTATUS NTAPI NtDeleteValueKey(HANDLE key, PUNICODE_STRING name);
NTSYSAPI NTSTATUS NTAPI NtEnumerateKey(HANDLE key, ULONG index, int cls, PVOID info, ULONG len, PULONG ret);
NTSYSAPI NTSTATUS NTAPI NtQueryKey(HANDLE key, int cls, PVOID info, ULONG len, PULONG ret);
NTSYSAPI NTSTATUS NTAPI NtFlushKey(HANDLE key);
/* SHUTDOWN_ACTION: 0 ShutdownNoReboot, 1 ShutdownReboot, 2 ShutdownPowerOff */
NTSYSAPI NTSTATUS NTAPI NtShutdownSystem(ULONG action);
/* POWER_ACTION: 2 Sleep, 3 Hibernate, 4 Shutdown, 5 ShutdownReset, 6 ShutdownOff;
 * sleep returns once the machine is awake again */
NTSYSAPI NTSTATUS NTAPI NtSetSystemPowerState(ULONG action, ULONG min_state, ULONG flags);
NTSYSAPI NTSTATUS NTAPI NtInitiatePowerAction(ULONG action, ULONG min_state, ULONG flags, BOOLEAN async);
/* POWER_INFORMATION_LEVEL 5 (SystemBatteryState): SYSTEM_BATTERY_STATE */
NTSYSAPI NTSTATUS NTAPI NtPowerInformation(ULONG level, PVOID in, ULONG inlen, PVOID out, ULONG outlen);
NTSYSAPI NTSTATUS NTAPI NtRenameKey(HANDLE key, PUNICODE_STRING name);
NTSYSAPI PVOID    NTAPI RtlPcToFileHeader(PVOID pc, PVOID *base);
NTSYSAPI NTSTATUS NTAPI NtTerminateProcess(HANDLE p, NTSTATUS status);
NTSYSAPI NTSTATUS NTAPI NtQuerySystemTime(PLARGE_INTEGER t);
NTSYSAPI NTSTATUS NTAPI NtQueryPerformanceCounter(PLARGE_INTEGER c, PLARGE_INTEGER f);
NTSYSAPI NTSTATUS NTAPI NtDelayExecution(BOOLEAN alertable, PLARGE_INTEGER interval);
NTSYSAPI NTSTATUS NTAPI NtYieldExecution(void);

/* Threads and synchronization */
NTSYSAPI NTSTATUS NTAPI NtCreateThreadEx(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, HANDLE process,
                                         PVOID start, PVOID arg, ULONG flags, SIZE_T zero_bits,
                                         SIZE_T stack_size, SIZE_T max_stack_size, PVOID attrs);
NTSYSAPI NTSTATUS NTAPI NtTerminateThread(HANDLE h, NTSTATUS status);
NTSYSAPI NTSTATUS NTAPI NtResumeThread(HANDLE h, PULONG prev);
NTSYSAPI NTSTATUS NTAPI NtSuspendThread(HANDLE h, PULONG prev);
NTSYSAPI NTSTATUS NTAPI NtQueryInformationThread(HANDLE h, ULONG cls, PVOID info, ULONG len, PULONG ret);
NTSYSAPI NTSTATUS NTAPI NtSetInformationThread(HANDLE h, ULONG cls, PVOID info, ULONG len);
NTSYSAPI NTSTATUS NTAPI NtQueryInformationProcess(HANDLE h, ULONG cls, PVOID info, ULONG len, PULONG ret);
NTSYSAPI NTSTATUS NTAPI NtCreateEvent(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, EVENT_TYPE type, BOOLEAN state);
NTSYSAPI NTSTATUS NTAPI NtSetEvent(HANDLE h, PLONG prev);
NTSYSAPI NTSTATUS NTAPI NtResetEvent(HANDLE h, PLONG prev);
NTSYSAPI NTSTATUS NTAPI NtClearEvent(HANDLE h);
NTSYSAPI NTSTATUS NTAPI NtCreateMutant(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, BOOLEAN owner);
NTSYSAPI NTSTATUS NTAPI NtReleaseMutant(HANDLE h, PLONG prev);
NTSYSAPI NTSTATUS NTAPI NtOpenEvent(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa);
NTSYSAPI NTSTATUS NTAPI NtCreateSection(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PLARGE_INTEGER size, ULONG protect, ULONG attrs, HANDLE file);
NTSYSAPI NTSTATUS NTAPI NtOpenSection(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa);
NTSYSAPI NTSTATUS NTAPI NtMapViewOfSection(HANDLE sec, HANDLE proc, PVOID *base, ULONG_PTR zero_bits, SIZE_T commit, PLARGE_INTEGER offset, PSIZE_T view, ULONG inherit, ULONG type, ULONG protect);
NTSYSAPI NTSTATUS NTAPI NtUnmapViewOfSection(HANDLE proc, PVOID base);
NTSYSAPI NTSTATUS NTAPI NtOpenMutant(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa);
NTSYSAPI NTSTATUS NTAPI NtOpenSemaphore(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa);
NTSYSAPI NTSTATUS NTAPI NtCreateSemaphore(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, LONG init, LONG max);
NTSYSAPI NTSTATUS NTAPI NtReleaseSemaphore(HANDLE h, LONG count, PLONG prev);
NTSYSAPI NTSTATUS NTAPI NtWaitForSingleObject(HANDLE h, BOOLEAN alertable, PLARGE_INTEGER timeout);
NTSYSAPI NTSTATUS NTAPI NtWaitForMultipleObjects(ULONG n, const HANDLE *h, WAIT_TYPE type, BOOLEAN alertable,
                                                 PLARGE_INTEGER timeout);
NTSYSAPI NTSTATUS NTAPI NtCompareObjects(HANDLE first, HANDLE second);
NTSYSAPI NTSTATUS NTAPI NtDuplicateObject(HANDLE sp, HANDLE src, HANDLE tp, PHANDLE dst, ULONG access,
                                          ULONG attrs, ULONG options);
/* Exceptions */
NTSYSAPI NTSTATUS NTAPI NtContinue(PCONTEXT ctx, BOOLEAN alert);
NTSYSAPI NTSTATUS NTAPI NtRaiseException(PEXCEPTION_RECORD rec, PCONTEXT ctx, BOOLEAN first_chance);
/* NovaOS */
NTSYSAPI NTSTATUS NTAPI NtNovaLoadDll(const char *name, ULONG len, PVOID *base, ULONG flags);
NTSYSAPI NTSTATUS NTAPI NtNovaDebugPrint(const char *s, ULONG len);
NTSYSAPI NTSTATUS NTAPI NtNovaWatchDirectory(HANDLE dir, BOOLEAN subtree, HANDLE event, ULONG remove);
NTSYSAPI NTSTATUS NTAPI NtNovaFlushView(PVOID base);
/* Console handles: modes and input records (op: 0 get mode, 1 set mode,
 * 2 read records, 3 peek, 4 write, 5 count, 6 flush, 7 size in cells) */
NTSYSAPI NTSTATUS NTAPI NtNovaConsole(HANDLE h, ULONG op, PVOID buf, ULONG len, PULONG res);
/* Fill buf with len (<= 4096) cryptographically random bytes from the kernel entropy pool */
NTSYSAPI NTSTATUS NTAPI NtNovaGetRandom(void *buf, ULONG len);
NTSYSAPI INT_PTR  NTAPI NtNovaSocket(ULONG type, ULONG family);
NTSYSAPI LONG_PTR NTAPI NtNovaSockConnect(INT_PTR h, const void *sockaddr, ULONG len);
NTSYSAPI LONG_PTR NTAPI NtNovaSockSend(INT_PTR h, const void *buf, ULONG len);
NTSYSAPI LONG_PTR NTAPI NtNovaSockRecv(INT_PTR h, void *buf, ULONG len);
NTSYSAPI LONG_PTR NTAPI NtNovaSockBind(INT_PTR h, const void *sockaddr, ULONG len);
NTSYSAPI LONG_PTR NTAPI NtNovaSockListen(INT_PTR h, ULONG backlog);
NTSYSAPI INT_PTR  NTAPI NtNovaSockAccept(INT_PTR h, void *addr);
NTSYSAPI LONG_PTR NTAPI NtNovaSockCtl(INT_PTR h, ULONG op, ULONG_PTR arg, void *out);
NTSYSAPI LONG_PTR NTAPI NtNovaSockSendTo(INT_PTR h, const void *buf, ULONG len, const void *addr);
NTSYSAPI LONG_PTR NTAPI NtNovaSockRecvFrom(INT_PTR h, void *buf, ULONG len, void *addr);
NTSYSAPI LONG_PTR NTAPI NtNovaResolve(const char *name, void *sockaddrs, ULONG max, ULONG family);
/* Sound: streams of 48 kHz s16 stereo frames (kernel/um/um_audio.c) */
NTSYSAPI INT_PTR  NTAPI NtNovaAudioOpen(ULONG frames);
NTSYSAPI LONG_PTR NTAPI NtNovaAudioWrite(INT_PTR h, const void *frames, ULONG n);
NTSYSAPI LONG_PTR NTAPI NtNovaAudioCtl(INT_PTR h, ULONG op, ULONG_PTR arg, void *out);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiCreate(void *info);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiGetMessage(ULONG_PTR hwnd, void *msg, ULONG wait);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiInvalidate(ULONG_PTR hwnd);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiSetText(ULONG_PTR hwnd, const void *title16);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiShow(ULONG_PTR hwnd, ULONG show);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiDestroy(ULONG_PTR hwnd);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiSetTimer(ULONG_PTR hwnd, ULONG_PTR id, ULONG ms);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiKillTimer(ULONG_PTR hwnd, ULONG_PTR id);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiMessageBox(const void *text16, const void *cap16, ULONG type);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiScreenSize(ULONG *w, ULONG *h);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiPostMessage(ULONG_PTR hwnd, ULONG msg, ULONG_PTR wp, ULONG_PTR lp);
NTSYSAPI LONG_PTR NTAPI NtNovaGuiCtl(ULONG_PTR hwnd, ULONG op, ULONG_PTR arg, PVOID data);

/* Loader */
NTSYSAPI NTSTATUS NTAPI LdrLoadDll(const WCHAR *path, PULONG flags, PUNICODE_STRING name, PVOID *base);
NTSYSAPI NTSTATUS NTAPI LdrNovaLoadDllA(const char *name, PVOID *base);
NTSYSAPI NTSTATUS NTAPI LdrNovaLoadDllExA(const char *name, ULONG flags, PVOID *base);
NTSYSAPI NTSTATUS NTAPI LdrGetDllHandle(const WCHAR *path, PULONG flags, PUNICODE_STRING name, PVOID *base);
NTSYSAPI PVOID    NTAPI LdrNovaGetModuleA(const char *name);
NTSYSAPI NTSTATUS NTAPI LdrGetProcedureAddress(PVOID base, const char *name, ULONG ordinal, PVOID *addr);
NTSYSAPI NTSTATUS NTAPI LdrDisableThreadCalloutsForDll(PVOID base);
NTSYSAPI PLDR_DATA_TABLE_ENTRY NTAPI LdrNovaFindEntry(PVOID address);
NTSYSAPI VOID     NTAPI LdrNovaZeroTlsCell(ULONG index);
NTSYSAPI __declspec(noreturn) VOID NTAPI RtlExitUserThread(NTSTATUS status);
NTSYSAPI NTSTATUS NTAPI RtlNovaCreateThread(PUSER_THREAD_START_ROUTINE start, PVOID arg, SIZE_T stack,
                                            BOOL suspended, PHANDLE h, PULONG tid);

/* Locks */
NTSYSAPI NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSYSAPI NTSTATUS NTAPI RtlInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION cs, ULONG spin);
NTSYSAPI NTSTATUS NTAPI RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSYSAPI NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSYSAPI NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSYSAPI BOOLEAN  NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSYSAPI VOID     NTAPI RtlInitializeSRWLock(PRTL_SRWLOCK l);
NTSYSAPI VOID     NTAPI RtlAcquireSRWLockExclusive(PRTL_SRWLOCK l);
NTSYSAPI VOID     NTAPI RtlAcquireSRWLockShared(PRTL_SRWLOCK l);
NTSYSAPI VOID     NTAPI RtlReleaseSRWLockExclusive(PRTL_SRWLOCK l);
NTSYSAPI VOID     NTAPI RtlReleaseSRWLockShared(PRTL_SRWLOCK l);
NTSYSAPI BOOLEAN  NTAPI RtlTryAcquireSRWLockExclusive(PRTL_SRWLOCK l);
NTSYSAPI BOOLEAN  NTAPI RtlTryAcquireSRWLockShared(PRTL_SRWLOCK l);
NTSYSAPI VOID     NTAPI RtlInitializeConditionVariable(PRTL_CONDITION_VARIABLE cv);
NTSYSAPI VOID     NTAPI RtlWakeConditionVariable(PRTL_CONDITION_VARIABLE cv);
NTSYSAPI VOID     NTAPI RtlWakeAllConditionVariable(PRTL_CONDITION_VARIABLE cv);
NTSYSAPI NTSTATUS NTAPI RtlSleepConditionVariableCS(PRTL_CONDITION_VARIABLE cv, PRTL_CRITICAL_SECTION cs,
                                                    PLARGE_INTEGER timeout);
NTSYSAPI NTSTATUS NTAPI RtlSleepConditionVariableSRW(PRTL_CONDITION_VARIABLE cv, PRTL_SRWLOCK l,
                                                     PLARGE_INTEGER timeout, ULONG flags);
NTSYSAPI VOID     NTAPI RtlRunOnceInitialize(PRTL_RUN_ONCE once);
NTSYSAPI NTSTATUS NTAPI RtlRunOnceBeginInitialize(PRTL_RUN_ONCE once, ULONG flags, PVOID *ctx);
NTSYSAPI NTSTATUS NTAPI RtlRunOnceComplete(PRTL_RUN_ONCE once, ULONG flags, PVOID ctx);

/* Exceptions */
NTSYSAPI VOID     NTAPI RtlCaptureContext(PCONTEXT ctx);
NTSYSAPI VOID     NTAPI RtlRestoreContext(PCONTEXT ctx, PEXCEPTION_RECORD rec);
NTSYSAPI VOID     NTAPI RtlRaiseException(PEXCEPTION_RECORD rec);
NTSYSAPI BOOLEAN  NTAPI RtlDispatchException(PEXCEPTION_RECORD rec, PCONTEXT ctx);
NTSYSAPI VOID     NTAPI RtlUnwind(PVOID frame, PVOID target_ip, PEXCEPTION_RECORD rec, PVOID retval);
#ifdef __x86_64__
NTSYSAPI VOID     NTAPI RtlUnwindEx(PVOID frame, PVOID target_ip, PEXCEPTION_RECORD rec, PVOID retval,
                                    PCONTEXT ctx, PUNWIND_HISTORY_TABLE history);
NTSYSAPI PRUNTIME_FUNCTION NTAPI RtlLookupFunctionEntry(DWORD64 pc, PDWORD64 base, PUNWIND_HISTORY_TABLE history);
NTSYSAPI BOOLEAN NTAPI RtlAddFunctionTable(PRUNTIME_FUNCTION tab, DWORD n, DWORD64 base);
NTSYSAPI BOOLEAN NTAPI RtlDeleteFunctionTable(PRUNTIME_FUNCTION tab);
NTSYSAPI BOOLEAN NTAPI RtlInstallFunctionTableCallback(DWORD64 id, DWORD64 base, DWORD len,
                                                       PRUNTIME_FUNCTION (NTAPI *cb)(DWORD64, PVOID), PVOID ctx, PCWSTR dll);
NTSYSAPI NTSTATUS NTAPI RtlAddGrowableFunctionTable(PVOID *h, PRUNTIME_FUNCTION tab, DWORD n, DWORD max, ULONG_PTR base, ULONG_PTR end);
NTSYSAPI void NTAPI RtlGrowFunctionTable(PVOID h, DWORD n);
NTSYSAPI void NTAPI RtlDeleteGrowableFunctionTable(PVOID h);
NTSYSAPI PEXCEPTION_ROUTINE NTAPI RtlVirtualUnwind(ULONG type, DWORD64 base, DWORD64 pc, PRUNTIME_FUNCTION f,
                                                   PCONTEXT ctx, PVOID *handler_data, PDWORD64 frame,
                                                   PKNONVOLATILE_CONTEXT_POINTERS ptrs);
#endif
NTSYSAPI PVOID    NTAPI RtlAddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
NTSYSAPI ULONG    NTAPI RtlRemoveVectoredExceptionHandler(PVOID h);
NTSYSAPI PVOID    NTAPI RtlAddVectoredContinueHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
NTSYSAPI ULONG    NTAPI RtlRemoveVectoredContinueHandler(PVOID h);
NTSYSAPI VOID     NTAPI RtlSetUnhandledExceptionFilter(PTOP_LEVEL_EXCEPTION_FILTER f);
NTSYSAPI USHORT   NTAPI RtlCaptureStackBackTrace(ULONG skip, ULONG n, PVOID *frames, PULONG hash);

NTSYSAPI PPEB     NTAPI RtlGetCurrentPeb(void);
NTSYSAPI ULONG    NTAPI RtlNtStatusToDosError(NTSTATUS s);
NTSYSAPI PVOID    NTAPI RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T n);
NTSYSAPI BOOLEAN  NTAPI RtlFreeHeap(PVOID heap, ULONG flags, PVOID p);
NTSYSAPI PVOID    NTAPI RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID p, SIZE_T n);
NTSYSAPI SIZE_T   NTAPI RtlSizeHeap(PVOID heap, ULONG flags, const VOID *p);
NTSYSAPI PVOID    NTAPI RtlGetProcessHeap(void);
NTSYSAPI VOID     NTAPI RtlInitUnicodeString(PUNICODE_STRING us, const WCHAR *s);
NTSYSAPI __declspec(noreturn) VOID NTAPI RtlExitUserProcess(NTSTATUS status);
_NOVA_END
