/* winbase2.h — more of kernel32 (included by windows.h): overlapped I/O and
 * completion ports, file mapping, processes, file information, NLS,
 * console, time zones, waitable timers, interlocked lists */
#pragma once
typedef DWORD_PTR *PDWORD_PTR;
typedef ULONG64 *PULONG64;
typedef ULONGLONG *PULONGLONG;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
typedef struct _OVERLAPPED_ENTRY {
    ULONG_PTR lpCompletionKey;
    LPOVERLAPPED lpOverlapped;
    ULONG_PTR Internal;
    DWORD dwNumberOfBytesTransferred;
} OVERLAPPED_ENTRY, *LPOVERLAPPED_ENTRY;
typedef VOID (WINAPI *LPOVERLAPPED_COMPLETION_ROUTINE)(DWORD err, DWORD bytes, LPOVERLAPPED ov);

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

typedef enum _FILE_INFO_BY_HANDLE_CLASS {
    FileBasicInfo, FileStandardInfo, FileNameInfo, FileRenameInfo, FileDispositionInfo,
    FileAllocationInfo, FileEndOfFileInfo, FileStreamInfo, FileCompressionInfo, FileAttributeTagInfo,
    FileIdBothDirectoryInfo, FileIdBothDirectoryRestartInfo, FileIoPriorityHintInfo, FileRemoteProtocolInfo,
    FileFullDirectoryInfo, FileFullDirectoryRestartInfo, FileStorageInfo, FileAlignmentInfo, FileIdInfo,
    FileIdExtdDirectoryInfo, FileIdExtdDirectoryRestartInfo, FileDispositionInfoEx, FileRenameInfoEx,
} FILE_INFO_BY_HANDLE_CLASS;
typedef struct _FILE_BASIC_INFO { LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime; DWORD FileAttributes; } FILE_BASIC_INFO;
typedef struct _FILE_STANDARD_INFO { LARGE_INTEGER AllocationSize, EndOfFile; DWORD NumberOfLinks; BOOLEAN DeletePending, Directory; } FILE_STANDARD_INFO;
typedef struct _FILE_NAME_INFO { DWORD FileNameLength; WCHAR FileName[1]; } FILE_NAME_INFO;
typedef struct _FILE_ATTRIBUTE_TAG_INFO { DWORD FileAttributes, ReparseTag; } FILE_ATTRIBUTE_TAG_INFO;
typedef struct _FILE_DISPOSITION_INFO { BOOLEAN DeleteFile; } FILE_DISPOSITION_INFO;
typedef struct _FILE_END_OF_FILE_INFO { LARGE_INTEGER EndOfFile; } FILE_END_OF_FILE_INFO;
typedef struct _FILE_ID_128 { BYTE Identifier[16]; } FILE_ID_128;
typedef struct _FILE_ID_INFO { ULONGLONG VolumeSerialNumber; FILE_ID_128 FileId; } FILE_ID_INFO;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef enum { FindExInfoStandard, FindExInfoBasic } FINDEX_INFO_LEVELS;
typedef enum { FindExSearchNameMatch, FindExSearchLimitToDirectories } FINDEX_SEARCH_OPS;

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress, AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
#define MEM_FREE    0x10000
#define MEM_PRIVATE 0x20000
#define MEM_MAPPED  0x40000
#define MEM_IMAGE   0x1000000

