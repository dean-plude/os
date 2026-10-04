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
  hooks, per-monitor and per-thread DPI awareness with `WM_DPICHANGED`, controls and fonts at each window's DPI and rescaled when it changes, window coordinates converted between awareness contexts,
  touch, pens and the mouse as `WM_POINTER` messages with `GetPointerPenInfo`, title bars included, and the pen signature in `GetMessageExtraInfo`; `SetCursorPos` and `ClipCursor` for the window in front, as games recentre and confine the pointer), `gdiplus` (GDI+ on the MIT-licensed plutovg rasteriser),
  `comdlg32` (the Open and Save As dialogs, classic and `IFileDialog`),
  `comctl32`, `riched20`/`msftedit` (Rich Edit controls that take RTF,
  as setup programs' licence pages need), `shell32`, `ole32`/`oleaut32` (COM and OLE Automation with
  type libraries, and calls between processes over named pipes with the standard marshaler and the `IDispatch` proxy; the Windows Runtime's strings and the few runtime classes Win32 programs ask for, such as `UISettings` for the user's colours), `rpcrt4` (the NDR engine COM proxy/stub DLLs run on:
  stubless proxies, `NdrStubCall2`, `CStdStubBuffer`, the `NdrDll*` entry points), `advapi32`, `ws2_32`, `oleacc`,
  `winmm` and `mmdevapi` (sound: `waveOut`, `waveIn`, `PlaySound`, MIDI,
  WASAPI playback and capture, a device ID or endpoint for each sound
  device, device 0 being the default as on Windows, each with its own
  endpoint volume; sound plays at its own rate and is converted once, by
  the mixer, to the device's), `dsound` (DirectSound, with
  every device enumerated and openable by its GUID),
  `xaudio2_7`/`xaudio2_8`/`xaudio2_9` and `x3daudio1_7` (on FAudio; every
  output listed and openable by its device ID), `msi`
  (with `msiscript` running JScript and VBScript custom actions on the
  ISC-licensed mujs; product, patch and source-list queries by
  installation context, as bootstrappers such as WiX Burn make them), `msxml6` (MSXML: the XML DOM with XPath, SAX and
  `XMLHTTP` on the MIT-licensed libxml2, answering the MSXML 3 classes
  such as `Msxml2.DOMDocument` too),
  `secur32` with Schannel (TLS 1.3/1.2 for programs, on Mbed TLS),
  `crypt32` and `wintrust` (certificate stores and chains, signed PKCS #7
  messages, and Authenticode: `WinVerifyTrust` checks a program's
  signature, its timestamp and its chain to the trusted roots, and the
  Microsoft root policy tells Microsoft's own signatures apart),
  `usp10` (Uniscribe), `normaliz` (IDN), `urlmon` (`CreateUri`, and the
  Internet security manager's zones),
  `wintab32` (Wintab pen tablets: pressure, tilt and barrel rotation for GTK,
  Qt and Krita), `cabinet` (the FDI functions installers extract cabinets
  with, MSZIP and LZX, on `msi`'s cabinet readers), `wldap32` (LDAP
  sessions, for programs that link to it; no directory servers yet),
  `opengl32` (Mesa from the App Store, or with no driver NovaOS's own
  OpenGL 1.1, which draws 2D as ScummVM needs: textures, vertex arrays,
  blending, scissor), and
  the DLLs Firefox delay-loads (`d3d11`, `credui`, `winspool.drv`,
  `dhcpcsvc`, `d3dcompiler_47`), the ones Qt WebEngine imports
  (`bthprops.cpl`, `d3d12`, `winusb`: no Bluetooth radio, Direct3D 12
  device or WinUSB device, as on a PC without them, with Bluetooth's SDP
  record parsers working in full), and more.
