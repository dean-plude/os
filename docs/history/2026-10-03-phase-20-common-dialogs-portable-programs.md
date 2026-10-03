## Phase 20: common dialogs and portable programs

Phase 20.1 and 20.2 of the roadmap: the common file dialogs, and three of
the App Store's "untested" portable programs (SumatraPDF, WinMerge, PuTTY)
opening a file or a connection and running in the nightly corpus.

- **Common file dialogs** (`userland/comdlg32/filedlg.c`, `ifiledlg.c`):
  `GetOpenFileName` and `GetSaveFileName` show a real Open / Save As
  dialog: the folder path with an Up button, the folder's contents in a
  list view (folders first, then the files the chosen filter matches, with
  the shell's icons, sizes, types and dates; the drives at the top), the
  file name and the file types.  Double-click or Enter opens a folder or
  picks a file, typing a path goes there, typing a wildcard filters the
  list; Open checks that the file exists, Save As asks before replacing
  one and adds the filter's or the default extension; multi-select and
  folder picking work.  The Vista-style COM dialogs (`CLSID_FileOpenDialog`,
  `CLSID_FileSaveDialog`: `IFileOpenDialog`, `IFileSaveDialog`,
  `IFileDialogEvents`, `IFileDialogCustomize`) sit on the same dialog and
  hand the choice back as shell items, so shell32 gained
  `IShellItem`/`IShellItemArray` (`SHCreateItemFromParsingName`,
  `SHCreateShellItemArray`...).  user32: a key released after a modal
  dialog opened reaches the dialog, and Alt opens the menu bar only when
  pressed alone.  `dlgtest` covers the objects' settings and has
  interactive modes for each dialog; Notepad++ and 7-Zip open and save
  through them.
- **GDI+** (`userland/gdiplus/`, new): the flat `Gdip*` API that the C++
  wrapper classes compile down to, drawn with
  [plutovg](https://github.com/sammycage/plutovg) (`third_party/plutovg`,
  MIT, with FreeType-licensed rasteriser and stroker files): graphics on a
  bitmap, a window or any DC (reading the affected part of the DC into a
  DIB and copying it back, so the DC's clipping holds), world transforms,
  clipping, solid, hatch, texture and gradient brushes, pens with dashes
  and caps, lines, Béziers, arcs, pies, polygons, paths kept as GDI+ keeps
  them (points and types, so `GetPathData`, markers and iterators see what
  programs expect), regions, matrices; bitmaps loaded from PNG, JPEG, BMP
  and GIF files or streams and saved as PNG, JPEG or BMP, converted to and
  from HBITMAPs and HICONs, `LockBits` in every common pixel format;
  fonts, families and string formats mapped onto the faces NovaOS ships
  the way gdi32 maps them, with `DrawString`/`MeasureString` laid out in
  GDI+'s way (wrapping, hotkey prefixes, tabs, alignment, trimming, the
  generic default's em/6 padding), `MeasureCharacterRanges` and driver
  strings.
- **New DLLs**: `winspool.drv` (the spooler's client calls on a machine
  without printers: an empty printer list, no default printer, so print
  menus load and printing reports no printer), `oleacc.dll` (Active
  Accessibility with no client to serve: `LresultFromObject` answers 0),
  and DDE in user32 (`DdeInitialize`, string handles, `DdeConnect` finds
  no server, the `WM_DDE_*` lParam packing), which SumatraPDF uses to look
  for a running copy before opening its own window.
- **MDI** (`userland/user32/mdi.c`): the `MDIClient` class with
  `WM_MDICREATE`, `DESTROY`, `ACTIVATE`, `NEXT`, `GETACTIVE`, `SETMENU`,
  `REFRESHMENU`, `MAXIMIZE`, `RESTORE`, `TILE` and `CASCADE`,
  `DefFrameProc`, `DefMDIChildProc`, `CreateMDIWindow` and
  `TranslateMDISysAccel` (Ctrl+F4, Ctrl+F6, Ctrl+Tab).  Children fill the
  client window, as a maximized child does.  WinMerge is an MFC MDI
  application and needs all of it.
- **user32 and gdi32 for MFC**: `WH_CBT` hooks called for window creation
  and destruction (MFC subclasses every window it creates from
  `HCBT_CREATEWND`; without the hook its frame window had no `CWnd` and was
  deleted); class names up to 255 characters (MFC's generated names such
  as `Afx:0000000140000000:b:...` were cut and not found);
  `GetClassInfo` copies its fields one by one (a `memcpy` from the wrong
  offset lost the window procedure on x64); `WM_SIZE` and `WM_MOVE` reach
  an overlapped window when it is first shown, as on Windows, not during
  `CreateWindow` (PuTTY's handler dereferences the terminal it creates
  after the window); `ToUnicode` with `KF_UP` in the scan code types
  nothing (PuTTY translates key-ups too and typed every character twice);
  framed windows' client areas are clamped to their bitmap, so a program's
  own caption (SumatraPDF) sits under the desktop's title bar instead of
  starting a repaint loop; `WS_CLIPCHILDREN` windows keep their children's
  pixels across a parent repaint.  gdi32: 24-bit `CreateDIBSection` gives
  the program 24-bit rows of its own (in its section when it passed one)
  synced with the 32-bit pixels gdi32 draws on, and `LoadImage` with
  `LR_CREATEDIBSECTION` keeps a 24-bit resource as such a section, in the
  resource's row order (WinMerge reads its toolbar strips back through
  `GetObject` and converts them itself; it got 32-bit zeros and drew black
  squares); mapping modes, world transforms (identity), palettes,
  `TranslateCharsetInfo`, `GetCharABCWidthsFloat`.  comctl32: `TBBUTTON`'s
  reserved bytes are 2 on x86 (the 64-bit layout broke every 32-bit
  toolbar), separators take their width from `TBBUTTON.iBitmap`,
  `TB_GETMETRICS`/`TB_SETMETRICS`.
- **Bootloader**: the identity map covers the first 64 GiB (1 GiB pages
  above 4 GiB), not only 4 GiB: with more than 4 GiB of RAM the firmware
  loads the bootloader near the top of memory and it page-faulted on its
  own code the moment it switched to its page tables (the nightly corpus
  boots with 4 GiB).
- **Loader**: a relative DLL path (`Merge7z\Merge7z.dll`) is searched from
  the program's folder, then the current one, and gets `.dll` when it has
  no extension; `ole32` logs a `CoCreateInstance` of an unregistered class.
- **Winsock** (`userland/ws2_32/wsa.c`): `WSAAsyncSelect`, the window
  message form of `WSAEventSelect` (one message per event, `FD_CONNECT`,
  `FD_ACCEPT`, `FD_READ`, `FD_WRITE`, `FD_CLOSE`; an event is reported
  once and re-enabled by the call that consumes it: `recv`, a `send` that
  would block, `accept`), which PuTTY drives all its networking with;
  `recv` with `MSG_PEEK` and `ioctlsocket(FIONREAD)` through a kernel
  peek (`NetSockPeek`) instead of consuming a byte; `FD_CONNECT` and
  `FD_WRITE` both reported on a socket that asked for both.
- **Programs**: SumatraPDF 3.4.6 (32-bit) opens a PDF and renders it with
  its toolbar, tabs and menus; WinMerge 2.16.50 compares two files side by
  side with the differences highlighted; PuTTY 0.81 makes a raw connection
  to a host, shows what the server sends and sends what is typed.  All
  three are in the nightly corpus (`tools/appcorpus.py`), which now runs
  the windowed programs one at a time (each takes the keyboard, Alt+F4
  closes it) and compares each screenshot with `tests/reference/NAME.png`:
  SumatraPDF on a PDF the script generates, WinMerge on two text files,
  PuTTY on a connection to an echo server the script runs on the host
  (10.0.2.2 on QEMU's user network; the typed line must reach it).
  SumatraPDF's official 32-bit build is taken from the npm package
  `pdf-to-printer` and PuTTY is built from its source release with MinGW,
  because neither project's download site is reachable from every
  network; the build is kept in the corpus cache.
- Not yet: SumatraPDF draws its own caption under the desktop's title bar
  (two title bars); printing (`winspool` has no printers); PuTTY's SSH is
  untested (no SSH server in the test network); WinMerge's folder compare
  and plugins (`icu.dll`, `mlang.dll`) are untried; `IFileDialogCustomize`
  adds no controls.
