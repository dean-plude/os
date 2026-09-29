/* windows.h — the Win32 subset NovaOS's kernel32.dll implements */
#pragma once
#include <_nova.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
_NOVA_BEGIN

#define WINAPI   __stdcall
#define APIENTRY WINAPI
#define CALLBACK __stdcall
#define CONST    const
#define VOID     void
#define TRUE     1
#define FALSE    0
#define MAX_PATH 260

typedef int                BOOL;
typedef unsigned char      BYTE, BOOLEAN, UCHAR;
typedef unsigned short     WORD, USHORT;
typedef unsigned long      DWORD, ULONG;
typedef long               LONG;
typedef int                INT;
typedef int                INT32;
typedef unsigned int       UINT32;
typedef short              INT16;
typedef unsigned short     UINT16;
typedef signed char        INT8;
typedef unsigned char      UINT8;
typedef short              SHORT;
typedef unsigned int       UINT;
typedef long long          LONGLONG, LONG64, INT64;
typedef unsigned long long ULONGLONG, DWORD64, ULONG64, UINT64;
typedef intptr_t           INT_PTR, LONG_PTR;
typedef uintptr_t          UINT_PTR, ULONG_PTR, DWORD_PTR, SIZE_T;
typedef intptr_t           SSIZE_T;
typedef char               CHAR;
typedef unsigned short     WCHAR;
typedef CHAR  *LPSTR;
typedef const CHAR *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef WCHAR *PWSTR;
typedef const WCHAR *PCWSTR;
typedef void  *LPVOID, *PVOID, *HANDLE, *HMODULE, *HINSTANCE, *HLOCAL, *HGLOBAL, *FARPROC;
typedef const void *LPCVOID;
typedef HANDLE *PHANDLE, *LPHANDLE;
typedef DWORD *LPDWORD, *PDWORD;
typedef LONG  *PLONG, *LPLONG;
typedef BOOL  *LPBOOL;
typedef BYTE  *LPBYTE, *PBYTE;
typedef ULONG_PTR *PULONG_PTR;
typedef ULONG *PULONG;
typedef DWORD64 *PDWORD64;
typedef WORD *PWORD, *LPWORD;
typedef WCHAR *PWCHAR;
typedef long long *PLONG64, *PLONGLONG;
typedef SIZE_T *PSIZE_T;
typedef unsigned long long DWORDLONG;
typedef long   HRESULT;

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef int *LPINT, *PINT;
typedef UINT *PUINT, *LPUINT;
typedef struct _GUID { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID, IID, CLSID, *LPGUID, *LPIID, *LPCLSID;
#ifdef __cplusplus
typedef const GUID &REFGUID, &REFIID, &REFCLSID;        /* C++ passes GUIDs by reference (same ABI) */
#else
typedef const GUID *REFGUID, *REFIID, *REFCLSID;
#endif
typedef const GUID *LPCGUID;
typedef DWORD LCID;
typedef WORD  LANGID;

/* HRESULTs (winerror.h) */
#define SUCCEEDED(hr)            (((HRESULT)(hr)) >= 0)
#define FAILED(hr)               (((HRESULT)(hr)) < 0)
#define MAKE_HRESULT(s, f, c)    ((HRESULT)(((unsigned long)(s) << 31) | ((unsigned long)(f) << 16) | ((unsigned long)(c))))
#define HRESULT_FROM_WIN32(e)    ((HRESULT)(e) <= 0 ? (HRESULT)(e) : (HRESULT)(((e) & 0x0000FFFF) | 0x80070000))
#define HRESULT_CODE(hr)         ((hr) & 0xFFFF)
#define HRESULT_FACILITY(hr)     (((hr) >> 16) & 0x1FFF)
#define S_OK                     ((HRESULT)0)
#define S_FALSE                  ((HRESULT)1)
#define E_NOTIMPL                ((HRESULT)0x80004001L)
#define E_NOINTERFACE            ((HRESULT)0x80004002L)
#define E_POINTER                ((HRESULT)0x80004003L)
#define E_ABORT                  ((HRESULT)0x80004004L)
#define E_FAIL                   ((HRESULT)0x80004005L)
#define E_UNEXPECTED             ((HRESULT)0x8000FFFFL)
#define E_ACCESSDENIED           ((HRESULT)0x80070005L)
#define E_HANDLE                 ((HRESULT)0x80070006L)
#define E_OUTOFMEMORY            ((HRESULT)0x8007000EL)
#define E_INVALIDARG             ((HRESULT)0x80070057L)

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

/* -----------------------------------------------------------------------
 * GUI shared types, then user32/gdi32 (winuser.h / wingdi.h)
 * ----------------------------------------------------------------------- */
typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT, *PPOINT;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT, *PRECT;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *LPSIZE, *PSIZE;
typedef const RECT *LPCRECT;
typedef void *HWND, *HDC, *HMENU, *HICON, *HCURSOR, *HBRUSH, *HPEN, *HFONT, *HGDIOBJ, *HBITMAP;
typedef HICON HANDLE_ICON;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM, LRESULT;
typedef DWORD COLORREF;
#define RGB(r,g,b) ((COLORREF)(((BYTE)(r))|(((WORD)((BYTE)(g)))<<8)|(((DWORD)((BYTE)(b)))<<16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c)>>8))
#define GetBValue(c) ((BYTE)((c)>>16))

_NOVA_END
#include <wingdi.h>
#include <winuser.h>
#include <winnt.h>
#include <excpt.h>
_NOVA_BEGIN

typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *PFILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _SECURITY_ATTRIBUTES {
    DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

typedef struct _SYSTEM_INFO {
    WORD wProcessorArchitecture, wReserved;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

typedef struct _OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    CHAR szCSDVersion[128];
} OSVERSIONINFOA, *LPOSVERSIONINFOA;


typedef struct _STARTUPINFOA {
    DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2; LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;


#define INVALID_HANDLE_VALUE     ((HANDLE)(LONG_PTR)-1)
#define INVALID_FILE_SIZE        ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define INVALID_FILE_ATTRIBUTES  ((DWORD)-1)
#define STD_INPUT_HANDLE         ((DWORD)-10)
#define STD_OUTPUT_HANDLE        ((DWORD)-11)
#define STD_ERROR_HANDLE         ((DWORD)-12)

#define GENERIC_READ             0x80000000
#define GENERIC_WRITE            0x40000000
#define GENERIC_EXECUTE          0x20000000
#define GENERIC_ALL              0x10000000
#define FILE_SHARE_READ          0x00000001
#define FILE_SHARE_WRITE         0x00000002
#define FILE_SHARE_DELETE        0x00000004
#define CREATE_NEW               1
#define CREATE_ALWAYS            2
#define OPEN_EXISTING            3
#define OPEN_ALWAYS              4
#define TRUNCATE_EXISTING        5
#define FILE_ATTRIBUTE_READONLY  0x00000001
#define FILE_ATTRIBUTE_HIDDEN    0x00000002
#define FILE_ATTRIBUTE_SYSTEM    0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE   0x00000020
#define FILE_ATTRIBUTE_NORMAL    0x00000080
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_BEGIN               0
#define FILE_CURRENT             1
#define FILE_END                 2
#define FILE_TYPE_UNKNOWN        0
#define FILE_TYPE_DISK           1
#define FILE_TYPE_CHAR           2
#define FILE_TYPE_PIPE           3

#define MEM_COMMIT               0x00001000
#define MEM_RESERVE              0x00002000
#define MEM_DECOMMIT             0x00004000
#define MEM_RELEASE              0x00008000
#define PAGE_NOACCESS            0x01
#define PAGE_READONLY            0x02
#define PAGE_READWRITE           0x04
#define PAGE_EXECUTE             0x10
#define PAGE_EXECUTE_READ        0x20
#define PAGE_EXECUTE_READWRITE   0x40
#define HEAP_ZERO_MEMORY         0x00000008
#define LMEM_ZEROINIT            0x0040
#define GMEM_ZEROINIT            0x0040
#define GMEM_FIXED               0x0000
#define GMEM_MOVEABLE            0x0002
#define LMEM_FIXED               0x0000
#define LMEM_MOVEABLE            0x0002
#define GPTR                     0x0040
#define LPTR                     0x0040
#define CopyMemory(d, s, n)      __builtin_memcpy((d), (s), (n))
#define MoveMemory(d, s, n)      __builtin_memmove((d), (s), (n))
#define FillMemory(d, n, c)      __builtin_memset((d), (c), (n))
#define ZeroMemory(d, n)         __builtin_memset((d), 0, (n))
#define SecureZeroMemory(d, n)   __builtin_memset((d), 0, (n))

#define CP_ACP                   0
#define CP_OEMCP                 1
#define CP_UTF8                  65001
#define INFINITE                 0xFFFFFFFF

#define ERROR_SUCCESS            0
#define ERROR_INVALID_FUNCTION   1
#define ERROR_FILE_NOT_FOUND     2
#define ERROR_PATH_NOT_FOUND     3
#define ERROR_TOO_MANY_OPEN_FILES 4
#define ERROR_ACCESS_DENIED      5
#define ERROR_INVALID_HANDLE     6
#define ERROR_NOT_ENOUGH_MEMORY  8
#define ERROR_OUTOFMEMORY        14
#define ERROR_NO_MORE_FILES      18
#define ERROR_HANDLE_EOF         38
#define ERROR_NOT_SUPPORTED      50
#define ERROR_FILE_EXISTS        80
#define ERROR_INVALID_PARAMETER  87
#define ERROR_DISK_FULL          112
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_INVALID_NAME       123
#define ERROR_MOD_NOT_FOUND      126
#define ERROR_PROC_NOT_FOUND     127
#define ERROR_DIR_NOT_EMPTY      145
#define ERROR_ALREADY_EXISTS     183
#define ERROR_ENVVAR_NOT_FOUND   203
#define ERROR_MORE_DATA          234
#define ERROR_INVALID_ADDRESS    487

#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)(((DWORD_PTR)(l) >> 16) & 0xFFFF))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))

/* Process and environment */
WINBASEAPI __declspec(noreturn) VOID WINAPI ExitProcess(UINT code);
WINBASEAPI BOOL    WINAPI TerminateProcess(HANDLE process, UINT code);
WINBASEAPI HANDLE  WINAPI GetCurrentProcess(void);
WINBASEAPI DWORD   WINAPI GetCurrentProcessId(void);
WINBASEAPI DWORD   WINAPI GetCurrentThreadId(void);
WINBASEAPI LPSTR   WINAPI GetCommandLineA(void);
WINBASEAPI LPWSTR  WINAPI GetCommandLineW(void);
WINBASEAPI HMODULE WINAPI GetModuleHandleA(LPCSTR name);
WINBASEAPI HMODULE WINAPI GetModuleHandleW(LPCWSTR name);
WINBASEAPI DWORD   WINAPI GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD size);
WINBASEAPI DWORD   WINAPI GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD size);
WINBASEAPI FARPROC WINAPI GetProcAddress(HMODULE m, LPCSTR name);
WINBASEAPI HMODULE WINAPI LoadLibraryA(LPCSTR name);
WINBASEAPI BOOL    WINAPI FreeLibrary(HMODULE m);
WINBASEAPI DWORD   WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size);
WINBASEAPI DWORD   WINAPI GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size);
WINBASEAPI BOOL    WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
WINBASEAPI LPWSTR  WINAPI GetEnvironmentStringsW(void);
WINBASEAPI BOOL    WINAPI FreeEnvironmentStringsW(LPWSTR env);
WINBASEAPI VOID    WINAPI GetStartupInfoA(LPSTARTUPINFOA si);
WINBASEAPI DWORD   WINAPI GetLastError(void);
WINBASEAPI VOID    WINAPI SetLastError(DWORD err);
WINBASEAPI VOID    WINAPI GetSystemInfo(LPSYSTEM_INFO si);
WINBASEAPI DWORD   WINAPI GetVersion(void);
WINBASEAPI BOOL    WINAPI GetVersionExA(LPOSVERSIONINFOA vi);
WINBASEAPI BOOL    WINAPI GetComputerNameA(LPSTR buf, LPDWORD size);
WINBASEAPI BOOL    WINAPI IsDebuggerPresent(void);
WINBASEAPI VOID    WINAPI OutputDebugStringA(LPCSTR s);

