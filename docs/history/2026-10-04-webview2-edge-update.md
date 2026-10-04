## WebView2: Microsoft Edge Update runs its install step

Roblox's login page needs the Microsoft Edge WebView2 runtime, whose
installer is Microsoft Edge Update (32-bit).  It used to stop at start on a
function NovaOS lacked; it now starts, reads its policies and runs its
install step, which ends at the next missing piece, MSXML 6
([compatibility.md](../compatibility.md#webview2)).  The nightly corpus runs
the offline installer (`tests/appcorpus/095-webview2.py`).

- **Task Scheduler 2.0** (`taskschd.dll`, the `TaskScheduler` class):
  `ITaskService`, `ITaskFolder`, `IRegisteredTask`, the task, folder and
  running-task collections and `ITaskDefinition`'s XML.  A registered task
  is its XML in `C:\Windows\System32\Tasks` (UTF-16, as Windows writes it)
  and an entry in the registry's `Schedule\TaskCache`, so tasks survive
  restarts; a task can be read back, disabled, enabled, run (its `<Exec>`
  actions start) and deleted.  Edge Update registers its update tasks this
  way.  Not yet: a scheduler service that starts tasks on their triggers,
  and the definition's object model (triggers and actions as objects).
- **The Data Protection API**: `CryptProtectData` and `CryptUnprotectData`
  (crypt32) seal data with a key of the user's (or, with
  `CRYPTPROTECT_LOCAL_MACHINE`, the machine's), made on first use; the blob
  is encrypted and authenticated (HMAC-SHA-256), so the wrong entropy or a
  changed blob fails with `NTE_BAD_DATA` as on Windows.
- **URLs**: shlwapi's `UrlCombineW`/`UrlCombineA` (RFC 3986 resolution,
  dot segments removed), `UrlEscapeA` and `UrlUnescapeA`.
- **Packages**: `PackageFamilyNameFromFullName`, `PackageIdFromFullName`
  and `GetPackagesByPackageFamily` (none is installed).
- **And**: `MDMRegistration.dll` (the machine is not enrolled in device
  management), `NetGetAadJoinInformation` (not joined to Azure AD),
  `WTSEnumerateSessionsW`/`A` (session 0 and the console's session 1),
  `MakeAbsoluteSD`, userenv's `EnterCriticalPolicySection`,
  `LeaveCriticalPolicySection` and `GetProfileType`, ole32's
  `CoRegisterPSClsid`, `CoGetCallContext` and `CoGetStdMarshalEx`,
  `InternetSetStatusCallback`, `WTHelperGetProvSignerFromChain`,
  `WerRegisterCustomMetadata` and `AppPolicyGetProcessTerminationMethod`.
- **cmd's `type`** shows a UTF-16 file (one starting with a byte-order
  mark, as Edge Update's log does) as text, as Windows' does.
- The `edgeupdtest` self-test checks them, 64- and 32-bit.
