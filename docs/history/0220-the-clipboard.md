## The clipboard

- **One clipboard for everything** (`kernel/wm/clipboard.c`, reached by
  programs through `NtNovaClipboard`): each format is a copy of its
  bytes; `CF_TEXT`/`CF_OEMTEXT` and `CF_UNICODETEXT` are converted into
  each other on request (with `CF_LOCALE`); formats a program registers
  travel by name, since each program numbers them differently.
- **user32** (`OpenClipboard` ... `GetClipboardData`,
  `EnumClipboardFormats`, `GetClipboardSequenceNumber`,
  `GetPriorityClipboardFormat`) now uses it, so text, files (`CF_HDROP`)
  and private formats copied in one program paste in another.
  `CF_BITMAP` travels as a `CF_DIB` and comes back as a bitmap; a format
  set with a NULL handle is rendered by its owner (`WM_RENDERFORMAT`) when
  the clipboard is closed.
- **The OLE clipboard** (`userland/ole32/clipbrd.c`): `OleSetClipboard`
  copies a data object's formats onto it, `OleGetClipboard` gives a data
  object that reads it (`GetData`, `QueryGetData`, `EnumFormatEtc`),
  `OleIsCurrentClipboard`, `OleFlushClipboard`.
- **The built-in apps**: Notepad selects (Shift with the arrows, Home,
  End, PgUp/PgDn; mouse drags; double-click for a word; Ctrl+A) and has
  Ctrl+C/X/V with Copy and Paste buttons.  The Terminal selects with a
  mouse drag (double-click: a word); Ctrl+C copies while something is
  selected (else it still interrupts), Ctrl+Shift+C always copies, and
  Ctrl+V, Shift+Insert or a right click paste into the prompt or the
  running program; the wheel scrolls.  File Explorer copies, cuts and
  pastes files and folders (Ctrl+C/X/V, Copy and Paste buttons) as
  `CF_HDROP` with a "Preferred DropEffect", so programs see them too.
- Tests: `cliptest.exe` (23 checks, 64-bit and 32-bit, each reading back
  in a second process): Unicode text read as `CF_TEXT` and the other way,
  a registered format by name, `CF_HDROP` with `DragQueryFile`, a bitmap
  as a 3x2 DIB and back, the sequence number, and the OLE clipboard.
