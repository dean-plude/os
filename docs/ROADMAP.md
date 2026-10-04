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
  App Store is the system OpenGL (OpenGL 4.5, 64- and 32-bit); see
  [OpenGL](HISTORY.md#opengl-mesa-as-the-system-opengl32dll).  NovaOS's
  own `opengl32.dll` hands it to Mesa's virgl on a 3D virtio-gpu (see
  [virgl](HISTORY.md#opengl-on-the-hosts-gpu-virgl)).
- ~~**Direct3D**, DXGI~~ Done: DXVK from the App Store is the system
  Direct3D 8–11 on Mesa's lavapipe Vulkan, through NovaOS's own
  `vulkan-1.dll` and behind NovaOS's own `d3d9.dll`, `d3d11.dll` and
  `dxgi.dll` (which load without DXVK and report no Direct3D); see
  [Direct3D](HISTORY.md#direct3d-dxvk-on-mesas-vulkan).
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
  the screen's own pixels, while unaware ones keep 96 DPI; user32's
  controls, menus, dialogs and fonts and comctl32's controls follow each
  window's DPI and are measured again when it changes, threads can have
  their own awareness context, and window coordinates are converted
  between awareness contexts.
- ~~**A GPU path in QEMU**~~ Done (Phase 19.7): the virtio GPU driver does
  3D, and Mesa's Venus (the App Store's "Venus") runs Vulkan, and Direct3D
  through DXVK, on the host's GPU through QEMU's `virtio-vga-gl,venus=on`;
  `d3dtest` passes on it and draws faster than on lavapipe, and ANGLE's
  Direct3D 11 back end (Chromium's GPU process) starts on it
  (`d3dtest angle`; [Chromium's GPU process](HISTORY.md#chromiums-gpu-process-on-direct3d-11)).
  ~~virgl (OpenGL on the host's GPU)~~ Done: the same App Store entry
  brings Mesa's virgl, which NovaOS's own `opengl32.dll` picks on that
  GPU; `gltest` passes on it and `gltest fps` draws faster than on
  llvmpipe; see [virgl](HISTORY.md#opengl-on-the-hosts-gpu-virgl).
  Still to do: showing Vulkan's and OpenGL's frames on the virtio GPU
  directly instead of reading them back and copying them through the GDI.
- ~~NetSurf: SVG; redrawing pages a script changes after layout.~~ Done
  (Phase 19.8): SVG images (`<img>`, `<object>`, CSS backgrounds, `.svg`
  pages) and `<svg>` elements written inline in HTML drawn anti-aliased,
  and pages laid out again when a script changes the DOM, a style or a
  stylesheet (only the changed part is built again, and the layout starts
  at the changed box).  Still to do: SVG text in the document's fonts.

<!-- END generated:next-graphics -->

### Application coverage
<!-- BEGIN generated:next-apps -->

- Bring the App Store catalog up program by program: ~~the "untested"
  portable ones (Notepad++, SumatraPDF, PuTTY, WinMerge)~~ Done (Phase
  20.2, in the nightly corpus); the Qt applications: ~~KeePassXC~~ Done
  (Phase 20.3, unlocks a database in the nightly corpus; see
  [Qt programs](HISTORY.md#qt-programs-keepassxc)), ~~Krita~~ Done (Phase
  20.3, Krita 5.3.4 opens a new image in the nightly corpus; see
  [Qt programs: Krita](HISTORY.md#qt-programs-krita)); the
  GTK ones: ~~Inkscape~~ Done (Phase 20.4, Inkscape 0.91, the GTK 2 build
  from conda-forge, opens a new document in the nightly corpus; see
  [GTK programs](HISTORY.md#gtk-programs-inkscape)); ~~then Firefox~~
  Done (Phase 20, stock Firefox 157 installs from the App Store and loads
  an HTTPS page in the nightly corpus; see
  [Firefox in the App Store](HISTORY.md#firefox-in-the-app-store)).
  Left from this item: painting strokes in Krita, and a system
  `opengl32.dll` so Krita runs without Mesa 3D.
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
- ~~First-boot setup: the user's name, the display resolution, a time
  zone page with daylight-saving rules (local time in the clock, kernel32
  and the C runtime) and a keyboard layout page (16 layouts with AltGr and
  dead keys, for the desktop, the Terminal and user32)~~ Done (Phase
  22.1): **Welcome to NovaOS** opens the first time an installed NovaOS
  starts, and `start welcome` opens it anywhere.
- ~~Updates: an update channel in the App Store that replaces the system
  safely~~ Done (Phase 22.2): the App Store's Updates page and the
  Terminal's `update` download a newer kernel and boot loader from the
  channel (a GitHub release by default, made with `tools/mkupdate.py`),
  check them, and stage them; the boot loader starts the new kernel once
  and goes back to the old one if it does not reach the desktop
  ([updates.md](updates.md)).  Signed channel files are still to come.
- Release 0.1 (Phase 22.5): pushing a version tag builds, tests and
  publishes a release (`.github/workflows/release.yml`,
  [releasing.md](releasing.md)): every CI suite on the tagged commit, with
  the audio DSP firmware fetched first, then `nova.iso`, its checksums,
  the update channel's files and notes from the history.  Still to do:
  tag `v0.1.0`, and start the release's ISO on the reference PC.
- The WebView2 runtime (Roblox's login page and many other programs show
  web content with it): ~~Edge Update's Windows APIs~~ Done
  (`edgeupdtest`); ~~MSXML 6~~ Done (`msxmltest`); ~~rpcrt4's NDR engine
  for COM proxy/stub DLLs~~ Done (`ndrtest`); ~~COM calls between
  processes~~ Done (`comoop`); ~~Edge Update's check of Microsoft's
  signature on the runtime's package~~ Done (`authtest`).  Still to do:
  Edge Update seeing its own install running (its background pass
  uninstalls it mid-install), Windows' `WOW6432Node` registry view, then
  the runtime's setup and the Chromium runtime itself.
- Games: ~~OpenTTD (free on GOG) to its main menu~~ Done; ~~Beneath a
  Steel Sky (free on GOG) on ScummVM, installed with its installer and
  played~~ Done; ~~Teeworlds in full screen to its start menu, its music
  checked on a sound card~~ Done (all three in the nightly corpus).  Still
  to do: joining a Teeworlds game on its own server (it times out under
  TCG while Mesa compiles its shaders; mouse needs Raw Input), a Direct3D 9
  game through DXVK, a game that switches the display mode for full
  screen, GOG GALAXY's client, Steam's login window, Roblox's player (its
  anti-cheat refuses virtual machines).

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
- ~~Scheduler and Winsock leftovers of the foreground boost: longer time
  slices for the foreground process, a Terminal's console program as the
  foreground process, and real `setsockopt`/`getsockopt`~~ Done
  (`prioritytest`: 60 ms slices in the foreground against 20 ms in the
  background; `looptest`: `TCP_NODELAY`, `SO_RCVTIMEO`, `SO_SNDTIMEO`,
  `SO_LINGER` and `SO_REUSEADDR` change what a socket does).
- ~~Sound threads above busy programs: the Multimedia Class Scheduler
  (`AvSetMmThreadCharacteristics`) and NT's windowing boost for input~~
  Done (`mmcsstest`: a registered thread woken every 5 ms runs within
  2 ms while TIME_CRITICAL threads spin on every CPU, where a plain
  TIME_CRITICAL one waits 24-34 ms; busy registered threads still leave a
  NORMAL thread room).

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
  ring of transfers per pipe), and USB Audio Class 1 and 2 speakers,
  headsets and microphones at the rates their clocks offer and with up
  to eight channels, as the sound output and input the mixer switches to
  when they are plugged in, or that Settings' Sound page or a program
  chooses; the mixer runs at each device's own rate, and asynchronous
  devices' rate feedback is followed.  Still to do: siTDs (full-speed
  isochronous behind a high-speed hub on EHCI; QEMU cannot test them,
  and Intel chipsets since 2015, the reference ThinkPad's included, have xHCI
  only), webcams.
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
- ~~Pick a reference machine for real hardware~~ Done (Phase 21.1): the
  Lenovo ThinkPad T14 Gen 4 (Intel), with each of its devices marked
  supported, partial or missing in [hardware.md](hardware.md); the
  Terminal's `devices` lists a machine's PCI devices and the driver each
  one has.
- Boot and test on real hardware, not only QEMU.
- ~~Start from a USB stick~~ Done (Phase 21.2): `nova.iso` is a USB stick
  image as well as a disc (its EFI System Partition is a GPT partition
  too), runs live from the stick and offers Install NovaOS, with the
  firmware's GOP framebuffer as the display, and writes its log into
  `\EFI\NOVA\bootlog.txt` on the stick (no serial port needed).  Checked
  in QEMU (`usbboot` and `cdboot` in the devices suite); on the
  reference ThinkPad T14 Gen 4 the check is by hand: start from the
  stick with Secure Boot off, reach the desktop, read the log on another
  computer.
- ~~Drive the reference machine's Intel I219 Ethernet controller~~ Done
  (Phase 21.3): the `e1000e` driver takes every I219-LM and I219-V
  (55 IDs, [ethernet.md](ethernet.md)) with the PHY bring-up from
  Intel's BSD-licensed code (ULP exit, LANPHYPC, the shared reset,
  per-chipset errata); tested in QEMU on the 82574L, which shares the
  rings and the PHY path.  Still to confirm on the T14 itself.
- ~~Sound and the touchpad on the reference laptop~~ Done (Phase 21.4): the
  HD Audio driver takes Intel's controllers with the audio DSP on (class
  04.01, as on the ThinkPad T14 Gen 4) as well as off, sets up Realtek's
  ALC256 family and turns the speakers off while headphones are plugged
  in; I2C-HID touchpads, found through ACPI on Intel's LPSS I2C
  controllers, run in their touchpad mode with tap to click, two-finger
  tap for the right button and two-finger scrolling (a follow-up PR).  Checked
  in QEMU against modelled devices (`hwcheck` in the core suite, with an
  ACPI table describing a touchpad); on the T14 the check is by hand
  ([hardware.md](hardware.md)).  ~~The digital microphones, behind the
  DSP~~ Done (Phase 21.4): Sound Open Firmware boots on the DSP, records
  them through an IPC4 capture pipeline and they are the "Microphone
  Array (DSP)" recording device, booted again after sleep (checked on a
  modelled DSP, `hwcheck mic`).  Not yet: the touchpad's interrupt line
  (polled for now), tap-and-drag and scrolling that coasts on after the
  fingers lift.
- ~~Install to the internal NVMe disk, and S3, batteries and the lid on
  the reference machine's tables~~ Done (Phase 21.5): the ACPI embedded
  controller that laptops keep their lid, battery and AC adapter behind;
  sleep as low-power S0 idle on firmware without S3 (the T14 Gen 4 has
  none), with the LPS0 device's calls; timers from CPUID leaf 0x15 where
  the firmware hides the HPET; the Terminal's `install`, a firmware boot
  entry added at the first start from the disk, and a hint when Intel
  VMD hides the NVMe disk ([install-and-power.md](install-and-power.md)).
  Tested in QEMU (`laptop` in the devices suite); on the T14 itself the
  checks are by hand.
- ~~Game controllers for Windows games~~ Done: wired Xbox 360 and Xbox
  One controllers (their motors and the Xbox 360 one's player light
  included) and HID game pads on USB, hot-plugged, for XInput
  (`xinput1_4`, `xinput1_3` and older, `xinput9_1_0`) and DirectInput 8
  (game controllers, keyboards and mice, immediate and buffered).  Still
  to do: Raw Input and `hid.dll` for game pads (`WM_INPUT` with HID
  reports, `HidP_*` on their report descriptors), the Xbox 360 wireless
  receiver, Bluetooth controllers, force feedback through DirectInput,
  and virtio game pads (QEMU has none).

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
  `latest` build from `main`, and a release from each version tag through
  `.github/workflows/release.yml`); the ISO is not committed.
- **Debugging:** the GDB stub over QEMU (`run-debug`), the serial log,
  crash reports naming the module and offset, and the Terminal's `trace
  NAME` for a program's failing system calls, and symbolized kernel
  backtraces (the kernel embeds its symbol table; faults, panics and
  failed assertions print function+offset frames).  Crashes leave a
  report a user can attach to an issue in `C:\NovaOS\Crashes` (a
  program's exception, place, stack and modules; a kernel fault's log
  and backtrace, written into `\NOVA\PANIC.TXT` on the disk and moved
  there at the next start).
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
