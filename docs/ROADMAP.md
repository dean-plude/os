# NovaOS — Roadmap to a Windows-Compatible Desktop OS
<!-- The regions between "BEGIN generated" and "END generated" markers are built from fragment files by tools/docgen.py: edit those files, not the regions (CONTRIBUTING.md). -->

**Goal:** a from-scratch x86-64 OS that runs **native Windows executables
without emulation**: the binaries run directly on the CPU while NovaOS
provides the NT system-call ABI, the Win32 API surface, the GUI server,
loader semantics, and the drivers they expect.

> "Without emulation" means no CPU emulation (the code is already native
> x86-64, or x86 run in compatibility mode).  It does **not** mean "without
> work": NovaOS has to reproduce the Windows *environment*.  This is the
> ReactOS problem, a clean-room Windows-compatible OS, and each step below
> is meant to be concrete and verifiable so progress is real, not
> aspirational.

---

## Where we are (Phases 1–14, done)

| Phase | What it delivered | Proof |
|-------|-------------------|-------|
| 1–6 | UEFI boot, memory, interrupts, scheduler; the NT executive (Ob, Ps, Se, Cm, Io), syscall dispatch, per-process page tables, PEB/TEB | Boots under OVMF |
| 7–8 | Software GDI, window manager, Windows 11-style desktop; PS/2 input; built-in apps; e1000 networking, lwIP, HTTP/1.1, HTTPS (Mbed TLS) | Interactive desktop; `curl https://...` |
| 9 | Real PE32+ programs in ring 3 on NovaOS's own `ntdll`, `kernel32`, `msvcrt`, `ws2_32`, `user32`, `gdi32`: threads, TLS, SEH, `DllMain`, sockets, windows | Self-tests; Win32 sample programs |
| 9.5 | The NetSurf browser built from source as a Windows program | Browses HTTP and HTTPS sites with JavaScript |
| 10 | UCRT and C++ exceptions, advapi32, shell32, COM, the registry, AHCI and FAT with drive C: saved to disk | Unmodified ripgrep, fd, jq, fzf |
| 11 | SMP: every core runs threads, per-core run queues, fine-grained locks | `cpus.exe` ~3.7× on 4 cores |
| 12 | A real Win32 window system in user32, comctl32, dialogs, menus, drag and drop, kernel sections; the App Store; Windows Installer; installing NovaOS on a disk | Unmodified 7-Zip installs and runs |
| 13 | WoW64: 32-bit programs in compatibility mode with a SysWOW64 userland; `.lnk` shortcuts | NSIS installers, 7-Zip self-extractors |
| 14 | Pipes and overlapped I/O, handle inheritance, `cmd.exe`, the shared clipboard, the MSYS2 runtime, JIT support in the loader and unwinder | MinGit (clone/fetch/push), Java 21, .NET 10, Node.js 24, Python 3.14 |

The details of each phase are in [HISTORY.md](HISTORY.md).

