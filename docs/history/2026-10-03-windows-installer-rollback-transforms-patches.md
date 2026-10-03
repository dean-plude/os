## Windows Installer rollback, transforms, patches, services at boot

The gaps [Windows Installer depth](#windows-installer-depth-custom-actions-dialogs-shortcuts-services)
left open, except script custom actions (next).  There is still no
permissive Windows Installer to borrow from (Wine's is LGPL), so this is
NovaOS's own code; the test packages are written by our own pure-Python
writer, so the build needs neither Windows nor msitools.

- **Rollback.**  While `InstallExecuteSequence` runs, the engine keeps a
  journal (`userland/msi/install.c`, "Rollback") of everything it
  changes: files it overwrites or deletes are moved to
  `C:\Config.Msi\*.rbf` first, new files, folders and registry keys are
  noted, registry values keep their old data, services note whether they
  were created, reconfigured, started or stopped, and the product's
  registration and cached package are journalled like any other key and
  file.  If an action fails the journal is played backwards: rollback
  custom actions (type flag `0x500`) run in their place with the
  `CustomActionData` they had, files come back from `Config.Msi`, new
  ones and the folders made for them go, keys and values return to what
  they were, services stop and are deleted or reconfigured back.  A
  successful install deletes the backups at `InstallFinalize`, before
  the commit actions.  `DISABLEROLLBACK=1` (or the `DisableRollback`
  action) turns it off, as on Windows.  Not rolled back: the removal of
  an older product by `RemoveExistingProducts`.
- **Transforms (`.mst`).**  `TRANSFORMS=a.mst;:embedded` on the command
  line, and `MsiDatabaseApplyTransform`, apply them to the database in
  memory (`msidb.c`): rows inserted, deleted or updated column by column,
  tables and columns added or dropped, the transform's own string pool
  merged, its streams laid over the package's.  The summary information's
  validation flags are checked (product code, upgrade code); a transform
  for another product fails with 1624.  The transforms a product was
  installed with are copied to `C:\Windows\Installer\{ProductCode}` and
  applied again for repair and removal.
- **Patches (`.msp`).**  `msiexec /p patch.msp` (or `/update`, or
  `PATCH=` with `/i`) finds the installed product the patch targets
  (1642 if there is none), applies the patch's transform pairs (`T` and
  `#T`, the ones whose validation fits), adds its cabinet streams and
  reinstalls; the patch is cached and recorded with the product, so a
  repair keeps it.  `msiexec /uninstall patch.msp` reinstalls without it
  (`MSIPATCHREMOVE`).  Patches that ship whole files work; binary delta
  patches (rows in the `Patch` table) are refused with a clear message.
- **Services at boot.**  `services.exe` (`userland/programs/services.c`)
  starts at boot, once the desktop is up on an installed system: it marks
  every service stopped (the registry's state is from the last boot),
  then starts each automatic service (`Start` 2), the services it
  depends on first and `DelayedAutoStart` ones last, and logs each result
  (`[SVC] Name: started`).  `services` with no argument lists them.
- **Test packages.**  `tools/msitest/mkmsi.py` writes compound files,
  databases, transforms, patches and MSZIP cabinets;
  `tools/msitest/mkpkg.py` uses it at build time for the packages the
  `msitest` self-test installs (`C:\Tests\Msi`).  The new core self-test
  (`tests/selftest/core/135-msitest.py`) runs `msitest transform`,
  `patch`, `rollback` and `service`, restarts, and checks that
  `services.exe` started the service.
- **Tested in QEMU** with real packages: Node.js 22.11 with a transform
  that adds a failing custom action just before `InstallFinalize`
  returned 1603 and rolled back 3176 changes (2525 files, folders,
  registry, shortcuts, its cached package and registration), leaving no
  `C:\Programs\nodejs` and no product key; Node.js then installed,
  took a whole-file patch (`msiexec /p`: the patched file, version
  22.11.1 in Programs and Features), kept it through a repair, lost it
  again with `msiexec /uninstall` (22.11.0, `node -e` runs) and was
  removed.  CMake 3.30 refused a transform made for another product
  (1624), installed with a validated transform adding a registry value,
  ran `cmake --version`, and its removal applied the cached transform
  and took the value away.  The test service package installed, and
  after a restart `services.exe` started it (`[SVC] NovaTestSvc:
  started`); `msiexec /x` stopped and deleted it.  A rollback custom
  action that fails is logged and the rollback carries on, as on Windows.
  Node.js's `WixRollbackInternetShortcuts` does fail that way (it finds
  no shortcut attributes in its `CustomActionData`); why is still open.
- Fixed on the way: a file is taken only from the cabinet its sequence
  number puts it in, so a patch's new copy is not overwritten by the
  product cabinet's entry with the same key.
