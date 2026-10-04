/* NovaOS builds cnc-ddraw without Microsoft Detours: its "hook=2" mode
 * (patching functions' first instructions) and the DirectInput detours are
 * never used, so these say every transaction failed. */
#pragma once
#include <windows.h>
static __inline LONG DetourTransactionBegin(void) { return ERROR_NOT_SUPPORTED; }
static __inline LONG DetourTransactionCommit(void) { return ERROR_NOT_SUPPORTED; }
static __inline LONG DetourUpdateThread(HANDLE h) { (void)h; return ERROR_NOT_SUPPORTED; }
static __inline LONG DetourAttach(PVOID *p, PVOID d) { (void)p; (void)d; return ERROR_NOT_SUPPORTED; }
static __inline LONG DetourDetach(PVOID *p, PVOID d) { (void)p; (void)d; return ERROR_NOT_SUPPORTED; }
