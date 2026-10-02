- **Windows userland** (`userland/`): about 35 system DLLs written from
  scratch and compiled with clang for `x86_64-pc-windows-msvc`, and again
  for `i686` in `SysWOW64`: `ntdll`, `kernel32`, `msvcrt`/`ucrtbase` with
  the `api-ms-win-crt-*` API sets, `vcruntime140` (C++ exceptions),
  `user32`/`gdi32` (a real window system, controls, menus, dialogs),
  `comctl32`, `shell32`, `ole32`/`oleaut32` (COM), `advapi32`, `ws2_32`,
  `winmm` and `mmdevapi` (sound: `waveOut`, `PlaySound`, WASAPI), `msi`,
  `secur32` with Schannel (TLS 1.3/1.2 for programs, on Mbed TLS),
  `usp10` (Uniscribe), `normaliz` (IDN), and more.
