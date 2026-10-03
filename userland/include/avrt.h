/* avrt.h — the Multimedia Class Scheduler client (userland/avrt) */
#pragma once
#include <windows.h>

#ifndef AVRTAPI
#define AVRTAPI __declspec(dllimport)
#endif

typedef enum _AVRT_PRIORITY {
    AVRT_PRIORITY_VERYLOW = -2,
    AVRT_PRIORITY_LOW,
    AVRT_PRIORITY_NORMAL,
    AVRT_PRIORITY_HIGH,
    AVRT_PRIORITY_CRITICAL
} AVRT_PRIORITY, *PAVRT_PRIORITY;

AVRTAPI HANDLE WINAPI AvSetMmThreadCharacteristicsA(LPCSTR task, LPDWORD index);
AVRTAPI HANDLE WINAPI AvSetMmThreadCharacteristicsW(LPCWSTR task, LPDWORD index);
AVRTAPI HANDLE WINAPI AvSetMmMaxThreadCharacteristicsA(LPCSTR first, LPCSTR second, LPDWORD index);
AVRTAPI HANDLE WINAPI AvSetMmMaxThreadCharacteristicsW(LPCWSTR first, LPCWSTR second, LPDWORD index);
AVRTAPI BOOL   WINAPI AvRevertMmThreadCharacteristics(HANDLE task);
AVRTAPI BOOL   WINAPI AvSetMmThreadPriority(HANDLE task, AVRT_PRIORITY priority);
AVRTAPI BOOL   WINAPI AvQuerySystemResponsiveness(HANDLE task, PULONG responsiveness);
