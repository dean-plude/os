## Windows Installer: the queries bootstrappers make, 32-bit packages in SysWOW64

The Visual C++ Redistributable's installer (a WiX Burn bundle, which GOG
GALAXY needs for `mfc140u.dll`) installed its first package, the Minimum
Runtime, and then crashed: Burn looks up `msi.dll`'s newer functions with
`GetProcAddress` and calls `MsiSourceListAddSourceExW` without checking,
and NovaOS had none of them.  It has them now, and both runtimes install.

- **Product information by installation context**: `MsiGetProductInfoEx`
  and `MsiEnumProductsEx` (per-machine, or the current user's for a
  per-user install; a SID or a context that does not fit is refused as on
  Windows), with the `INSTALLPROPERTY_*` names (`VersionString`, `State`,
  `PackageCode`, `AssignmentType`, `LocalPackage`, `PackageName`,
  `LastUsedSource`...).  `MsiGetProductInfo` and `MsiEnumProducts` now
  answer the same way, and string results follow the Windows Installer's
  length rules (`ERROR_MORE_DATA`, a NULL buffer asks for the length).
- **Source lists**: an install registers where its package came from
  under the product's Installer key (`SourceList`: `PackageName`,
  `LastUsedSource`, `Net\1`), as Windows does, and
  `MsiSourceListAddSourceEx` (Burn adds its package cache there),
  `EnumSources`, `GetInfo`, `SetInfo`, `ClearSource`, `ClearAllEx` and the
  older `AddSource`/`ClearAll` work on it, for products and for patches.
- **Patches**: `MsiDetermineApplicablePatches` and
  `MsiDeterminePatchSequence` say which patches fit a package or an
  installed product, from a patch file or its applicability XML (each
  patch that does not fit gets order -1 and its reason, such as 1642);
  `MsiEnumPatchesEx` and `MsiGetPatchInfoEx` list the patches applied to
  a product.  Patches are ordered as given: NovaOS does not read
  `MsiPatchSequence` yet.
- **32-bit packages**: a package whose summary names the `Intel` platform
  now puts its `SystemFolder` files in `C:\Windows\SysWOW64`, where
  32-bit programs load their DLLs, as 64-bit Windows does; `System64Folder`
  stays `System32`.  The engine running inside a 32-bit process (Burn
  calls `MsiInstallProduct` in-process) turns WoW64 file redirection off,
  as Windows' 64-bit installer service sees the folders as they are, so a
  64-bit package installed from a 32-bit bootstrapper lands in
  `System32`.
- **Upgrades by version range**: `FindRelatedProducts` now honours the
  Upgrade table's `VersionMin`, `VersionMax` (inclusive or not) and
  `Language` columns, and `RemoveExistingProducts` leaves alone what a
  detect-only row found.  Before, every product with the upgrade code
  counted, so a newer Visual C++ runtime (GOG GALAXY carries 14.51) was
  refused as "a later version is already installed" over 14.44.
- **Self-test** `msiqtest` (core 150, 64- and 32-bit) with two new test
  packages from `tools/msitest/mkpkg.py`, `wow32.msi`, `wow64.msi` and the
  upgrade `wow32v2.msi`.
- **Where GOG GALAXY stops now**: its setup installs the x86 and x64
  runtimes it carries and its files; `GalaxyClient.exe` (64-bit, Qt 6
  WebEngine) does not start because `d3d9.dll` is missing (Qt WebEngine
  imports it; NovaOS has Direct3D 9 only from DXVK in the App Store).
