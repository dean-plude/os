- **Windows userland** (`userland/`): about 35 system DLLs written from
  scratch and compiled with clang for `x86_64-pc-windows-msvc`, and again
  for `i686` in `SysWOW64`: `ntdll`, `kernel32`, `msvcrt`/`ucrtbase` with
  the `api-ms-win-crt-*` API sets (`errno` and the rest of the C
  runtime's per-thread state kept per thread, as on Windows), `vcruntime140`/`vcruntime140_1` (C++
  exceptions, FH3 and FH4 tables), `msvcp140` and its satellites (the C++
  standard library: Microsoft's own STL, compiled with clang, with
  Boost.Math under `msvcp140_2`'s special math functions),
  `user32`/`gdi32` (a real window system, controls, menus, dialogs, MDI,
  hooks), `gdiplus` (GDI+ on the MIT-licensed plutovg rasteriser),
  `comdlg32` (the Open and Save As dialogs, classic and `IFileDialog`),
  `comctl32`, `shell32`, `ole32`/`oleaut32` (COM and OLE Automation with
  type libraries), `advapi32`, `ws2_32`, `oleacc`,
  `winmm` and `mmdevapi` (sound: `waveOut`, `waveIn`, `PlaySound`, WASAPI
  playback and capture, endpoint volume), `msi` (with `msiscript` running
  JScript and VBScript custom actions on the ISC-licensed mujs),
  `secur32` with Schannel (TLS 1.3/1.2 for programs, on Mbed TLS),
  `usp10` (Uniscribe), `normaliz` (IDN), `urlmon` (`CreateUri`), and
  the DLLs Firefox delay-loads (`d3d11`, `credui`, `winspool.drv`,
  `dhcpcsvc`, `d3dcompiler_47`), and more.
