## Pipes and cmd.exe

- **Pipes in the kernel** (`kernel/um/um_pipe.c`): named pipes
  (`NtCreateNamedPipeFile`, `\\.\pipe\NAME` opened with `CreateFile`) and
  anonymous ones (`CreatePipe` makes a named pipe with a private name, as
  Windows does).  Each direction is a ring buffer; byte and message pipes
  (a reader in message mode gets one message per read, the rest with
  `ERROR_MORE_DATA`), `ConnectNamedPipe`, `DisconnectNamedPipe`,
  `WaitNamedPipe`, `PeekNamedPipe`, `TransactNamedPipe`/`CallNamedPipe`,
  `GetNamedPipeInfo`, `Set/GetNamedPipeHandleState` (message read mode,
  `PIPE_NOWAIT`) and instance limits.  The ends are kernel objects, so
  closing the last handle to one is what the other side sees: a reader
  gets what is left and then `ERROR_BROKEN_PIPE` (end of file), a writer
  `ERROR_NO_DATA`.
- **Overlapped I/O that really waits**: on a handle opened with
  `FILE_FLAG_OVERLAPPED` a read, write or `ConnectNamedPipe` that cannot
  finish stays pending (`ERROR_IO_PENDING`); whoever changes the pipe later
  finishes it, writing the data and the status into the waiting program
  and setting its event.  Completion ports get their packet and
  `ReadFileEx`/`WriteFileEx` routines run at the next alertable wait
  (through a kernel32 helper thread), `CancelIo`/`CancelIoEx` end requests
  with `ERROR_OPERATION_ABORTED`, and `GetOverlappedResult(Ex)` waits for
  them.  A 32-bit program's I/O status block goes to the kernel as its own
  (32-bit layout) so it can be finished later.
- **Handles for child processes**: handles carry an inherit flag
  (`SECURITY_ATTRIBUTES.bInheritHandle`, `SetHandleInformation`,
  `DuplicateHandle(..., TRUE, ...)`), and `CreateProcess(..., TRUE, ...)`
  gives the child every inheritable handle at the same value; the
  standard handles come from `STARTUPINFO` or, without
  `STARTF_USESTDHANDLES`, from the parent's own (so a program's children
  write where it writes).  `DuplicateHandle` works into and out of another
  process.  `DETACHED_PROCESS`/`CREATE_NO_WINDOW` start without a console.
- **The environment** is one sorted table in kernel32 that `SetEnvironmentVariable`,
  `GetEnvironmentStrings(A/W)` and `CreateProcess` all see; a child gets
  its parent's environment (or `lpEnvironment`, ANSI or Unicode) instead
  of a fixed default.  `ComSpec` names cmd.exe.
- **`NUL`** is the null device (`>nul`, `CreateFile("nul")` in any folder).
- **C runtime**: `system()`/`_wsystem()` run `cmd.exe /c`, `_popen`/
  `_pclose`/`_wpopen` read or write a command through a pipe, `_pipe`
  makes one, and reading a pipe whose writer is gone is end of file.
- **cmd.exe** (`userland/programs/cmd.c`, in System32 and SysWOW64): the
  command interpreter, interactive or `/c`/`/k`.  Lines are expanded
  (`%VAR%`, `%VAR:~1,2%`, `%VAR:a=b%`, `%ERRORLEVEL%`, `%CD%`, `%DATE%`,
  `%RANDOM%`; `%0`-`%9`, `%*` and `%~dpnxfatz0` in batch files; `!VAR!`
  with delayed expansion), then parsed: `&`, `&&`, `||`, `|`, `( )`
  blocks, `^` escapes and redirections (`<`, `>`, `>>`, `2>`, `2>&1`,
  `>nul`).  IF (`==`, `/i`, `not`, `EQU`...`GEQ`, `errorlevel`, `exist`,
  `defined`) with ELSE; FOR, `/d`, `/r`, `/l` and `/f` over files,
  strings and command output (`tokens=`, `delims=`, `skip=`, `eol=`,
  `usebackq`).  Batch files: labels, GOTO, CALL (files and `:labels`),
  SHIFT, SETLOCAL/ENDLOCAL (with `enabledelayedexpansion`), `exit /b`,
  ECHO ON/OFF and `@`.  Internal commands: echo, set (`/a` arithmetic,
  `/p` input), cd, dir (`/b`, `/s`, `/a`), type, copy, del, md, rd `/s`,
  ren, move, pushd/popd, path, prompt, start, pause, title, ver and more.
  Programs run with the redirected handles; each side of a pipe that is
  not a program runs in a child cmd.exe, as on Windows.  Also new in
  System32: `find`, `findstr` (its regular expressions), `sort`, `more`
  and `timeout`.
- **The Terminal hands lines to cmd.exe** when they use pipes,
  redirections or `&&`/`||`, and when they name a `.bat`/`.cmd` file;
  `cmd` starts it interactively.  Ctrl+C now stops every program on the
  console, not just the first.
- Tests: `pipetest.exe` (62 checks, 64-bit and 32-bit): anonymous pipes
  and end of file, a 300 KB write through a 4 KB pipe, children reading
  and writing redirected pipes, inherited handles by value, the
  environment, `cmd /c`, a pipeline into a program, `_popen`, `system`,
  `_pipe`, NUL, named pipes in byte and message mode, overlapped connect
  and reads with events, cancelling, a completion port and a completion
  routine.  `cmdtest.bat` (29 checks) covers expansion, SET /A, IF, the
  FOR forms, CALL, delayed expansion, pipes into `find` and `sort`,
  redirections and error levels.
- Not yet: `CREATE_SUSPENDED` is ignored and `CREATE_NEW_CONSOLE` shares
  the console; a file handed to a child has its own position (cmd.exe
  opens redirection targets for appending so output lands in order).
  *(Since done: `CREATE_SUSPENDED`, `CREATE_NEW_CONSOLE` windows and shared
  file positions came with "Phase 17: kernel and API correctness".)*
