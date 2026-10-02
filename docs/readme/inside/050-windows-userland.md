- **Windows userland** (`userland/`): about 35 system DLLs written from
  scratch and compiled with clang for `x86_64-pc-windows-msvc`, and again
  for `i686` in `SysWOW64`: `ntdll`, `kernel32`, `msvcrt`/`ucrtbase` with
  the `api-ms-win-crt-*` API sets, `vcruntime140`/`vcruntime140_1` (C++
  exceptions, FH3 and FH4 tables),
  `user32`/`gdi32` (a real window system, controls, menus, dialogs),
  `comctl32`, `shell32`, `ole32`/`oleaut32` (COM and OLE Automation with
  type libraries), `advapi32`, `ws2_32`,
  `winmm` and `mmdevapi` (sound: `waveOut`, `waveIn`, `PlaySound`, MIDI,
  WASAPI playback and capture, endpoint volume), `dsound` (DirectSound),
  `xaudio2_7`/`xaudio2_8`/`xaudio2_9` and `x3daudio1_7` (on FAudio), `msi`,
  `secur32` with Schannel (TLS 1.3/1.2 for programs, on Mbed TLS),
  `usp10` (Uniscribe), `normaliz` (IDN), and more.
