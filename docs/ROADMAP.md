# NovaOS — Roadmap to a Windows-Compatible Desktop OS
<!-- The regions between "BEGIN generated" and "END generated" markers are built from fragment files by tools/docgen.py: edit those files, not the regions (CONTRIBUTING.md). -->

**Goal:** a from-scratch x86-64 OS that runs **native Windows executables
without emulation**. The programs' own machine code runs directly on the
CPU; NovaOS provides the NT system-call ABI, the Win32 API, the loader,
the GUI and the drivers they expect.

> "Without emulation" means no CPU emulation: the code is already native
> x86-64, or 32-bit x86 run in compatibility mode through NovaOS's own
> WoW64. It does **not** mean "without work". NovaOS has to reproduce the
> Windows *environment*, the problem ReactOS works on. Every step below
> ends in a check anyone can repeat, so progress is demonstrated rather
> than claimed.

---

## Where we are

NovaOS 0.1.0, the first release, was published on 2026-10-04 from the
`v0.1.0` tag ([release notes](releases/0.1.0.md)). NovaOS 0.2 "Kepler"
(`0.2.0`, [release notes](releases/0.2.0.md)) was published on 2026-10-19
from the `v0.2.0` tag, after Phase 33, the everyday-OS phase (step 29.1 stays
open until its ISO boots on the T14). Since 0.1 every green
CI run on a push to `main` republishes a rolling **latest** release with
that commit's `nova.iso`. The release closed Phase 22 except its checks
on a real PC, which moved to Phase 23. Since then Phase 26 and Phase 30
are complete, and Phases 24 and 25 are partly done.

| Area | Current state | Evidence |
|------|---------------|----------|
| System | UEFI boot, 64-bit and 32-bit (WoW64) Windows programs, SMP, an interactive desktop, installation to a disk, live boot from a USB stick | Core, network and devices self-test suites in CI (QEMU with OVMF) |
| Applications | Unmodified command-line tools, language runtimes, installers (`.msi`, NSIS), Firefox, WebView2, Qt and GTK programs, media apps and games run to tested milestones | Nightly app corpus ([testing.md](testing.md)), [compatibility.md](compatibility.md) |
| Graphics and media | Mesa's llvmpipe built in as the system OpenGL; Direct3D 8 to 11 through DXVK and Direct3D 12 through VKD3D-Proton (both from the App Store); QEMU virtio-GPU acceleration (virgl, Venus) with direct presentation; sound, recording and media playback | `gltest`, `d3dtest`, `dcomptest` in the graphics suite; the app corpus |
| Drivers | NovaOS's built-in drivers, plus unchanged Windows driver packages (`.inf`/`.sys`) on an NT kernel-mode driver interface with KMDF, UMDF 2, USB, NDIS and Storport hosts (Phase 30); a display-only WDDM host runs a WDDM display-only test driver (`wddmdod.sys`) on QEMU's standard VGA | Devices suite boots (`wdm`, `edu`, `kmdfedu`, `vioser`, `usbserial`, `netkvm`, `viostor`, `umdf`, `wddm` and others), [drivers.md](drivers.md) |
| Updates and release | Update channel with Ed25519-signed channel files; tag-triggered release workflow; rolling `latest` build from `main` | [updates.md](updates.md), [releasing.md](releasing.md) |
| Performance | CI's **Performance budget** job times the boot and Firefox's first page and checks `smpstress` scaling on every code change | `tools/ci/perf.py`, `tools/ci/perf-budget.json` |
| Hardware | The Lenovo ThinkPad T14 Gen 4 is the reference machine; its drivers are tested only against QEMU and modelled devices | [hardware.md](hardware.md) |

The details of completed work are in [HISTORY.md](HISTORY.md).

**Current limits:**

- **Real hardware.** Dean confirmed the first hand checks on the reference
  ThinkPad T14 Gen 4 on 2026-10-15 (step 23.1).
- **No physical GPU driver.** GPU acceleration exists only through QEMU's
  virtio-GPU. The Intel GPU work (step 25.3) has its design and the
  display-only WDDM host, but Intel's own driver has not run yet.
- **No release signing key built in.** `kernel/fs/update_key.h` is still
  empty, so an installed NovaOS uses a channel without checking its
  signature (step 23.5).
- **Clean-room DLLs.** NovaOS's system DLLs are its own, not Microsoft's;
  broad compatibility with arbitrary Windows software is a long-term goal,
  not a current claim.

---

## The pivotal decision: how to get the Win32 API surface

- **Path A — Reimplement (Wine-style).** Write our own `ntdll`, `kernel32`,
  `user32`, `gdi32` and the rest. Clean-room, no licensing issues, total
  control; but the API surface is vast and real programs break on missing
  edge cases.
- **Path B — Binary compatibility (ReactOS-style).** Match a *specific*
  Windows build's NT syscall ABI exactly, then load the **real Microsoft
  user-mode DLLs**. Huge coverage at once; but brittle across versions and
  constrained by the licensing of Microsoft's DLLs.
- **Path C — Hybrid.** Reimplement clean-room while keeping ABI and
  structure compatibility, so real DLLs or drivers *can* be loaded when
  that is the better choice.

> **DECISION: Path C — Hybrid.** NovaOS's components are clean-room, but
> structures (PEB/TEB/LDR, `CONTEXT`, `KUSER_SHARED_DATA`) and syscall
> numbers follow one target build, **Windows 10 1903 x64**, checked by
> `abitest`. Every system DLL is NovaOS's own (Path A in practice), with
> open-source components such as Mesa, DXVK and VKD3D-Proton loaded
> unchanged behind them. The driver side takes the other half of Path C:
> since Phase 30, unchanged Windows driver packages (virtio-win's drivers,
> FTDI's USB driver, Intel's AX211 Wi-Fi driver) load on a kernel-mode
> interface with Windows-exact structures, with KMDF and UMDF built from
> Microsoft's MIT-licensed WDF source.

---

## What comes next

Feature checklists by area. Struck-through items are done, with a link to
the history entry or the test that shows it; the rest is still open. Each
item ends when a named program or test demonstrates it.

### Graphics, 3D and media
<!-- BEGIN generated:next-graphics -->