**Honest gaps:** the real Microsoft DLLs are not loaded (everything is
NovaOS's own clean-room code); there is no GPU (3D runs on the CPU); file
ACLs are kept only when drive C: is on NTFS (the installer's default); and most of the App Store's catalog (multimedia programs, Qt ones beyond
KeePassXC, and GTK ones beyond an older Inkscape) does not run yet.

---

## The pivotal decision: how to get the Win32 API surface

- **Path A — Reimplement (Wine-style).** Write our own `ntdll/kernel32/user32/
  gdi32/…`. Clean-room, no licensing issues, total control; but the API
  surface is vast and real apps break on missing edge cases.
- **Path B — Binary compat (ReactOS-style).** Match a *specific* Windows
  build's NT syscall ABI exactly, then load the **real Microsoft user-mode
  DLLs**. Instant huge coverage; but brittle to version and constrained by
  licensing on redistributing Microsoft DLLs.
- **Path C — Hybrid.** Reimplement clean-room while holding ABI and
  structure compatibility so real DLLs or drivers *can* be loaded when
  desired.

> **DECISION: Path C — Hybrid.** NovaOS's components are clean-room, but
> structures (PEB/TEB/LDR, `CONTEXT`, `KUSER_SHARED_DATA`) and syscall
> numbers follow a chosen target build, **Windows 10 1903 x64**.  In
> practice every DLL so far is NovaOS's own (Path A in effect), and that has
> been enough for the programs in the README; the ABI fidelity is what lets
> unmodified binaries, runtimes and JITs find what they expect.

---

## What comes next

Ordered by what unblocks the most real programs.  Each item ends when a
named program or test demonstrates it.

### Graphics, 3D and media
<!-- BEGIN generated:next-graphics -->

- ~~**OpenGL**: a working `opengl32.dll`~~ Done: Mesa's llvmpipe from the
  App Store is the system `opengl32.dll` (OpenGL 4.5, 64- and 32-bit); see
  [OpenGL](HISTORY.md#opengl-mesa-as-the-system-opengl32dll).
- ~~**Direct3D**, DXGI~~ Done: DXVK from the App Store is the system
  Direct3D 8–11 on Mesa's lavapipe Vulkan, through NovaOS's own
  `vulkan-1.dll`; see [Direct3D](HISTORY.md#direct3d-dxvk-on-mesas-vulkan).
- ~~**Audio**: `winmm` wave output and WASAPI over a real sound device
  (QEMU's Intel HDA)~~ Done; see [Sound](HISTORY.md#sound-intel-hd-audio-winmm-and-wasapi).
  ~~Recording (`waveIn`, capture endpoints) and endpoint volume
  (`IAudioEndpointVolume`)~~ Done (19.4); see
  [Recording](HISTORY.md#recording-wavein-wasapi-capture-and-endpoint-volume).
  ~~`dsound.dll` (DirectSound), XAudio2 and MIDI~~ Done (19.5); see
  [DirectSound, XAudio2 and MIDI](HISTORY.md#directsound-xaudio2-and-midi).
  ~~Testing VLC and Audacity on it~~ Done (19.6); see [VLC and
  Audacity](HISTORY.md#vlc-and-audacity-196).
- ~~**Complex scripts**: Uniscribe shaping Arabic and the Indic scripts~~
  Done (19.1): `usp10.dll` on HarfBuzz in `novatext.dll` (HarfBuzz +
  FreeType, shared with DirectWrite and Direct2D), and GDI's `ExtTextOut`
  draws complex text through it; see
  [Complex text](HISTORY.md#complex-text-harfbuzz-freetype-and-uniscribe).
- ~~**Direct2D** (`d2d1.dll`)~~ Done (19.2): a software Direct2D, with
  text through DirectWrite's text layouts; see [Direct2D](HISTORY.md#direct2d).
- ~~**ffmpeg's imports**~~ Done (19.3): see [ffmpeg](HISTORY.md#ffmpeg).
  `ffmpeg -i in.mp4 out.webm` (H.264 and AAC to VP9 and Opus) completes,
  and the nightly app corpus runs it.
- Display: GPU-backed or at least faster blits.  ~~Mode changes~~ Done:
  run-time resolutions (Phase 12 onwards); the chosen one is kept across
  restarts, and windows a smaller mode shrank grow back when it is undone
  (Phase 18.9).  ~~More than one
  monitor~~ Done: one desktop across several display adapters, arranged in
  Settings and kept across restarts, with the Win32 monitor calls
  reporting it.  ~~Several monitors on one card, plugged in and out while
  running~~ Done: each output of a virtio GPU is a monitor, and the
  desktop and programs (`WM_DISPLAYCHANGE`) follow monitors connected or
  disconnected.  ~~Per-monitor DPI that programs see~~ Done: a monitor at
  scale 2 can show DPI-aware programs 192 DPI (Settings > Display); they
  get `GetDpiForMonitor`, per-monitor-aware contexts, `WM_DPICHANGED` and
  the screen's own pixels, while unaware ones keep 96 DPI.
- ~~NetSurf: SVG; redrawing pages a script changes after layout.~~ Done
  (Phase 19.8): SVG images (`<img>`, `<object>`, CSS backgrounds, `.svg`
  pages) drawn anti-aliased, and pages laid out again when a script changes
  the DOM, a style or a stylesheet.  Still to do: inline `<svg>` in HTML,
  and SVG text in the document's fonts.

<!-- END generated:next-graphics -->

### Application coverage
<!-- BEGIN generated:next-apps -->

- Bring the App Store catalog up program by program: ~~the "untested"
  portable ones (Notepad++, SumatraPDF, PuTTY, WinMerge)~~ Done (Phase
  20.2, in the nightly corpus); the Qt applications: ~~KeePassXC~~ Done
  (Phase 20.3, unlocks a database in the nightly corpus; see
  [Qt programs](HISTORY.md#qt-programs-keepassxc)), Krita next; the
  GTK ones: ~~Inkscape~~ Done (Phase 20.4, Inkscape 0.91, the GTK 2 build
  from conda-forge, opens a new document in the nightly corpus; see
  [GTK programs](HISTORY.md#gtk-programs-inkscape)); ~~then Firefox~~
  Done (Phase 20, stock Firefox 157 installs from the App Store and loads
  an HTTPS page in the nightly corpus; see
  [Firefox in the App Store](HISTORY.md#firefox-in-the-app-store)).
  Krita is the only program left in this item.
- ~~Common dialogs: `GetOpenFileName`/`GetSaveFileName` and the
  `IFileDialog` interfaces~~ Done (Phase 20.1, `dlgtest`); see
  [Phase 20](HISTORY.md#phase-20-common-dialogs-and-portable-programs).
- ~~Windows Installer: custom actions that run code, the packages' own
  dialogs (`InstallUISequence`), the `Shortcut` table, services, merge
  modules; LZX cabinets tested against real packages~~ Done; see
  [Windows Installer depth](HISTORY.md#windows-installer-depth-custom-actions-dialogs-shortcuts-services).
  Rollback, transforms, patches and services at boot followed in
  [Windows Installer rollback, transforms, patches](HISTORY.md#windows-installer-rollback-transforms-patches-services-at-boot).
  Script custom actions (JScript and VBScript) followed in
  [Windows Installer script custom actions](HISTORY.md#windows-installer-script-custom-actions-jscript-and-vbscript).
- ~~COM type libraries (`LoadTypeLib`), the MSVC FH4 C++ exception
  tables~~ Done; see [Type libraries and FH4](HISTORY.md#com-type-libraries-and-fh4-c-exceptions).
- ~~.NET globalization through ICU, not only NLS for English and invariant
  cultures~~ Done; see [ICU](HISTORY.md#icu-net-globalization-and-kernel32s-locales).
- Keep the App Store's per-app compatibility notes in step with what has
  been verified.
- Firefox (tested with Floorp): the browser window opens and draws
  through its GPU process, and its sandboxed child processes start; see
  [Firefox](HISTORY.md#firefox-floorp), and it loads and shows web pages
  over HTTP; it completes TLS handshakes for HTTPS, scrolls and takes
  typing in forms.  Stock Firefox 157 now installs from the App Store
  and loads an HTTPS page (a test CA trusted through `policies.json`) in
  the nightly corpus.  Still open: a page from a publicly trusted HTTPS
  site (the test network has no internet).

<!-- END generated:next-apps -->

### Kernel and API compatibility
<!-- BEGIN generated:next-kernel -->

- ~~Processes: `CREATE_SUSPENDED` for `CreateProcess`, `CREATE_NEW_CONSOLE`
  with a console of its own, file handles that share their position with
  the processes they are handed to~~ Done (Phase 17.3, `proctest`).
- ~~Small visible bugs: This PC lists D:, E:, ...; `dir` reports each
  drive's own free space; Notepad++'s status bar draws~~ Done (Phase
  17.6, screenshots in the nightly app corpus).
- ~~Files: hard links.  `MoveFileEx` pending renames carried out at boot,
  `RegNotifyChangeKeyValue` change events~~ Done (Phase 17.5, `filetest`,
  `linktest` and a restart in the core suite).
- ~~Interactive MSYS2 `sh` sessions~~ Done, with Neovim: console input
  is waitable and the Terminal runs full-screen programs (Phase 17.2).
- The Win32 console screen-buffer calls (`SetConsoleCursorPosition`,
  `FillConsoleOutputCharacter`... are still no-ops), so programs that draw
  through them rather than VT sequences work and `less` can be the real
  one (git pages through NovaOS's own `less` today).
- Pseudo consoles (`CreatePseudoConsole`, for Neovim's `:terminal` and
  Windows Terminal-style hosts) exist only as functions that fail.
- ~~Move files, the registry, process creation and the console off the
  big kernel lock~~ Done (Phase 17.7): file and registry throughput scale
  about 3x from one CPU to four (`smpstress scaling 3`, run nightly).
- ~~Security: enforce tokens and ACLs on objects~~ Done for named kernel
  objects: real tokens, restricted tokens, impersonation, and descriptors
  checked on open (Phase 17.4, `sectest`, `acltest`).  Files' descriptors
  come with Phase 18.5.
- ~~Locales: `GetDateFormat`, `GetNumberFormat` and `GetCurrencyFormat` in
  the requested locale, and a user locale other than `en-US`~~ Done:
  `GetDateFormat`, `GetTimeFormat`, `GetNumberFormat` and
  `GetCurrencyFormat` (A, W, Ex) format in any of the 864 locales from ICU's
  data, and the user locale is set with `intl NAME` or Settings > Time &
  language and kept in the registry across restarts (`nlstest`).
- ~~Locales: the user's overrides (`sShortDate` and the rest),
  `GetDurationFormat`, and calendars other than Gregorian~~ Done:
  `GetLocaleInfo` and the formatting functions answer the user's overrides
  (`SetLocaleInfo`), `GetDurationFormat`/`Ex`, and twelve calendars with
  `GetCalendarInfo`, `EnumCalendarInfo`, `EnumDateFormats` and
  `DATE_USE_ALT_CALENDAR` (`nlstest calendars`, `nlstest override`).
- .NET: an unhandled managed exception prints "Stack overflow." instead of
  the exception and its stack trace (with NLS as well as ICU).
- ~~Scheduler: a thread woken by a timer preempts the running thread
  instead of waiting up to a 20 ms time slice~~ Done (`sleeptest timer`
  holds the timer queue case to 1 ms under load).
- ~~Saving drive C: without holding the kernel, desktop and file-system
  locks for the whole disk write~~ Done (`savetest`: the save holds the
  file-system lock well under a millisecond).
- ~~Scheduler: timer queue timers fire on time with every CPU busy (a
  deadline the timer had not fired yet was re-armed over, a timer wake
  behind a kernel thread went last, a thread preempted at a tick lost its
  place)~~ Done (`sleeptest timer`: the 1 ms timer queue timer within 1 ms
  under load again).
- ~~The desktop's redraws hold the file-system lock for the whole redraw
  (50-180 ms in QEMU without KVM), so file calls wait them out~~ Done
  (`savetest`: the file-system call waits under 100 ms during a save).
- ~~Scheduler: NT's priority boosts (a woken thread runs above its base
  priority by the waker's increment and decays back one level per
  quantum; the balance set lifts starving threads)~~ Done (`boosttest`:
  an event-woken thread runs within 2 ms while same-priority threads
  spin, where it waited out a 20 ms slice).

<!-- END generated:next-kernel -->

### Storage, network and hardware
<!-- BEGIN generated:next-hardware -->

- ~~NTFS read~~ ~~NTFS write~~ Done: NTFS volumes mount as drives D:, E:,
  ...; files on them are written, created, renamed and deleted (volumes
  Windows left hibernated or unclean stay read-only).  ~~NTFS as drive C:~~
  Done: the installer formats C: as NTFS (or FAT32), and files there keep
  security descriptors that opening, deleting and renaming obey.  Hard
  links are still to do.  ~~NVMe~~ Done: NVMe disks, installed to and booted from.
- ~~IPv6, HTTP/2~~ Done (Phase 18.8): lwIP's IPv6 (SLAAC, RDNSS, MLD),
  dual-stack Winsock with `getaddrinfo`, `ping -6` and `curl -6`; a
  virtio-net driver; `winhttp` on Schannel with HTTP/2 by ALPN (nghttp2).
  The `network` self-test suite checks both.  Still to do: DHCPv6,
  connection reuse in `winhttp`, WinHTTP WebSockets, HTTP/3.
- ~~USB (xHCI) with HID keyboards and mice~~ Done: keyboards, mice,
  tablets and touch screens in report protocol, on root ports or behind
  hubs, with hot-plug; USB mass storage (FAT and NTFS sticks as the next
  drive letter; NTFS ones writable); the older EHCI, OHCI and UHCI
  controllers (EHCI passing full- and low-speed devices to its
  companions), any number of controllers, keyboard LEDs, media keys and
  mice's side buttons and horizontal wheel.
  ~~Isochronous transfers, USB audio~~ Done: isochronous streams on xHCI,
  EHCI (high-speed devices, iTDs), OHCI and UHCI (alternate settings, a
  ring of transfers per pipe), and USB Audio Class 1 speakers, headsets
  and microphones as the sound output and input the mixer switches to
  when they are plugged in.  Still to do: USB Audio 2.0, siTDs (full-speed
  isochronous behind a high-speed hub on EHCI), webcams.
- ACPI beyond the MADT: ~~shutdown, reboot, sleep, batteries~~ Done:
  power-off (S5), sleep (S3), reset and the fixed power button from the
  FADT; the AML interpreter (uACPI) for batteries, AC adapters,
  control-method power buttons and `_PTS`/`_WAK`; the lid (closing it
  sleeps), thermal zones (passive cooling reported, sleep at `_HOT`,
  shutdown at `_CRT`), wake devices from `_PRW` (the lid, power buttons,
  USB controllers, with USB keyboards set for remote wakeup), the SCI as
  a real interrupt through the I/O APIC and PCI interrupt routing from
  `_PRT` (Phase 18.6).  Still to do: CPU throttling for passive cooling;
  GPE blocks other than `\_GPE`; routing behind PCI bridges.  (USB wake
  from S3 is confirmed on a real PC; QEMU can't emulate it.)
  (Display modes after S3 are set again on every adapter NovaOS drives:
  the VBE ones, QXL, virtio-vga, VMware SVGA and Cirrus.  Real GPUs have
  no driver yet.)
- ~~HPET or TSC-deadline timers~~ Done (Phase 18.7): the HPET calibrates
  the TSC and the APIC timer, which is one-shot (TSC-deadline where the
  CPU has it), armed for the next tick or the earliest `Sleep` or wait
  timeout; `sleeptest timer` checks 1 ms resolution under load.
  Waitable timers, their completion routines, timer queues, threadpool
  timers and `timeSetEvent` followed (2026-10-03): they end on the TSC
  too.
- Boot and test on real hardware, not only QEMU.

<!-- END generated:next-hardware -->

---

## Cross-cutting (maintain throughout)

- **Automated boot CI** (done): every pull request builds NovaOS, boots it
  under QEMU + OVMF and runs the self-tests and the OpenGL/Direct3D tests
  (`tools/selftest.py`, `.github/workflows/ci.yml`); a failing test fails
  the check.
- **Reproducible build:** CMake drives `nasm`, clang/lld and `lld-link`
  for the kernel, bootloader and Windows userland; CI builds `nova.iso`
  with `scripts/create-iso.sh` (a run artifact on every pull request, the
  `latest` release from `main`); the ISO is not committed.
- **Debugging:** the GDB stub over QEMU (`run-debug`), the serial log,
  crash reports naming the module and offset, and the Terminal's `trace
  NAME` for a program's failing system calls, and symbolized kernel
  backtraces (the kernel embeds its symbol table; faults, panics and
  failed assertions print function+offset frames).
- **ABI conformance tests** (done): `abitest` asserts PEB/TEB/KUSER/CONTEXT
  offsets, ntdll's stub layout and every syscall number against Windows 10
  1903 x64, in CI.
- **Parallel changes without conflicts** (done): DLLs, programs,
  self-tests, the app corpus and the docs' lists are one file per item
  (CONTRIBUTING.md), generated doc regions are rebuilt on main by
  `tools/docgen.py`, and CI fails on leftover conflict markers.
- **Test corpus:** every self-test program in `userland/programs/` is a
  permanent regression test, built for x64 and x86; `tools/pe_imports.py`
  shows what a new program needs before it is tried.
- **Nightly app corpus** (done): `tools/appcorpus.py` runs ripgrep, fd, jq,
  7-Zip, MinGit, Python, Node.js, .NET, ffmpeg, SumatraPDF, WinMerge, VLC,
  Audacity, Notepad++ and PuTTY every night, checks
  NovaOS's own `dir` and This PC screens, and posts a pass/fail table per
  program.

## Reality check

This is ReactOS-scale.  NovaOS already runs a meaningful set of real
command-line programs, runtimes, installers and one large GUI program
unmodified.  "Runs arbitrary commercial Windows software" remains a
long-horizon goal; the value of this plan is an ordered path where each
step produces something demonstrably working.