/* Console and files */
WINBASEAPI HANDLE  WINAPI GetStdHandle(DWORD which);
WINBASEAPI BOOL    WINAPI SetStdHandle(DWORD which, HANDLE h);
WINBASEAPI BOOL    WINAPI WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPVOID overlapped);
WINBASEAPI BOOL    WINAPI ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPVOID overlapped);
WINBASEAPI BOOL    WINAPI WriteConsoleA(HANDLE h, const VOID *buf, DWORD n, LPDWORD written, LPVOID reserved);
WINBASEAPI BOOL    WINAPI WriteConsoleW(HANDLE h, const VOID *buf, DWORD n, LPDWORD written, LPVOID reserved);
WINBASEAPI BOOL    WINAPI ReadConsoleA(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPVOID control);
WINBASEAPI BOOL    WINAPI GetConsoleMode(HANDLE h, LPDWORD mode);
WINBASEAPI BOOL    WINAPI SetConsoleMode(HANDLE h, DWORD mode);
WINBASEAPI UINT    WINAPI GetConsoleCP(void);
WINBASEAPI UINT    WINAPI GetConsoleOutputCP(void);
WINBASEAPI BOOL    WINAPI SetConsoleOutputCP(UINT cp);
WINBASEAPI BOOL    WINAPI SetConsoleTitleA(LPCSTR title);
WINBASEAPI DWORD   WINAPI GetFileType(HANDLE h);
WINBASEAPI HANDLE  WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disposition, DWORD flags, HANDLE templ);
WINBASEAPI HANDLE  WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disposition, DWORD flags, HANDLE templ);
WINBASEAPI BOOL    WINAPI CloseHandle(HANDLE h);
WINBASEAPI DWORD   WINAPI GetFileSize(HANDLE h, LPDWORD high);
WINBASEAPI BOOL    WINAPI GetFileSizeEx(HANDLE h, PLARGE_INTEGER size);
WINBASEAPI DWORD   WINAPI SetFilePointer(HANDLE h, LONG dist, PLONG high, DWORD method);
WINBASEAPI BOOL    WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newpos, DWORD method);
WINBASEAPI BOOL    WINAPI SetEndOfFile(HANDLE h);
WINBASEAPI BOOL    WINAPI FlushFileBuffers(HANDLE h);
WINBASEAPI BOOL    WINAPI DeleteFileA(LPCSTR name);
WINBASEAPI BOOL    WINAPI DeleteFileW(LPCWSTR name);
WINBASEAPI BOOL    WINAPI CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa);
WINBASEAPI BOOL    WINAPI CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES sa);
WINBASEAPI BOOL    WINAPI RemoveDirectoryA(LPCSTR name);
WINBASEAPI DWORD   WINAPI GetFileAttributesA(LPCSTR name);
WINBASEAPI DWORD   WINAPI GetFileAttributesW(LPCWSTR name);
WINBASEAPI HANDLE  WINAPI FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd);
WINBASEAPI BOOL    WINAPI FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd);
WINBASEAPI HANDLE  WINAPI FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW fd);
WINBASEAPI BOOL    WINAPI FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW fd);
WINBASEAPI BOOL    WINAPI FindClose(HANDLE h);
WINBASEAPI DWORD   WINAPI GetCurrentDirectoryA(DWORD size, LPSTR buf);
WINBASEAPI DWORD   WINAPI GetCurrentDirectoryW(DWORD size, LPWSTR buf);
WINBASEAPI BOOL    WINAPI SetCurrentDirectoryA(LPCSTR path);
WINBASEAPI DWORD   WINAPI GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buf, LPSTR *filepart);

