## Shortcuts (.lnk) and overlapping controls

- **`IShellLink`** (`userland/shell32/shlink.c`): shell32's ShellLink
  class, `CLSID_ShellLink`, registered under `HKCR\CLSID` (a bare
  `shell32.dll`, so 32-bit and 64-bit programs each get their own), with
  `IShellLinkW`, `IShellLinkA` and `IPersistFile`.  `Save` writes the
  Windows `.lnk` format (MS-SHLLINK: header, LinkInfo with the target in
  ANSI and Unicode, Unicode strings for the description, working folder,
  arguments and icon); `Load` reads links made by Windows too.
- **The shell uses them**: opening a `.lnk` (Explorer, the desktop, the
  Start menu, `start` in the Terminal) runs its target with the
  shortcut's arguments in its working folder, or opens the folder or
  document it points to; a shortcut shows its target's icon and is
  called "Shortcut" in Explorer.  The Terminal's `start` opens any
  document, folder or shortcut the way the shell does.  The Start menu
  lists the shortcuts in `C:\AppData\Roaming\Start Menu\Programs`
  (where `$SMPROGRAMS` and `CSIDL_PROGRAMS` point) ahead of
  `C:\Programs`, keeping "Uninstall ..."
  entries out of the grid (search finds them), and the desktop shows
  what is in `C:\Desktop` after its own icons, noticing new files within
  half a second.
- **Overlapping controls paint as on Windows**: siblings paint from the
  top of the z-order down (the first control of a dialog first), so where
  controls without `WS_CLIPSIBLINGS` overlap, the lower one's pixels end
  on top.  NSIS's page header is a white static above its bold title,
  subtitle and icon; they were hidden under it and now show, and the
  welcome and finish pages cover the header as they should.
- Tested with a real NSIS installer that makes a desktop shortcut (with
  arguments) and two Start menu shortcuts: the desktop icon appears, a
  double-click starts the 32-bit program with its arguments in its
  folder, the Start menu lists it, and the uninstaller takes them away.
  comtest round-trips a shortcut through `IShellLinkW`, `IPersistFile`
  and `IShellLinkA` in both 64-bit and 32-bit builds.
