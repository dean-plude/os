/*
 * pdh.dll — performance counters.  NovaOS publishes none: opening a query
 * works, but no counter can be added to it.
 */
#include <windows.h>

#define PDHAPI __declspec(dllexport)
#define PDH_CSTATUS_NO_OBJECT_ 0xC0000BB8L
#define PDH_INVALID_HANDLE_    0xC0000BBCL
#define PDH_NO_DATA_           0x800007D5L

static int g_query;

PDHAPI LONG WINAPI PdhOpenQueryW(LPCWSTR src, DWORD_PTR user, PHANDLE q) { (void)src; (void)user; *q = &g_query; return 0; }
PDHAPI LONG WINAPI PdhOpenQueryA(LPCSTR src, DWORD_PTR user, PHANDLE q) { (void)src; (void)user; *q = &g_query; return 0; }
PDHAPI LONG WINAPI PdhCloseQuery(HANDLE q) { return q == &g_query ? 0 : PDH_INVALID_HANDLE_; }
PDHAPI LONG WINAPI PdhAddEnglishCounterW(HANDLE q, LPCWSTR path, DWORD_PTR user, PHANDLE c) { (void)q; (void)path; (void)user; *c = 0; return PDH_CSTATUS_NO_OBJECT_; }
PDHAPI LONG WINAPI PdhAddCounterW(HANDLE q, LPCWSTR path, DWORD_PTR user, PHANDLE c) { (void)q; (void)path; (void)user; *c = 0; return PDH_CSTATUS_NO_OBJECT_; }
PDHAPI LONG WINAPI PdhCollectQueryData(HANDLE q) { (void)q; return PDH_NO_DATA_; }
PDHAPI LONG WINAPI PdhGetFormattedCounterValue(HANDLE c, DWORD fmt, LPDWORD type, PVOID value) { (void)c; (void)fmt; (void)type; (void)value; return PDH_INVALID_HANDLE_; }
