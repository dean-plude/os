/* winternl.h — NT native API subset (ntdll.dll) used by kernel32 */
#pragma once
#include <windows.h>
_NOVA_BEGIN
typedef LONG NTSTATUS;
#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS            ((NTSTATUS)0x00000000)
#define STATUS_PENDING            ((NTSTATUS)0x00000103)
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
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;
typedef struct _PEB {
    BYTE Reserved1[2]; BYTE BeingDebugged; BYTE Reserved2[5];
    PVOID Mutant, ImageBaseAddress, Ldr;
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters;
    PVOID SubSystemData, ProcessHeap;
} PEB, *PPEB;

#define NtCurrentProcess() ((HANDLE)(LONG_PTR)-1)

NTSYSAPI NTSTATUS NTAPI NtClose(HANDLE h);
NTSYSAPI NTSTATUS NTAPI NtCreateFile(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                                     PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disposition,
                                     ULONG options, PVOID ea, ULONG ealen);
NTSYSAPI NTSTATUS NTAPI NtOpenFile(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                                   ULONG share, ULONG options);
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
NTSYSAPI NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE p, PVOID *base, PSIZE_T size, ULONG type);
NTSYSAPI NTSTATUS NTAPI NtProtectVirtualMemory(HANDLE p, PVOID *base, PSIZE_T size, ULONG prot, PULONG old);
NTSYSAPI NTSTATUS NTAPI NtTerminateProcess(HANDLE p, NTSTATUS status);
NTSYSAPI NTSTATUS NTAPI NtQuerySystemTime(PLARGE_INTEGER t);
NTSYSAPI NTSTATUS NTAPI NtQueryPerformanceCounter(PLARGE_INTEGER c, PLARGE_INTEGER f);
NTSYSAPI NTSTATUS NTAPI NtDelayExecution(BOOLEAN alertable, PLARGE_INTEGER interval);
NTSYSAPI NTSTATUS NTAPI NtYieldExecution(void);

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
