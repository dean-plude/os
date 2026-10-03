- **Windows userland** (`userland/`): about 35 system DLLs written from
  scratch and compiled with clang for `x86_64-pc-windows-msvc`, and again
  for `i686` in `SysWOW64`: `ntdll`, `kernel32`, `msvcrt`/`ucrtbase` with
  the `api-ms-win-crt-*` API sets (`errno` and the rest of the C
  runtime's per-thread state kept per thread, as on Windows, with
  `_configthreadlocale` per-thread locales and `getenv` results other
  threads cannot overwrite; `ntdll` gives fiber-local storage its own
  slots and runs `FlsAlloc` callbacks when a thread or the process ends;
  `TlsAlloc` has the 1024 expansion slots past the TEB's 64), `vcruntime140`/`vcruntime140_1` (C++
  exceptions, FH3 and FH4 tables), `msvcp140` and its satellites (the C++
  standard library: Microsoft's own STL, compiled with clang, with
  Boost.Math under `msvcp140_2`'s special math functions),
  `user32`/`gdi32` (a real window system, controls, menus, dialogs, MDI,
  hooks, per-monitor and per-thread DPI awareness with `WM_DPICHANGED`, controls and fonts at each window's DPI), `gdiplus` (GDI+ on the MIT-licensed plutovg rasteriser),
  `comdlg32` (the Open and Save As dialogs, classic and `IFileDialog`),
  `comctl32`, `shell32`, `ole32`/`oleaut32` (COM and OLE Automation with
  type libraries), `advapi32`, `ws2_32`, `oleacc`,
  `winmm` and `mmdevapi` (sound: `waveOut`, `waveIn`, `PlaySound`, MIDI,
  WASAPI playback and capture, endpoint volume), `dsound` (DirectSound),
  `xaudio2_7`/`xaudio2_8`/`xaudio2_9` and `x3daudio1_7` (on FAudio), `msi`
  (with `msiscript` running JScript and VBScript custom actions on the
  ISC-licensed mujs),
  `secur32` with Schannel (TLS 1.3/1.2 for programs, on Mbed TLS),
  `usp10` (Uniscribe), `normaliz` (IDN), `urlmon` (`CreateUri`),
  `wintab32` (Wintab pen tablets: pressure, tilt and barrel rotation for GTK,
  Qt and Krita), and
  the DLLs Firefox delay-loads (`d3d11`, `credui`, `winspool.drv`,
  `dhcpcsvc`, `d3dcompiler_47`), and more.
