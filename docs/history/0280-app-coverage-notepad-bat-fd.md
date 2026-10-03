## App coverage: Notepad++, bat, fd

- **Notepad++ 8.7.9** (64-bit portable) opens with its menus, toolbar and
  editor, and takes typing.  What it needed: the `.exe`'s TLS slot is now
  always 0 (MSVC's thread-safe statics assume it), `GetFileAttributesEx`
  leaves its output alone when the file is missing, and `RtlUnwindEx`
  runs a consolidating unwind's callback, so `catch` blocks in programs
  with the static MSVC C++ runtime run (a `throw` used to resume after
  the throw site).
- **bat** (`CompareObjectHandles`, a new `NtCompareObjects` call) and
  **fd** (`GetModuleHandle` of an API set name) now run; rg and jq ran
  already.
- **New DLLs**: `uxtheme` (no theme; real buffered paint), `dwmapi`
  (composition off), `imm32` (no IME), `msimg32`, `wintrust` (nothing is
  signed), `sensapi`, `wininet` (URL parsing; offline).  `kernel32` has
  `.ini` files (`GetPrivateProfileString` and friends), `gdi32` gradient
  fills, pattern brushes and coordinate conversion, `crypt32`
  `CryptStringToBinary`.
- **`tools/novarun.py`** boots the image in QEMU with programs copied onto
  a data disk, types Terminal commands and takes screenshots:
  `python3 tools/novarun.py --put 'DIR=C:\Apps\x' 'cd C:\Apps\x' 'x.exe' '!shot x.png'`.
- Not yet: ~~Notepad++'s status bar draws black and its toolbar is cut
  short~~ (fixed in Phase 17.6); ~~Neovim hangs on exit (console input handles cannot be waited
  on)~~ fixed in Phase 17.2;
  ffmpeg needs `avrt`, `ncrypt`, `d2d1`, `dwrite` and more (see
  [More compatibility](#more-compatibility-schannel-uniscribe-idn-crt-gaps)).
