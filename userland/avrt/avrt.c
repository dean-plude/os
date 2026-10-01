/*
 * avrt.dll — the Multimedia Class Scheduler client (audio threads ask it
 * for "Pro Audio" or "Games" scheduling).  NovaOS raises the thread's
 * priority instead and hands back a dummy task handle.
 */

#include <windows.h>

#define EXPORT __declspec(dllexport)

EXPORT HANDLE WINAPI AvSetMmThreadCharacteristicsW(LPCWSTR task, LPDWORD index)
{
    if (!task || !index) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    SetThreadPriority(GetCurrentThread(), 2 /* THREAD_PRIORITY_HIGHEST */);
    *index = 1;
    return (HANDLE)(ULONG_PTR)0x4156;
}
EXPORT HANDLE WINAPI AvSetMmThreadCharacteristicsA(LPCSTR task, LPDWORD index)
{
    return AvSetMmThreadCharacteristicsW(task ? L"" : 0, index);
}
EXPORT HANDLE WINAPI AvSetMmMaxThreadCharacteristicsW(LPCWSTR a, LPCWSTR b, LPDWORD index)
{
    (void)b;
    return AvSetMmThreadCharacteristicsW(a, index);
}
EXPORT HANDLE WINAPI AvSetMmMaxThreadCharacteristicsA(LPCSTR a, LPCSTR b, LPDWORD index)
{
    (void)b;
    return AvSetMmThreadCharacteristicsA(a, index);
}
EXPORT BOOL WINAPI AvRevertMmThreadCharacteristics(HANDLE h)
{
    if (!h) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    SetThreadPriority(GetCurrentThread(), 0);
    return TRUE;
}
EXPORT BOOL WINAPI AvSetMmThreadPriority(HANDLE h, int prio)
{
    if (!h) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return SetThreadPriority(GetCurrentThread(), prio < -1 ? -1 : prio > 2 ? 2 : prio);   /* AVRT_PRIORITY_LOW..CRITICAL */
}
EXPORT BOOL WINAPI AvQuerySystemResponsiveness(HANDLE h, PULONG v)
{
    if (!h || !v) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *v = 20;
    return TRUE;
}