/* Memory */
WINBASEAPI LPVOID  WINAPI VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
WINBASEAPI BOOL    WINAPI VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
WINBASEAPI BOOL    WINAPI VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, LPDWORD old);
WINBASEAPI HANDLE  WINAPI GetProcessHeap(void);
WINBASEAPI HANDLE  WINAPI HeapCreate(DWORD flags, SIZE_T initial, SIZE_T max);
WINBASEAPI BOOL    WINAPI HeapDestroy(HANDLE heap);
WINBASEAPI LPVOID  WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
WINBASEAPI LPVOID  WINAPI HeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T n);
WINBASEAPI BOOL    WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID p);
WINBASEAPI SIZE_T  WINAPI HeapSize(HANDLE heap, DWORD flags, LPCVOID p);
WINBASEAPI HLOCAL  WINAPI LocalAlloc(UINT flags, SIZE_T n);
WINBASEAPI HLOCAL  WINAPI LocalFree(HLOCAL p);
WINBASEAPI HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T n);
WINBASEAPI HGLOBAL WINAPI GlobalFree(HGLOBAL p);

/* Time */
WINBASEAPI VOID    WINAPI Sleep(DWORD ms);
WINBASEAPI DWORD   WINAPI GetTickCount(void);
WINBASEAPI ULONGLONG WINAPI GetTickCount64(void);
WINBASEAPI BOOL    WINAPI QueryPerformanceCounter(PLARGE_INTEGER c);
WINBASEAPI BOOL    WINAPI QueryPerformanceFrequency(PLARGE_INTEGER f);
WINBASEAPI VOID    WINAPI GetSystemTimeAsFileTime(LPFILETIME ft);
WINBASEAPI VOID    WINAPI GetSystemTime(LPSYSTEMTIME st);
WINBASEAPI VOID    WINAPI GetLocalTime(LPSYSTEMTIME st);
WINBASEAPI BOOL    WINAPI FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st);
WINBASEAPI BOOL    WINAPI SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft);

