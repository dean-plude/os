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
/* The display mode changed: WM_DISPLAYCHANGE to every program window */
void UmGuiDisplayChanged(int w, int h);

/* Find a program by name: a path, or a bare name searched in @cwd,
 * C:\Programs and C:\Windows\System32 (".exe" added if missing), then as
 * C:\Programs\NAME\NAME.exe. */
RamNode   *UmFindProgram(RamNode *cwd, const char *name);

/* Start @exe with command line @cmdline (UTF-8/ASCII), current directory
 * @cwd, standard handles on @con (NULL: output is discarded and input is
 * at end of file).  NULL with @err set on failure. */
UmProcess *UmSpawn(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con,
                   char *err, int err_cap);
/* UmSpawn without waiting (from the desktop thread, under the desktop
 * lock): the program is mapped on a worker thread.  Poll with UmSpawnPoll
 * until it returns true (then *proc is the process, or NULL with @err set;
 * the job is gone); UmSpawnAbandon gives up on it (the process is ended
 * if it started).  NULL if out of memory. */
typedef struct UmSpawnJob UmSpawnJob;
UmSpawnJob *UmSpawnStart(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con);
bool        UmSpawnPoll(UmSpawnJob *j, UmProcess **proc, char *err, int err_cap);
void        UmSpawnAbandon(UmSpawnJob *j);
/* Start a program to run on its own (UmSpawn + UmDetach) without waiting
 * for it to load; a failure is logged.  False if out of memory. */
bool        UmSpawnDetached(RamNode *exe, const char *cmdline, RamNode *cwd);
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
/* Keep a process made by CreateProcess after its creator closes its
 * handles (the Terminal window of a CREATE_NEW_CONSOLE program) */
void       UmHold(UmProcess *p);
void       UmUnhold(UmProcess *p);

UINT32      UmPid(const UmProcess *p);
const char *UmName(const UmProcess *p);

/* Process list, for tasklist: fills up to @max entries. */
typedef struct { UINT32 pid; char name[32]; UINT32 mem_kb; UINT32 threads; bool exited; } UmProcInfo;
int  UmList(UmProcInfo *out, int max);
/* taskkill: false if no running process has @pid. */
bool UmKillPid(UINT32 pid);
void UmKillConsole(UmConsole *con, UINT32 status);   /* every program on @con */

/* -----------------------------------------------------------------------
 * Consoles: one per Terminal session that runs programs
 * ----------------------------------------------------------------------- */
/* An input record, laid out as Windows' INPUT_RECORD (20 bytes) */
typedef struct {
    UINT16 type, pad;                   /* CON_KEY_EVENT, ... */
    union {
        struct { UINT32 down; UINT16 repeat, vk, scan, ch; UINT32 ctrl; } key;
        UINT32 raw[4];
    };
} UmConInput;
#define CON_KEY_EVENT     0x0001
#define CON_MOUSE_EVENT   0x0002
#define CON_WINDOW_EVENT  0x0004
#define CON_FOCUS_EVENT   0x0010
/* dwControlKeyState */
#define CON_RIGHT_ALT     0x0001
#define CON_LEFT_ALT      0x0002
#define CON_RIGHT_CTRL    0x0004
#define CON_LEFT_CTRL     0x0008
#define CON_SHIFT         0x0010
#define CON_ENHANCED_KEY  0x0100
/* Console modes (SetConsoleMode) */
#define CON_ENABLE_PROCESSED_INPUT  0x0001
#define CON_ENABLE_LINE_INPUT       0x0002
#define CON_ENABLE_ECHO_INPUT       0x0004
#define CON_ENABLE_WINDOW_INPUT     0x0008
#define CON_ENABLE_VT_INPUT         0x0200
#define CON_ENABLE_VT_PROCESSING    0x0004   /* output */
#define CON_IN_DEFAULT   0x01E7  /* processed, line, echo, insert, quick edit, extended flags, auto position */
#define CON_OUT_DEFAULT  0x0007  /* processed, wrap at end of line, VT processing */

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
/* Raw input: queue records / one key press (@ch: its character or 0) */
int        UmConsolePushInput(UmConsole *c, const UmConInput *recs, int n);
void       UmConsoleKey(UmConsole *c, UINT16 vk, UINT16 scan, UINT16 ch, UINT32 ctrl, bool down);
/* The modes the program set (CON_ENABLE_*) */
UINT32     UmConsoleInputMode(UmConsole *c);
UINT32     UmConsoleOutputMode(UmConsole *c);
/* The Terminal's size in character cells */
void       UmConsoleSetSize(UmConsole *c, int cols, int rows);
/* Windows virtual-key code for a set-1 scan code (E0-prefixed: @ext) */
UINT32     UmScancodeToVk(UINT8 sc, bool ext);

/* -----------------------------------------------------------------------
 * Hooks for the architecture code (interrupts disabled)
 * ----------------------------------------------------------------------- */
/* Only the NT services the subsystem implements are open to programs. */
bool UmSyscallAllowed(UINT64 num);
/* True for the services that run without the big kernel lock */
bool UmSyscallLockFree(UINT64 num);
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
int  UmGuardFault(UINT64 va);           /* a guard page touched: 1 handled, -1/-2 raise, 0 not one */
void um_registry_add_cpus(UINT32 n);         /* the processor keys, for the CPUs started */
void um_registry_set_dword(const char *path, const char *name, UINT32 val);   /* an installer's registration */
void UmFault(UINT32 status, UINT64 rip, UINT64 addr) __attribute__((noreturn));
void UmFaultAt(UINT32 status, UINT64 rip, UINT64 addr, UINT64 sp) __attribute__((noreturn));
/* A 32-bit program's system call (int 0x2E) keeps its registers in @frame */
void UmNoteSyscallFrame(void *frame);

/* -----------------------------------------------------------------------
 * The desktop lock: ramfs and the window system are used by the desktop
 * thread; program threads take this lock around file-system access.
 * ----------------------------------------------------------------------- */
void DesktopLock(void);
void DesktopUnlock(void);
struct Thread *DesktopLockOwner(void);     /* diagnostics */

/* Save the registry and drive C: to disk now (before a restart or shutdown). */
void UmSaveAll(void);
/* Timer tick: advances the clocks in KUSER_SHARED_DATA. */
void UmTimerTick(UINT64 ticks);
/* The number of online CPUs changed: update what programs see. */
void UmCpuCountChanged(void);
/* Log every program thread's state (serial), for diagnosing hangs. */
void UmDumpAll(void);

/* A page fault at @va in the current program: back a committed page that
 * was never touched (demand-zero).  True if the access can be retried. */
bool UmDemandFault(UINT64 va);

/* Log the failing system calls of programs named @name ("" or NULL: off) */
void UmSetTrace(const char *name);