typedef struct _STARTUPINFOW {
    DWORD cb; LPWSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2; LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW, *LPSTARTUPINFOW;
typedef struct _PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION, *LPPROCESS_INFORMATION;
typedef struct _PROC_THREAD_ATTRIBUTE_LIST *LPPROC_THREAD_ATTRIBUTE_LIST;
#define STARTF_USESTDHANDLES 0x00000100
#define STARTF_USESHOWWINDOW 0x00000001
#define CREATE_NO_WINDOW            0x08000000
#define CREATE_NEW_CONSOLE          0x00000010
#define CREATE_UNICODE_ENVIRONMENT  0x00000400
#define DETACHED_PROCESS            0x00000008

typedef struct _CPINFO { UINT MaxCharSize; BYTE DefaultChar[2]; BYTE LeadByte[12]; } CPINFO, *LPCPINFO;
typedef struct _COORD { SHORT X, Y; } COORD;
typedef struct _SMALL_RECT { SHORT Left, Top, Right, Bottom; } SMALL_RECT;
typedef struct _CONSOLE_SCREEN_BUFFER_INFO {
    COORD dwSize, dwCursorPosition;
    WORD wAttributes;
    SMALL_RECT srWindow;
    COORD dwMaximumWindowSize;
} CONSOLE_SCREEN_BUFFER_INFO, *PCONSOLE_SCREEN_BUFFER_INFO;
typedef BOOL (WINAPI *PHANDLER_ROUTINE)(DWORD type);
#define FOREGROUND_BLUE      0x0001
#define FOREGROUND_GREEN     0x0002
#define FOREGROUND_RED       0x0004
#define FOREGROUND_INTENSITY 0x0008
#define BACKGROUND_BLUE      0x0010
#define BACKGROUND_GREEN     0x0020
#define BACKGROUND_RED       0x0040
#define BACKGROUND_INTENSITY 0x0080
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004

typedef struct _TIME_ZONE_INFORMATION {
    LONG Bias; WCHAR StandardName[32]; SYSTEMTIME StandardDate; LONG StandardBias;
    WCHAR DaylightName[32]; SYSTEMTIME DaylightDate; LONG DaylightBias;
} TIME_ZONE_INFORMATION, *LPTIME_ZONE_INFORMATION;
typedef struct _DYNAMIC_TIME_ZONE_INFORMATION {
    LONG Bias; WCHAR StandardName[32]; SYSTEMTIME StandardDate; LONG StandardBias;
    WCHAR DaylightName[32]; SYSTEMTIME DaylightDate; LONG DaylightBias;
    WCHAR TimeZoneKeyName[128]; BOOLEAN DynamicDaylightTimeDisabled;
} DYNAMIC_TIME_ZONE_INFORMATION;
#define TIME_ZONE_ID_UNKNOWN 0

typedef struct _SLIST_ENTRY { struct _SLIST_ENTRY *Next; } SLIST_ENTRY, *PSLIST_ENTRY;
#ifdef _WIN64
typedef union __attribute__((aligned(16))) _SLIST_HEADER { struct { ULONGLONG Alignment, Region; }; } SLIST_HEADER, *PSLIST_HEADER;
#else   /* 8 bytes on x86: the first entry, then depth and sequence */
typedef union __attribute__((aligned(8))) _SLIST_HEADER {
    ULONGLONG Alignment;
    struct { SLIST_ENTRY Next; USHORT Depth; USHORT Sequence; };
} SLIST_HEADER, *PSLIST_HEADER;
#endif


/* completion ports, overlapped I/O */
WINBASEAPI HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE port, ULONG_PTR key, DWORD threads);
WINBASEAPI BOOL   WINAPI GetQueuedCompletionStatus(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED *ov, DWORD ms);
WINBASEAPI BOOL   WINAPI GetQueuedCompletionStatusEx(HANDLE port, LPOVERLAPPED_ENTRY e, ULONG n, PULONG got, DWORD ms, BOOL alertable);
WINBASEAPI BOOL   WINAPI PostQueuedCompletionStatus(HANDLE port, DWORD bytes, ULONG_PTR key, LPOVERLAPPED ov);
WINBASEAPI BOOL   WINAPI GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD bytes, BOOL wait);
WINBASEAPI BOOL   WINAPI CancelIo(HANDLE h);
WINBASEAPI BOOL   WINAPI CancelIoEx(HANDLE h, LPOVERLAPPED ov);
WINBASEAPI BOOL   WINAPI ReadFileEx(HANDLE h, LPVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE fn);
WINBASEAPI BOOL   WINAPI WriteFileEx(HANDLE h, LPCVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE fn);
WINBASEAPI DWORD  WINAPI SleepEx(DWORD ms, BOOL alertable);
WINBASEAPI BOOL   WINAPI SetFileCompletionNotificationModes(HANDLE h, UCHAR flags);
/* file mapping */
WINBASEAPI HANDLE WINAPI CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name);
WINBASEAPI HANDLE WINAPI CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCSTR name);
WINBASEAPI LPVOID WINAPI MapViewOfFile(HANDLE map, DWORD access, DWORD hi, DWORD lo, SIZE_T n);
WINBASEAPI HANDLE WINAPI OpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name);
WINBASEAPI HANDLE WINAPI OpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name);
WINBASEAPI LPVOID WINAPI MapViewOfFileEx(HANDLE map, DWORD access, DWORD hi, DWORD lo, SIZE_T n, LPVOID base);
WINBASEAPI BOOL   WINAPI UnmapViewOfFile(LPCVOID p);
WINBASEAPI BOOL   WINAPI FlushViewOfFile(LPCVOID p, SIZE_T n);
#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ  0x0004
#define FILE_MAP_COPY  0x0001
#define FILE_MAP_ALL_ACCESS 0xF001F
/* processes */
WINBASEAPI BOOL   WINAPI CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi);
WINBASEAPI BOOL   WINAPI CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi);
WINBASEAPI HANDLE WINAPI OpenProcess(DWORD access, BOOL inherit, DWORD pid);
WINBASEAPI DWORD  WINAPI GetProcessId(HANDLE h);
WINBASEAPI BOOL   WINAPI InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST l, DWORD n, DWORD flags, PSIZE_T size);
WINBASEAPI BOOL   WINAPI UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST l, DWORD flags, DWORD_PTR attr, PVOID v, SIZE_T n, PVOID prev, PSIZE_T ret);
WINBASEAPI VOID   WINAPI DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST l);
WINBASEAPI VOID   WINAPI GetStartupInfoW(LPSTARTUPINFOW si);
WINBASEAPI BOOL   WINAPI GetProcessAffinityMask(HANDLE p, PDWORD_PTR proc, PDWORD_PTR sys);
WINBASEAPI BOOL   WINAPI SetProcessAffinityMask(HANDLE p, DWORD_PTR mask);
WINBASEAPI BOOL   WINAPI SetProcessPriorityBoost(HANDLE p, BOOL disable);
WINBASEAPI int    WINAPI GetThreadPriority(HANDLE t);
WINBASEAPI BOOL   WINAPI SetThreadPriority(HANDLE t, int prio);
WINBASEAPI BOOL   WINAPI Beep(DWORD freq, DWORD ms);
WINBASEAPI BOOL   WINAPI SetThreadStackGuarantee(PULONG size);
WINBASEAPI UINT   WINAPI SetErrorMode(UINT mode);
WINBASEAPI UINT   WINAPI GetErrorMode(void);
WINBASEAPI BOOL   WINAPI IsProcessorFeaturePresent(DWORD f);
WINBASEAPI PVOID  WINAPI EncodePointer(PVOID p);
WINBASEAPI PVOID  WINAPI DecodePointer(PVOID p);
/* files and paths */
WINBASEAPI BOOL   WINAPI GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info);
WINBASEAPI BOOL   WINAPI GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID buf, DWORD n);
WINBASEAPI BOOL   WINAPI SetFileInformationByHandle(HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID buf, DWORD n);
WINBASEAPI DWORD  WINAPI GetFinalPathNameByHandleW(HANDLE h, LPWSTR buf, DWORD n, DWORD flags);
WINBASEAPI DWORD  WINAPI GetFinalPathNameByHandleA(HANDLE h, LPSTR buf, DWORD n, DWORD flags);
WINBASEAPI DWORD  WINAPI GetFullPathNameW(LPCWSTR name, DWORD n, LPWSTR buf, LPWSTR *part);
WINBASEAPI BOOL   WINAPI GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS l, LPVOID info);
WINBASEAPI BOOL   WINAPI GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS l, LPVOID info);
WINBASEAPI BOOL   WINAPI SetFileAttributesW(LPCWSTR name, DWORD attr);
WINBASEAPI BOOL   WINAPI SetFileAttributesA(LPCSTR name, DWORD attr);
WINBASEAPI HANDLE WINAPI FindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS l, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags);
WINBASEAPI BOOL   WINAPI MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
WINBASEAPI BOOL   WINAPI MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
WINBASEAPI BOOL   WINAPI MoveFileW(LPCWSTR from, LPCWSTR to);
WINBASEAPI BOOL   WINAPI MoveFileA(LPCSTR from, LPCSTR to);
WINBASEAPI BOOL   WINAPI CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists);
WINBASEAPI BOOL   WINAPI CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
WINBASEAPI BOOL   WINAPI RemoveDirectoryW(LPCWSTR name);
WINBASEAPI BOOL   WINAPI SetCurrentDirectoryW(LPCWSTR path);
WINBASEAPI UINT   WINAPI GetSystemDirectoryW(LPWSTR buf, UINT n);
WINBASEAPI UINT   WINAPI GetSystemDirectoryA(LPSTR buf, UINT n);
WINBASEAPI UINT   WINAPI GetWindowsDirectoryW(LPWSTR buf, UINT n);
WINBASEAPI UINT   WINAPI GetWindowsDirectoryA(LPSTR buf, UINT n);
WINBASEAPI DWORD  WINAPI GetTempPathW(DWORD n, LPWSTR buf);
WINBASEAPI DWORD  WINAPI GetTempPathA(DWORD n, LPSTR buf);
WINBASEAPI UINT   WINAPI GetTempFileNameW(LPCWSTR dir, LPCWSTR prefix, UINT unique, LPWSTR out);
WINBASEAPI UINT   WINAPI GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR out);
WINBASEAPI DWORD  WINAPI GetLongPathNameW(LPCWSTR s, LPWSTR l, DWORD n);
WINBASEAPI DWORD  WINAPI GetShortPathNameW(LPCWSTR s, LPWSTR l, DWORD n);
WINBASEAPI BOOL   WINAPI GetHandleInformation(HANDLE h, LPDWORD flags);
WINBASEAPI BOOL   WINAPI SetHandleInformation(HANDLE h, DWORD mask, DWORD flags);
WINBASEAPI BOOL   WINAPI GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free);
WINBASEAPI BOOL   WINAPI GetVolumeInformationW(LPCWSTR root, LPWSTR name, DWORD nn, LPDWORD serial, LPDWORD maxlen, LPDWORD flags, LPWSTR fs, DWORD nfs);
WINBASEAPI UINT   WINAPI GetDriveTypeW(LPCWSTR root);
WINBASEAPI DWORD  WINAPI GetLogicalDrives(void);
WINBASEAPI DWORD  WINAPI GetLogicalDriveStringsW(DWORD n, LPWSTR buf);
WINBASEAPI DWORD  WINAPI GetLogicalDriveStringsA(DWORD n, LPSTR buf);
WINBASEAPI BOOL   WINAPI AreFileApisANSI(void);
/* modules */
WINBASEAPI HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE f, DWORD flags);
WINBASEAPI BOOL    WINAPI GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *out);
WINBASEAPI BOOL    WINAPI SetDllDirectoryW(LPCWSTR dir);
/* resources */
typedef HANDLE HRSRC;
WINBASEAPI HRSRC  WINAPI FindResourceW(HMODULE m, LPCWSTR name, LPCWSTR type);
WINBASEAPI HRSRC  WINAPI FindResourceA(HMODULE m, LPCSTR name, LPCSTR type);
WINBASEAPI HRSRC  WINAPI FindResourceExW(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang);
WINBASEAPI HGLOBAL WINAPI LoadResource(HMODULE m, HRSRC r);
WINBASEAPI LPVOID WINAPI LockResource(HGLOBAL h);
WINBASEAPI DWORD  WINAPI SizeofResource(HMODULE m, HRSRC r);
#define MAKEINTRESOURCEW(i) ((LPWSTR)(ULONG_PTR)(WORD)(i))
#define MAKEINTRESOURCEA(i) ((LPSTR)(ULONG_PTR)(WORD)(i))
#define MAKEINTRESOURCE MAKEINTRESOURCEA
/* global/local memory */
WINBASEAPI LPVOID  WINAPI GlobalLock(HGLOBAL h);
WINBASEAPI BOOL    WINAPI GlobalUnlock(HGLOBAL h);
WINBASEAPI SIZE_T  WINAPI GlobalSize(HGLOBAL h);
WINBASEAPI HGLOBAL WINAPI GlobalReAlloc(HGLOBAL h, SIZE_T n, UINT flags);
WINBASEAPI LPVOID  WINAPI LocalLock(HLOCAL h);
WINBASEAPI BOOL    WINAPI LocalUnlock(HLOCAL h);
WINBASEAPI SIZE_T  WINAPI LocalSize(HLOCAL h);
WINBASEAPI HLOCAL  WINAPI LocalReAlloc(HLOCAL h, SIZE_T n, UINT flags);
/* memory */
WINBASEAPI SIZE_T WINAPI VirtualQuery(LPCVOID p, PMEMORY_BASIC_INFORMATION mbi, SIZE_T n);
typedef struct _MEMORYSTATUSEX {
    DWORD dwLength, dwMemoryLoad;
    DWORDLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;
WINBASEAPI BOOL   WINAPI GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms);
/* thread context (another thread: it must be suspended) */
WINBASEAPI BOOL   WINAPI GetThreadContext(HANDLE t, LPCONTEXT c);
WINBASEAPI BOOL   WINAPI SetThreadContext(HANDLE t, const CONTEXT *c);
/* environment, system */
WINBASEAPI BOOL   WINAPI SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value);
WINBASEAPI DWORD  WINAPI ExpandEnvironmentStringsW(LPCWSTR s, LPWSTR out, DWORD n);
WINBASEAPI DWORD  WINAPI ExpandEnvironmentStringsA(LPCSTR s, LPSTR out, DWORD n);
WINBASEAPI BOOL   WINAPI GetComputerNameW(LPWSTR buf, LPDWORD n);
WINBASEAPI BOOL   WINAPI GetComputerNameExW(int type, LPWSTR buf, LPDWORD n);
WINBASEAPI BOOL   WINAPI GetVersionExW(LPVOID vi);
WINBASEAPI DWORD  WINAPI FormatMessageA(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPSTR buf, DWORD n, va_list *args);
WINBASEAPI DWORD  WINAPI FormatMessageW(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPWSTR buf, DWORD n, va_list *args);
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200
#define FORMAT_MESSAGE_FROM_STRING     0x00000400
#define FORMAT_MESSAGE_FROM_HMODULE    0x00000800
#define FORMAT_MESSAGE_FROM_SYSTEM     0x00001000
#define FORMAT_MESSAGE_ARGUMENT_ARRAY  0x00002000
#define FORMAT_MESSAGE_MAX_WIDTH_MASK  0x000000FF
/* lstr* (UTF-16) */
WINBASEAPI LPWSTR WINAPI lstrcpyW(LPWSTR d, LPCWSTR s);
WINBASEAPI LPWSTR WINAPI lstrcpynW(LPWSTR d, LPCWSTR s, int n);
WINBASEAPI LPSTR  WINAPI lstrcpynA(LPSTR d, LPCSTR s, int n);
WINBASEAPI LPWSTR WINAPI lstrcatW(LPWSTR d, LPCWSTR s);
WINBASEAPI LPSTR  WINAPI lstrcatA(LPSTR d, LPCSTR s);
WINBASEAPI int    WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b);
WINBASEAPI int    WINAPI lstrcmpiW(LPCWSTR a, LPCWSTR b);
/* national language support */
WINBASEAPI BOOL   WINAPI GetCPInfo(UINT cp, LPCPINFO info);
WINBASEAPI UINT   WINAPI GetOEMCP(void);
WINBASEAPI BOOL   WINAPI IsValidCodePage(UINT cp);
WINBASEAPI BOOL   WINAPI IsDBCSLeadByte(BYTE c);
WINBASEAPI BOOL   WINAPI IsDBCSLeadByteEx(UINT cp, BYTE c);
WINBASEAPI int    WINAPI CompareStringW(DWORD lcid, DWORD flags, LPCWSTR a, int na, LPCWSTR b, int nb);
WINBASEAPI int    WINAPI CompareStringEx(LPCWSTR loc, DWORD flags, LPCWSTR a, int na, LPCWSTR b, int nb, LPVOID v, LPVOID r, LONG_PTR p);
WINBASEAPI int    WINAPI CompareStringOrdinal(LPCWSTR a, int na, LPCWSTR b, int nb, BOOL ignore_case);
WINBASEAPI int    WINAPI LCMapStringW(DWORD lcid, DWORD flags, LPCWSTR s, int n, LPWSTR out, int cap);
WINBASEAPI int    WINAPI LCMapStringEx(LPCWSTR loc, DWORD flags, LPCWSTR s, int n, LPWSTR out, int cap, LPVOID v, LPVOID r, LONG_PTR p);
WINBASEAPI BOOL   WINAPI GetStringTypeW(DWORD type, LPCWSTR s, int n, LPWORD out);
WINBASEAPI DWORD  WINAPI GetUserDefaultLCID(void);
WINBASEAPI DWORD  WINAPI GetThreadLocale(void);
WINBASEAPI WORD   WINAPI GetUserDefaultLangID(void);
WINBASEAPI WORD   WINAPI GetUserDefaultUILanguage(void);
WINBASEAPI int    WINAPI GetUserDefaultLocaleName(LPWSTR name, int n);
WINBASEAPI int    WINAPI GetLocaleInfoW(DWORD lcid, DWORD type, LPWSTR buf, int n);
WINBASEAPI int    WINAPI GetLocaleInfoEx(LPCWSTR loc, DWORD type, LPWSTR buf, int n);
#define CSTR_LESS_THAN    1
#define CSTR_EQUAL        2
#define CSTR_GREATER_THAN 3
#define NORM_IGNORECASE   0x00000001
#ifndef LOCALE_USER_DEFAULT
#define LOCALE_USER_DEFAULT 0x0400
#define LOCALE_SYSTEM_DEFAULT 0x0800
#endif
#define LCMAP_LOWERCASE   0x00000100
#define LCMAP_UPPERCASE   0x00000200
#define CT_CTYPE1 1
#define CT_CTYPE2 2
#define CT_CTYPE3 4
/* console */
WINBASEAPI BOOL   WINAPI GetConsoleScreenBufferInfo(HANDLE h, PCONSOLE_SCREEN_BUFFER_INFO info);
WINBASEAPI BOOL   WINAPI SetConsoleTextAttribute(HANDLE h, WORD attr);
WINBASEAPI BOOL   WINAPI SetConsoleCtrlHandler(PHANDLER_ROUTINE fn, BOOL add);
WINBASEAPI BOOL   WINAPI ReadConsoleW(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPVOID c);
WINBASEAPI BOOL   WINAPI SetConsoleCP(UINT cp);
WINBASEAPI BOOL   WINAPI SetConsoleTitleW(LPCWSTR t);
WINBASEAPI BOOL   WINAPI FlushConsoleInputBuffer(HANDLE h);
WINBASEAPI BOOL   WINAPI GetNumberOfConsoleInputEvents(HANDLE h, LPDWORD n);
WINBASEAPI BOOL   WINAPI AllocConsole(void);
WINBASEAPI BOOL   WINAPI FreeConsole(void);
WINBASEAPI BOOL   WINAPI AttachConsole(DWORD pid);
WINBASEAPI HANDLE WINAPI GetConsoleWindow(void);
/* time */
WINBASEAPI DWORD  WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz);
WINBASEAPI BOOL   WINAPI GetTimeZoneInformationForYear(USHORT year, DYNAMIC_TIME_ZONE_INFORMATION *d, LPTIME_ZONE_INFORMATION tz);
WINBASEAPI DWORD  WINAPI GetDynamicTimeZoneInformation(DYNAMIC_TIME_ZONE_INFORMATION *d);
WINBASEAPI BOOL   WINAPI SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *u, LPSYSTEMTIME l);
WINBASEAPI BOOL   WINAPI FileTimeToLocalFileTime(const FILETIME *u, LPFILETIME l);
WINBASEAPI BOOL   WINAPI LocalFileTimeToFileTime(const FILETIME *l, LPFILETIME u);
WINBASEAPI VOID   WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME ft);
WINBASEAPI HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access);
WINBASEAPI HANDLE WINAPI CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name);
WINBASEAPI BOOL   WINAPI SetWaitableTimer(HANDLE t, const LARGE_INTEGER *due, LONG period, LPVOID fn, LPVOID arg, BOOL resume);
WINBASEAPI BOOL   WINAPI CancelWaitableTimer(HANDLE t);
/* wait on address */
WINBASEAPI BOOL   WINAPI WaitOnAddress(volatile VOID *addr, PVOID cmp, SIZE_T size, DWORD ms);
WINBASEAPI BOOL   WINAPI CompareObjectHandles(HANDLE first, HANDLE second);
WINBASEAPI VOID   WINAPI WakeByAddressSingle(PVOID addr);
WINBASEAPI VOID   WINAPI WakeByAddressAll(PVOID addr);
/* interlocked singly linked lists */
WINBASEAPI VOID         WINAPI InitializeSListHead(PSLIST_HEADER h);
WINBASEAPI PSLIST_ENTRY WINAPI InterlockedPushEntrySList(PSLIST_HEADER h, PSLIST_ENTRY e);
WINBASEAPI PSLIST_ENTRY WINAPI InterlockedPopEntrySList(PSLIST_HEADER h);
WINBASEAPI PSLIST_ENTRY WINAPI InterlockedFlushSList(PSLIST_HEADER h);
WINBASEAPI USHORT       WINAPI QueryDepthSList(PSLIST_HEADER h);
/* pipes */
#define PIPE_ACCESS_INBOUND            0x00000001
#define PIPE_ACCESS_OUTBOUND           0x00000002
#define PIPE_ACCESS_DUPLEX             0x00000003
#define PIPE_CLIENT_END                0x00000000
#define PIPE_SERVER_END                0x00000001
#define PIPE_WAIT                      0x00000000
#define PIPE_NOWAIT                    0x00000001
#define PIPE_READMODE_BYTE             0x00000000
#define PIPE_READMODE_MESSAGE          0x00000002
#define PIPE_TYPE_BYTE                 0x00000000
#define PIPE_TYPE_MESSAGE              0x00000004
#define PIPE_ACCEPT_REMOTE_CLIENTS     0x00000000
#define PIPE_REJECT_REMOTE_CLIENTS     0x00000008
#define PIPE_UNLIMITED_INSTANCES       255
#define FILE_FLAG_FIRST_PIPE_INSTANCE  0x00080000
#define NMPWAIT_WAIT_FOREVER           0xFFFFFFFF
#define NMPWAIT_NOWAIT                 0x00000001
#define NMPWAIT_USE_DEFAULT_WAIT       0x00000000
#define HANDLE_FLAG_INHERIT            0x00000001
#define HANDLE_FLAG_PROTECT_FROM_CLOSE 0x00000002
WINBASEAPI HANDLE WINAPI CreateNamedPipeA(LPCSTR name, DWORD mode, DWORD pmode, DWORD max, DWORD out, DWORD in, DWORD ms, LPSECURITY_ATTRIBUTES sa);
WINBASEAPI HANDLE WINAPI CreateNamedPipeW(LPCWSTR name, DWORD mode, DWORD pmode, DWORD max, DWORD out, DWORD in, DWORD ms, LPSECURITY_ATTRIBUTES sa);
WINBASEAPI BOOL   WINAPI CreatePipe(PHANDLE r, PHANDLE w, LPSECURITY_ATTRIBUTES sa, DWORD size);
WINBASEAPI BOOL   WINAPI PeekNamedPipe(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPDWORD avail, LPDWORD left);
WINBASEAPI BOOL   WINAPI ConnectNamedPipe(HANDLE h, LPOVERLAPPED ov);
WINBASEAPI BOOL   WINAPI DisconnectNamedPipe(HANDLE h);
WINBASEAPI BOOL   WINAPI WaitNamedPipeA(LPCSTR name, DWORD ms);
WINBASEAPI BOOL   WINAPI WaitNamedPipeW(LPCWSTR name, DWORD ms);
WINBASEAPI BOOL   WINAPI TransactNamedPipe(HANDLE h, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len, LPDWORD read, LPOVERLAPPED ov);
WINBASEAPI BOOL   WINAPI CallNamedPipeA(LPCSTR name, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len, LPDWORD read, DWORD ms);
WINBASEAPI BOOL   WINAPI CallNamedPipeW(LPCWSTR name, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len, LPDWORD read, DWORD ms);
WINBASEAPI BOOL   WINAPI GetNamedPipeInfo(HANDLE h, LPDWORD flags, LPDWORD out, LPDWORD in, LPDWORD max);
WINBASEAPI BOOL   WINAPI GetNamedPipeHandleStateA(HANDLE h, LPDWORD state, LPDWORD inst, LPDWORD count, LPDWORD ms, LPSTR user, DWORD user_len);
WINBASEAPI BOOL   WINAPI GetNamedPipeHandleStateW(HANDLE h, LPDWORD state, LPDWORD inst, LPDWORD count, LPDWORD ms, LPWSTR user, DWORD user_len);
WINBASEAPI BOOL   WINAPI SetNamedPipeHandleState(HANDLE h, LPDWORD mode, LPDWORD count, LPDWORD ms);
WINBASEAPI BOOL   WINAPI GetNamedPipeClientProcessId(HANDLE h, PULONG pid);
WINBASEAPI BOOL   WINAPI GetNamedPipeServerProcessId(HANDLE h, PULONG pid);
WINBASEAPI BOOL   WINAPI SetHandleInformation(HANDLE h, DWORD mask, DWORD flags);
WINBASEAPI BOOL   WINAPI GetHandleInformation(HANDLE h, LPDWORD flags);
WINBASEAPI BOOL   WINAPI CancelIo(HANDLE h);
WINBASEAPI BOOL   WINAPI CancelIoEx(HANDLE h, LPOVERLAPPED ov);
WINBASEAPI BOOL   WINAPI GetOverlappedResultEx(HANDLE h, LPOVERLAPPED ov, LPDWORD bytes, DWORD ms, BOOL alertable);
#define ERROR_IO_PENDING           997
#define ERROR_IO_INCOMPLETE        996
#define ERROR_OPERATION_ABORTED    995
#define ERROR_BROKEN_PIPE          109
#define ERROR_BAD_PIPE             230
#define ERROR_PIPE_BUSY            231
#define ERROR_NO_DATA              232
#define ERROR_PIPE_NOT_CONNECTED   233
#define ERROR_PIPE_CONNECTED       535
#define ERROR_PIPE_LISTENING       536
#define ERROR_SEM_TIMEOUT          121
#define ERROR_NOT_FOUND            1168
#define ERROR_BAD_LENGTH           24
#define ERROR_BUFFER_OVERFLOW      111
#define ERROR_DIRECTORY            267
#define ERROR_ABANDONED_WAIT_0     735
#define WAIT_IO_COMPLETION         0x000000C0

WINBASEAPI int WINAPI MulDiv(int a, int b, int c);
WINBASEAPI WORD WINAPI GlobalAddAtomW(LPCWSTR s);
WINBASEAPI WORD WINAPI GlobalFindAtomW(LPCWSTR s);
WINBASEAPI UINT WINAPI GlobalGetAtomNameW(WORD a, LPWSTR buf, int n);
WINBASEAPI WORD WINAPI GlobalDeleteAtom(WORD a);
#define LOAD_LIBRARY_AS_DATAFILE 0x00000002
#define ERROR_TIMEOUT 1460
#define ERROR_NO_SYSTEM_RESOURCES 1450
#define ERROR_NOT_ENOUGH_QUOTA 1816
#define ERROR_INVALID_WINDOW_HANDLE 1400
#define ERROR_INVALID_MENU_HANDLE 1401
#define ERROR_CANNOT_FIND_WND_CLASS 1407
#define ERROR_INVALID_THREAD_ID 1444
#define ERROR_CONTROL_ID_NOT_FOUND 1421
#define ERROR_MENU_ITEM_NOT_FOUND 1456
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813

