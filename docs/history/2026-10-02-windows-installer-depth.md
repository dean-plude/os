## Windows Installer depth: custom actions, dialogs, shortcuts, services

Roadmap step 20.5.  Before writing anything we looked for a permissive
(MIT, BSD, zlib) Windows Installer to reuse; there is none (Wine's is
LGPL), so the engine stays NovaOS's own, with `stb_image` (public domain
or MIT) for the dialogs' pictures.

- **Custom actions that run code.**  DLL actions (types 1 and 17) run out
  of process, as on Windows: `msiexec /novaca` is a custom-action server
  that loads the DLL and calls its entry point, and a 32-bit DLL (WiX's
  `WixCA` usually is, even in x64 packages) runs in `SysWOW64`'s
  `msiexec`.  Its `MsiGetProperty`, `MsiDatabaseOpenView`,
  `MsiProcessMessage`... calls cross a pipe back to the installation.
  EXE actions (2, 18, 34, 50) start the program from the Binary table, an
  installed file, a folder or a property; type 19 shows its error; 35 and
  51 set folders and properties.  The flags are honoured: continue on
  error, asynchronous, run once, deferred with `CustomActionData`, commit
  actions after `InstallFinalize`; rollback actions are kept unused (there
  is no rollback yet).  Script actions (VBScript, JScript) and nested
  installs are logged and skipped.
- **The MSIHANDLE API** (`userland/msi/api.c`), exported at Windows'
  ordinals: records, views with an SQL engine (`sql.c`: `SELECT ... WHERE
  ... ORDER BY`, joins, `INSERT`/`UPDATE`/`DELETE`, `CREATE TABLE` and
  temporary rows through `MsiViewModify`), properties, formatted strings
  (`[Prop]`, `[#File]`, `[!File]`, `[$Comp]`, `[%ENV]`, `[1]`, `{...}`
  groups), feature and component states, target and source paths,
  `MsiDoAction`, `MsiSequence`, `MsiEvaluateCondition`, the summary
  information stream, `MsiGetMode` and `MsiSetMode`.
- **The packages' own dialogs.**  At full UI a package with an
  `InstallUISequence` shows its own wizard (`dialog.c`): the Dialog,
  Control, ControlEvent, ControlCondition, EventMapping, TextStyle,
  RadioButton, CheckBox, ListBox and ComboBox tables; text, bitmaps (BMP,
  PNG, JPEG), icons, lines and group boxes painted in place; push buttons,
  check boxes, edit and path fields, radio groups, the licence's RTF,
  progress bars, the feature tree (with check boxes), folder lists and
  combos, volume lists, list and combo boxes as real controls.  Events:
  property changes, `NewDialog`, `SpawnDialog`, `EndDialog`, `DoAction`,
  `SetTargetPath`, `Reset`, `AddLocal`, `Remove`, `SetInstallLevel`,
  `SelectionBrowse` and the folder-list events; the progress dialog
  follows `ActionText`, `ActionData` and `SetProgress`; Cancel asks first.
  The success, cancel and failure dialogs (-1, -2, -3) end it.  Packages
  without dialogs keep the progress window.
- **Shortcuts**: the Shortcut table (`IShellLink`, with arguments,
  working folder, description, show command and icons from the Icon
  table, saved under `C:\Windows\Installer\{ProductCode}`), into the
  Start menu (`C:\AppData\Roaming\Start Menu\Programs`) and the
  desktop; `msiexec /x` removes them.  shell32 gains the
  InternetShortcut class (`.url` files with `IUniformResourceLocator`,
  `IPersistFile` and its property set) that WiX's internet shortcuts use.
- **Services**: `ServiceInstall` and `ServiceControl` (install, start,
  stop, delete, on install and on uninstall), on a **service control
  manager** in `advapi32` (`service.c`).  Services live under
  `HKLM\SYSTEM\CurrentControlSet\Services` as on Windows;
  `StartService` runs the image, whose `StartServiceCtrlDispatcher` runs
  `ServiceMain` on a thread and takes controls on a pipe; `ControlService`,
  `QueryServiceStatus(Ex)`, `QueryServiceConfig(2)`, `ChangeServiceConfig(2)`,
  `EnumServicesStatus(Ex)`, `DeleteService` and the A forms work.
  `tools/msitest/make_service_package.sh` builds a package around a test
  service (`svc.c`).  Services marked automatic do not start at boot yet.
- **AppSearch** follows DrLocator (with Parent chains and Depth) and the
  Signature table (file names, minimum and maximum versions and sizes),
  besides RegLocator, whose directory and file types now check the
  signature too.
- **Smaller pieces**: `CostingComplete`, empty files missing from the
  cabinet are created, `MsiEnumRelatedProducts`, the features installed
  are remembered so maintenance runs and removals know them, and
  `ADDLOCAL`/`REMOVE` follow the dialogs' feature choices.  New:
  `activeds.dll` (ADSI; binding fails, as on a machine in no domain),
  which WiX's util custom actions import.  `advapi32` names all the usual
  well-known SIDs (`LOCAL SERVICE`, `Guests`, `Performance Log Users`...)
  and clears the last error on success where callers look at it.
- **FAT**: creating a file whose 8.3 alias was taken walked the folder
  once per `~N` tried, so a folder of a thousand similar names (CMake's
  documentation) froze the desktop while drive C: was saved.  One walk
  now marks the taken numbers.
- **Merge modules** are merged into a package's own tables when it is
  built (the `Module*` tables only record what came from where), so the
  engine installs them like any other component; none of the packages
  tested here carries one, so that is untested.
- **LZX on real packages**: 7-Zip's MSI (LZX:21) extracts byte for byte
  as `cabextract` does, and Node.js, CMake, Temurin and KeePassXC
  install.
- **`taskkill.exe`** (`/IM`, `/PID`, `/F`), which WiX's quiet-exec
  actions run to close a program before it is replaced.
- **C runtime**: MinGW programs lock a stream themselves, entering the
  critical section Microsoft's CRT keeps after each `FILE` (`_FILEX`) and
  setting a flag in `_flag` for the standard three.  msvcrt now allocates
  streams that way and keeps its flags where `_flag` sits, so a MinGW
  `fprintf` to a file no longer hangs (the test service's `ServiceMain`
  did).
- Tested in QEMU: 7-Zip and CMake through their own wizards (welcome,
  licence, options, folder, ready, progress, finish; CMake's
  `ValidatePath` and `DetectNsisOverwrite` DLL actions run from its
  dialogs); 7-Zip's two Start menu shortcuts; with `/qn`, CMake
  (`cmake --version`), Node.js (64-bit and 32-bit WiX custom actions,
  `node -e`, Start menu and internet shortcuts) and Temurin (the
  `WixRemoveFoldersEx` action, `java -version`) installed, ran and were
  removed with `/x`; KeePassXC (32-bit `WixQuietExec` running
  `taskkill`) installed and was removed, though it needs `MSVCP140.dll`
  (the Visual C++ runtime) to start; the test service installed,
  started, wrote its state, stopped and was deleted.  PowerShell 7 stops
  at its own launch condition: it wants Windows' `pwrshplugin.dll`
  (WinRM remoting), which NovaOS lacks.
- Not yet: rollback, script custom actions, nested installs, patches
  (`.msp`) and transforms (`.mst`), advertised features, services at boot.