/* Strings */
WINBASEAPI int     WINAPI MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR out, int cap);
WINBASEAPI int     WINAPI WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR s, int n, LPSTR out, int cap,
                                              LPCSTR defchar, LPBOOL used);
WINBASEAPI UINT    WINAPI GetACP(void);
WINBASEAPI int     WINAPI lstrlenA(LPCSTR s);
WINBASEAPI int     WINAPI lstrlenW(LPCWSTR s);
WINBASEAPI LPSTR   WINAPI lstrcpyA(LPSTR d, LPCSTR s);
WINBASEAPI int     WINAPI lstrcmpA(LPCSTR a, LPCSTR b);
WINBASEAPI int     WINAPI lstrcmpiA(LPCSTR a, LPCSTR b);


/* -----------------------------------------------------------------------
 * Threads, synchronization, dynamic loading (kernel32)
 * ----------------------------------------------------------------------- */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID param);
typedef VOID (WINAPI *PFLS_CALLBACK_FUNCTION)(PVOID);
typedef BOOL (WINAPI *PINIT_ONCE_FN)(PINIT_ONCE, PVOID, PVOID *);
typedef VOID (WINAPI *PAPCFUNC)(ULONG_PTR);

#define WAIT_OBJECT_0        0x00000000
#define WAIT_ABANDONED_0     0x00000080
#define WAIT_ABANDONED       0x00000080
#define WAIT_TIMEOUT         0x00000102
#define WAIT_FAILED          0xFFFFFFFF
#define CREATE_SUSPENDED     0x00000004
#define STILL_ACTIVE         0x00000103
#define TLS_OUT_OF_INDEXES   0xFFFFFFFF
#define MAXIMUM_WAIT_OBJECTS 64
#define EVENT_ALL_ACCESS     0x1F0003
#define MUTEX_ALL_ACCESS     0x1F0001
#define SEMAPHORE_ALL_ACCESS 0x1F0003
#define SYNCHRONIZE          0x00100000
#define INIT_ONCE_ASYNC      0x00000002

