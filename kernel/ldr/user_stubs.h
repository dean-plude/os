/*
 * user_stubs.h — User-mode SYSCALL thunk page generator (Phase 6)
 *
 * Phase 6 replaces the kernel-mode function-pointer stubs from Phase 3 with
 * genuine user-mode x86-64 code pages.  Each "stub DLL" gets one 4 KiB page
 * mapped read+exec into every user process.  Each exported function occupies
 * a 16-byte slot containing:
 *
 *   4C 8B D1             mov r10, rcx          ; Windows ABI: arg1 → r10
 *   B8 [lo] [hi] 00 00   mov eax, <syscall_N>  ; NT or kernel-helper number
 *   0F 05                syscall
 *   C3                   ret
 *   90 90 90 90 90       nop × 5 (pad to 16 bytes)
 *
 * Non-syscall Win32 helpers (RtlAllocateHeap, GetCurrentProcessId, etc.)
 * are routed through "kernel helper" numbers 0x01F0–0x01FF, which sit at the
 * top of the SYSCALL_MAX=0x0200 dispatch table and are handled in syscall.c.
 *
 * Fixed user-mode VAs (one 4 KiB page each):
 *
 *   ntdll.dll     0x00007FFFF7D00000
 *   kernel32.dll  0x00007FFFF7C00000
 *   msvcrt.dll    0x00007FFFF7B00000
 *   user32.dll    0x00007FFFF7A00000
 */

#pragma once
#include "../include/types.h"

/* -----------------------------------------------------------------------
 * Fixed user-mode VAs for stub DLL code pages
 * ----------------------------------------------------------------------- */
#define USER_NTDLL_STUB_VA      UINT64_C(0x00007FFFF7D00000)
#define USER_KERNEL32_STUB_VA   UINT64_C(0x00007FFFF7C00000)
#define USER_MSVCRT_STUB_VA     UINT64_C(0x00007FFFF7B00000)
#define USER_USER32_STUB_VA     UINT64_C(0x00007FFFF7A00000)

/* Bytes per stub slot; max stubs per 4 KiB page */
#define USER_STUB_SLOT_SIZE   16
#define USER_STUB_MAX_SLOTS   256   /* 4096 / 16 */

/* -----------------------------------------------------------------------
 * Kernel-helper syscall numbers (0x01F0–0x01FF).
 * Handled by KiSystemCallDispatch() in syscall.c, same path as NT syscalls.
 * These extend the NT table for functions that have no real syscall number
 * but need kernel-side logic (heap, TEB access, debug output, etc.).
 * ----------------------------------------------------------------------- */
#define KH_RtlAllocateHeap          0x01F0
#define KH_RtlFreeHeap              0x01F1
#define KH_RtlReAllocateHeap        0x01F2
#define KH_GetCurrentProcessId      0x01F3
#define KH_GetCurrentThreadId       0x01F4
#define KH_GetLastError             0x01F5
#define KH_SetLastError             0x01F6
#define KH_DbgPrint                 0x01F7
#define KH_GetStdHandle             0x01F8
#define KH_RtlInitUnicodeString     0x01F9
#define KH_GetProcessHeap           0x01FA
#define KH_IsDebuggerPresent        0x01FB
#define KH_RtlZeroMemory            0x01FC
#define KH_RtlMoveMemory            0x01FD
#define KH_GetCurrentProcess        0x01FE  /* always returns (HANDLE)-1 */
#define KH_GetCurrentThread         0x01FF  /* always returns (HANDLE)-2 */

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/*
 * Allocate one physical page per stub DLL and emit SYSCALL thunk machine
 * code into each.  Must be called once at boot after PMM is initialised.
 */
void LdrInitUserStubs(void);

/*
 * Map all stub DLL pages (read+exec, no write) into a process page table.
 * @pt_phys: physical address of the process PML4.
 * Called during process creation before the first thread runs.
 */
void LdrMapUserStubPages(uintptr_t pt_phys);

/*
 * Return the user-mode VA of the named export in the named stub DLL.
 * Returns 0 if the function or DLL is not found.
 */
UINT64 LdrGetStubVA(const char *dll_name, const char *func_name);

/*
 * Return the user-mode VA of the NtYieldExecution stub (harmless no-op
 * fallback for unknown imports).
 */
UINT64 LdrGetFallbackStubVA(void);
