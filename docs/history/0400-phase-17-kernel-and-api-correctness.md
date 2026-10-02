## Phase 17: kernel and API correctness

- **Every thread has a real `ETHREAD`** (17.1): `PsGetCurrentThread` used
  to cast the running scheduler `Thread` to an `ETHREAD`, which is only
  true for threads made by `PsCreateSystemThread`.  Kernel threads from
  `sched_create_thread`, Windows programs' threads and the idle threads
  are bare `Thread`s, so writes through it (`PsInitialize` naming the boot
  thread TID 4, `PsTerminateSystemThread`) landed past the end of the
  structure.  `Thread.ethread` now leads to the thread's `ETHREAD`; a bare
  thread gets one (a real Thread object in the System process) the first
  time it asks, released when the scheduler frees it.  With interrupts
  off the lookup returns NULL rather than touch the heap.  The boot-time
  `[PSTEST]` self-test (`kernel/ps/ps_test.c`) checks it from plain
  kernel threads, including `NtCurrentThread()`,
  `PsLookupThreadByThreadId` and `PsTerminateSystemThread`.
- **Waitable console input and `WriteConsoleInputW`** (17.2): console
  input is a kernel queue of `INPUT_RECORD`s (keys, window size changes,
  what programs write into it) with a waitable object, so console input
  handles work with `WaitForSingleObject`, `RegisterWaitForSingleObject`
  and libuv.  `ReadConsoleInput`, `PeekConsoleInput`, `WriteConsoleInput`,
  `GetNumberOfConsoleInputEvents`, `FlushConsoleInputBuffer` and the
  console modes go through one NovaOS service, `NtNovaConsole`.  A program
  that turns off line input gets its keys as records, or as xterm
  sequences with `ENABLE_VIRTUAL_TERMINAL_INPUT`.
- **Full-screen programs in the Terminal**: while a program is on the
  alternate screen or reads raw input, its output goes to a libvterm 0.3.3
  (MIT) screen grid that the Terminal paints in colour, and libvterm's
  answers to terminal queries come back through console input.
- **What Neovim needed besides**: completion packets for overlapped pipe
  requests now go to the port the request started with, so closing a
  handle still delivers the cancellation libuv waits for (Neovim, and any
  `nvim -l` script, used to hang on exit); a `RegisterWaitForSingleObject`
  callback may unregister itself; `CreatePseudoConsole` and friends exist
  (and fail), which tells Neovim's `--embed` server to keep its RPC on the
  pipes and use `CONIN$`/`CONOUT$` for the terminal; the CRT reuses closed
  descriptors 0-2 first and moves the standard handles with them;
  `RtlRunOnceExecuteOnce`, `RtlUTF8ToUnicodeN`, `RtlUnicodeToUTF8N`, and
  UCRT `_o_` imports resolve to the plain functions.
- **What MSYS2 `sh` needed**: `\Device\Null` opens as the null device and
  counts as existing (Cygwin asks `NtOpenSymbolicLinkObject`); an empty
  name relative to a file handle reopens that file; a line feed on the
  screen grid also returns the carriage unless the program set
  `DISABLE_NEWLINE_AUTO_RETURN`.