WINBASEAPI HANDLE  WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start,
                                       LPVOID param, DWORD flags, LPDWORD tid);
WINBASEAPI HANDLE  WINAPI GetCurrentThread(void);
WINBASEAPI __declspec(noreturn) VOID WINAPI ExitThread(DWORD code);
WINBASEAPI DWORD   WINAPI ResumeThread(HANDLE t);
WINBASEAPI DWORD   WINAPI SuspendThread(HANDLE t);
WINBASEAPI BOOL    WINAPI TerminateThread(HANDLE t, DWORD code);
WINBASEAPI BOOL    WINAPI GetExitCodeThread(HANDLE t, LPDWORD code);
WINBASEAPI BOOL    WINAPI GetExitCodeProcess(HANDLE p, LPDWORD code);
WINBASEAPI DWORD   WINAPI GetThreadId(HANDLE t);
WINBASEAPI BOOL    WINAPI SwitchToThread(void);
WINBASEAPI DWORD   WINAPI WaitForSingleObject(HANDLE h, DWORD ms);
WINBASEAPI DWORD   WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
WINBASEAPI DWORD   WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms);
WINBASEAPI DWORD   WINAPI WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable);

WINBASEAPI HANDLE  WINAPI CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name);
WINBASEAPI HANDLE  WINAPI CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name);
WINBASEAPI BOOL    WINAPI SetEvent(HANDLE h);
WINBASEAPI BOOL    WINAPI ResetEvent(HANDLE h);
WINBASEAPI HANDLE  WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name);
WINBASEAPI HANDLE  WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name);
WINBASEAPI BOOL    WINAPI ReleaseMutex(HANDLE h);
WINBASEAPI HANDLE  WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCSTR name);
WINBASEAPI HANDLE  WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCWSTR name);
WINBASEAPI BOOL    WINAPI ReleaseSemaphore(HANDLE h, LONG count, LPLONG prev);
WINBASEAPI BOOL    WINAPI DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, LPHANDLE dst, DWORD access, BOOL inherit, DWORD options);
#define DUPLICATE_SAME_ACCESS  0x00000002
#define DUPLICATE_CLOSE_SOURCE 0x00000001