- ~~**OpenGL**: a working `opengl32.dll`~~ Done: Mesa's llvmpipe, built
  in since [Mesa's OpenGL built in](HISTORY.md#mesas-opengl-built-in), is
  the system OpenGL (OpenGL 4.5, 64- and 32-bit); see
  [OpenGL](HISTORY.md#opengl-mesa-as-the-system-opengl32dll).  NovaOS's
  own `opengl32.dll` hands it to Mesa's virgl on a 3D virtio-gpu (see
  [virgl](HISTORY.md#opengl-on-the-hosts-gpu-virgl)).
- ~~**Direct3D**, DXGI~~ Done: DXVK from the App Store is the system
  Direct3D 8–11 on Mesa's lavapipe Vulkan, through NovaOS's own
  `vulkan-1.dll` and behind NovaOS's own `d3d9.dll`, `d3d11.dll` and
  `dxgi.dll` (which load without DXVK and report no Direct3D); see
  [Direct3D](HISTORY.md#direct3d-dxvk-on-mesas-vulkan).  Without DXVK,
  `dxgi.dll` lists Windows' Microsoft Basic Render Driver and `d3d11.dll`
  gives a WARP device that maps and copies textures, with `dcomp.dll`
  showing its swap chains, for Chromium's software compositor
  ([WebView2 draws its page](HISTORY.md#webview2-draws-its-page)); with
  DXVK the WARP device and its swap chains stay NovaOS's
  ([WebView2 on Venus](HISTORY.md#webview2-on-venus)).
- **Direct3D 12** (After 0.1, step 24.4): VKD3D-Proton (LGPL-2.1, from the
  App Store as its own unchanged DLLs, `d3d12_vkd3d.dll` and `d3d12core.dll`,
  since 3.0.1 built by `tools/build_vkd3d_proton.py`)
  behind NovaOS's `d3d12.dll`.  ~~A Direct3D 12 sample renders~~ Done:
  `d3d12test` draws, reads back and presents a triangle on lavapipe,
  64- and 32-bit ([Direct3D 12](HISTORY.md#direct3d-12-through-vkd3d-proton),
  [32-bit present](HISTORY.md#direct3d-12-presents-in-32-bit-programs)).
  ~~One Direct3D 12 game renders~~ Done: Squash the Creeps on Godot 4.7's
  Forward+ renderer ([A Direct3D 12 game](HISTORY.md#a-direct3d-12-game-squash-the-creeps-on-godot)).
  ~~Sound in Godot (WASAPI)~~ Done: the game's music plays
  ([Godot's sound](HISTORY.md#godots-sound-through-wasapi)).
  ~~Two of Godot's compute pipelines lavapipe refuses~~ Done: VKD3D-Proton
  3.0.1 built with a newer DXIL translator
  ([Godot's compute pipelines](HISTORY.md#godots-compute-pipelines-on-lavapipe)).
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
  Direct2D 1.1's devices and device contexts followed for XAML (32.3):
  see [Direct2D device contexts](HISTORY.md#direct2d-device-contexts).
- ~~**ffmpeg's imports**~~ Done (19.3): see [ffmpeg](HISTORY.md#ffmpeg).
  `ffmpeg -i in.mp4 out.webm` (H.264 and AAC to VP9 and Opus) completes,
  and the nightly app corpus runs it.
- Display: GPU-backed or at least faster blits.  On QEMU's virtio GPU,
  frames from OpenGL, Vulkan and DirectComposition go to the screen
  without CPU copies (see the GPU path item below); physical GPUs wait on
  step 25.3.  ~~Mode changes~~ Done:
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
  ~~Showing OpenGL's frames on the virtio GPU directly~~ Done (step
  25.2): virgl's frames are written into the screen by the host while the
  window is alone there, instead of being read back and drawn through the
  GDI (`gltest present`, and `gltest fps` against the GDI way); see
  [direct presentation](HISTORY.md#direct-presentation-on-the-virtio-gpu).
  ~~The same for Vulkan's frames (Venus, DXVK, VKD3D-Proton)~~ Done:
  a swapchain's frames are copied by the host from Venus's memory into
  the screen while the window is alone (`d3dtest present`, and
  `d3dtest fps` against the GDI way); see
  [Vulkan's frames](HISTORY.md#direct-presentation-of-vulkans-frames).
  ~~Windows on the other monitors and at scale 2~~ Done: the host writes
  the frame into whichever monitor's screen the window is on, twice as
  wide and high on a monitor at scale 2 (`gltest monitors`); see
  [the other monitors](HISTORY.md#direct-presentation-on-every-monitor-and-at-scale-2).
  ~~DirectComposition's swap chains (WebView2's GPU process)~~ Done: a
  target showing only swap chains of one GPU device is composited on that
  GPU into a swap chain of its own on the window, whose frames the host
  writes into the screen, embedded windows included (`dcomptest direct`);
  see [DirectComposition on the GPU](HISTORY.md#directcomposition-composited-on-the-gpu).
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
  Krita's corpus test now also draws a stroke on the canvas, and Krita
  runs on the built-in OpenGL with nothing installed first
  ([Mesa's OpenGL built in](HISTORY.md#mesas-opengl-built-in)).  The rest
  of the catalog continues as step 27.1.
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
  ([updates.md](updates.md)).  The channel file is signed with Ed25519;
  a NovaOS with the release key built in installs only from a channel
  signed with it.
- Release 0.1 (Phase 22.5): pushing a version tag builds, tests and
  publishes a release (`.github/workflows/release.yml`,
  [releasing.md](releasing.md)): every CI suite on the tagged commit, with
  the audio DSP firmware fetched first, then `nova.iso`, its checksums,
  the update channel's files and notes from the history.  `v0.1.0` was
  tagged and published on 2026-10-04 ([release notes](releases/0.1.0.md)),
  and every green CI run on a push to `main` republishes a rolling
  `latest` release.  Still to do: start the release's ISO on the
  reference PC (step 23.1).
- The WebView2 runtime (Roblox's login page and many other programs show
  web content with it): ~~Edge Update's Windows APIs~~ Done
  (`edgeupdtest`); ~~MSXML 6~~ Done (`msxmltest`); ~~rpcrt4's NDR engine
  for COM proxy/stub DLLs~~ Done (`ndrtest`); ~~COM calls between
  processes~~ Done (`comoop`); ~~Edge Update's check of Microsoft's
  signature on the runtime's package~~ Done (`authtest`); ~~Edge Update
  seeing its own install running~~ Done (`proclisttest`); ~~delete on
  close~~ Done (`filetest`); ~~the runtime's setup unpacking its archive,
  `wer.dll`~~ Done (`wvsetuptest`); ~~the setup's permissions on its
  install folder~~ Done (`acltest`); ~~starting the runtime's browser
  process~~ Done (`wvstarttest`, `unwindtest`); ~~the browser keeping
  its host's connection (pipe process ids)~~ Done (`pipetest`); ~~windows
  of one process inside another's (the host's `SetParent` and
  `SetWindowPos` on the browser's window, another process drawing into
  it; the controller, a page and a script)~~ Done (`xpwin`,
  `chrometest`); ~~drawing the page without a GPU (the Basic Render
  Driver adapter, a WARP device, DirectComposition, the GPU process's
  window inside the browser's)~~ Done (`dcomptest`); ~~Windows'
  `WOW6432Node` registry view for machine-wide installs~~ Done (`regtest`).
  ~~WebView2 with DXVK installed and Venus the GPU (the GPU process keeps
  ANGLE's Direct3D 11 device on Venus and presents through the WARP device
  and DirectComposition instead of restarting until the browser crashed)~~
  Done (`dcomptest dxvk`).  Still to do: the GPU-composited output path
  (DirectComposition on the GPU): ~~composition swap chains on the GPU
  device~~ Done (`dcomptest dxvk`); ~~`DCompositionCreateDevice3`
  (`IDCompositionDesktopDevice`, `IDCompositionDevice3`, surfaces, clips),
  the DXVK adapter's factory making the swap chains, `IDXGISwapChain3`, so
  Chromium turns the path on~~ Done (`dcomptest dxvk`; WebView2's GPU
  process composites the page through a swap chain on Venus); ~~that
  path through resizes, teardown while frames are presented, and the GPU
  process's window inside the browser's, 64- and 32-bit~~ Done
  (`dcomptest life`).  Presenting without the CPU readback and GDI copy
  (direct virtio GPU scan-out) is done for OpenGL's frames, for Venus's
  Vulkan swapchains and for the DirectComposition targets WebView2's GPU
  process presents into (`dcomptest direct`; see
  [DirectComposition on the GPU](HISTORY.md#directcomposition-composited-on-the-gpu)).
- Games: ~~OpenTTD (free on GOG) to its main menu~~ Done; ~~Beneath a
  Steel Sky (free on GOG) on ScummVM, installed with its installer and
  played~~ Done; ~~Teeworlds in full screen to its start menu, its music
  checked on a sound card~~ Done; ~~a Direct3D 9 game through DXVK
  (OpenTyrian: Tyrian 2.1 in full screen, into a new game with its
  music)~~ Done; ~~a game that switches the display mode for full screen
  (Blobby Volley 2: 800x600 in Direct3D 9 exclusive full screen, and back
  when it ends)~~ Done; ~~an SDL 1.2 game (LBreakout2: GDI drawing, 640x480
  full screen with the window's frame taken off, and back)~~ Done; ~~a
  DirectDraw game (Cave Story on cnc-ddraw: 640x480 full screen, its title
  screen and a new game)~~ Done; ~~a game played with a game pad (SuperTux:
  SDL 1.2 through winmm's joystick functions, into its first level)~~ Done;
  ~~a DirectInput game pad game (Cave Story with an Xbox 360 pad through
  DirectInput 7: the opening talk and Quote walking in the Start Point
  cave)~~ Done (all nine in the nightly corpus); ~~joining a Teeworlds
  game on its own server (`teeworlds_srv.exe` on the same machine, over
  the loopback interface; it times out under TCG while Mesa compiles its
  shaders, so KVM runs judge it)~~ Done (in the nightly corpus).  Still
  to do: GOG GALAXY's client, signing in to Steam (its login window opens
  and draws), Roblox's player (its anti-cheat refuses virtual machines).
- Windows Calculator (github.com/microsoft/calculator): ~~its engine,
  CalcManager, in a Win32 `calc.exe` with the Standard, Scientific and
  Programmer calculators~~ Done (`calc`); the converters (CalcManager's
  `UnitConverter` needs the Concurrency Runtime's `task<>`, and the unit
  and currency data comes from the app's C# view models); then the
  shipped UWP package itself, which needs MSIX deployment,
  `Windows.UI.Xaml` with WinUI 2 (Phase 32, under way: WinRT activation,
  the app model and the compositor are done, Direct2D device contexts
  too; the XAML build is next) and the UWP .NET runtime.

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
- Win32 console screen buffers: the console keeps each buffer's cells,
  cursor and text attributes, so `GetConsoleScreenBufferInfo` reports the
  cursor, and cell reads and writes, fills, scrolling, colours and
  independent buffers work (`consolecursortest`, x64 and x86); the first
  buffer follows the Terminal's size.  libvterm, the Terminal's own VT
  emulator, edits the cells from what programs write, so a full-screen
  program leaves the same cells in a console window and through a pseudo
  console (`ptyscreentest`).  Still to do: verify a real console-screen
  API application; Git still uses NovaOS's own `less`.
- Pseudo consoles (`CreatePseudoConsole`, for Neovim's `:terminal` and
  Windows Terminal-style hosts): a child started with
  `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` gets a console of its own whose
  input and output travel over the host's pipes, and
  `ResizePseudoConsole` sets its size and draws the screen again for the
  host (`pipetest`); its host gets line feeds as CR LF, as from conhost,
  and draws the same cells as the console holds (`ptyscreentest`).  Keys
  arrive as a terminal sends them (VT sequences for arrows, Delete and
  F-keys, win32-input-mode records too) and become key records; while a
  program reads lines the console echoes and edits them, so a terminal
  host types into, resizes and exits an interactive `cmd.exe`
  (`conptytest`).  Still to do: run Neovim's `:terminal` on it.
- ~~Move files, the registry, process creation and the console off the
  big kernel lock~~ Done (Phase 17.7): file and registry throughput scale
  about 3x from one CPU to four (`smpstress scaling 3`).  The nightly
  run and CI's performance budget run `smpstress scaling 0` and fail
  when files and the registry scale less than half as well as bare
  system calls.
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
- Network security, part 2: the filtering engine and the Windows Firewall
  are in ([design](design/network-security.md),
  [history](HISTORY.md#network-security-a-filtering-engine-and-the-windows-firewall));
  next are WFP callouts (`FwpmCalloutAdd0` and kernel-mode callout
  drivers), IPsec and connection security rules, the "allow this app?"
  notification when a program first listens, ICMP rules, IDispatch for
  `hnetcfg`'s objects (PowerShell and scripts), the legacy `INetFwMgr`
  API, `INetworkListManager` (`netprofm.dll`), service-restricted rules,
  SmartScreen-style reputation checks, and a check on the reference laptop.

<!-- END generated:next-kernel -->

### Storage, network and hardware
<!-- BEGIN generated:next-hardware -->

- ~~NTFS read~~ ~~NTFS write~~ Done: NTFS volumes mount as drives D:, E:,
  ...; files on them are written, created, renamed and deleted (volumes
  Windows left hibernated or unclean stay read-only).  ~~NTFS as drive C:~~
  Done: the installer formats C: as NTFS (only, since
  2026-10-24), the installed system mounts it in place, and files there keep
  security descriptors that opening, deleting and renaming obey.  ~~Hard
  links and persistence~~ Done (Phase 17.5; `linktest` and `linktest
  restarted`; on a mounted NTFS volume across a restart, with
  `FindFirstFileNameW` and an offline volume check, in the devices
  suite's "ntfs" boot).  ~~Interrupted writes and a full volume~~ Done:
  a volume NovaOS is writing to is marked dirty until the changes are on
  the disk, and after a power cut NovaOS checks and repairs it as chkdsk
  would, leaving it clean (`ntfstest churn`, the machine reset, `ntfstest
  cut`; every cut point on the host, `tests/tools/test_ntfs_cut.py`); a write past the free space fails
  with `ERROR_DISK_FULL` (`ntfstest fill`), both on the "ntfs" boot's
  disposable disk.  ~~Many files~~ Done: the MFT grows by half its size
  (and its bitmap with it), so a volume takes as many files as fit
  (`tests/tools/test_ntfs_many.py`: 40,000).  ~~NVMe~~ Done: NVMe disks, installed to and booted from.
- ~~IPv6, HTTP/2~~ Done (Phase 18.8): lwIP's IPv6 (SLAAC, RDNSS, MLD),
  dual-stack Winsock with `getaddrinfo`, `ping -6` and `curl -6`; a
  virtio-net driver; `winhttp` on Schannel with HTTP/2 by ALPN (nghttp2).
  The `network` self-test suite checks both.  DHCPv6 is stateless (DNS
  servers) and stateful (RFC 8415: an address, DNS servers and the search
  list, with renew, rebind and release), written into lwIP's client.  WinHTTP reuses HTTP/1.1 connections and supports
  WebSockets.  HTTP/3 remains to do; it needs a QUIC transport.
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
- ~~ACPI beyond the MADT: shutdown, reboot, sleep, batteries~~ Done:
  power-off (S5), sleep (S3), reset and the fixed power button from the
  FADT; the AML interpreter (uACPI) for batteries, AC adapters,
  control-method power buttons and `_PTS`/`_WAK`; the lid (closing it
  sleeps), thermal zones (passive cooling reported, sleep at `_HOT`,
  shutdown at `_CRT`), wake devices from `_PRW` (the lid, power buttons,
  USB controllers, with USB keyboards set for remote wakeup), the SCI as
  a real interrupt through the I/O APIC and PCI interrupt routing from
  `_PRT` (Phase 18.6).  ~~CPU throttling for passive cooling; GPE blocks
  other than `\_GPE`; routing behind PCI bridges~~ Done (2026-10-15):
  T-states from `_PTC`/`_TSS`/`_TPC` (I/O, memory or the clock
  modulation MSR) or `P_CNT`, driven by each zone's `_PSL`, `_TC1`,
  `_TC2` and `_TSP` and reported as `MhzLimit`; GPE block devices
  (ACPI0006) on the SCI or their own interrupt, with their `_Lxx`/`_Exx`
  methods and wake GPEs; bridges' own `_PRT`, else the standard swizzle.
  (USB wake from S3 is confirmed on a real PC; QEMU can't emulate it.)
  (Display modes after S3 are set again on every adapter NovaOS drives:
  the VBE ones, QXL, virtio-vga, VMware SVGA and Cirrus.  Physical GPUs,
  such as the T14's Intel Iris Xe, have no driver yet; see step 25.3.)
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
- Boot and test on real hardware, not only QEMU.  Not done yet: nothing
  has been started on the reference ThinkPad T14 Gen 4; the hand checks
  are step 23.1.
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
  modelled DSP, `hwcheck mic`).  ~~The touchpad's interrupt line~~ Done
  (Phase 21.4): a driver for Intel's GPIO controllers (Tiger Lake to
  Meteor Lake) takes the touchpad's GpioInt pin and the touchpad is read
  when it fires, polled only when no interrupt can be had (checked on a
  modelled controller, `hwcheck`).  ~~Tap-and-drag and scrolling that
  coasts on after the fingers lift~~ Done: a tap then a touch drags with
  the left button held until the finger lifts (no drag lock, as Windows
  11), and a two-finger flick keeps scrolling, slowing down, until it
  stops or a touch stops it (`hwcheck`, `digitest`).
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
  (`xinput1_4`, `xinput1_3` and older, `xinput9_1_0`), DirectInput 8
  (game controllers, keyboards and mice, immediate and buffered), Raw
  Input (`WM_INPUT` with each controller's HID reports) and `hid.dll`
  (`HidD_*` on a controller's HID path, `HidP_*` on its report
  descriptor; an Xbox controller has the one Windows' Xbox driver
  gives; a HID game pad's path is its HID class collection's, on the
  `gamehid` class driver).  A vendor's USB HID device runs on the HID class driver, where
  `hid.dll` has output and feature reports and `IOCTL_HID_*` through
  `DeviceIoControl`, and overlapped `ReadFile`, `WriteFile` and
  `DeviceIoControl` with `CancelIo` work as hidapi uses them
  ([drivers.md](drivers.md#hid-the-class-driver-and-minidrivers));
  hidapi's own Windows backend, built unchanged, finds such a device as
  USB through the device interface and device node properties
  (`CM_Get_Device_Interface_PropertyW`, `DEVPKEY_Device_*`).
  Raw Input lists the class driver's collections too and sends their
  reports as `WM_INPUT`.
  Still to do: raw keyboard and mouse input, the Xbox
  360 wireless receiver, Bluetooth controllers, force feedback through
  DirectInput, and virtio game pads (QEMU has none).
- Plug and Play: a device tree the Windows way.  Devices plugged in or
  removed while NovaOS runs already work (USB hot-plug: keyboards, mice,
  hubs, sticks, audio, game controllers), and ~~a PnP device tree~~
  Done: the root, the PCI functions, the USB root hubs and devices, their
  functions and HID and disk children, with Windows instance, hardware
  and compatible IDs, classes, drivers, devices not present kept, and
  arrival and removal events for programs (`CTL_PNP`).  ~~The PlugPlay
  service, `WM_DEVICECHANGE`~~ Done: the PlugPlay service running and
  keeping `HKLM\SYSTEM\CurrentControlSet\Enum`,
  `RegisterDeviceNotification` (`DBT_DEVICEARRIVAL`,
  `DBT_DEVICEREMOVECOMPLETE`, `DBT_DEVNODES_CHANGED`, volumes, services and
  open device handles) and `CM_Register_Notification`.  ~~setupapi's and cfgmgr32's
  enumeration of the tree~~ Done: `SetupDiGetClassDevs` for every class,
  enumerator and interface class, `CM_Locate_DevNode`,
  `CM_Get_Child`/`Sibling`/`Parent`, device ID lists, status, registry
  and `DEVPKEY` properties (a device's bus type and container too), a
  device interface's properties (`CM_Get_Device_Interface_PropertyW`),
  device keys, and `pnputil /enum-devices`
  matching the driver store.  ~~A synchronous query-remove round-trip~~
  Done: before `IRP_MN_QUERY_REMOVE_DEVICE` the PnP manager sends
  `DBT_DEVICEQUERYREMOVE` to the programs holding a handle registration on
  the device and waits (at most ten seconds) for their answers; a
  `BROADCAST_QUERY_DENY` vetoes the removal (`DBT_DEVICEQUERYREMOVEFAILED`),
  agreeing programs close their handles first (`DBT_DEVICEREMOVEPENDING`
  after); a surprise removal asks nobody.
- Windows drivers the Windows 11 way (Phase 30, complete: all eleven
  steps are in; design in [design/windows-drivers.md](design/windows-drivers.md),
  what ships in [drivers.md](drivers.md)): unchanged Windows driver
  packages run on an NT kernel-mode driver interface, beside NovaOS's own
  built-in drivers.  A kernel PE loader and `ntoskrnl.exe`/`hal.dll`
  exports with Windows-exact structures load `.sys` images (30.1, 30.2);
  `pnputil /add-driver /install` puts a driver on a PCI device with no
  built-in driver, started with its memory, interrupts (line, MSI or
  MSI-X) and DMA (30.3); KMDF (`Wdf01000.sys`) and UMDF 2 (`WUDFHost.exe`,
  `Wudfx02000.dll`) are built from Microsoft's MIT-licensed WDF source
  (30.4, 30.10); `pnputil /scan-devices /search-online` downloads a
  device's package from the Microsoft Update Catalog by hardware ID
  (30.6); Windows' kernel-mode signing policy is enforced (30.7).  Third-
  party drivers checked in QEMU's devices suite: virtio-win's `vioser.sys`
  (30.5), `netkvm.sys` on NovaOS's NDIS host (30.9) and `viostor.sys` on
  its Storport host (30.11), and FTDI's USB serial driver on its USB bus
  interface (30.8).  Intel's AX211 Wi-Fi driver (`Netwtw14.sys`) installs
  and registers with the NDIS host on a stand-in device; joining a
  network on the T14's card is step 28.1.  The display-only half of a
  WDDM host (`dxgkrnl.sys`'s role) followed as step 1 of the Intel GPU
  design (step 25.3).
- [ ] Hyper-V Generation 2: every synthetic device a Gen 2 VM has, one step
  at a time, each leaving NovaOS bootable ([hyperv.md](hyperv.md)).
  Microsoft's own VMBus drivers are inbox and not redistributable, so
  these are NovaOS drivers written from the protocols as FreeBSD's
  BSD-licensed Hyper-V drivers speak them, each with a host test, since
  CI has no Hyper-V.  Done when a Gen 2 VM installs NovaOS to its virtual
  disk, reaches the network and changes resolution.
  - [x] Keyboard and mouse over VMBus (#255, #367)
  - [x] Integration services: the host's Shut Down and Restart, heartbeat,
    time sync (#401)
  - [x] storvsc: SCSI disks, so NovaOS installs to a VHDX and keeps drive
    C: (#402)
  - [x] Synthetic video: a resolution chosen in Settings (#403)
  - [x] netvsc: the network adapter (NVSP and RNDIS) (#404)
  - [x] Channel interrupts instead of polling, devices added and removed
    while running (#405).  The reference TSC page, synthetic timers and
    the SynIC on every CPU turned out not to be needed: the TSC and the
    local APIC timer already run at the frequencies Hyper-V's MSRs give,
    and every channel interrupts CPU 0
  - [x] Key-value exchange (the VM's name and addresses in Hyper-V
    Manager) (#407)

<!-- END generated:next-hardware -->

---

## After 0.1: Phases 23 to 33

These phases follow the first release. They keep the rule of Phases 15 to
22: each step is one focused change with a check anyone can repeat. A
ticked step has been demonstrated in this repository; an unticked one is
open. 0.1.0 passed every CI suite in QEMU before it was published, but it
has not yet been started on a real PC, so Phase 23 is first in priority.

### Phase 23: Finish 0.1 on real hardware

**Goal:** close what 0.1 still owes. **Gate:** the 0.1 ISO boots on the
ThinkPad T14 Gen 4 and the nightly corpus is green for a week.

- [x] **23.1 T14 hand checks.** Run the boot and device checklist in
  [install-and-power.md](install-and-power.md) on the T14 and keep its boot
  log and PCI IDs. Mark each device in [hardware.md](hardware.md) verified
  or missing; this settles the real-machine checks in 21.2–21.5 and 22.5.
  Done 2026-10-15: Dean confirmed the hand checks on the T14.
- [ ] **23.2 First missing T14 driver.** Use the 23.1 log to choose and fix
  the first device that does not work, keeping a QEMU regression test for
  it.
- [x] **23.3 A clean nightly week.** Fix failures until seven scheduled
  nightly runs in a row pass; this also completes 15.4 and 17.7. Done
  2026-10-18: every scheduled run from 2026-10-14 on passed (13 in a row by
  the evening of the 18th), and `v0.2.0` was released from that state.
- [ ] **23.4 Hyper-V.** Check VMBus keyboard and mouse in a Hyper-V
  Generation 2 VM. Done when NovaOS reaches the desktop and accepts input.
  The synthetic mouse's attach fault and the VMBus handshake were fixed in
  #367 ([hyperv.md](hyperv.md)); the check on a real Hyper-V host is still
  to do.  The first real boot (10-21) looked frozen after
  `Hyper-V video: channel 5 open`: the synthetic video took the screen
  over but reported no changes until the scheduler ran; the boot log's
  lines are reported now too.  The second boot stopped at `Version 3.5
  accepted`: the boot log is now reported from the moment the version is
  agreed, and still when a later video step fails.  The third boot (10-22)
  showed the host refusing the video memory at 0xf8000000; the driver now
  waits 10 s as Linux does, asks twice, and logs which answer was missing
  or wrong.  That boot then stopped while formatting the blank VHDX
  (after `[PERSIST] hv0 is empty`): the format, a slow SCSI request and
  the CMOS clock's update wait (now bounded) are logged.  That boot did
  reach the desktop, only slowly: the video memory was refused because the
  driver's messages were not packed, so the host read the address as 0
  (the dirty rectangles and pointer messages were off too).  The messages
  are packed now, checked against Linux's layout; should the host still
  refuse, the video memory moves into the VMBus's ACPI memory ranges (the
  one above 4 GiB first) and the boot log stops drawing on a screen the
  host does not show.  The fourth boot (10-23) reached the desktop with a working
  screen; the installer sat at "Copying NovaOS" and the watchdog found the
  desktop in a 10 ms sleep that had not ended after 3 s.  The disk driver
  now logs how fast the host answers (every 5 s) and the watchdog says
  whose timer a sleeping desktop waits for.  The cause: a disk request
  polled for its answer with the kernel lock held, starving the other
  threads, and froze the VM when the vmbus thread was switched out
  mid-ring; the asker sleeps until the answer now.  An install with
  drive C: on FAT32 completed in that boot.
- [ ] **23.5 Update signing key.** Ship the release public key in
  `kernel/fs/update_key.h` (still empty); verify that a 0.1 install accepts
  a signed 0.1.1 test update and rejects an unsigned one.

### Phase 24: Gaming

**Goal:** Steam and GOG Galaxy each install, sign in and launch a DRM-free
game with sound and a controller.

- [ ] **24.1 Steam library.** Sign in through Steam's own browser and show
  the account's library.
- [ ] **24.2 GOG Galaxy library.** Complete sign-in and show an owned game.
- [ ] **24.3 Install and play from a store.** Install a DRM-free game through
  Steam or Galaxy and reach gameplay with sound and a controller.
- [x] **24.4 Direct3D 12.** VKD3D-Proton renders a Direct3D 12 sample and
  the Godot 4.7 game Squash the Creeps; see [the D3D12 game
  check](HISTORY.md#a-direct3d-12-game-squash-the-creeps-on-godot).
- [ ] **24.5 Roblox on the T14.** Check Roblox's VM test on real hardware
  without bypassing it; done when Roblox reaches its home page.
- [ ] **24.6 Epic Games Launcher.** Sign in and show its library. It
  depends on WebView2's GPU path, which is in (25.1).
- [x] **24.7 Anti-cheat and DRM notes.** Record what common systems require,
  what NovaOS lacks and the faithful fix; never bypass or patch them; see
  [anti-cheat and DRM](design/anti-cheat-drm.md).

### Phase 25: GPU

**Goal:** fast graphics without CPU copies, keeping CPU rendering when no
supported GPU exists. **Gate:** WebView2 and a game render on the GPU with a
measured frame-time gain over the CPU path.

- [x] **25.1 WebView2 on the GPU.** ANGLE and DirectComposition present
  WebView2's page on Venus across resize, cross-process parenting and
  teardown, on x64 and x86 (`dcomptest dxvk`, `dcomptest life`).
- [x] **25.2 Direct virtio-GPU presentation.** OpenGL frames on virgl reach
  the display without CPU readback or GDI copies; `gltest present` and
  `gltest fps` compare the direct and fallback paths. See
  [direct presentation](HISTORY.md#direct-presentation-on-the-virtio-gpu).
  Vulkan swapchains and DirectComposition's GPU swap chains followed
  (`d3dtest present`, `dcomptest direct`).
- [ ] **25.3 Intel GPU design.** Design the driver for the T14's Intel Iris
  Xe, covering kernel display and memory management. Done when the design
  is merged and display modes change on the T14's panel. The design is in
  [design/intel-gpu.md](design/intel-gpu.md) (#366): Intel's own Windows
  driver, unchanged, through a WDDM host (a `dxgkrnl.sys` stand-in), with
  Intel's Vulkan driver under DXVK and VKD3D-Proton; a native driver under
  Mesa's iris and anv is the fallback. Its step 1 is in (#368): the
  display-only WDDM host, with the test driver `wddmdod.sys` driving
  QEMU's standard VGA in the devices suite's `wddm` boot
  ([history](HISTORY.md#a-windows-display-driver-on-the-boot-display)).
  Modes changing on the panel is its step 2, on the T14.  Step 3 (the
  render path, on QEMU) has begun: the `D3DKMT*` calls reach the kernel,
  which lists a Basic Render adapter with video memory allocations mapped
  by `D3DKMTLock2` and fences signalled and waited on from the CPU
  (`kmttest`; [history](HISTORY.md#d3dkmt-calls-reach-the-kernel)), and
  render miniports register with `DxgkInitialize`: the test driver
  `wddmtest.sys`'s adapter is listed beside Basic Render, with devices and
  allocations the driver sizes in its segment (`kmttest wddm` in the
  `wddm` boot; [history](HISTORY.md#a-windows-render-driver-registers)),
  and with GPU virtual addresses: per-process page tables VidMm makes and
  the driver writes through paging buffers
  ([history](HISTORY.md#gpu-virtual-addresses)), and command submission:
  contexts, a scheduler that runs their DMA buffers in order through the
  driver, its interrupt and DPC, and fences the GPU signals and waits on
  ([history](HISTORY.md#command-submission-and-gpu-fences)), and
  preemption and timeout recovery: a buffer that runs too long is
  preempted, one that will not stop (or faults) has its engine reset and
  its context lost while the others go on
  ([history](HISTORY.md#gpu-preemption-and-timeout-recovery)).
- [ ] **25.4 Intel GPU 3D.** Pass `gltest` and `d3dtest` on the T14 GPU and
  outperform llvmpipe and lavapipe.
- [ ] **25.5 Second monitor.** Drive an external display through HDMI or
  USB-C and move windows between it and the T14 panel.

### Phase 26: Kernel, console and storage

**Goal:** close the remaining API gaps. **Gate:** a terminal app hosts
`cmd.exe` through ConPTY, and NTFS C: survives a power cut. All four steps
are done (#370, #372, #375, #376).

- [x] **26.1 Console cells on libvterm.** Make console screen buffers and
  pseudo consoles share one cell model. Done when a full-screen program
  draws the same in a console window and through a pseudo console.
  libvterm, the Terminal's emulator, now edits every screen buffer's
  cells, and `ptyscreentest` compares a full-screen program's cells in a
  console window, under a pseudo console and as the pseudo console's host
  draws them.
- [x] **26.2 ConPTY end to end.** Complete process attachment, pipe lifetime,
  resize and VT output together. Done when a terminal app runs and resizes
  an interactive `cmd.exe` through `CreatePseudoConsole`. The pseudo
  console turns the host's keys (VT sequences included) into key records,
  echoes and edits lines as conhost does, and draws its screen again on
  `ResizePseudoConsole`; `conptytest` types into, resizes and exits
  `cmd.exe` as a terminal host and draws the same cells.
- [x] **26.3 NTFS robustness.** Verify interrupted writes and a full disk:
  after a power cut during writes, `chkdsk` reports no errors; a full disk
  returns `ERROR_DISK_FULL` without corruption. A volume NovaOS was cut
  off writing to (drive C: included) is checked and repaired from its MFT
  records at the next mount, as chkdsk would. Cut off after each of a
  churn's 800-odd disk writes in turn (`tests/tools/test_ntfs_cut.py`)
  and after a machine reset (`ntfstest cut`), it comes back with every
  file whole and passes `scripts/ntfs-check.py` with no errors.
- [x] **26.4 Performance budget.** Build on the lock and redraw work in
  PRs #297–#306. CI should report boot time, Firefox start time and
  `smpstress` scaling on every run and fail on a large regression.
  CI's **Performance budget** job (`tools/ci/perf.py`, part of the
  "Build and boot-test" check) boots NovaOS once, times the boot to the
  desktop and the bundled Firefox from `start` to its first page on the
  screen, and runs `smpstress scaling 0` on four CPUs. A time over its
  budget (`tools/ci/perf-budget.json`: 20 s to boot, 10 s for Firefox,
  judged only under KVM), Firefox never drawing its page, or files and
  the registry scaling less than half as well as bare system calls fails
  the check; every run's numbers are in its summary.

### Phase 27: Apps for everyday use

**Goal:** make NovaOS a desktop someone can use all day. **Gate:** 40 GUI
apps from the App Store catalog pass their nightly scripts.

- [x] **27.1 Grow the corpus.** Add an App Store app per pull request,
  starting from failures in the existing corpus, until 40 GUI apps pass.
  The corpus had no failures left (39 of 39 on 2026-10-13), so it starts
  from the Store's untested apps: WinSCP (installs and opens its Login
  dialog; [history](history/2026-10-14-winscp.md)), Thunderbird (installs
  and opens an e-mail message;
  [history](history/2026-10-14-thunderbird.md)), qBittorrent (installs
  and opens a `.torrent`; [history](history/2026-10-14-qbittorrent.md)),
  OBS Studio (installs, adds a scene and saves it;
  [history](history/2026-10-14-obs-studio.md)), GIMP (installs and
  opens a picture through its plug-ins;
  [history](history/2026-10-14-gimp.md)) and ShareX, the fortieth
  (installs, captures the screen with its Print Screen hot key and saves
  a PNG; [history](history/2026-10-15-sharex.md)).
- [x] **27.2 An office suite.** Add LibreOffice from the catalog; open, edit
  and save `.docx` and `.xlsx` files.  LibreOffice 24.8 installs silently
  from the Store's Windows Installer package; the nightly corpus types a
  line into a `.docx` in Writer and a formula into an `.xlsx` in Calc,
  saves both in their own formats and reads the edits back out of the
  saved files ([history](history/2026-10-14-libreoffice.md)).
- [x] **27.3 Krita.** Provide Mesa OpenGL 2 or later; draw and save an image.
  OpenGL is built in (Mesa's llvmpipe, OpenGL 4.5); the corpus test
  installs Krita from the App Store, draws a stroke and saves it as a
  `.kra` ([history](history/2026-10-14-krita-saves.md)).
- [x] **27.4 The public web.** Load a publicly trusted HTTPS site in Firefox
  from the T14 over its own network.  Done 2026-10-15: Dean confirmed it on
  the T14.
- [x] **27.5 App Store badges from CI.** Show each app's verified badge from
  the latest nightly run (completing 20.7).  Each scheduled nightly run
  pushes its verdicts to the `store-badges` branch; a catalog refresh
  fetches them, and the App Store shows "Verified: Silver" or "Failed
  last nightly" beside each program the corpus tests with the Store's
  own download, offline too ([history](history/2026-10-14-store-badges.md)).

### Phase 28: Laptop hardware

**Goal:** make the T14 fully usable away from a desk. **Gate:** it goes
online over Wi-Fi, takes a video call and runs a day on battery with sleep.

- [ ] **28.1 Wi-Fi.** Support the Intel AX211 and its redistributable
  firmware; join WPA2 and WPA3 networks and fetch HTTPS content. Intel's
  own Windows driver (`Netwtw14.sys`) already downloads by hardware ID,
  installs and registers with NovaOS's NDIS host on a stand-in device in
  QEMU (step 30.9). NovaOS's WDI upper edge (`kernel/nt/wdi.c`) has
  five parts done. The first is the command layer: adapter capabilities,
  the station port and scans, with `netsh wlan show interfaces` and
  `netsh wlan show networks`. The second is the data path: the TAL TX/RX
  interface, so 802.11 frames go both ways, plus joining open networks
  with `netsh wlan connect`, after which DHCP and ping run over Wi-Fi.
  The third is WPA2-Personal (`netsh wlan connect name=SSID
  key=PASSPHRASE`): the 4-way handshake in `kernel/nt/wpa.c`, the EAPOL
  exemption and the keys handed to the driver
  (`OID_WDI_SET_ADD_CIPHER_KEYS`). The fourth is WPA3-Personal: SAE in
  `kernel/nt/sae.c` through the driver's `SAE_AUTH_PARAMS` messages, then
  the WPA3 4-way handshake with management frame protection's IGTK. The
  fifth is for people and programs: Settings' Wi-Fi page lists the
  networks and joins one with its password, and `wlanapi.dll` (the Native
  Wifi API) gives programs the adapters, scans, network lists, profiles
  and connections with their notifications. All five are tested with
  NovaOS's own WDI miniport on QEMU's `edu` device (the devices suite's
  `wifi` boot). Still needed: scanning and joining a network on the T14's
  card.
- [ ] **28.2 Bluetooth.** Pair and use a Bluetooth mouse and headset, including
  after sleep. Done in QEMU: NovaOS's Bluetooth host stack
  (`kernel/nt/bthport.c`, the role of Windows' `bthport.sys`: HCI, inquiry,
  Secure Simple Pairing and PIN pairing, link keys in the registry, L2CAP
  and an SDP client), its USB transport `bthusb` and HID over Bluetooth
  (`hidbth`), with `bthprops.cpl`'s radio and device API. The inbox
  `bth.inf` lets a vendor's radio package such as Intel's `ibtusb` put its
  driver under `bthusb` as Windows does. Shown with a fake USB radio and a
  mouse and a headset in range in the devices suite's `bluetooth` boot:
  inquiry, pairing both, the mouse moving the cursor and reconnecting
  after a restart, an `ibtusb`-style filter installed with `pnputil`.
  Headset audio is done in QEMU too (`kernel/nt/bthaudio.c`): A2DP stereo
  with NovaOS's own SBC encoder and the hands-free profile's voice link
  (eSCO on the radio's isochronous endpoints) give a paired headset two
  outputs and a microphone, as a USB headset has; the `bluetooth` boot
  plays a tone through each and records from the headset. Still needed:
  reconnects the device starts, Settings' Bluetooth page, and pairing a
  real mouse and headset on the T14 with Intel's driver, including after
  sleep.
- [ ] **28.3 Camera.** Add USB Video Class capture through Media Foundation;
  show the built-in webcam in a camera app. Done in QEMU: the UVC class
  driver (`kernel/nt/usbvideo.c`), Media Foundation's capture path
  (`MFEnumDeviceSources`, the camera media source and `IMFSourceReader`
  with RGB32 conversion) and the Camera app, shown with a fake UVC camera
  in the devices suite's `camera` boot; isochronous streaming (URBs in
  `usbd.c`, the driver choosing the alternate setting) and MJPEG frames
  decoded by the source reader, shown with a fake isochronous MJPEG
  camera in the `isocam` boot. Still needed: a boot of `latest` on the
  T14 showing its built-in webcam's picture in Camera.
- [ ] **28.4 Battery life.** Add processor idle states and device power
  management; measure idle power within 50% of Windows on the same T14.
  Done in QEMU: C-states from `_CST` (after `_OSC`/`_PDC`) entered with
  MWAIT, a P_LVLx port or HLT (`kernel/ke/idle.c`, `kernel/hal/cstate.c`),
  the tick stopped on idle CPUs other than the first, and idle devices of
  Windows drivers sent to D3 through `PoRequestPowerIrp` (KMDF's S0 idle,
  WDM idle detection, PCI PMCSR in the bus driver); tested in the devices
  suite's `laptop` boot (`idletest`) and `kmdfedu` boot (D3 and back).
  Still needed: the idle-drain measurement on the T14
  ([install-and-power.md](install-and-power.md#checks-on-the-t14), check 7),
  then PoFx runtime idle and `_LPI` if the drain is too high.
- [ ] **28.5 A second reference PC.** Pick an AMD machine and list its
  devices in [hardware.md](hardware.md), each marked supported or missing.

### Phase 29: NovaOS 0.2 release

**Goal:** a release that runs games and works on real laptops. **Gate:** a
tagged `v0.2.0` passes the full CI suite and T14 checks.

- [x] **29.1 Release.** Tag the release, attach the ISO and checksums, and
  write release notes from [HISTORY.md](HISTORY.md). The release page's ISO
  must boot in QEMU and on both reference PCs. `v0.2.0` ("Kepler",
  [release notes](releases/0.2.0.md)) was published on 2026-10-19 and its ISO
  was confirmed to boot on the T14 the same day. Ticked on that; the second
  reference PC (28.5) is not picked yet, so the ISO has not booted there.

### Phase 30: Windows drivers

**Goal:** run unchanged Windows driver packages (`.inf` with its `.sys`,
`.cat` and firmware) the way Windows 11 does, beside NovaOS's own built-in
drivers. Design: [design/windows-drivers.md](design/windows-drivers.md)
(#343). **Complete:** all eleven steps were merged on 2026-10-12 and
2026-10-13, each checked by a boot of the devices suite in QEMU.
[drivers.md](drivers.md) describes what ships.

- [x] **30.1 Load a kernel-mode driver** (#345). A kernel PE loader and
  the `ntoskrnl.exe`/`hal.dll` export layer (`kernel/nt/`); a test driver
  (`wdmtest.sys`) starts with `sc start`, serves a program's `ReadFile`
  and `DeviceIoControl` as IRPs and unloads with `sc stop`.
- [x] **30.2 The kernel API a driver lives on** (#346, #349). IRQL in
  `CR8`, DPCs, spin locks, events, timers, the pool, MDLs, kernel-mode
  SEH, work items, system threads, `Zw*` registry and file calls and
  ETW/WMI stubs; CI checks that every import of `vioser.sys`,
  `Wdf01000.sys` and `WdfLdr.sys` is provided
  (`tools/ci/check-driver-imports.py`).
- [x] **30.3 PnP drivers on hardware resources** (landed with #357; MSI
  and MSI-X in #359). `pnputil /add-driver /install` puts a driver on a
  PCI device with no built-in driver, and the PnP manager starts it with
  the device's memory, interrupt (line, MSI or MSI-X) and DMA; tested on
  QEMU's `edu` device.
- [x] **30.4 KMDF** (#353, #363). `Wdf01000.sys`, built from Microsoft's
  MIT-licensed WDF source, and NovaOS's `WdfLdr.sys` run a KMDF control
  device and a KMDF driver for the `edu` device.
- [x] **30.5 The first third-party driver** (#357). virtio-win's
  `vioser.sys` installs with `pnputil`, runs on KMDF, and a program
  exchanges bytes with the host through `\\.\Global\<port name>`.
- [x] **30.6 Downloading drivers by hardware ID** (#347).
  `pnputil /scan-devices /search-online` finds a device's package in the
  Microsoft Update Catalog, downloads it and stages it.
- [x] **30.7 Signing policy** (#348). A kernel driver loads only with a
  Microsoft driver signature, a test-signed one only with test signing on,
  an unsigned or tampered one never; a package whose files do not match
  its catalog is not staged.
- [x] **30.8 USB client drivers** (#356). FTDI's own driver, downloaded by
  hardware ID, runs QEMU's `usb-serial` converter through NovaOS's USB bus
  interface as a COM port.
- [x] **30.9 Network miniports through NDIS** (#361; Intel AX211 in #362,
  #364, #365). NovaOS's NDIS host runs virtio-win's `netkvm.sys` on
  QEMU's virtio-net card for DHCP, ping and downloads; Intel's AX211
  Wi-Fi driver loads and registers on a stand-in device (joining a
  network is step 28.1).
- [x] **30.10 UMDF 2** (#360). The reflector `WUDFRd.sys` starts
  `WUDFHost.exe`, which runs a UMDF 2 driver on `Wudfx02000.dll` (built
  from the same WDF source) and serves a program's I/O.
- [x] **30.11 Storage miniports through Storport** (#355). NovaOS's
  Storport host runs virtio-win's `viostor.sys` on a QEMU virtio-blk disk,
  and NovaOS mounts the NTFS volume on it.

The HID class host (`hidclass.sys`'s part, with NovaOS's USB HID
minidriver) is built; test `hid`.  USB keyboards and mice go through it
to the `kbdhid` and `mouhid` class drivers; test `kbdmoutest`.  USB touch screens, pens and
precision touchpads go to the `hiddigi` digitizer class driver; test `digitest`.  USB HID game pads and
joysticks go to the `gamehid` class driver, which feeds DirectInput's controller slots while Raw Input and
`hid.dll` read the collection itself; tests `padtest` and `rawpadtest`.  Later, only when a target device
needs them: PortCls and AVStream drivers, and the full WDDM render path (step 25.3 onwards).

### Phase 31: DOS programs

**Goal:** 16-bit DOS programs (`.COM` and `.EXE`) run in a NovaOS console
window, started from `cmd.exe` or Explorer, as they did on Windows through
NTVDM. The DOS is MS-DOS 4.00 built from Microsoft's MIT-licensed source
([github.com/microsoft/MS-DOS](https://github.com/microsoft/MS-DOS)),
unchanged; since x86-64 long mode has no virtual-8086 mode, it runs on a
small real-mode CPU inside a NovaOS process. See [dos.md](dos.md).

- [x] **31.1 Build MS-DOS 4.0 from source.** CI builds `v4.0/src` with
  the tree's own MASM, C compiler, LINK and NMAKE
  (`tools/msdos/build_msdos.py`) and boots the result from a floppy image
  in QEMU (`tools/msdos/boot_test.py`): `COMMAND.COM` reports
  "MS-DOS Version 4.00" and `MEM.EXE` runs.
- [x] **31.2 A real-mode CPU and a PC BIOS for it.** An 8086/80286
  interpreter (MIT/zlib/BSD code, no GPL) with the BIOS services DOS uses
  (video text mode, keyboard, disk, clock) on a NovaOS console window;
  the built `IO.SYS` and `MSDOS.SYS` boot on it to the `COMMAND.COM`
  prompt.  `ntvdm.exe` runs DOS on NovaOS's own 8086/80186 interpreter
  and a BIOS in C, and boots the MS-DOS 4.00 disk the build now ships
  (`C:\Windows\System32\DOS\MSDOS.IMG`) to `A:\>` in the console
  window; `dostest` types `VER` and `DIR` there, and CI's MS-DOS job runs
  the same CPU and BIOS on Linux through `MEM`, `CHKDSK` and `DEBUG`.
- [x] **31.3 Start DOS programs like Windows programs.** `CreateProcess`
  on a 16-bit image starts the DOS host (as Windows starts `ntvdm.exe`),
  so a `.COM` or `.EXE` runs from `cmd.exe`, the Terminal and Explorer,
  with the NovaOS drives visible to DOS.  The kernel's process start runs
  a DOS `MZ` or `.COM` image as `ntvdm.exe -run`; DOS sees NovaOS's C:
  through its own network redirector interface (`INT 2Fh AH=11h`, 8.3
  aliases for long names), `REDIR.COM` runs the program and the console
  returns with its exit code; `dostest` runs a `.COM` from `cmd` by name
  and an `.EXE` from `CreateProcess`, each reading and writing a file on
  C:.
- [x] **31.4 Programs and documentation.** A DOS utility and a freeware
  DOS game bundled for CI, their compatibility entries, a screenshot,
  and the user guide.  bootRogue (BSD 2-clause, `third_party/bootrogue`)
  ships as `C:\Tests\Dos\ROGUE.COM`; `dostest` plays it with the arrow
  keys and CI's MS-DOS job plays it and writes a file in `EDLIN`;
  compatibility entries for MS-DOS 4.00's utilities and bootRogue,
  `docs/screenshots/dos-bootrogue.png`, and [dos.md](dos.md) as the
  user guide.

### Phase 32: Windows.UI.Xaml

**Goal:** UWP and XAML apps draw their windows on NovaOS: the Windows
Runtime's app model (`CoreApplication`, `CoreWindow`), its compositor,
and the XAML framework built from Microsoft's MIT source
([github.com/microsoft/microsoft-ui-xaml](https://github.com/microsoft/microsoft-ui-xaml))
as `Windows.UI.Xaml.dll`, then WinUI 2 and app packages.  See
[design/windows-ui-xaml.md](design/windows-ui-xaml.md).

- [x] **32.1 WinRT activation from the registry; CoreApplication and
  CoreWindow.** `RoGetActivationFactory` loads the in-process server
  named under `ActivatableClassId`; `twinapi.appcore.dll` runs an
  `IFrameworkView` through `CoreApplication.Run` on a `CoreWindow` with
  its events and dispatcher; DXGI presents into a CoreWindow.  Test
  `coreapptest`.
- [x] **32.2 Windows.UI.Composition.** The compositor's visuals, brushes
  and drawing surfaces on NovaOS's DirectComposition; CoreWindow's
  pointer events.  Test `comptest`.  Since the Phase 32 push: the later
  interface versions XAML's compositor tree host asks for (`ICompositor2`
  to `8`, `IVisual2` to `4`, `ICompositionObject2` to `5`), answered
  under the Windows App SDK's `Microsoft.UI.Composition` IIDs as well,
  property sets, key frame animations, animation groups, scoped batches,
  easing functions and rectangle clips (expression animations are kept,
  not yet evaluated).
- [x] **32.3 The XAML core builds.** The `dxaml` core as
  `Windows.UI.Xaml.dll`: `Application.Start` runs and `XamlReader.Load`
  parses a page.  First part done: Direct2D 1.1's devices and device
  contexts, and DirectComposition and Composition surfaces drawn with
  them, which XAML draws its text and shapes with (test `d2dctxtest`).
  Second part done: the Windows Runtime C++ headers MIDL would make,
  generated by `tools/winrt/abigen.py` from Windows' metadata and the
  XAML framework's IDL (test `winrtabitest`).  Third part done: the
  `dxaml` toolchain (Microsoft's MSBuild projects read, WIL vendored,
  NovaOS's WRL, the Windows SDK pieces MinGW-w64 lacks, MIDL's per-IDL-file
  headers, ETW compiled out) and the XAML core's lowest layer built from its
  sources (test `xamlbasetest`).  Fourth part done: NovaOS stand-ins for
  the closed Windows App SDK headers the core's lower layers include
  (`InputSiteHelpers.h` and FrameworkUdk's other helpers, the lifted input
  and composition interop headers, WRL's `async.h`), the type metadata
  built and linked (every built-in type and dependency property, looked up
  in `xamlbasetest`) and `CDependencyObject`'s property system compiled.
  Fifth part done: the collections and the value types under them
  (`CValue`, flyweight, primitive dependency objects, math, theming)
  built and linked with `CDependencyObject`, stand-ins for the composition
  tree's `CompositionHelper.h`, and a `DependencyObjectCollection` filled,
  searched and emptied in `xamlbasetest` on Microsoft's unit-test stubs.
  Sixth part done: the resources (resource keys, dictionaries' maps, the
  theme walk's cache) and the XAML markup parser (`core/Parser`) built,
  reading XML through NovaOS's new `xmllite.dll` (over libxml2's reader),
  with `IUriBuilder` in urlmon; a page, `{StaticResource}`, a
  `ResourceDictionary` and bad markup parsed in `xamlparsetest`.  Seventh
  part done: `CResourceDictionary` (`core/core/elements/Resources.cpp`)
  and the parser's object writer runtime (`core/Parser/runtime`) built,
  so XAML text loads into the core's objects: a `ResourceDictionary` of
  strings, numbers, booleans and colours, looked up by key, in
  `xamlloadtest`.  Eighth part done: the properties' runtime data (the
  field offsets the core's metadata tables give field-backed properties)
  for the classes that build, so a `ResourceDictionary`'s
  `MergedDictionaries` and `ThemeDictionaries` load from XAML and lookup
  searches them (last merged dictionary first, then the theme's) in
  `xamlloadtest`.  Ninth part done: the core services (`CCoreServices`,
  `core/dll/xcpcore.cpp`) built, with generated stand-ins that name each
  function of the rest of the core it calls until that builds, and XAML
  loaded through `CCoreServices::ParseXaml` (`XamlReader.Load`'s path) in
  `xamlloadtest`.  Tenth part done: `CCoreServices::Create` runs for
  real on Microsoft's platform layer (`CWindowsServices`, handed to the
  core through `ObtainPlatformServices`) with the text core, media queue,
  render target bitmaps, geometry builder and system theming it makes,
  built from their sources, in `xamlloadtest`.  Eleventh part done: the
  framework's entry points from `dxaml/lib` (`DXamlCore`, `XamlReader`,
  `FrameworkApplication`, the instance storage and `DXamlServices`) built
  from their sources on that core, with `XamlReader`'s activation factory
  made and called in `xamlloadtest`.  Twelfth part done: the
  `DispatcherQueue` every XAML thread runs on (`CoreMessaging.dll`, both
  `Microsoft.UI.Dispatching` and `Windows.System`, test `dispatchqtest`),
  the restricted error info in ole32, and the control layer
  (`CJupiterControl`, `CControlBase`, `CXcpDispatcher`, the browser host)
  and the error helper built from their sources, with
  `CJupiterControl::Create` running up to the rendering seam in
  `xamlloadtest`.  Thirteenth part done: the rendering seam (the
  graphics device manager with its Direct3D 11 and Direct2D devices made
  on their own thread, the compositor and UI thread schedulers, the
  window render target with its hardware walk and compositor tree host)
  built from their sources, so `CJupiterControl::Create` succeeds in
  `xamlloadtest`, on NovaOS's software Direct3D 11 device without DXVK.
  Phase 32 push: the XAML dispatcher
  (`DispatcherImpl`), the default styles' cache (`DefaultStyles`), the
  `Window` and the framework's peer base class (`DependencyObject`)
  built from their sources, the dispatcher and styles run in
  `xamlloadtest`.
  Value boxer part (Phase 32 push) done: the
  Windows Runtime's value boxes (`wintypes.dll`'s
  `Windows.Foundation.PropertyValue`, test `propvaltest`), the value
  boxer `CValueBoxer`, the lifetime library (`ReferenceTrackerManager`),
  `StaticStore`, the release queue and the core's error service built,
  with the framework started as the XAML compiler starts it and parsed
  values unboxed in `xamlloadtest`.
  `XamlReader.Load` part (Phase 32 push) done: the element classes, the
  `TextBlock` text core, the real type table, `CUIElement` and
  `CFrameworkElement`, and the framework peers of `Grid`, `TextBlock`,
  `Button` and the classes under them built from their sources, so
  `XamlReader.Load` of a `Grid` with a `TextBlock` and a `Button` hands
  back their framework objects on a thread started as
  `Application.Start` starts it, in `xamlloadtest`.
  `Application.Start` part (Phase 32 push) done, completing 32.3:
  `Application.Start` built from its sources takes the desktop path: a
  `DispatcherQueue` on the thread, the app's initialization callback
  (in which the app composes its `Application` over the framework's),
  `WindowsXamlManager` starting the application on the thread (the core's
  application, visual tree, content root, layout, focus, time manager and
  input services), `OnLaunched` loading a page with `XamlReader.Load`,
  queued work run from the desktop message loop and `Application.Exit`
  returning from `Application.Start`, in `xamlapptest`; ole32 serves
  classes a program registers with `RoRegisterActivationFactories`.  A
  `Window` (`DesktopWindowImpl`, on the Windows App SDK's content
  islands) comes with 32.4.
- [ ] **32.4 A XAML page draws.** A `TextBlock` and a `Button` laid out,
  drawn, typed into and clicked in a self-test, with a screenshot.
  Content islands part (Phase 32 push) done: the Windows App SDK's
  lifted content, input and windowing classes a `Window` sits on
  (`Microsoft.UI.Input.dll`: `ContentIsland`, `DesktopChildSiteBridge`,
  the island's input sources and focus controller, `AppWindow`), in
  `contentislandtest`, and the framework's `DesktopWindowImpl`,
  `DesktopWindowXamlSource`, island root and window chrome built.  Theme
  resources part done: the framework's control styles and theme
  resources (`themes/generic.xaml`, split as Microsoft splits it) in
  `Microsoft.UI.Xaml.Resources.19H1.dll`, as XAML text (the XBF compiler
  is Windows-only), found and parsed when a `Window` is created, with the
  core's resource provider and the framework's metadata provider; checked
  in `xamlapptest`.  Fonts part done: DirectWrite's Windows 10 font
  model (`IDWriteFactory4` to `7`, font sets, family models, font faces
  4 to 6, text layouts 1 to 4, text analyzers 1 and 2), which the theme
  resources' first `FontFamily` and XAML's text formatting start from;
  checked in `dw6test`.  Island part done: NovaOS's own `XamlIsland`
  (Microsoft's open sources ship only its header), which a `Window`'s
  `DesktopWindowXamlSource` makes its island root through.  Root
  `ScrollViewer` part done: the framework's `ScrollViewer`,
  `ScrollContentPresenter` and `RootScrollViewer` (with
  `ScrollContentControl`, `SolidColorBrush` and the core's generated
  control classes), which a page set as a `Window`'s content is put in.
  First-frame part done: NovaOS's own `CompositionHelper` (the Windows
  App SDK's is closed) on `dcomp.dll`, whose `Compositor` is now also
  its DirectComposition device, and what the first frame makes after it
  (the island's `FocusController`, the window's present target, the
  popup, print, full-window media and transition roots' peers, the focus
  event args); a page test window now runs to its checks.  Page-drawn
  part done: the page's first frame, a `Border`, a `TextBlock` and a
  `Button`, laid out and drawn through the framework's composition tree
  (its render walk, composition nodes, transforms, geometry clips and
  DirectWrite text drawn into DirectComposition surfaces) on
  `dcomp.dll`'s compositor, which gained mask brushes (XAML's text is
  its glyphs' alpha over a colour brush), surface brush and clip
  transforms; NovaOS's `XamlIsland` now asks for its content, the
  island's `CompositionSurfaceHelper` is NovaOS's own, and the window
  closes cleanly; checked in `xamlpagetest`, with
  [a screenshot](screenshots/winui3-first-page.png).  Left: the page
  typed into (a `TextBox`) and clicked (the `Button`).
- [ ] **32.5 WinUI 2.** `Microsoft.UI.Xaml.dll` 2.x on the system XAML.
  First part done: the framework under its `Windows.UI.Xaml` names,
  which WinUI 2 and UWP apps are compiled against.  `tools/winrt/winfaces.py`
  writes, from Windows' metadata and the framework's IDL, a face for each
  `Windows.UI.Xaml` interface over the framework's lifted one (2,106
  faces, 7,550 methods matched by name, the rest answering `E_NOTIMPL`
  and naming themselves), and the framework's overridable interfaces,
  delegates and collections over Windows' ones; the framework objects'
  `QueryInterface` hands them out, its classes are activated by their
  Windows names, a Windows-boxed enum or struct is boxed again as the
  framework's, a `TypeName` is renamed both ways, and an app's control
  composed over a `Windows.UI.Xaml` class has its Windows overrides
  called by the framework's layout.  Test `wuxamltest`.  Second part
  done: WinUI 2's own controls built from its `winui2/main` sources as
  `Microsoft.UI.Xaml.dll` 2.x (`userland/microsoft_ui_xaml`): its IDL
  written to a `.winmd` (`tools/winrt/winmdwrite.py`), C++/WinRT's
  generator (vendored, built for the build machine) writing the
  projection and component files, its XAML type table written as
  Microsoft's T4 template writes it; the inner-loop feature set and
  `InfoBadge` so far.  The framework's metadata provider asks the app's
  `Application` for WinUI 2's types, and `urlmon.dll` serves
  `Windows.Foundation.Uri`.  Test `winui2test`.  Third part done: the
  interface IDs apps were compiled with (Microsoft's, from the
  `Microsoft.UI.Xaml` 2.8.7 package's metadata, for the interfaces MIDL 3
  synthesizes) and WinUI 2's XAML resources: each feature's pages merged
  as Microsoft's build merges them (its MSBuild tasks ported to
  `userland/microsoft_ui_xaml/mergexaml.py`, `CommonStyles` included) into
  the `19h1_themeresources.xaml` and `19h1_generic.xaml` files
  `XamlControlsResources` and the controls load (`C:\Programs\Microsoft.UI.Xaml\Themes`;
  loading them waited on the framework's data binding and 32.4's fonts).
  Fourth part done: the framework's data binding (`{Binding}`, `Binding`,
  `BindingExpression`, `RelativeSource`, property paths) and control
  templates (`ApplyTemplate` with `{TemplateBinding}` and `Style`
  templates, the default styles' theme transitions), the parser's error
  texts, and the UWP start-up as far as the app's `Application`:
  `winui2test` installs itself as a UWP app and starts by its AUMID,
  `Application.Start` takes its UWP route (the framework's view source,
  view and `CoreWindow` wrapper; `CoreApplication`'s `ICoreApplication2`
  and `CoreCursor`).  Next: the animation component (the core's
  `DispatcherTimer`, where the UWP route stops) on to `Window.Current`,
  WinUI 2's theme resources (`AcrylicBrush`'s inherited `FallbackColor`)
  and its controls' default styles.  Fifth part done: the animation
  (the core's animation library, the animation component, the
  framework's `DispatcherTimer`, `Storyboard` and animation classes) and
  WinUI 2 types' properties inherited from the framework's types
  (`AcrylicBrush.FallbackColor`); the UWP route now stops at the window's
  present target (32.4's composition) and the theme resources at
  `InkToolbarFlyoutItem`, a type the framework lacks.  Next: those two,
  the controls' default styles, and the controls Calculator uses
  (`NavigationView`, `SwipeControl`, `RadioButtons`, `ProgressRing`,
  `BackdropMaterial`, with `ItemsRepeater`); Calculator itself is a .NET
  Native app, which needs that runtime's framework packages too.  Sixth
  part done: the UWP start-up runs through: inside its package
  `winui2test` gets `Window.Current` (the view's window), its
  `CoreWindow`, sets its content, activates it and exits
  (`CoreWindow`'s `ICoreWindow5`, `CompositionTarget`, `XamlLight`,
  `WindowCreatedEventArgs`); WinUI 2's theme resources leave out what
  neither WinUI 2 nor the framework has (`InkToolbar`, `Pivot`,
  `LoopingSelector`, `MediaTransportControls`: Windows-only controls),
  register attached properties and content properties as Microsoft's
  XBF compiler resolves them, and find `DesignMode` and `PowerManager`
  (twinapi.appcore); they now stop at `RevealBrush.TargetTheme`
  (`E_NOINTERFACE` inside WinUI 2's reveal code).  Seventh part done:
  `XamlControlsResources` loads whole and answers lookups (the Windows
  faces pass `Windows.UI.Composition` objects as the framework's
  `Microsoft.UI.Composition` ones; the merged theme resources list each
  resource after those it names, as XBF's lookup-time creation allows;
  WinUI 2's `AcrylicBrush` answers Windows' `IAcrylicBrush`).  Next: the
  controls' default styles applied (`InfoBadge`'s template), then
  Calculator's controls; Calculator's start-up itself waits on the .NET
  Native framework packages (32.6).
- [ ] **32.6 Packages.** `.msix`/`.appx` install and start, `resources.pri`;
  then the Calculator package.  First part done: `appx` installs,
  lists, removes and starts unsigned packages (block map checked, signed
  packages refused until their signatures can be verified), package
  identity in kernel32, packaged Windows Runtime classes,
  `ApplicationActivationManager`, `resources.pri` through Microsoft's MRT
  Core (`MRM.dll`), test `appxtest`.  Next, for Calculator: bundles,
  framework packages (VCLibs, `Microsoft.UI.Xaml`) and package signature
  verification, after 32.3 to 32.5.

---

### Phase 33: Everyday OS

**Goal:** NovaOS is safe and recoverable enough to be someone's only
computer: it has accounts and a sign-in, runs apps without administrator
rights, can be updated and rolled back, installs beside Windows, and prints.
Each step follows the Windows way: the same account, token, elevation,
servicing and print models Windows apps and drivers already expect.
**Gate:** after 0.2 (Phase 29); starts once the gap list ahead of it is done.

- [x] **33.1 User accounts and sign-in.** Local accounts in a SAM/LSA
  store, a sign-in and lock screen (password or PIN), `LogonUser`,
  per-user profiles with their own `HKCU` hive, and fast user switching.
  Done when a second account is created in Settings, signs in, gets its own
  profile and registry hive, switches with the first account's apps still
  running, and a wrong password is refused (self-test `accounttest`, plus a
  screenshot of the sign-in screen).
  Done: local accounts in the SAM with Argon2id passwords, a token,
  profile and `HKCU` hive per account, `LogonUser`, `NetUserAdd` and the
  rest of `netapi32`, `LoadUserProfile`, the sign-in screen at boot and the
  lock screen (Win+L, `LockWorkStation`), tested by `accounttest`;
  Settings' Accounts page (add, remove, account type, password, picture),
  fast user switching from the lock and sign-in screens and the Start
  menu's account button, and sign-out, tested by `switchtest`, whose
  screenshot is the sign-in screen with both accounts.  Per-user desktop
  settings and moving the first profile to `C:\Users` follow
  ([design/accounts.md](design/accounts.md)).
- [x] **33.2 Standard-user security and UAC.** New accounts are standard
  users with a filtered token and integrity levels; `consent.exe` shows the
  real UAC consent flow on a secure desktop for elevation. Done when a
  standard user running an installer sees the consent prompt, a wrong
  administrator password is refused, an unelevated process cannot write to
  `C:\Windows` or `HKLM`, and a medium-integrity process cannot open a
  high-integrity one (self-test `uactest`).
  *Done:* UAC's prompt on the secure desktop: `runas` and a manifest's
  `requireAdministrator` or `highestAvailable` ask an administrator Yes or
  No and a standard user for an administrator's password (a wrong one
  refused), as the Windows policy values say; tokens carry integrity
  levels and a medium process cannot open a high one for more than
  waiting on it; `C:\Windows` and `C:\Programs` have Windows' DACLs and
  `HKLM` refuses writes from programs that are not elevated, which still
  read them; the Terminal's `sudo` runs a program as administrator after
  the prompt; all tested by `uactest`.  Windows' virtualization of those
  writes for old programs, the linked token at identification level and
  the publisher from the signature follow ([design/uac.md](design/uac.md)).
- [x] **33.3 Update rollback and a recovery boot.** Updates install into a
  new boot image beside the running one (A/B or snapshot) and roll back
  automatically when the new image fails to reach the desktop; a recovery
  boot offers safe mode, last known good drivers, startup repair and reset.
  Done when CI applies a deliberately broken update in QEMU and the next boot
  returns to the previous image on its own, and the recovery menu starts safe
  mode and a repair run (boot test `rollbacktest`).
  Part 1: a kernel stop during an update's first start restarts the PC,
  which goes back to the previous kernel on its own; the devices suite's
  `update` boot stages a kernel stamped to stop (`tools/mkupdate.py
  --fail-boot panic`) and checks it.
  Part 2: an update's first start counts as good only once the desktop is
  up with someone signed in or the sign-in screen showing; until then the
  chipset's TCO watchdog resets a kernel that hangs, and a start that makes
  no progress for 90 seconds restarts itself, both going back to the
  previous kernel (checked with `--fail-boot hang` and `stall`).
  Part 3: the boot loader's Advanced startup menu (F8, `shutdown /r /o`, or
  by itself after two starts that did not reach the desktop) starts the
  previous version (the known-good set: `kernel.old` with `bootx64.old`),
  safe mode (NovaOS's own drivers and the firmware's framebuffer) and
  startup repair (boot files put back from the known-good set, an
  unfinished update removed, a report in `SrtTrail.txt`); the devices
  suite's `rollbacktest` boot starts each.
  Leftovers: the firmware's ACPI WDAT watchdog drives the hang watch where
  the firmware describes one (else the TCO timer), Reset this PC removes
  apps and settings and keeps the accounts and their files, and Settings >
  System > Recovery has Reset PC and Advanced startup's Restart now; the
  `rollbacktest` boot checks the first two (design in
  [design/recovery.md](design/recovery.md)).
- [x] **33.4 A graphical installer that installs beside Windows.** Replace
  the whole-disk `install` command with a graphical installer that shrinks an
  existing NTFS partition, adds NovaOS to the firmware boot menu (and chains
  Windows Boot Manager so a boot menu lists both), and keeps user data on a
  reinstall. Done when, in QEMU, an installer run on a disk holding a
  Windows-style NTFS volume leaves that volume's files intact, boots NovaOS
  and offers the other system in the boot menu; a screenshot of each
  installer page is in `docs/screenshots/`.
  *Part 1 ([design](design/installer.md)):* Setup's window
  shows each disk's partitions and free space and installs NovaOS beside
  them in the free space (its own ESP and data partition; Windows'
  partitions untouched), tested by the devices suite's `beside` boot;
  *Part 2:* Setup adds the firmware boot entry "NovaOS"
  first in `BootOrder` through the UEFI runtime services, and NovaOS's
  boot loader offers "Windows Boot Manager" in a boot menu and starts it;
  *Part 3:* on a disk Windows fills, Setup shrinks Windows' NTFS volume to
  make room (NovaOS's own NTFS driver moves the files past the new end,
  then cuts `$Bitmap`, `$BadClus` and the boot sector; the partition entry
  is made smaller), and reinstalling over NovaOS formats only its ESP and
  keeps drive C: (`NOVADATA`, `C:\Users`); tested on the host
  (`tests/host/ntfs_shrink.c`, cut off at every point) and by the devices
  suite's `shrink` boot, which checks every Windows file byte for byte;
  each Setup page's screenshot is listed in the design.
- [x] **33.5 Printing.** The Windows print model: a spooler service
  (`spoolsv`), `winspool.drv` and the `winspool` API fully implemented,
  Print Management, real printer drivers loaded unchanged where they are
  user mode, driverless IPP network printing, and an XPS/PDF print path with
  "Microsoft Print to PDF". Done when Notepad and SumatraPDF list a printer,
  print a page to the PDF printer and to an IPP printer served by a test
  host in CI, and the output matches a reference file (self-test
  `spooltest`); WIA scanning is a follow-up.
  Part 1: the Print Spooler service, `winspool.drv`, Microsoft Print to
  PDF and the print dialogs (`spooltest`). Part 2: driverless network
  printing (IPP ports in the spooler sending PDF or PWG Raster, offline
  printers keeping their jobs) and Settings > Printers (add a network
  printer by address, named after its model; default; remove), tested
  against a fake IPP printer on the CI host (`spooltest ipp`). Part 3:
  the app corpus's Notepad++ and SumatraPDF print their document from
  their own Print dialog to the IPP printer on the corpus host (the page
  it gets matches `tests/reference/NAME-print.png`) and to Microsoft
  Print to PDF (the saved file is the same PDF, `spooltest same`)
  ([design/printing.md](design/printing.md)). Notepad++ stands in for
  Notepad: Windows' Notepad is not redistributable.

## Cross-cutting (maintain throughout)

- **Automated boot CI** (done): every pull request that changes more than
  documentation (and every push to `main`) builds NovaOS, boots it
  under QEMU with OVMF and runs the core, network and devices self-test
  suites, the graphics (OpenGL and Direct3D) tests and the performance
  budget (`tools/selftest.py`, `tools/ci/perf.py`,
  `.github/workflows/ci.yml`); a failing test fails the check.
  [ci.md](ci.md) and [testing.md](testing.md) have the details.
- **Reproducible build:** CMake drives `nasm`, clang/lld and `lld-link`
  for the kernel, boot loader and Windows userland; CI builds `nova.iso`
  with `scripts/create-iso.sh`. The ISO is a run artifact of every CI
  build, the rolling `latest` release after each green push to `main`,
  and a versioned release for each `v*` tag
  (`.github/workflows/release.yml`). The ISO is not committed.
- **Debugging:** the GDB stub over QEMU (the `run-debug` target), the
  serial log, the Terminal's `trace NAME` for a program's failing system
  calls, and symbolized kernel backtraces (faults, panics and failed
  assertions print function+offset frames). Crash reports a user can
  attach to an issue go to `C:\NovaOS\Crashes`: a program's exception,
  place, stack and modules, or a kernel fault's log and backtrace, which
  is written to `\NOVA\PANIC.TXT` on the disk and moved there at the next
  start.
- **ABI conformance tests** (done): `abitest` checks PEB, TEB,
  `KUSER_SHARED_DATA` and `CONTEXT` offsets, ntdll's stub layout and every
  syscall number against Windows 10 1903 x64, in CI.
- **Parallel changes without conflicts** (done): DLLs, programs,
  self-tests, the app corpus and the docs' lists are one file per item
  (CONTRIBUTING.md); generated doc regions are rebuilt from fragments by
  `tools/docgen.py`, and CI fails on leftover conflict markers.
- **Test corpus:** every self-test program in `userland/programs/` is a
  permanent regression test, built for x64 and x86;
  `tools/pe_imports.py` shows what a new program needs before it is
  tried, and `tools/sys_imports.py` does the same for a driver.
- **Nightly app corpus** (done): `tools/appcorpus.py` runs each program in
  `tests/appcorpus/` (command-line tools, runtimes, GUI apps, installers
  and games) every night under KVM, with two later scheduled slots that
  run again only if the night's run failed, checks `smpstress` scaling,
  and posts a pass/fail table per program. The current list is generated into
  [testing.md](testing.md).

## Reality check

This is ReactOS-scale work. NovaOS already runs a meaningful set of real
command-line programs, runtimes, installers, browsers, creative apps and
games unmodified, and loads unchanged Windows drivers in QEMU. It has not
yet run on a real PC, and "runs arbitrary commercial Windows software"
remains a long-horizon goal. The value of this plan is an ordered path in
which each step produces something demonstrably working.