- **Process creation flags** (17.3): `CREATE_SUSPENDED` starts the first
  thread suspended until `ResumeThread` (the same change as the Firefox
  work's).  `CREATE_NEW_CONSOLE` gives a console program a console of its
  own in a Terminal window titled with its path; the window closes when
  the program ends, and closing it ends the programs on that console.
  GUI programs ignore the flag, as on Windows, and `cmd`'s `start` uses it
  unless given `/B`.  `GetConsoleProcessList` now lists the processes on
  the caller's console.  A file's position belongs to the open file, not
  the handle: duplicates, inherited handles and handles passed as a
  child's standard output share it, so a parent and child writing to one
  log file follow each other instead of overwriting.  `proctest` covers
  all three in the core suite.
- **Security on objects** (17.4): tokens are kernel objects.  Each process
  has a primary token, inherited from the process that started it (the
  desktop user's otherwise: a standard user in Users, with Administrators
  only for denying since nothing is elevated), and a thread can
  impersonate an impersonation token.  `NtOpenProcessToken(Ex)`,
  `NtOpenThreadToken(Ex)`, `NtDuplicateToken`, `NtFilterToken`,
  `NtQueryInformationToken`, `NtImpersonateAnonymousToken`,
  `NtAccessCheck` and `NtQuery/SetSecurityObject` moved from ntdll into
  the kernel at their Windows 10 1903 numbers, and
  `NtSetInformationThread(ThreadImpersonationToken)` sets or ends
  impersonation.  `NtFilterToken` (advapi32's `CreateRestrictedToken`)
  makes SIDs deny-only, removes privileges and adds restricting SIDs, and
  can make the token write-restricted.  A named event, mutex, semaphore,
  timer, section, directory or symbolic link keeps the security descriptor
  it was created with (`SECURITY_ATTRIBUTES`), and opening it, or creating
  an existing name, checks the access asked for against it as the calling
  thread: the DACL in order, deny-only groups only in deny ACEs, the
  owner's implicit `READ_CONTROL | WRITE_DAC`, and for a restricted token a
  second pass with its restricting SIDs.  kernel32's `OpenEvent`,
  `OpenMutex` and `OpenSemaphore` pass the access asked for, not all.
  `GetKernelObjectSecurity`, `SetKernelObjectSecurity`, `GetSecurityInfo`
  and `SetSecurityInfo` read and change the descriptor; `CheckTokenMembership`,
  `ImpersonateSelf`, `RevertToSelf`, `SetThreadToken` and
  `ImpersonateLoggedOnUser` work on real tokens.  Files keep no
  descriptor yet: the file system takes that part in Phase 18 (18.5), on
  the same check (`um_access_check_sd`).  `sectest` and `acltest` (also
  32-bit) show a restricted token refused a protected event.
- **Registry change events and pending renames** (17.5):
  `RegNotifyChangeKeyValue` works, on the new `NtNotifyChangeKey`: a watch
  on a key (optionally with its subkeys) signals its event once when a
  value is set or deleted, a subkey is added, deleted or renamed, or the
  key itself is deleted, as its filter asks; without `async` the call
  waits for that.  `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)` writes full
  `\??\` paths to Session Manager's `PendingFileRenameOperations` (with
  `!` for `MOVEFILE_REPLACE_EXISTING`), and the kernel now carries the
  list out at boot, after loading the registry and before any program
  runs, then deletes it.  A running program's files (its `.exe` and DLLs)
  are now held while it runs, as on Windows, so they cannot be deleted or
  replaced until it ends (renaming them still works).  The core suite
  runs an installer that has to replace a running program, restarts
  (`tools/novarun.py` and `tools/selftest.py` can restart NovaOS) and
  checks the replacement happened.  Hard links are still to come.
- **Small visible bugs** (17.6): File Explorer's This PC is now a list of
  the drives (C: and each mounted volume, D:, E:, ...) with their free
  space and size, and opening `C:\` or the desktop's This PC shows it; the
  up button and Backspace go from a drive's root back to This PC.  The
  Terminal's `dir` and cmd's `dir` name the drive they list and give that
  drive's own free space (cmd always asked C: before), and
  `GetDiskFreeSpaceEx` asks each volume (`FileFsFullSizeInformation`),
  C: included, whose free space is the free memory it lives in.
  Notepad++'s tab bar and status bar drew black: it double-buffers them by
  sending `WM_PRINT` into a memory DC, which `DefWindowProc` ignored.
  `WM_PRINT` now erases, sends `WM_PRINTCLIENT` and prints the children;
  the status bar, tab control and progress bar draw on `WM_PRINTCLIENT`;
  and the tab control lets its parent draw `TCS_OWNERDRAWFIXED` tabs
  (`WM_DRAWITEM`), sizes tabs from their text, icon and `TCM_SETPADDING`
  (`TCM_SETITEMSIZE`'s width only with `TCS_FIXEDWIDTH`) and takes
  `TCM_SETMINTABWIDTH`.  The nightly app corpus now boots with an empty
  NTFS drive D: and keeps screenshots of `dir C:\` and `dir D:\` (each
  with its own free space), This PC and Notepad++ (the last two compared
  with references in `tests/reference/`).
- **Off the big kernel lock** (17.7): files, the registry, the console and
  starting processes and threads now run beside each other on every CPU.
  The file system has a reader/writer lock: opening a file that is there,
  reading, writing, seeking and closing take it shared (each open file's
  contents under a lock of its own), and only changes to the tree take it
  alone.  The desktop thread's loop no longer holds it every tick, only
  around input, drawing and built-in windows' timers that may use files.
  The registry has a reader/writer lock of its own, a lock per key and
  value names on the stack.  A process's lock is a reader/writer lock too,
  and handles are opened, looked up and closed under it shared, each slot
  with its own lock; console output takes turns on the console's lock.
  Programs' memory allocation got arenas picked by thread (one heap lock
  was 18% of a 4-thread registry run) and stopped losing every freed small
  block (a tag bug kept them from being reused); kmalloc keeps a few
  objects per CPU; copies to and from programs move 8 bytes at a time and
  msvcrt's `memcmp` compares 8 at a time.  On one CPU in QEMU, files went
  from 1,800 to 18,000 operations a second and the registry from 5,000 to
  70,000; four CPUs do about 3x that (`smpstress scaling 3`; computing
  alone scales 3.7–4x on the same host).  smpstress also starts copies of
  itself from several threads at once, and the nightly run boots it on 4
  CPUs.  The Terminal's new `profile` command samples where the CPUs spend
  their time.
- Tested in QEMU: Neovim 0.10.4 and 0.11.4 open `t.txt`, take `ihello
  world<Esc>:wq` and exit with code 0 leaving the file written; MinGit's
  `sh --login -i` shows its coloured prompt and runs `ls`, pipes,
  `$(...)` and redirections to `/dev/null`; the core self-tests pass.
