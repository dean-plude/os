## Phase 13 — 32-bit Windows programs (WoW64)

NovaOS runs 32-bit (x86, PE32) Windows programs next to 64-bit ones, the
way 64-bit Windows does: the CPU runs them in compatibility mode under
the 64-bit kernel, and they get a 32-bit copy of the whole userland in
`C:\Windows\SysWOW64`.

- **Kernel** (`kernel/um/`): a 32-bit user code segment (0x38) and a flat
  4 GiB data segment; FS based at each thread's 32-bit TEB (switched with
  the thread); a 32-bit address layout, everything below 2 GiB (PEB, TEBs,
  loader list, stacks, heap and DLLs); the PE32 loader (4-byte import
  thunks, HIGHLOW relocations); x86 PEB, TEB and process parameters; DLLs
  looked up in SysWOW64, and file system redirection (a 32-bit program's
  `C:\Windows\System32` is `SysWOW64`, unless it calls
  `Wow64DisableWow64FsRedirection`).  32-bit code enters the kernel
  through `int 0x2E` with a block of 64-bit arguments.  A crash at a bad
  address now also names the caller in the log.
- **The 32-bit ntdll** (`ntdll_wow.c`) turns every system call's
  arguments into the kernel's 64-bit forms: handles sign-extended,
  pointers zero-extended, `OBJECT_ATTRIBUTES`, `UNICODE_STRING`,
  `IO_STATUS_BLOCK`, `CONTEXT`, `EXCEPTION_RECORD`, memory and
  thread/process information, handle arrays and window messages
  rebuilt in their 64-bit layout and the results copied back.
- **x86 exceptions** (`exc_x86.h`): frames chained from `fs:[0]`,
  `RtlUnwind`, `RtlRaiseException`, and the `__try` handlers of MSVC and
  clang (`_except_handler3`, `_except_handler4_common`); vcruntime140
  has x86 C++ exceptions (`__CxxFrameHandler3`, `_CxxThrowException`)
  and RTTI with absolute addresses; msvcrt has x86 `setjmp`/`longjmp`.
- **Build**: `tools/build_userland.py` builds the userland twice, the
  second time with `--target=i686-pc-windows-msvc`.  Stdcall functions are
  exported undecorated (`GetLastError`, not `_GetLastError@0`) through a
  generated `.def`, which also caught every mismatched calling convention
  between our DLLs at link time.  The 64-bit division helpers x86 code
  calls are in `lib/x86rt.c`.  32-bit builds of the test programs are in
  `C:\Programs\x86`; `NOVA_NO_WOW64=1` leaves the 32-bit pass out.
- **Programs see WoW64**: `IsWow64Process` is TRUE, `GetSystemInfo`
  reports an x86 machine and `GetNativeSystemInfo` the AMD64 one,
  `GetSystemDirectory` is `SysWOW64`.
- **Tested**: every self-test (crttest, filetest, threads/SEH, DLL/TLS,
  posixtest, apitest, comtest, C++ exceptions, shared memory) passes as a
  32-bit program as well as a 64-bit one; 32-bit GUI programs (winhello,
  guitest); programs from the 32-bit MinGW toolchain (C, and C++ with
  exceptions); 7-Zip's own 32-bit self-extractors, console and GUI,
  unpacking an archive; and a real NSIS (Modern UI) installer going
  through its welcome, folder, progress and finish pages, installing
  files, its uninstaller and registry keys, and uninstalling again.
- Pointer-size assumptions fixed on the way: TEB offsets in kernel32 and
  ws2_32, PE data directories in `GetProcAddress` and resources,
  `Get/SetWindowLongPtr` and the `DWLP_*` offsets, rename information,
  SRW locks, `%p`/`%z`/`%I` in printf and scanf, `FILE` (32 bytes on x86).
- Also fixed on the way (for 64-bit programs too): `EndDialog` called
  from a message another thread sent now ends the modal loop (NSIS's
  finish page); `MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT)` records
  the operation in `PendingFileRenameOperations` instead of acting at
  once (NSIS uninstallers copy themselves to Temp and schedule that copy
  for deletion; it used to vanish before it could run); `shfolder.dll`
  exists (`SHGetFolderPath`, which NSIS takes from it); msvcrt exports
  `_controlfp`, `_control87`, `__p___initenv` and friends.  The
  Terminal's `trace` now shows the file name of file system calls.
- Not yet: pending renames are not carried out at the next start.
