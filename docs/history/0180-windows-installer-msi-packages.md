## Windows Installer (.msi packages)

NovaOS has its own Windows Installer: `msi.dll` (`userland/msi/`) and
`msiexec.exe` in `C:\Windows\System32`.  Double-clicking a `.msi` in
Explorer, `start package.msi` in the Terminal, and the App Store's
Install button all go through it, and programs can call
`MsiInstallProduct`, `MsiConfigureProduct`, `MsiQueryProductState`,
`MsiGetProductInfo` and `MsiEnumProducts`.

- **The package format**: the OLE compound file container (FAT, mini
  FAT, DIFAT, the encoded stream names), the string pool and the
  column-major table streams, and the cabinets inside (or beside) the
  package with MSZIP (deflate) and LZX decompression, files spanning
  data blocks and cabinets.  These readers use only the C library and
  are tested on the host against packages built with msitools.
- **The engine** runs `InstallExecuteSequence`: launch conditions,
  `AppSearch`/`RegLocator`, feature and component selection (levels,
  the Condition table, `ADDLOCAL`/`REMOVE`, component conditions),
  Directory resolution onto NovaOS's folders (`ProgramFilesFolder` is
  `C:\Programs`, `SystemFolder` is `C:\Windows\System32`, ...),
  `CreateFolders`, `InstallFiles`, the Registry table (all value
  types, `[Property]`, `[#File]` and `[$Component]` formatting),
  `FindRelatedProducts`/`RemoveExistingProducts` through the Upgrade
  table, and product registration under the Uninstall key with a
  cached copy of the package in `C:\Windows\Installer`.  Custom actions
  that set properties or directories (types 51 and 35) run; ones that
  execute code are logged and skipped.  `msiexec /x` reverses it all,
  on the folder chosen at install time.
- **msiexec** takes `/i`, `/x` (a package or a `{ProductCode}`), `/qn`,
  `/qb`, `/passive`, `/l*v FILE` and `PROPERTY=value` overrides, and
  shows the familiar progress window with Cancel (full UI adds the
  completion message box).  Logs also go to the kernel log (`dmesg`).
- Not yet: the packages' own dialogs (`InstallUISequence`), the
  `Shortcut` table, services, environment variables, and
  merge modules.  LZX decoding is written to the specification but has
  only been exercised with MSZIP cabinets so far.
