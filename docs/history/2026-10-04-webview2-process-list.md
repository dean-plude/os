## WebView2: Edge Update sees its install running; delete on close

Microsoft Edge Update's install of the WebView2 runtime failed with
`0x80070003` because Edge Update uninstalled itself in the middle of it.
Its background update pass (`/ua`) runs alongside the install and
uninstalls Edge Update unless it finds an install worker running: it lists
processes (`EnumProcesses`), opens each one, asks for its image path with
`GetProcessImageFileName` and turns that device path into a drive path with
`QueryDosDevice`, checks the process's user and reads its command line
from its PEB (`/handoff`, `/install`).  NovaOS answered
`GetProcessImageFileName` for the calling process only, so no worker was
ever found.  Nothing in Edge Update is changed: NovaOS now answers as
Windows does.

- **Another process's image and command line**: `NtQueryInformationProcess`
  answers `ProcessImageFileName` (`\Device\HarddiskVolume1\...`),
  `ProcessImageFileNameWin32`, `ProcessWow64Information` and
  `ProcessCommandLineInformation` for any process the caller has opened,
  also for 32-bit callers.  kernel32's `GetProcessImageFileName`,
  `GetModuleFileNameEx` and `GetModuleBaseName` (of another process's
  program), `QueryFullProcessImageName` (with `PROCESS_NAME_NATIVE`, and
  `ERROR_INSUFFICIENT_BUFFER` for a short buffer) and `IsWow64Process(2)`
  are built on them.
- **Delete on close**: a file marked for deletion
  (`FILE_FLAG_DELETE_ON_CLOSE`, `FileDispositionInfo`, or `DeleteFile` of
  a file someone still has open) is now deleted when its last handle
  closes, as on Windows; until then opening it again fails with
  `ERROR_ACCESS_DENIED` (`STATUS_DELETE_PENDING`).  NovaOS used to try the
  delete at the marking handle's close and silently gave up when another
  handle or mapping still held the file, so the file stayed: the runtime
  setup's cleanup loop (open, mark, close, until the file is gone) then
  never ended.  A running program's or loaded DLL's file cannot be marked
  (`STATUS_CANNOT_DELETE`), as on Windows.
- **Tests**: the new `proclisttest` finds a running copy of itself (and,
  from the 64-bit build, the 32-bit one) by name, user and command line;
  `filetest` checks delete on close.  The WebView2 corpus test passes
  again, now in about two and a half minutes under emulation.
- **Where the install stops now**: Edge Update starts the runtime's setup
  (Chromium's `mini_installer` and `setup.exe`), which cannot map its
  archive into memory ("Can't map file to memory: Incorrect function") and
  then stops on the missing `wer.dll`
  ([compatibility.md](../compatibility.md#webview2)).
