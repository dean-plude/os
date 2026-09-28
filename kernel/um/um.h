/*
 * um.h — NovaOS user-mode subsystem: Windows programs in ring 3
 *
 * Runs unmodified PE32+ executables from drive C: in their own address
 * spaces.  The system DLLs they import (ntdll, kernel32, msvcrt — built
 * from userland/ and installed in C:\Windows\System32) are loaded by the
 * kernel loader like any other DLL; they reach the kernel through NT
 * system calls (um_syscall.c) with the real NT signatures.
 *
 * A process has threads, a private page table (upper half shared with the
 * kernel), a handle table (files on C:, console, synchronization objects,
 * threads, sockets, windows) and a console that carries its standard
 * input/output to a Terminal window.
 *
 * Lifetime: the spawner holds a reference until UmRelease(); the process
 * struct is freed once it has exited, its thread is off the CPU, and it is
 * released.  UmPoll() (desktop thread) does the reclaiming.
 */

#pragma once

#include "../include/types.h"
#include "../fs/ramfs.h"

typedef struct UmProcess UmProcess;
typedef struct UmConsole UmConsole;

/* CPU features for user code (SSE) and the NT system services. */
void UmInit(void);
/* Reclaim exited processes (call regularly from the desktop loop). */
void UmPoll(void);

/* Find a program by name: a path, or a bare name searched in @cwd,
 * C:\Programs and C:\Windows\System32 (".exe" added if missing), then as
 * C:\Programs\NAME\NAME.exe. */
RamNode   *UmFindProgram(RamNode *cwd, const char *name);

/* Start @exe with command line @cmdline (UTF-8/ASCII), current directory
 * @cwd, standard handles on @con (NULL: output is discarded and input is
 * at end of file).  NULL with @err set on failure. */
UmProcess *UmSpawn(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con,
                   char *err, int err_cap);
/* Ask the process to end with @status (it stops at its next kernel exit). */
void       UmKill(UmProcess *p, UINT32 status);
/* True once the process has exited; *status gets its exit code and
 * @why (may be NULL) a description for abnormal ends (crash, killed). */
bool       UmHasExited(UmProcess *p, UINT32 *status, char *why, int why_cap);
/* The spawner is done with the process (kill it first if still running). */
void       UmRelease(UmProcess *p);
/* Let the process run on its own (started from the desktop, no console):
 * it is reclaimed when it exits. */
void       UmDetach(UmProcess *p);

UINT32      UmPid(const UmProcess *p);
const char *UmName(const UmProcess *p);

/* Process list, for tasklist: fills up to @max entries. */
typedef struct { UINT32 pid; char name[32]; UINT32 mem_kb; UINT32 threads; bool exited; } UmProcInfo;
int  UmList(UmProcInfo *out, int max);
/* taskkill: false if no running process has @pid. */
bool UmKillPid(UINT32 pid);

/* -----------------------------------------------------------------------
 * Consoles: one per Terminal session that runs programs
 * ----------------------------------------------------------------------- */
UmConsole *UmConsoleNew(void);
void       UmConsoleRelease(UmConsole *c);
/* Drain program output (Terminal side).  Returns bytes copied. */
int        UmConsoleRead(UmConsole *c, char *buf, int cap);
/* Queue keyboard input for the program (a line including "\r\n"). */
void       UmConsoleWrite(UmConsole *c, const char *data, int len);
/* End of input (Ctrl+Z): the program's next read returns 0 bytes. */
void       UmConsoleEof(UmConsole *c);
/* A program is waiting for keyboard input right now. */
bool       UmConsoleWantsInput(UmConsole *c);

/* -----------------------------------------------------------------------
 * Hooks for the architecture code (interrupts disabled)
 * ----------------------------------------------------------------------- */
/* Only the NT services the subsystem implements are open to programs. */
bool UmSyscallAllowed(UINT64 num);
/* Run service @num for the current program (interrupts enabled). */
UINT64 UmSyscall(UINT64 num, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
/* On every return to user mode: ends the thread if its process was killed. */
void UmReturnToUser(void);
/* The same from an interrupt taken in user mode (@frame: InterruptFrame) */
void UmReturnToUserFrame(void *frame);
/* A CPU exception in user mode (@frame: the InterruptFrame): passed on to
 * the program's exception handlers (SEH) through its stack. */
void UmUserException(void *frame, UINT64 cr2);
/* The current user thread raised an exception it cannot handle. */
void UmFault(UINT32 status, UINT64 rip, UINT64 addr) __attribute__((noreturn));

/* -----------------------------------------------------------------------
 * The desktop lock: ramfs and the window system are used by the desktop
 * thread; program threads take this lock around file-system access.
 * ----------------------------------------------------------------------- */
void DesktopLock(void);
void DesktopUnlock(void);

/* Save the registry and drive C: to disk now (before a restart or shutdown). */
void UmSaveAll(void);