WINBASEAPI VOID    WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs);
WINBASEAPI BOOL    WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin);
WINBASEAPI VOID    WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs);
WINBASEAPI VOID    WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs);
WINBASEAPI VOID    WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs);
WINBASEAPI BOOL    WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION cs);
WINBASEAPI VOID    WINAPI InitializeSRWLock(PSRWLOCK l);
WINBASEAPI VOID    WINAPI AcquireSRWLockExclusive(PSRWLOCK l);
WINBASEAPI VOID    WINAPI ReleaseSRWLockExclusive(PSRWLOCK l);
WINBASEAPI VOID    WINAPI AcquireSRWLockShared(PSRWLOCK l);
WINBASEAPI VOID    WINAPI ReleaseSRWLockShared(PSRWLOCK l);
WINBASEAPI BOOLEAN WINAPI TryAcquireSRWLockExclusive(PSRWLOCK l);
WINBASEAPI BOOLEAN WINAPI TryAcquireSRWLockShared(PSRWLOCK l);
WINBASEAPI VOID    WINAPI InitializeConditionVariable(PCONDITION_VARIABLE cv);
WINBASEAPI VOID    WINAPI WakeConditionVariable(PCONDITION_VARIABLE cv);
WINBASEAPI VOID    WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE cv);
WINBASEAPI BOOL    WINAPI SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms);
WINBASEAPI BOOL    WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags);
WINBASEAPI VOID    WINAPI InitOnceInitialize(PINIT_ONCE once);
WINBASEAPI BOOL    WINAPI InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID *ctx);

WINBASEAPI DWORD   WINAPI TlsAlloc(void);
WINBASEAPI BOOL    WINAPI TlsFree(DWORD i);
WINBASEAPI LPVOID  WINAPI TlsGetValue(DWORD i);
WINBASEAPI BOOL    WINAPI TlsSetValue(DWORD i, LPVOID v);
WINBASEAPI DWORD   WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION cb);
WINBASEAPI BOOL    WINAPI FlsFree(DWORD i);
WINBASEAPI PVOID   WINAPI FlsGetValue(DWORD i);
WINBASEAPI BOOL    WINAPI FlsSetValue(DWORD i, PVOID v);

WINBASEAPI HMODULE WINAPI LoadLibraryW(LPCWSTR name);
WINBASEAPI HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE f, DWORD flags);
WINBASEAPI BOOL    WINAPI GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out);
WINBASEAPI VOID    WINAPI RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args);
WINBASEAPI LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f);
WINBASEAPI LONG    WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS info);
WINBASEAPI PVOID   WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
WINBASEAPI ULONG   WINAPI RemoveVectoredExceptionHandler(PVOID h);
WINBASEAPI VOID    WINAPI GetNativeSystemInfo(LPSYSTEM_INFO si);
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002

#include <winbase2.h>
#include <winsec.h>
#include <winreg.h>

#define CreateFile           CreateFileA
#define DeleteFile           DeleteFileA
#define CreateDirectory      CreateDirectoryA
#define GetFileAttributes    GetFileAttributesA
#define FindFirstFile        FindFirstFileA
#define FindNextFile         FindNextFileA
#define WIN32_FIND_DATA      WIN32_FIND_DATAA
#define GetModuleHandle      GetModuleHandleA
#define GetModuleFileName    GetModuleFileNameA
#define GetCommandLine       GetCommandLineA
#define GetEnvironmentVariable GetEnvironmentVariableA
#define GetCurrentDirectory  GetCurrentDirectoryA
#define SetCurrentDirectory  SetCurrentDirectoryA
#define WriteConsole         WriteConsoleA
#define OutputDebugString    OutputDebugStringA
#define LoadLibrary          LoadLibraryA
#define CreateEvent          CreateEventA
#define CreateMutex          CreateMutexA
#define CreateSemaphore      CreateSemaphoreA
#define GetModuleHandleEx    GetModuleHandleExA

_NOVA_END
