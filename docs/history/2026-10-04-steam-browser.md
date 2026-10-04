## Steam's browser starts its child processes

Steam's browser, `steamwebhelper.exe` (Chromium 126 through CEF), started
on NovaOS but never started its GPU and page processes, so Steam's login
window never came up.  Chromium now starts its GPU process, its network
service and its storage service the way it does on Windows.  What it
needed, each a NovaOS gap:

- **Exports by number.**  Chromium imports some functions by ordinal.
  shlwapi's `IsOS` (437) was missing, so a delay-load failed and Chromium's
  delay-load hook stopped the browser at a breakpoint.  Now exported under
  Windows' numbers: shlwapi `IsOS` and `QISearch`, oleaut32
  `VarUI4FromStr`, `VarBstrCat` and `VarBstrCmp`, and uxtheme
  `DrawThemeBackgroundEx` (47, which NovaOS had given to another
  function).  shell32's `SHChangeNotifyRegister` and
  `SHChangeNotifyDeregister` had ordinals but no code.
  `tools/pe_imports.py` now checks imports by ordinal too, and
  `GetProcAddress` logs a missing ordinal as `#N`.
- **The power API set.**  `api-ms-win-power-*` names `powrprof.dll` for
  programs that load it at run time, as it already did for the loader.
- **Read-only shared memory.**  Chromium hands its children read-only
  shared memory and checks first that the handle cannot be widened:
  duplicating it for `FILE_MAP_WRITE` must fail.  NovaOS now records the
  rights each handle was opened with, and a duplicate that asks for more
  is checked against the object's security descriptor, as on Windows.
  `CreateFileMapping` keeps the descriptor the caller passes (Chromium's
  sections are unnamed, with an empty DACL), `OpenFileMapping` opens with
  the rights asked for, and `NtQuerySection` answers for sections that
  are not images.
- **Handle lists.**  `CreateProcess` with `STARTUPINFOEX` and
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` passes only the listed inheritable
  handles to the child, the way Chromium hands each child its pipe and
  shared memory.  The attribute list functions keep real attribute lists.
- **The rest of `libcef.dll`'s imports.**  With GOG GALAXY's imports
  (already on main), these were left: kernel32 `GetFirmwareType` (UEFI),
  `GetConsoleDisplayMode` and `Wow64GetThreadContext`, userenv's Group
  Policy notifications, and wintrust `CryptCATCatalogInfoFromContext`.
- **Windows' version, read from files.**  Chromium reads the version of
  `kernelbase.dll` (or `kernel32.dll`) with `GetFileVersionInfo` and stops
  when neither has one.  Both now carry a version resource (Windows 10
  build 18362, as `ntdll.dll` does), and `GetFileVersionInfo` finds a bare
  DLL name along the search path, as Windows does.
- **The event log.**  New `wevtapi.dll`: `EvtQuery`, `EvtNext`,
  `EvtCreateRenderContext`, `EvtRender` and `EvtClose`.  NovaOS keeps no
  event channels, so a query finds no events, as for an empty log;
  Chromium asks the System log how the last shutdown went.
- **A machine-wide freeze.**  Chromium's stack sampler suspends threads.
  A suspended thread waited for its resume with interrupts off, so its
  CPU never answered another CPU's TLB shootdown for the same process,
  and that CPU waited forever holding the process lock; the whole machine
  stopped.  A suspended thread now waits with interrupts on.

The new self-test `chrometest` checks each of these on 64- and 32-bit.
How far Steam's browser gets now is in
[compatibility.md](../compatibility.md#steam).
