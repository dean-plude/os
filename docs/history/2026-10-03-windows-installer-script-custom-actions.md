## Windows Installer script custom actions (JScript and VBScript)

The last gap [Windows Installer rollback, transforms, patches](#windows-installer-rollback-transforms-patches-services-at-boot)
left: custom actions written in JScript or VBScript.  `msi.dll` used to
log and skip them; it now runs them on a new DLL, `msiscript.dll`
(`userland/msiscript`), in the installing process as Windows does.

- **The engine.**  JScript runs on [mujs](https://mujs.com/) 1.3.7
  (`third_party/mujs`, ISC licence), an ES5 interpreter in one C file.
  One change to it (`third_party/mujs/NOVA-VENDOR.txt`): an assignment
  to a method call, `Session.Property("X") = "1"`, compiles to a call of
  the property's setter, because Microsoft's JScript accepts that form for
  COM properties and installers use it everywhere.  There is no
  permissively licensed VBScript engine, so VBScript is translated to
  JavaScript (`userland/msiscript/vbscript.js`) and runs on the same
  engine: names are matched without regard to case, operators follow
  VBScript's variant rules (`"2" + "3"` is `"23"`, `2 + "3"` is 5, `Not`,
  `And` and `Or` are bitwise on numbers), each statement keeps its source
  line so errors name the script's line, and `vbslib.js` provides about
  seventy built-in functions and sixty constants (`Left`, `Mid`, `InStr`,
  `Replace`, `Split`, `Join`, `UBound`, `CInt`, `Hex`, `TypeName`,
  `vbCrLf`...).  Covered: `Dim`/`ReDim`/`Const`/`Set`, block and one-line
  `If`, `For`, `For Each`, `Do`/`Loop`, `While`/`Wend`, `Select Case`,
  `With`, `Sub` and `Function` with `Exit`, `Call`, `On Error Resume
  Next` with `Err`.  Not covered: `Class`, `Execute`, and `ByRef`
  arguments (every argument is passed by value).
- **What scripts see** (`userland/msiscript/runtime.js`): `Session`
  (`Property`, `TargetPath`, `SourcePath`, `Mode`, `Language`,
  `FeatureRequestState` and the other feature and component states,
  `EvaluateCondition`, `FormatRecord`, `Message`, `DoAction`, `Sequence`,
  `Installer`, `Database`), `Installer` (`CreateRecord`, `OpenDatabase`,
  `Environment`, `FileVersion`, `RegistryValue`, `ProductState`),
  `Database`, `View` and `Record` for SQL queries, and, through
  `CreateObject` or `new ActiveXObject`, `Scripting.FileSystemObject`
  (files, folders, text streams), `Scripting.Dictionary` and
  `WScript.Shell` (`RegRead`, `RegWrite`, `RegDelete`, `Run`,
  `ExpandEnvironmentStrings`, `Environment`, `SpecialFolders`).  The
  installer objects call `msi.dll`'s own API with the session's handle.
  `MsgBox` and `InputBox` are logged and answered with the default, since
  installs run unattended.
- **Every source type**: the script in the `Binary` table (types 5 and
  6), in a file the package installs (21 and 22), as the action's own
  `Target` text (37 and 38) and in a property's value (53 and 54), each
  calling the named function after the script's top level runs.  The
  function's result follows Windows: 2 cancels the install, 3 fails it;
  a script error is logged with its line and fails the action, unless
  the action may fail (`0x40`).  Deferred script actions read their
  `CustomActionData` as other deferred actions do.
- **Test packages.**  None of the real packages NovaOS installs (7-Zip,
  CMake, Node.js, Temurin, KeePassXC, PowerShell 7) carries a script
  custom action, so `tools/msitest/mkpkg.py` builds two:
  `script.msi` with a JScript and a VBScript action of each source type
  (`tools/msitest/scripts/`) that set properties, read them back, query
  the package's `Property` table, write files and the registry, and log
  through `Session.Message`; and `scriptfail.msi`, whose ignored failure
  and real failure end the install with 1603.  The new core self-test
  (`tests/selftest/core/136-msiscript.py`, `msitest script`) checks the
  properties as the `Registry` table wrote them, the files, the log, the
  removal, and the failed install (22 checks).
