# NovaOS — Windows-Compatible Operating System
<!-- The regions between "BEGIN generated" and "END generated" markers are built from fragment files by tools/docgen.py: edit those files, not the regions (CONTRIBUTING.md). -->

A clean-room, from-scratch x86-64 operating system that runs native Windows
executables without emulation.  The programs' own machine code runs directly
on the CPU; NovaOS supplies what they expect from Windows: the NT system-call
ABI, the Win32 API, the loader, a GUI, and the drivers underneath.  64-bit
(x64, PE32+) programs run natively, and 32-bit (x86, PE32) ones run through
NovaOS's own WoW64 layer, as on 64-bit Windows.

**Status:** Phases 1–15 are done.  NovaOS boots on UEFI machines (tested in
QEMU with OVMF), uses every CPU core, keeps its files on a SATA disk, and
runs unmodified Windows programs: 7-Zip, Git, NSIS installers, `.msi`
packages (with their own dialogs, custom actions, shortcuts and
services), the Java, .NET, Node.js and Python runtimes, and OpenGL, Vulkan
and Direct3D 8–11 programs through Mesa and DXVK.  It can install
itself on a disk from its live ISO.

- [What runs today](#what-runs-today)
- [What is inside](#what-is-inside)
- [Quick start](#quick-start)
- [Testing](#testing)
- [Roadmap](#roadmap)
- [Repository layout](#repository-layout)
- [Key design decisions](#key-design-decisions)
- [Documentation](#documentation)
- [License](#license)

## What runs today

### Unmodified Windows programs

These are official release builds, run as shipped; every fix that made them
work is in NovaOS.  "Tested" is what has been checked in QEMU.

<!-- BEGIN generated:programs -->

| Program | Kind | Tested on NovaOS |
|---|---|---|
| **7-Zip 26.03** (x64) | GUI installer, file manager, `7zG`, `7z.exe` | Installs; the file manager browses, opens archives, adds and extracts with the full dialogs, and drags files out of archives and folders onto other programs; Options has all six pages.  The App Store uses `7z.exe` to unpack downloads. |
| **7-Zip self-extractors** (x86) | 32-bit console and GUI SFX | Unpack an archive. |
| **NSIS installers** (x86, Modern UI) | 32-bit setup programs | Welcome, folder, progress and finish pages; files, registry, desktop and Start menu shortcuts; the uninstaller removes it all. |
| **MinGit 2.47** | Git for Windows (console) | `init`, `add`, `commit`, `log` and `diff` (paged by NovaOS's `less`), `status`, `checkout -b`, `merge`, `gc`, `fsck`, and `clone`/`fetch`/`push` between local repositories. |
| **MSYS2 runtime** (MinGit's `usr\bin`) | `sh.exe` (bash), `ls`, `cat`, `wc`… | Interactive `sh --login -i` sessions in the Terminal (prompt, line editing, colours); `sh -c` with pipes, `$(...)`, subshells, globbing, `fork`, `/dev/null`. |
| **Neovim 0.10 and 0.11** (x64 `.zip`) | Full-screen terminal editor (libuv, LuaJIT) | Opens a file, edits it, `:wq` saves it and exits with code 0; `nvim -l` scripts, `vim.system`, `jobstart` and RPC to an embedded `nvim`. |
| **Eclipse Temurin 21** | Java JRE `.msi`, JDK `.zip` | `java -version`, a threads/exceptions/files stress test, `javac` compiling a program that then runs. |
| **.NET 10** | Runtime and host from NuGet, Roslyn | `dotnet --info`, `dotnet hello.dll`, `dotnet csc.dll` compiling a C# test that passes; globalization through ICU, so German and Japanese numbers, dates, names and sorting come out as on Windows (`tests/dotnet/culturetest.cs`). |
| **Node.js 24** | `.msi`, `.zip` | `node -v`, `-e`, `npm -v`, a crypto/fs/JSON/timers test script; the `.msi` runs its 64-bit and 32-bit custom actions, makes its Start menu shortcuts and uninstalls. |
| **Windows Installer packages** | 7-Zip, CMake, Node.js, Temurin, KeePassXC `.msi` | 7-Zip and CMake install through their own wizards (licence, options, feature tree, progress), CMake's dialogs running its DLL custom actions; custom-action DLLs run in 64-bit and 32-bit custom-action servers; shortcuts and a test service are created and removed again by `msiexec /x`; transforms (`TRANSFORMS=`), whole-file patches (`msiexec /p`, `/uninstall`) and rollback of a failed install; automatic services start at boot; JScript and VBScript custom actions run on `msiscript.dll`. |
| **Python 3.14** | NuGet package | `-c`, a hashlib/JSON/regex/threads/subprocess test script. |
| **NumPy 2.5.3** | The Windows wheel (`cp314`) in Python 3.14, on NovaOS's own `vcruntime140`, `msvcp140` and UCRT | `import numpy`, complex `exp`/`sqrt`/`log`/`sin`/`tanh`/`power`, `linalg.inv`, `fft`, `eigvals`: the same output as NumPy on Linux. |
| **Mesa 3D 24.2.4** (mesa-dist-win) | `opengl32.dll` (llvmpipe) and the Vulkan driver (lavapipe), x64 and x86, from the App Store | OpenGL 4.5: `tools/gltest` (pixel formats, immediate mode, GLSL, read-back, animated `SwapBuffers`) passes as a 64-bit and a 32-bit program. |
| **DXVK 2.5.3** | `d3d8`, `d3d9`, `d3d10core`, `d3d11` (as `d3d11_dxvk`, behind NovaOS's own `d3d11`), `dxgi`, x64 and x86, from the App Store, on Mesa's Vulkan and NovaOS's own `vulkan-1.dll` | Direct3D 9 and 11: `tools/d3dtest` (device creation, a D3D9 triangle, D3D11 clear, read-back, animated `Present` in a window) passes as a 64-bit and a 32-bit program. |
| **Notepad++ 8.8.3** (x64 portable) | Scintilla editor, static MSVC C++ runtime | Opens with its menus, toolbar, tab bar, editor and status bar, takes typing, and opens and saves files through the common file dialogs. |
| **SumatraPDF 3.4.6** (x86 portable) | PDF reader on MuPDF; GDI+ toolbar, tabs and caption | Opens a PDF and renders its pages; the toolbar, tabs and menus draw; printing reports no printer. |
| **WinMerge 2.16.50** (x64) | MFC application: MDI frame, docking bars, rebars, toolbars with 24-bit image strips | Compares two files side by side with the differences highlighted, location pane and status bars. |
| **PuTTY 0.81** (x64, built from source with MinGW) | Terminal emulator on `WSAAsyncSelect` networking | A raw connection to a host: the server's greeting shows, typed lines go out and the echo comes back. |
| **ripgrep, fd, bat, jq, fzf** | Rust (MSVC), C (MinGW), Go | Searching, walking folders, printing files, filtering, from the Terminal. |
| **Floorp 12.19** (Firefox 157 engine, x64) | Gecko browser | Starts, creates its profile, and draws the full browser window (toolbar, address bar, sidebar) with DirectWrite text through its GPU process, and takes keyboard input.  Its sandboxed child processes (tab, extension, GPU, network, media) start and talk to the main process.  It fetches and shows pages over HTTP and HTTPS, scrolls and takes typing in forms.  See [Firefox](docs/HISTORY.md#firefox-floorp). |
| **Firefox 157.0** | Mozilla's full installer (x64), from the App Store | The App Store's Install button unpacks it with 7-Zip into `C:\Programs\Mozilla Firefox`; Firefox starts unmodified (its launcher process sets up the browser process with its DLL blocklist hooks), draws its whole window through its GPU process, and loads and shows a page over HTTPS (a test CA trusted through `distribution\policies.json`); in the nightly corpus.  Window titles show their dashes as `-`. |
| **KeePassXC 2.7.12** | Portable zip (Qt 5, x64) | Opens and unlocks a KDBX 4 password database (Argon2d) and shows its groups, entries and an entry's details, Qt's widget text drawn through `GetGlyphOutline`; in the nightly corpus.  Windows Hello quick unlock reports "not supported". |
| **Inkscape 0.91** | conda-forge's win-64 package (GTK 2, MinGW, x64) | Starts with a new document: menus, tool bars, toolbox, rulers, canvas, palette and status bar, drawn by GTK 2 through cairo and pango on gdi32, with its tools' own pointers; in the nightly corpus.  Inkscape 1.x (GTK 3) is not tested yet: its downloads are not reachable from CI. |
| **VLC 3.0.21** (x86, the PortableApps package) | Qt 5 media player | Plays an H.264 and AAC MP4 with its Qt interface, the video in its window (GDI output) and the sound through WASAPI; part of the nightly app corpus. |
| **Audacity 3.7.4** (x64 zip) | wxWidgets audio editor | Records from the microphone (WASAPI capture), draws the waveform, stops and saves the project as an `.aup3` (SQLite) through its save dialog; part of the nightly app corpus. |

<!-- END generated:programs -->

### Built in

- **Desktop**: a Windows 11-style shell with a Start menu (live search over
  apps, settings and files), dock, tray, snapping and resizing windows,
  Alt+Tab, right-click menus, three wallpapers.
- **Apps**: Terminal, File Explorer, Notepad, Settings, Calendar, Photos,
  the **App Store** and **Install NovaOS** (Setup).
- **Web browser**: NetSurf 3.11, built from source as a Windows program,
  with HTTPS (TLS 1.3/1.2), JavaScript (pages a script changes are laid
  out again) and SVG images, in a window you can resize, maximize or snap
  (the page is laid out again to fit).
- **Command line**: the Terminal's own commands (`dir`, `copy`, `ping`,
  `curl`, `wget`, `certutil`, `tasklist`, `trace NAME`, `vol`, `sync`…)
  and NovaOS's `cmd.exe` with batch files, plus `find`, `findstr`, `sort`,
  `more`, `less` (git's pager), `timeout`, `taskkill`, `reg`, `regsvr32`, `msiexec` and
  `intl` (the user's regional format).

### The App Store

The dock's App Store downloads the official 64-bit packages of 21 open-source
programs (Firefox, VLC, LibreOffice, GIMP, Notepad++, PuTTY…) and five
runtimes, and installs them with 7-Zip, NovaOS's Windows Installer or the
program's own setup.  `store install NAME` in the Terminal does what the
row's button does (CI installs Mesa 3D and DXVK that way).  Most of those programs still need more of Windows than
NovaOS has (more of the GUI); the ones in the table
above are the ones verified.  See [the App Store](docs/HISTORY.md#the-app-store).

## What is inside

A one-paragraph tour; [docs/HISTORY.md](docs/HISTORY.md) has the details of
every part, phase by phase.

<!-- BEGIN generated:inside -->

- **Boot**: a UEFI bootloader (a PE32+ EFI application) loads the ELF kernel
  from the EFI System Partition, or from a CD.
- **Kernel** (`kernel/`): NT-style executive: object manager and handles,
  processes and threads, virtual memory with sections and guard pages, I/O,
  registry, security tokens (restricted tokens, impersonation) and
  security descriptors checked when named objects and files on drive C:
  are opened.  SMP with per-core scheduling and fine-grained
  locks; wait queues; APCs; pipes; the NT system-call table at Windows 10
  1903 numbers.  Timers are the local APIC's, one-shot or TSC-deadline and
  calibrated against the HPET, so `Sleep(1)`, wait timeouts and waitable
  timers end within a fraction of a millisecond even with every CPU busy;
  a thread woken by a timer preempts the running one instead of waiting
  for its time slice to end.  NT's priority boosts: a thread woken by an
  event, a lock, I/O, a window message or input runs above its base
  priority (+1 to +6) and preempts busy threads of that priority, then
  decays back one level per quantum; a balance set lifts threads that
  have starved for 3 s.
- **Drivers**: AHCI SATA and NVMe disks (NovaOS installs to and boots from
  either), FAT16/FAT32, GPT, NTFS (read, write and format: drive C: with
  file ACLs and hard links, and other drives); Intel e1000/e1000e and virtio-net network
  cards; Intel High Definition Audio (playback and recording) with a kernel
  mixer;
  PS/2 keyboards and mice; USB (xHCI, EHCI, OHCI and UHCI controllers, any
  number of each) with hubs and HID keyboards (lock-key LEDs and media
  keys included), mice (five buttons and both wheels), tablets, pens
  (pressure, barrel buttons and eraser, for Wintab) and
  multi-touch screens (report protocol), USB sticks (FAT and NTFS, as
  the next drive letter, hot-plugged) and USB speakers, headsets and
  microphones (USB Audio Class 1 over isochronous transfers, played on and
  recorded from as soon as they are plugged in); virtio multi-touch screens; CMOS clock; a VBE display
  driver for QEMU's standard VGA, QXL, virtio-vga and VMware adapters,
  bochs-display and VirtualBox (resolutions switched at run time, page
  flipping, the mode set again after sleep and kept across restarts; more
  adapters, such as QEMU's secondary-vga, are more monitors of one
  desktop, arranged in Settings, each able to show DPI-aware programs
  its own DPI) and a Cirrus GD5446 one, with
  the UEFI framebuffer as the fallback; a 2D virtio GPU driver for
  QEMU's virtio-vga and virtio-gpu-pci, whose outputs are several
  monitors on one card, plugged in and unplugged while NovaOS runs; ACPI power-off, reset, power buttons,
  sleep (S3), batteries and AC adapters, the lid, thermal zones, wake
  devices and PCI interrupt routing (AML interpreted by uACPI, with the SCI
  a real interrupt through the I/O APIC).
- **Networking**: lwIP (TCP/IP over IPv4 and IPv6: DHCP, SLAAC, DNS over
  either), an HTTP/1.1 client, and Mbed TLS with the Mozilla root store.
  The loopback interface carries 127.0.0.1 and ::1 (and traffic to the
  machine's own addresses), and `localhost` resolves to both without DNS.
  Winsock (`ws2_32`, and `wsock32` with Winsock 1.1's ordinals) speaks IPv6
  and dual-stack sockets with `getaddrinfo`;
  `winhttp` is a real HTTP client over Schannel TLS, with HTTP/2 by ALPN
  (nghttp2).
- **Windows userland** (`userland/`): about 35 system DLLs written from
  scratch and compiled with clang for `x86_64-pc-windows-msvc`, and again
  for `i686` in `SysWOW64`: `ntdll`, `kernel32`, `msvcrt`/`ucrtbase` with
  the `api-ms-win-crt-*` API sets (`errno` and the rest of the C
  runtime's per-thread state kept per thread, as on Windows, with
  `_configthreadlocale` per-thread locales and `getenv` results other
  threads cannot overwrite; `ntdll` gives fiber-local storage its own
  slots and runs `FlsAlloc` callbacks when a thread ends), `vcruntime140`/`vcruntime140_1` (C++
  exceptions, FH3 and FH4 tables), `msvcp140` and its satellites (the C++
  standard library: Microsoft's own STL, compiled with clang, with
  Boost.Math under `msvcp140_2`'s special math functions),
  `user32`/`gdi32` (a real window system, controls, menus, dialogs, MDI,
  hooks, per-monitor DPI awareness with `WM_DPICHANGED`), `gdiplus` (GDI+ on the MIT-licensed plutovg rasteriser),
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
  `wintab32` (Wintab pen tablets: pressure for GTK, Qt and Krita), and
  the DLLs Firefox delay-loads (`d3d11`, `credui`, `winspool.drv`,
  `dhcpcsvc`, `d3dcompiler_47`), and more.
- **Text**: `novatext.dll`, the text core built once and shared, carries
  HarfBuzz (shaping) and FreeType (fonts).  Uniscribe (`usp10`) itemizes
  text by script and direction and shapes it with HarfBuzz, and GDI's
  `ExtTextOut` sends complex scripts through it, as Windows' LPK does, so
  Arabic, Hebrew and the Indic scripts join, reorder and run right to left.
  Arabic and Devanagari draw with Noto Sans; GDI falls back to them by
  script.  DirectWrite (`dwrite`) lays text out on the same core, with
  font fallback: `tools/dwtest` draws Latin, Arabic and Devanagari in one
  line from a Latin-only font.
- **2D drawing**: Direct2D (`d2d1.dll`) draws in software: geometries
  (rectangles, ellipses, paths with Béziers and arcs, groups, transforms,
  combining, widening, tessellation), strokes with caps, joins and dashes,
  solid, gradient and bitmap brushes, layers and clips, on HWND, DC and
  bitmap render targets.  Text goes through DirectWrite's text formats and
  layouts.
- **Media tools**: a current Windows build of ffmpeg runs unchanged and
  converts H.264 and AAC to VP9 and Opus with all its threads.
- **Media players and editors**: VLC plays video with sound and Audacity
  records and saves, on the same WASAPI, GDI and message-loop code paths
  real Windows programs take (Qt's `WH_GETMESSAGE` hook, wxWidgets'
  buffered painting).
- **ICU** as Windows 10 ships it: `icu.dll` (ICU 77.1, x64 and x86,
  built by `tools/build_icu.py`) with its data in
  `C:\Windows\Globalization\ICU`.  .NET does its globalization through
  it, and kernel32 answers `GetLocaleInfoEx` from it for every one of the
  864 Windows locales and formats dates, times, numbers and money in them
  (`GetDateFormat`, `GetTimeFormat`, `GetNumberFormat`,
  `GetCurrencyFormat`, `GetDurationFormat`) in their calendars (Japanese
  eras, Thai Buddhist, Hebrew, Hijri, Um Al Qura, Persian...).  The user's
  regional format is set with `intl NAME` or Settings > Time & language and
  kept in the registry, with the user's own changes to it
  (`SetLocaleInfo`).
- **Program support**: the PE loader with TLS, `DllMain`, forwarders and
  API sets; x64 and x86 structured exceptions; registry saved to disk;
  COM in-process servers and type libraries; drag and drop; a shared clipboard; `.lnk`
  shortcuts; Windows Installer packages; services (`advapi32`'s service
  control manager).

<!-- END generated:inside -->

## Quick start

Linux (Ubuntu/Debian) is the build host; on Windows use WSL2.

Install the prerequisites, build the bootloader, the kernel with the whole
userland inside, and `nova.img`, then run it in QEMU:

```bash
sudo apt install cmake nasm clang lld llvm python3 \
                 qemu-system-x86 ovmf mtools dosfstools xorriso
mkdir build && cd build
cmake ..
make -j$(nproc)
cmake --build . --target run
```

The first build compiles the NetSurf browser (about 800 files; later builds
reuse the objects).  Set `NOVA_NO_NETSURF=1` to leave the browser out, and
`NOVA_NO_WOW64=1` to skip the 32-bit userland.  `run` attaches a second
disk, `build/nova-data.img`, where drive C: is kept across restarts and
rebuilds.  [docs/building.md](docs/building.md) covers the build, QEMU
options, putting your own programs on the disk, and debugging.

### Bootable ISO and installing on a disk

A ready-to-boot UEFI ISO, `nova.iso`, is built by CI rather than committed.
Download the one built from `main` from the
[latest release](https://github.com/dean-plude/os/releases/latest/download/nova.iso), or the `nova-iso` artifact of any pull request's
CI run (the **Artifacts** list on the run's Summary page).  It is also the installation disc: booted from it, NovaOS runs live and
opens **Install NovaOS**, which writes a GPT disk with an EFI System
Partition and a data partition for drive C:.

```bash
truncate -s 1G disk.img
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -drive file=disk.img,format=raw -cdrom nova.iso
```

After installing, the same command without `-cdrom nova.iso` starts from
`disk.img`.

Give the machine 2 GB so downloaded installers fit in drive C: (which lives
in memory and is saved to disk).  On macOS with Homebrew QEMU, the firmware
ships with QEMU:

```bash
FW="$(brew --prefix qemu)/share/qemu/edk2-x86_64-code.fd"
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file="$FW" \
  -cdrom nova.iso -serial stdio
```

To make the ISO yourself from a fresh build, run
`scripts/create-iso.sh nova.iso build/bootx64.efi build/kernel.elf`
(needs `xorriso`).  `*.iso` is in `.gitignore`: the ISO is never committed.

## Testing

**Every pull request is boot-tested.**  GitHub Actions
(`.github/workflows/ci.yml`) builds the kernel, bootloader, userland and
`build/nova.img`, boots it in QEMU with OVMF and runs three suites with
`tools/selftest.py`:

- **Build and boot-test** (core): <!-- BEGIN generated:core-tests -->`apitest`, `abitest` (the PEB, TEB, `KUSER_SHARED_DATA`, `CONTEXT` and
  loader layouts, ntdll's stubs and the system-call numbers, against
  Windows 10 1903 x64), `filetest`, `linktest`, `pipetest`, `proctest`,
  `sectest`, `acltest` (64- and 32-bit), `guitest auto`, `inputtest`
  (side buttons, horizontal wheel, volume keys), `usbcheck` (media keys,
  AC Pan, pen pressure), `anitest` (animated cursors and program
  pointers), `cursortest` (system pointers, SetSystemCursor, cursors at
  the display scale), `bmpcurtest` (1-, 4-, 8- and 16-bit DIB sections,
  cursors from bitmaps with alpha or monochrome masks), `wintabtest`
  (wintab32 with no pen and with a synthetic one: contexts, packets,
  pressure), `dpitest` (per-monitor DPI: the manifest's `dpiAwareness`,
  `GetDpiForMonitor`, `GetDpiForWindow`, `WM_DPICHANGED` and its
  suggested rectangle when the monitor goes to 192 DPI and back, and the
  window and monitor coordinates an aware, an unaware and a system-aware
  process see), `disptest`, `icutest` (ICU and locales, 64- and 32-bit),
  `comtest`, `nlstest` (date, time, number and currency formats in
  German and Japanese, 64- and 32-bit; `nlstest user`), `nlstest
  calendars` (Japanese eras, Buddhist, Taiwan, Tangun, Hebrew, Hijri, Um
  Al Qura and Persian dates; `GetDurationFormat`) and `nlstest override`
  (`SetLocaleInfo`), 64- and 32-bit, `tlbtest` (type libraries, 64- and
  32-bit), `usptest` (Arabic and Devanagari shaped through Uniscribe and
  drawn by `ExtTextOut`, 64- and 32-bit), `cppeh`, `dlgtest` (the common
  file dialogs: `OPENFILENAME` settings, `IFileDialog` objects),
  `stltest` (the C++ standard library, 64- and 32-bit), `delaytest` (the
  DLLs Firefox delay-loads), `rttest` (complex math, conio, DLL
  directories, 64- and 32-bit), `qttest` (what Qt programs need:
  `GetGlyphOutline`, C++/WinRT `HSTRING`s, Windows Hello, the process
  DACL), `battery` (against the battery in `tests/acpi/battery.asl`),
  `soundtest` (the recorded WAV must hold the tones played), `soundtest
  record`, `capture` and `volume` (`waveIn` and WASAPI capture must
  record the tone the microphone hears, and a quarter of the endpoint
  volume must sound 12 dB quieter), `soundtest dsound` and `dscapture`
  (DirectSound playback with notifications, frequency and volume, and
  DirectSoundCapture recording the microphone), `sleeptest timer`
  (`Sleep(1)`, 1 ms wait timeouts and waitable timers (periodic ones,
  their completion routines and timer queue timers too) end within a
  millisecond with every CPU busy), `savetest` (while NovaOS saves 32
  MiB of drive C: to its disk, calls behind the kernel, desktop and
  file-system locks keep answering, the file-system one within 100 ms;
  the save holds the file-system lock under 20 ms), `xa2test` (XAudio2
  2.9 and 2.7 voices, callbacks and effects, and X3DAudio panning),
  `powertest` (closing the lid in `tests/acpi/lid-thermal.asl` sleeps, a
  USB key and the lid wake it, the thermal zone's readings), `disptest
  1024 768` (saves the mode the restart must keep), `miditest` (MIDI
  through `midiOut`, `midiStream` and the MCI sequencer), `nlstest set
  ja-JP` (the user locale the restart must keep), an installer that
  replaces a running program and finishes after a restart (`filetest
  install`, `shutdown /r`, `filetest installed`), `nlstest after-restart
  ja-JP` (the restart kept the user locale), `disptest saved 1024 768`
  (the restart kept the saved display mode), hard links kept across a
  restart (`linktest restarted`), Windows Installer transforms, patches
  and rollback, and an installed service started at the next boot
  (`msitest transform`, `patch`, `rollback`, `service`, `shutdown /r`,
  `msitest service-boot`), Windows Installer JScript and VBScript custom
  actions setting and reading properties and writing files, and a
  failing script rolling its install back (`msitest script`), `smftest`
  (the C++17 special math functions in `msvcp140_2.dll`, 64- and
  32-bit), `errnotest` (errno and the other C runtime per-thread state
  belong to each thread, 64- and 32-bit), a power cut in the middle of
  saving drive C: (the FAT holds chains no file reaches; at the next
  boot NovaOS frees them and has as much free space as `fsck.fat`
  finds), `boosttest` (a woken thread is boosted above its base priority
  and runs within 2 ms while threads of the same priority spin on every
  processor; the boost decays back to base, 64- and 32-bit),
  `crtthreads` (FLS callbacks, per-thread locale and thread-safe
  `getenv`, 64- and 32-bit), and last `crash kernel`, a deliberate
  kernel fault whose serial log must show a backtrace with function
  names.<!-- END generated:core-tests -->
- **Network** (in the boot-test job): two boots with a virtio-net card.
  On QEMU's user network, `ipconfig`, `ping`, Winsock over IPv4 and
  `httptest suite` (winhttp with HTTP/2 by ALPN) against
  `tools/h2server.js`, and `looptest` (Winsock over 127.0.0.1 and ::1);
  on an IPv6-only network that is `tools/v6peer.py`,
  SLAAC and RDNSS, `ping -6`, `curl -6` and Winsock over IPv6.
- **Graphics tests**: `tools/d2dtest` (Direct2D geometry answers, and a
  scene that must match the reference `tools/d2dtest/reference.py` draws
  with Skia), then installs Mesa 3D and DXVK with the App Store
  (`store install NAME` in the Terminal; `tools/ci/stage-graphics.sh`
  stages the downloads) and runs `tools/gltest` (14 tests) and
  `tools/d3dtest` (17 tests); each 64- and 32-bit, with a screenshot of
  each while it draws.

The build compiles through ccache, and the boot-test job saves the cache
after each build, so a pull request recompiles only what it changed.  A pull
request that changes only Markdown, `docs/` or `LICENSE` skips the build and
boot jobs (GitHub counts a skipped job as passing a required check).

A failing test fails its check; each run's summary has a table of results,
and the serial logs and screenshots are kept as artifacts, along with the
bootable ISO (`nova-iso`).  When a push to `main` passes both suites, the
**Publish nova.iso** job puts that ISO on the `latest` release.  Run the same
gates locally with `python3 tools/selftest.py` (and `--suite network`, `--suite graphics`)
after a build.

**Every night, real programs.**  `.github/workflows/nightly.yml` builds
main and runs `tools/appcorpus.py`: the official Windows x64 releases of
<!-- BEGIN generated:corpus -->ripgrep, fd, jq, 7-Zip, MinGit (cloning a repository), Python, Node.js, .NET (German and Japanese formatting through ICU), ffmpeg (an MP4 converted to WebM), SumatraPDF, WinMerge, KeePassXC, VLC (plays an H.264 and AAC MP4 with sound), Audacity (records 10 s from the microphone and saves the project), Inkscape, Firefox, Notepad++ and PuTTY<!-- END generated:corpus -->.  The
windowed programs run last, one at a time: SumatraPDF opens a PDF,
WinMerge compares two files, Firefox installs from the App Store and
loads a page from an HTTPS server on the host, Notepad++ opens a file and PuTTY makes a raw
connection to an echo server on the host and types a line; each one's
screenshot must match its `tests/reference/NAME.png`.
It also checks NovaOS's own screens: `dir` on C: and on an NTFS drive D:
(each with its own free space) and File Explorer's This PC listing both.
It posts a pass/fail table per program to the "Nightly app corpus" issue.

- **Self-test programs** in `userland/programs/`, installed in
  `C:\Programs` (and 32-bit builds in `C:\Programs\x86`).  Run them from the
  Terminal; each prints "N passed, 0 failed": <!-- BEGIN generated:selftest-programs -->`abitest`, `acltest`, `anitest`, `apitest`, `bmpcurtest`, `boosttest`, `cliptest`, `comtest`, `cppeh`, `crttest`, `crtthreads`, `cursortest`, `delaytest`, `disptest`, `dlgtest`, `dlltest`, `dpitest`, `errnotest`, `filetest`, `httptest`, `icutest`, `inputtest`, `linktest`, `looptest`, `montest`, `msitest`, `nlstest`, `nstest`, `pipetest`, `posixtest`, `powertest`, `proctest`, `qttest`, `rttest`, `savetest`, `sectest`, `shmtest`, `smftest`, `smpstress`, `stltest`, `threads`, `touchtest`, `usptest`, `wintabtest`<!-- END generated:selftest-programs -->.  `soundtest`
  plays tones through `waveOut`, WASAPI, `PlaySound` and `Beep`, and records
  through `waveIn` and WASAPI capture;
  `tools/novarun.py --wav out.wav` records what NovaOS plays, `--rec in.wav`
  feeds a WAV to its microphone, and
  `tools/wavcheck.py out.wav` lists each tone's length and pitch.  `disktest
  write`, a restart and `disktest verify` check that drive C: survives a
  reboot.
- **GUI and interactive checks**: `winhello`, `guitest` (`guitest auto`
  drives its own menus, dialog, message box and property sheet and
  reports), `droptest`, `cpus` (SMP speed-up).
- **Real programs** are tested from a second disk image holding them
  (not in this repository), driven by a QEMU harness that types, clicks and takes screenshots.
- **On the host**: `tools/pe_imports.py PROGRAM.exe` lists the imports a
  Windows program needs that NovaOS's DLLs lack; `tools/msitest/` exercises
  the Windows Installer's package readers and SQL, and builds test
  packages (one with a service).  `tools/gltest/`,
  `tools/d3dtest/` and `tools/d2dtest/` are OpenGL, Direct3D 9/11 and
  Direct2D test programs, built with MinGW, for checking Mesa, DXVK and
  `d2d1.dll` on NovaOS.
- **Debugging**: the serial log (COM1) has every kernel message; the
  Terminal's `dmesg` shows it, and `trace NAME` logs a program's failing
  system calls.  A kernel fault, panic or failed assertion prints a
  backtrace with function names and offsets (the kernel carries its own
  symbol table); `crash kernel` shows one on purpose.

See [docs/building.md#tests](docs/building.md#tests) for how to run them.

## Roadmap

| Phase | Focus | Status |
|-------|-------|--------|
| 1 | Boot and kernel foundation (UEFI, memory, interrupts, scheduler) | ✅ Done |
| 2 | NT kernel personality (Object Manager, syscalls, registry) | ✅ Done |
| 3 | I/O subsystem (IRPs, section objects, PE loader) | ✅ Done |
| 4 | VFS and InitRD | ✅ Done |
| 5 | Process isolation (per-process page tables, KPCR, PEB/TEB) | ✅ Done |
| 6 | User-mode foundation (syscall thunks, Win32 helpers, CSRSS shim) | ✅ Done |
| 7 | GUI (GDI renderer, window manager, desktop shell) | ✅ Done |
| 8 | Interactive desktop, built-in apps, networking, HTTPS | ✅ Done |
| 9 | Windows programs in ring 3: loader, threads, TLS, SEH, sockets, GUI; the NetSurf browser (9.5) | ✅ Done |
| 10 | Standard DLLs (UCRT, C++ EH, advapi32, shell32…), registry, COM, persistent storage | ✅ Done |
| 11 | Multiprocessor: every core runs threads, fine-grained kernel locking | ✅ Done |
| 12 | Win32 GUI subsystem; unmodified 7-Zip; App Store; Windows Installer; installing NovaOS | ✅ Done |
| 13 | 32-bit programs (WoW64); NSIS installers; `.lnk` shortcuts | ✅ Done |
| 14 | Pipes, `cmd.exe`, shared clipboard, Git and MSYS2, the Java/.NET/Node.js/Python runtimes | ✅ Done |
| 15 | 3D graphics on the CPU: OpenGL 4.5 (Mesa llvmpipe), Vulkan 1.3 (lavapipe), Direct3D 8–11 (DXVK) | ✅ Done |
| 16 | Sound: Intel HD Audio, a kernel mixer, `waveOut`/`PlaySound`/`Beep`, WASAPI | ✅ Done |

What comes next (broader app coverage, the GPU, the
remaining kernel and API gaps, storage and hardware) is in
[docs/ROADMAP.md](docs/ROADMAP.md).

## Repository layout

```
os/
├── bootloader/           # UEFI bootloader (PE32+ EFI application)
├── include/              # Headers shared by bootloader and kernel (boot_protocol.h)
├── kernel/               # NT-style kernel (freestanding ELF64)
│   ├── arch/x86_64/      # GDT, IDT, APIC, paging, SMP trampoline, syscall entry
│   ├── ke/               # Startup, scheduler, SMP, wait queues, KPCR, syscall table
│   ├── mm/               # Physical pages, kernel heap, VMAs, sections
│   ├── ob/ ps/ se/ cm/ io/  # Object, process, security, configuration, I/O managers
│   ├── um/               # Windows programs: processes, threads, loader, NT services,
│   │                     #   WoW64, pipes, registry, sockets, windows, consoles
│   ├── fs/               # VFS, RAM disk (drive C:), FAT16/32, saving C:, Setup engine
│   ├── drivers/          # AHCI (SATA), NVMe, e1000/e1000e, virtio-net, USB core, xHCI/EHCI/OHCI/UHCI, hubs, HID, mass storage
│   ├── hal/              # Serial, framebuffer, display (VBE), PCI, PS/2, CMOS clock, HPET, I/O APIC, ACPI (uACPI host)
│   ├── net/              # lwIP port, HTTP client, TLS (Mbed TLS)
│   ├── gdi/              # Software renderer, fonts, ICO and PNG decoding
│   ├── wm/               # Window manager, desktop shell, input, clipboard
│   ├── apps/             # Built-in apps: Terminal, Explorer, Notepad, Settings,
│   │                     #   Calendar, Photos, App Store, Setup
│   ├── ldr/              # Early PE loader and syscall thunk pages
│   └── lib/              # Freestanding string and memory library
├── userland/             # The Windows userland, built with clang + lld-link:
│   ├── ntdll/ kernel32/ user32/ gdi32/ ...   # one directory per system DLL (dll.json)
│   ├── msvcrt/ crt/      # C runtime (msvcrt.dll and ucrtbase.dll), program startup
│   ├── msi/              # Windows Installer (msi.dll)
│   ├── programs/         # cmd.exe, msiexec, reg, find..., samples and self-tests
│   ├── netsurf/          # NetSurf port: fetcher, window surface, fonts
│   └── include/          # The Windows SDK headers NovaOS provides
├── third_party/          # lwIP, Mbed TLS, nghttp2, uACPI, musl (libm), HarfBuzz, FreeType, NetSurf, stb, fonts, ICU (icu.dll + data), 7-Zip installer
├── tools/                # Host tools: build_userland.py, build_netsurf.py, mkfont,
│                         #   make_icons.py, mkani.py, pe_imports.py, msitest/, docgen.py
├── tests/                # CI self-tests and app corpus (one file per test), ACPI
│                         #   tables, reference screenshots
├── scripts/              # build.sh, run-qemu.sh, create-disk.sh, create-iso.sh
└── docs/                 # Building, roadmap, feature history, Phase 1 architecture,
                          #   and the fragments README and docs/*.md are built from
```

## Key design decisions

- **Clean-room, ABI-faithful (the "hybrid" path)**: every DLL and service is
  written from scratch, but syscall numbers, `NTSTATUS` codes and structure
  layouts (PEB, TEB, loader data, `CONTEXT`…) follow Windows 10 1903 x64, so
  unmodified binaries find what they expect.  See
  [docs/ROADMAP.md](docs/ROADMAP.md#the-pivotal-decision-how-to-get-the-win32-api-surface).
- **Native execution, both bitnesses**: x64 code runs in long mode; x86 code
  runs in compatibility mode with a 32-bit `ntdll` that converts each system
  call to the kernel's 64-bit form, the way Windows' WoW64 does.
- **The window system lives in the program**: `user32` keeps each program's
  window tree; the kernel's window manager composites only top-level
  windows, drawn from bitmaps the programs own.  The kernel also draws the
  pointer: a program's `SetCursor` shape (animated .ani cursors included)
  over its own windows, the desktop's arrow elsewhere, and the system
  pointers (I-beam, busy ring, resize arrows on window edges, hand, cross,
  "no" and the rest of `IDC_*`) from vector outlines, sharp at 200 %.
- **Software rendering**: GDI is a CPU rasterizer drawing into a back
  buffer in RAM at integer HiDPI scale.  On QEMU's standard VGA, QXL,
  virtio-vga and VMware adapters (and Bochs, VirtualBox's VBoxVGA) a VBE
  "DISPI" driver sets the resolution at run time (Settings > Display,
  `ChangeDisplaySettings`) and flips between two pages of video memory
  when both fit; Cirrus gets 800x600 and 640x480; elsewhere frames are
  copied to the UEFI framebuffer in the boot mode.  There is no 3D GPU driver.
- **Drive C: in memory, saved to FAT**: the RAM disk is saved to a FAT32
  volume a second after each change and restored at boot.  System files
  come from the kernel image, so a new build always brings its own.  The
  save runs on its own thread and holds no lock while the disk is written
  (`savetest`), and a crash during a FAT save leaves each file old or new.
- **SMP with fine-grained locks**: the scheduler, memory,
  synchronization, sockets, the GUI, the program loader, files, the
  registry, the console and starting processes have their own locks, and
  file and registry throughput scale about 3x from one CPU to four
  (`smpstress scaling 3`); the SATA, NVMe and USB disk drivers take a lock
  per disk; the rest of the kernel keeps a big lock (rules
  and lock order in `kernel/ke/smp.h`; the Terminal's `profile` command
  shows where the CPUs spend their time).
- **Kernel-helper syscalls (0x01F0–0x01FF)** are private to NovaOS's own
  DLLs and invisible to Windows programs.

## Documentation

- [CONTRIBUTING.md](CONTRIBUTING.md): where a change goes (one file per
  DLL, test and doc item, so parallel pull requests do not conflict) and
  how to merge main into a branch.
- [docs/building.md](docs/building.md): building, running, the data disk,
  tests, debugging.
- [docs/macos.md](docs/macos.md): running and building on a Mac (Apple
  Silicon and Intel).
- [docs/ROADMAP.md](docs/ROADMAP.md): the compatibility strategy and what
  comes next.
- [docs/HISTORY.md](docs/HISTORY.md): what every phase added, in detail.
- [docs/phase1-architecture.md](docs/phase1-architecture.md): the boot flow,
  address-space layout and early kernel design.

## License

NovaOS is MIT licensed. The operating system (kernel, bootloader, system
DLLs, C runtime, desktop and apps) contains no GPL code; bundled third-party
code keeps its own permissive licence (<!-- BEGIN generated:licenses -->lwIP: BSD 3-clause; Mbed TLS: Apache-2.0; nghttp2: MIT; mujs: ISC; uACPI: MIT; musl's libm and complex functions: MIT; ICU: Unicode License v3 (`third_party/icu/LICENSE`); kernel32's locale table, from .NET: MIT; HarfBuzz: MIT; FreeType: the FreeType License (BSD-style; portions of this software are copyright © 2024 The FreeType Project (www.freetype.org), all rights reserved); Boost.Math (the C++17 special math functions in `msvcp140_2.dll`): Boost Software License 1.0; Microsoft's C++ standard library (STL): Apache-2.0 WITH LLVM-exception; plutovg: MIT (with FreeType-licensed rasteriser and stroker files); Inter and Cascadia Mono: SIL OFL 1.1; Noto Sans Arabic and Devanagari: SIL OFL 1.1; DejaVu Sans Mono: Bitstream Vera licence; stb_truetype/stb_image: public domain or MIT; FAudio: zlib; TinySoundFont: MIT<!-- END generated:licenses -->).  All Win32 API implementations are clean-room, based on public
Microsoft documentation, the ReactOS reference and study of Wine's source,
but independently written.

**NetSurf** (`third_party/netsurf/netsurf`) is licensed under the **GNU GPL
version 2**; its libraries are MIT, zlib and libpng licensed (see
`third_party/netsurf/NOVA-VENDOR.txt`).  `netsurf.exe`, NetSurf linked with
the NovaOS glue in `userland/netsurf` (MIT, GPL-compatible), is a separate
program distributed under the GPL-2.0, with its complete source in this
repository; the kernel image merely carries it as a file for drive C:.
Build with `NOVA_NO_NETSURF=1` for an image without it.

**7-Zip** (`third_party/7z2603-x64.exe`) is Igor Pavlov's unmodified
installer, kept as a test case under 7-Zip's own licence
(<https://www.7-zip.org/license.txt>); it is not part of NovaOS.
