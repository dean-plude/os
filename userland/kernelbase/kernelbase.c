/*
 * kernelbase.dll — what Windows keeps in kernelbase and not in kernel32.
 *
 * Nearly all of kernel32 is implemented in kernelbase on Windows, and
 * NovaOS's loader treats the two as one for the API sets: a function
 * imported through api-ms-win-core-* that kernel32 lacks is looked for
 * here, and one kernelbase lacks in kernel32.  What must live here alone
 * is what kernel32 does not export: WaitOnAddress and its wakers.  VLC
 * asks kernel32 for WaitOnAddress and takes its condition-variable path
 * only when kernel32 says no, as it does on Windows.
 */
#include <windows.h>
#include <winternl.h>

/* (the kernel32 declarations as exports, as kernel32 itself builds them) */
#undef WINBASEAPI
#define WINBASEAPI __declspec(dllexport)


WINBASEAPI BOOL WINAPI WaitOnAddress(volatile VOID *addr, PVOID cmp, SIZE_T size, DWORD ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    NTSTATUS s = RtlWaitOnAddress(addr, cmp, size, ms == INFINITE ? NULL : &t);
    if (s == (NTSTATUS)STATUS_TIMEOUT) { SetLastError(1460 /* ERROR_TIMEOUT */); return FALSE; }
    if (!NT_SUCCESS(s)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

WINBASEAPI VOID WINAPI WakeByAddressAll(PVOID addr)    { RtlWakeAddressAll(addr); }
WINBASEAPI VOID WINAPI WakeByAddressSingle(PVOID addr) { RtlWakeAddressSingle(addr); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
