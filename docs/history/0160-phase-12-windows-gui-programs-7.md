## Phase 12 — Windows GUI programs: 7-Zip, unmodified

The goal of this phase is running real, unchanged Windows programs with
a graphical interface.  The test case is **7-Zip 26.03 (x64)** from
`third_party/7z2603-x64.exe`: the installer, the file manager (`7zFM.exe`)
and the GUI (`7zG.exe`) all run as shipped.  Nothing in 7-Zip was
changed; every fix is in NovaOS.

- **What 7-Zip does on NovaOS**: the installer shows its dialog, picks a
  folder with the "..." browser and installs with the progress bar; the
  file manager shows its toolbar, path box and file list (icons, sizes,
  dates, sortable columns), browses drives and folders, opens archives
  and lists their contents; Add to Archive opens `7zG`'s full options
  dialog and writes a real `.7z`; Extract goes through the Copy dialog
  and the Confirm File Replace prompt; Tools → Options opens with all six
  pages.  The file manager refreshes when files change on disk, and
  7-Zip's own icons show in the dock and title bars.
- **A real window system in user32** (`userland/user32/`): the window
  tree lives in the program, and only top-level windows have a desktop
  window (made when first shown); children are rectangles of their
  parent's bitmap, painted parent-first with invalidation cascading to
  siblings.  Per-thread message queues, sent messages across threads,
  timers, hit-testing, capture, double clicks, `WM_SETCURSOR`, mouse
  leave tracking, keyboard focus and `TranslateMessage`, A/W conversion
  of every message, hooks, properties, atoms, clipboard.  The standard
  controls (button, static, edit, list box, combo box, scroll bar), menus
  (bars, pop-ups, sub-menus, check marks, accelerators), dialogs from
  resource templates with tab and group navigation, `IsDialogMessage`,
  message boxes with icons, `DrawText` (wrapping, ellipsis, prefixes),
  `DrawEdge`, `DrawFrameControl`, icons from `.ico` and PE resources.
  `gdi32` draws TrueType text (Inter and DejaVu through stb_truetype)
  with clipping, viewport origins and raster operations.
- **The common controls** (`userland/comctl32/`), a real DLL that
  registers its classes when loaded: list view (details, list, small and
  large icons; owner data, callbacks, the Windows selection rules,
  keyboard, type-ahead, sorting, custom draw, label editing), header
  (resizing, ordering, sort arrows), toolbar (image lists and bitmaps,
  text, check groups, drop-downs), rebar, ComboBoxEx, tree view (lines
  and buttons, check boxes, expand/select notifications, label editing,
  sorting), tab control, property sheets (`PropertySheet` with the pages'
  dialogs, OK/Cancel/Apply/Help, wizards), progress bar, status bar,
  up-down, trackbar, tooltips, SysLink, image lists (ARGB, masks, icons),
  `DPA`/`DSA`, and real subclassing.  A program that uses a common control
  without linking `comctl32` gets it loaded on demand, as Windows does
  through the manifest.
- **The shell** (`shell32`): a folder picker (`SHBrowseForFolder` with
  the `BFFM_*` callback protocol), the system image lists with drawn icons
  for folders, drives and the common file types (`SHGetFileInfo`,
  `SHGetImageList`), `DragAcceptFiles`/`DragQueryFile` for dropped files.
- **Kernel sections**: named shared memory between processes
  (`NtCreateSection`, `NtMapViewOfSection`; `CreateFileMapping` and
  `OpenFileMapping` by name), which 7zFM and 7zG use to talk to each
  other.  File-backed mappings are sections too: filled from the file and
  written back on unmap, flush and last close, so a mapping of a file is
  shared between processes.  `shmtest.exe` checks both.
- **Drag and drop between programs**: OLE `DoDragDrop` with
  `IDropSource`/`IDropTarget` and `RegisterDragDrop` within a process, and
  across programs through the desktop, which tells the dragging program
  which window is under the pointer and carries the dropped file list to
  the other program; there it reaches the registered `IDropTarget` (as a
  `CF_HDROP` data object) or arrives as `WM_DROPFILES`.  `droptest.exe`
  has a source window and both kinds of target.  As on Windows the drop
  is synchronous: the dragging program's `DoDragDrop` returns only once
  the target has handled it, with the effect the target took, and no
  window keeps the mouse afterwards.  7-Zip's file manager depends on
  both: files dragged out of an archive are extracted to a temporary
  folder that 7-Zip deletes as soon as `DoDragDrop` returns, and its panel
  takes the mouse while it extracts.
- **Directory change notifications** (`FindFirstChangeNotification`): the
  kernel signals a program's event when a directory or its subtree
  changes.
- **Compatibility fixes found along the way**: x64 programs pass
  `HKEY_LOCAL_MACHINE` sign-extended (`0xFFFFFFFF80000002`);
  `SetWindowPlacement` must show a hidden window; `GetWindowPlacement`
  gives a child's position in its parent; `FindFirstFile` was dropping the
  file times; `GetDIBits` was dropping the alpha channel; static controls
  without word wrap still break lines.  `mpr.dll` (`WNet*`: no network),
  the `Lsa*` policy functions and `OpenFileMapping` were missing.
  Crashes now name the DLL and offset, a process's exit code is logged,
  `OutputDebugString` reaches the kernel log, and the Terminal's `trace
  NAME` prints a program's failing system calls.
- **Test programs**: `guitest.exe` (menus, accelerators, a resource
  dialog with combos, radios, checks and a list box, message boxes, a
  property sheet with a form page and a tree view page), `droptest.exe`,
  `shmtest.exe`, `ftprobe.exe` (what `FindFirstFile` reports).  Real
  programs are tested from a second disk image holding 7-Zip, Git, CMake,
  Ninja, Neovim, Notepad++ and others, driven by a QEMU harness that types
  Terminal commands, clicks, drags and takes screenshots.
- Drags from 7-Zip's file manager onto other programs work: one or more
  files from an archive or from a folder, onto a `WM_DROPFILES` window or
  an OLE drop target (tested with both `droptest` targets; each reports
  the dropped files' sizes, so a file that has already gone shows as
  missing).  (Pipes, `cmd.exe` and the clipboard: see below.)
