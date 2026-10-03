## The MSYS2 runtime: `sh.exe`, `clone`, `push`

MinGit's shell and Unix tools (`usr\bin`: `sh.exe` is bash, `ls`, `cat`,
`wc`...) are MSYS2 programs, built on `msys-2.0.dll`, a fork of the
Cygwin runtime.  They run unmodified: `sh -c "..."` with pipes,
`$(...)`, subshells, globbing and redirection, starting Windows programs
and MSYS ones.  git starts `git-upload-pack` and `git-receive-pack`
through `sh`, so `git clone`, `fetch` and `push` between repositories on
C: work.  What the runtime needed from NovaOS:

- **Native API breadth**: object directories and symbolic links
  (`\BaseNamedObjects\...` with `RootDirectory`-relative names), timer
  objects, `NtQueryEvent`/`NtQuerySemaphore`, `NtOpenThread`,
  `NtRead/WriteVirtualMemory` of another process,
  `NtAllocateVirtualMemoryEx` and `NtMapViewOfSectionEx` with address
  requirements, `NtQueryObject` names (`\Device\HarddiskVolume1\...`,
  `\Device\NamedPipe\...`), more `NtQueryInformationFile`,
  `NtQueryDirectoryFile` and volume classes, tokens, SIDs, ACLs and
  security descriptors, LSA policy and account queries, and the
  `RtlGetCurrentDirectory_U` code shape the runtime searches for its
  `FAST_CWD` pointer.
- **Guard pages and stack growth**: `PAGE_GUARD` pages raise
  `STATUS_GUARD_PAGE_VIOLATION` once; on a thread's stack (from the TEB)
  the next page down becomes the guard and `StackLimit` follows, and new
  stack pages are read/write even when the reservation says
  `PAGE_NOACCESS`.  The runtime moves the main stack to its own area.
- **APCs at start-up**: the runtime queues its signal thread as an APC
  from a DLL's initialisation; ntdll now runs queued APCs once the loader
  is done, and alertable waits run them.
- **fork**: `STARTUPINFO.lpReserved2` reaches the child (the process
  parameters' `RuntimeData`), inheritable handles are really inheritable
  (`OBJ_INHERIT` and `bInheritHandle` on events, mutexes, semaphores,
  sections, timers and directories), so the child finds its parent's
  shared memory and rebuilds itself at the same addresses.
- **Pipes by directory handle**: the runtime opens `\Device\NamedPipe\`
  and creates its pipes relative to that handle.
- **Windows' directory listings**: every directory but a drive's root
  lists `.` and `..` first, so opening an empty directory succeeds (git
  moves pushed objects out of an empty quarantine folder).
- **Winsock tells sockets from pipes**: `WSAEnumNetworkEvents` and
  `WSAEventSelect` fail with `WSAENOTSOCK` on other handles, which is how
  git's `poll` finds out a pipe was closed.
- Smaller pieces: C runtime `swprintf` in msvcrt's legacy form (git's
  `git-*.exe` launchers), `_findfirst*`/`_findnext*`, console input queries
  failing for files, `GetConsoleWindow`, new stub DLLs (`iphlpapi`,
  `netapi32`, `secur32`, `authz`, `dnsapi`, `pdh`) and more kernel32
  (`VirtualAlloc2`, `MapViewOfFile3`, `QueryDosDeviceW`, console buffer
  and input calls...).
- Not yet: hard links (drive C: behaves like FAT, so git renames), and
  interactive `sh` sessions have not been tried; `sh -c` and scripts
  are what is tested.  *(Since done: `sh --login -i` runs interactively since
  Phase 17.2; hard links are still to come.)*
