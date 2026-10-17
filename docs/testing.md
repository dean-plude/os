# Testing NovaOS

NovaOS is tested inside NovaOS: test programs run in a QEMU virtual machine
booted from the built image, driven by `tools/selftest.py` (the regression
suites) and `tools/appcorpus.py` (real Windows programs). Every pull request
runs the suites in CI; the app corpus runs nightly.
[building.md](building.md#tests) says how to run them yourself, and
[ci.md](ci.md) how the CI jobs fit together.

## What runs on every pull request

`.github/workflows/ci.yml` builds the kernel, boot loader, userland,
`build/nova.img` and `build/nova.iso` once, then boots that image in QEMU
with OVMF in separate jobs:

| Job | What it runs |
|---|---|
| Checks (conflict markers, manifests, generated docs) | host-side checks and the unit tests in `tests/tools/`; no VM |
| OS tests (core) | `tools/selftest.py --suite core`: one long boot ([below](#the-core-suite)) |
| OS tests (network) | `--suite network`: virtio-net on IPv4 and on IPv6, then an Intel e1000e ([ci.md](ci.md#what-the-network-and-graphics-suites-run)) |
| OS tests (devices) | `--suite devices`: one boot per device or machine ([below](#the-devices-suite)) |
| Compatibility smoke | `tools/compat_smoke.py`: seven core tests (loader, wait migration, Chromium start-up, unwinding, WebView2 start-up) in a short boot of their own |
| Performance budget | `tools/ci/perf.py`: boot time, Firefox start time and SMP scaling against `tools/ci/perf-budget.json` ([ci.md](ci.md#the-performance-budget)) |
| Graphics tests (OpenGL, Direct3D) | `--suite graphics`: Direct2D, DirectWrite, OpenGL, Direct3D 9/11/12, DirectComposition on Mesa, DXVK, VKD3D-Proton and Venus ([ci.md](ci.md#what-the-network-and-graphics-suites-run)) |

**Build and boot-test** passes when the build, all three OS test jobs and
the performance budget passed. **CI result** passes when every job passed.
A pull request that changes only Markdown, files under `docs/` or `LICENSE`
runs the Checks job and skips the rest, and both summary checks still pass.

A failing test fails its job. Each job's summary page has a table of
results, and the serial logs, screenshots and sound recordings are kept as
artifacts. When a push to `main` passes, CI republishes the rolling
`latest` release with that build's ISO; a pushed version tag runs the same
jobs and publishes a release ([releasing.md](releasing.md)).

### How a test passes

Each test is one command typed into the NovaOS Terminal
(`tools/novarun.py`'s `Nova` class types it). It passes when the program
exits with code 0 within its time limit, prints no `FAIL` line and no
"N failed" count above zero, and prints what the test expects. A kernel
panic fails the run. Some tests also compare a screenshot, check a recorded
WAV, or restart or reset the machine and check the next boot. The suites
are one file per test in `tests/selftest/SUITE/`, run in file-name order;
`python3 tools/selftest.py --suite SUITE --list` prints them.

A program that outlives its limit is asked to stop (Ctrl+Alt+F12 logs where
every thread is, then Ctrl+C). If the Terminal still does not come back, the
machine is reset and the tests after it run on the fresh boot, so one hung
program fails one test instead of every later test waiting out its own limit
(one stalled graphics job burned its whole hour that way). Give a test a limit
a few times its normal run on a loaded runner, not a generous one.

### The core suite

The core suite boots once with an HD Audio card whose microphone hears a
523 Hz tone (the host needs PulseAudio; without it the recording tests
fail and the rest run), ACPI test tables compiled from `tests/acpi/` with
`iasl`, and a second disk for drive C:'s saved files. It runs, in order:

- **Core suite** (`tests/selftest/core/`): <!-- BEGIN generated:core-tests -->`apitest`, `abitest` (the PEB, TEB, `KUSER_SHARED_DATA`, `CONTEXT` and
  loader layouts, ntdll's stubs and the system-call numbers, against
  Windows 10 1903 x64), `syscalltest` (the system-call table reached
  without ntdll: ntdll's Zw exports ranked by address give each service
  its Windows 10 1903 number, and the kernel answers that number, as
  anti-cheat code expects), `aligntest` (a misaligned SSE access is
  fixed up to the unaligned move when alignment-fault fixup is on, as
  anti-cheat code expects; ThreadHideFromDebugger and the
  extended-context functions), `gatetest` (Windows' segment selectors,
  32-bit code in a 64-bit program through a far jump to 0x23,
  GetThreadContext on the calling thread and `__fastfail`, as anti-cheat
  code expects), `filetest`, `linktest`, `pipetest`, `proctest`,
  `dlltest` (DllMain, static TLS, and a DllMain returning FALSE stopping
  the program with 0xC0000142, module file names past the 64th module,
  exports by ordinal alone, image lists in streams and the shell's
  desktop folder, 64- and 32-bit), `sectest`, `acltest` (64- and
  32-bit), `consolecursortest` (classic console screen buffers: cursor,
  cell I/O, fills, scrolling, attributes and independent buffers; 64-
  and 32-bit), `ptyscreentest` (a full-screen program drawn in a console
  window and through a pseudo console gives the same cells),
  `conptytest` (a terminal host types into, resizes and exits an
  interactive `cmd.exe` through `CreatePseudoConsole`), `guitest auto`,
  `inputtest` (side buttons, horizontal wheel, volume keys), `usbcheck`
  (media keys, AC Pan, pen pressure, tilt and twist, virtio pens and
  tablets), `anitest` (animated cursors and program pointers),
  `cursortest` (system pointers, SetSystemCursor, cursors at the display
  scale), `bmpcurtest` (1-, 4-, 8- and 16-bit DIB sections, cursors from
  bitmaps with alpha or monochrome masks), `pentest` (a pen as
  WM_POINTER* messages: enter, down, update, up, leave;
  GetPointerPenInfo pressure, tilt, rotation, barrel and eraser;
  DefWindowProc promotion to mouse with the pen signature in
  GetMessageExtraInfo; WM_NCPOINTER* over a title bar and
  WM_POINTERACTIVATE; the mouse as a pointer, entering and leaving
  windows; 64- and 32-bit), `wintabtest` (wintab32 with no pen and with
  a synthetic one: contexts, packets, pressure, tilt and rotation),
  `dpitest` (per-monitor DPI: the manifest's `dpiAwareness`,
  `GetDpiForMonitor`, `GetDpiForWindow`, `WM_DPICHANGED` and its
  suggested rectangle when the monitor goes to 192 DPI and back, and the
  window and monitor coordinates an aware, an unaware and a system-aware
  process see; user32's controls, menus, scroll bars, dialogs, metrics
  and fonts at 192 DPI; per-thread awareness contexts; controls and
  dialogs rescaled when a window's DPI changes, comctl32 at 192 DPI, and
  coordinates converted between awareness contexts), `disptest`,
  `icutest` (ICU and locales, 64- and 32-bit), `comptest`
  (Windows.UI.Composition: visuals, colour and surface brushes, a
  desktop window target, drawing surfaces on Direct3D 11 and Direct2D;
  64- and 32-bit), `comtest`, `coreapptest` (CoreApplication.Run,
  CoreWindow events and dispatcher, RunAsync, a swap chain for a
  CoreWindow; 64- and 32-bit), `d2dctxtest` (Direct2D device contexts:
  ID2D1Device on Direct3D 11, bitmaps on DXGI surfaces,
  DirectComposition surfaces drawn with Direct2D; 64-bit), `wictest`,
  `winrtabitest` (the generated Windows Runtime ABI headers: IIDs, and
  the Compositor driven through them; 64- and 32-bit), `xamlbasetest`
  (Microsoft's XAML core, lowest layer, built from its sources, and
  NovaOS's WRL: ComPtr, RuntimeClass, weak references, Callback,
  EventSource, HString), `nlstest` (date, time, number and currency
  formats in German and Japanese, 64- and 32-bit; `nlstest user`),
  `nlstest calendars` (Japanese eras, Buddhist, Taiwan, Tangun, Hebrew,
  Hijri, Um Al Qura and Persian dates; `GetDurationFormat`) and `nlstest
  override` (`SetLocaleInfo`), 64- and 32-bit, `tlbtest` (type
  libraries, 64- and 32-bit), `usptest` (Arabic and Devanagari shaped
  through Uniscribe and drawn by `ExtTextOut`, 64- and 32-bit), `cppeh`,
  `dlgtest` (the common file dialogs: `OPENFILENAME` settings,
  `IFileDialog` objects), `unwindtest`, `stltest` (the C++ standard
  library, 64- and 32-bit), `delaytest` (the DLLs Firefox delay-loads),
  `rttest` (complex math, conio, DLL directories, sscanf_s, 64- and
  32-bit), `qttest` (what Qt programs need: `GetGlyphOutline`, C++/WinRT
  `HSTRING`s, Windows Hello, the process DACL), `battery` (against the
  battery in `tests/acpi/battery.asl`), `soundtest` (the recorded WAV
  must hold the tones played), `soundtest godot` (WASAPI through
  IAudioClient3 as Godot plays, 64-bit and 32-bit), `soundtest record`,
  `capture` and `volume` (`waveIn` and WASAPI capture must record the
  tone the microphone hears, and a quarter of the endpoint volume must
  sound 12 dB quieter), `soundtest dsound` and `dscapture` (DirectSound
  playback with notifications, frequency and volume, and
  DirectSoundCapture recording the microphone), `sleeptest timer`
  (`Sleep(1)`, 1 ms wait timeouts and waitable timers (periodic ones,
  their completion routines and timer queue timers too) end within a
  millisecond with every CPU busy), `soundtest mme` (`waveIn` recorded
  as PortAudio's MME host does, the program held up 250 ms a second: no
  input overflow, and no frame missing from the recorded tone),
  `savetest` (while NovaOS saves 32 MiB of drive C: to its disk, calls
  behind the kernel, desktop and file-system locks keep answering, the
  file-system one within 100 ms; the save holds the file-system lock
  under 20 ms), `xa2test` (XAudio2 2.9 and 2.7 voices, callbacks and
  effects, and X3DAudio panning), `powertest` (closing the lid in
  `tests/acpi/lid-thermal.asl` sleeps, its GPE in a GPE block device and
  a USB key wake it, the thermal zone's readings, passive cooling
  throttling CPU 0 to its deepest T-state and back), `disptest 1024 768`
  (saves the mode the restart must keep), `tpsimpletest` (5120 one-shot
  thread-pool callbacks: callback context and calling convention, no
  leaked handles; 64- and 32-bit), `miditest` (MIDI through `midiOut`,
  `midiStream` and the MCI sequencer), `nlstest set ja-JP` (the user
  locale the restart must keep), `waitmigrationtest` (yielding workers
  repeatedly wait-any and wait-all on unsignaled events, exercising
  current-thread lookup and timeout registration on SMP; x64 and x86),
  an installer that replaces a running program and finishes after a
  restart (`filetest install`, `shutdown /r`, `filetest installed`),
  `nlstest after-restart ja-JP` (the restart kept the user locale),
  `disptest saved 1024 768` (the restart kept the saved display mode),
  hard links kept across a restart (`linktest restarted`), Windows
  Installer transforms, patches and rollback, and an installed service
  started at the next boot (`msitest transform`, `patch`, `rollback`,
  `service`, `shutdown /r`, `msitest service-boot`), Windows Installer
  JScript and VBScript custom actions setting and reading properties and
  writing files, and a failing script rolling its install back (`msitest
  script`), `smftest` (the C++17 special math functions in
  `msvcp140_2.dll`, 64- and 32-bit), `errnotest` (errno and the other C
  runtime per-thread state belong to each thread, 64- and 32-bit), a
  power cut in the middle of saving drive C: (the FAT holds chains no
  file reaches; at the next boot NovaOS frees them and has as much free
  space as `fsck.fat` finds), `boosttest` (a woken thread is boosted
  above its base priority and runs within 2 ms while threads of the same
  priority spin on every processor; the boost decays back to base, 64-
  and 32-bit), `crtthreads` (FLS callbacks, per-thread locale,
  thread-safe `getenv`, `WaitOnAddress` by API set, 64- and 32-bit),
  `mmcsstest` (MMCSS through avrt.dll: "Pro Audio" runs at 18 and
  "Games" at 16 without the privilege and reverts to the thread's own
  priority; a registered thread woken by an event runs within 2 ms while
  TIME_CRITICAL threads spin on every processor; registered threads
  spinning on every processor still leave a NORMAL thread room, 64- and
  32-bit), `prioritytest` (every priority class and thread level gives
  NT's base priority and reads back; REALTIME without the privilege is
  HIGH; with boosts off a HIGHEST thread woken by an event runs within 2
  ms while NORMAL threads spin on every processor; the Terminal's
  console program is the foreground process, gets the foreground boost
  and three times a background process's time slice, 64- and 32-bit),
  `tlsslots` (1088 TLS indexes and FLS callbacks at process exit, 64-
  and 32-bit), `cmdlinetest` (a 1,200-character command typed into the
  Terminal and into cmd.exe, CreateProcess with up to 32,766 characters
  and from an unquoted path with spaces, 64- and 32-bit), `devices`
  (every PCI function and the driver that claimed it: AHCI, the VGA
  card, HD Audio, xHCI; bridges marked), `gl2test` (OpenGL 3+ with
  shaders on the built-in Mesa llvmpipe, 64- and 32-bit), `glgeneric`
  (OpenGL 1.1 with no OpenGL driver installed, drawing in 2D, 64- and
  32-bit), `hwcheck` audio DSP: NHLT digital microphones, the SOF
  firmware manifest, the DSP boot (ROM, code loader, FW_READY, IPC4),
  the IPC4 capture pipeline into a host ring, paused and run again, and
  the boot again after sleep, on a modelled DSP, `hwcheck` (HD Audio
  controller matching, a modelled ALC257 codec with its headphone jack,
  a modelled I2C-HID touchpad); the touchpad in the boot's ACPI table is
  found and its missing controller reported, `crashtest` (crash.exe's
  report in `C:\NovaOS\Crashes`: exception, module+offset, stack,
  modules; 64- and 32-bit), and the Terminal's `Crash report:` line and
  `crashes last`, `hwcheck` touchpad gestures on a modelled precision
  touchpad: tap to click, two-finger tap for the right button,
  two-finger scrolling (vertical and horizontal) as wheel notches, palms
  ignored, tap and drag, `wndthreads` (child windows of other threads,
  sends to an ended thread's window, 64- and 32-bit), `hwcheck mic`: a
  modelled audio DSP as the "Microphone Array" recording device
  (`waveIn`, WASAPI and DirectSound list it, `soundtest record` and
  `capture` hear its tone, before and after a modelled sleep), `fpstate`
  (x87 control word and MXCSR on new threads, across switches,
  `RtlRestoreContext` and SEH unwinds, 64- and 32-bit), `ramdisktest`
  (files on drive C: take the memory their contents need: appended
  files, SetEndOfFile, deleting; the memory programs are told), the
  first-boot setup, `start welcome` (name, time zone, keyboard layout
  and display pages from the keyboard; German typed in the layout's test
  field), `kbdtest` with German in effect and US put back, then `whoami`
  as the name it gave (the program and the Terminal command), `tztest`
  in the zone it chose (Tokyo: UTC+9 in GetLocalTime and localtime;
  Berlin, Sydney and Los Angeles rules) and `tzutil /s UTC`,
  `edgeupdtest` (Task Scheduler 2.0, DPAPI, UrlCombine and the other
  APIs Microsoft Edge Update needs, 64- and 32-bit), `setuptest` (UAC
  on, run as administrator, Rich Edit with RTF, variant arithmetic, a
  300 MB file on C:, 64- and 32-bit), `msxmltest` (MSXML 6 DOM, XPath,
  XSL Patterns, SAX2 and registration, 64- and 32-bit), `cabtest`
  (cabinet.dll: MSZIP and LZX cabinets, an attached container, a cabinet
  set; 64- and 32-bit), `authtest` (Authenticode: WinVerifyTrust, signer
  chains and timestamps, CryptQueryObject; 64- and 32-bit), `ndrtest`
  (rpcrt4's NDR engine: stubless proxies, NdrStubCall2, CStdStubBuffer
  and the NdrDll* entry points, 64- and 32-bit), `regtest` (isolated 32-
  and 64-bit machine registry views, shared keys, and registry keys and
  values changed concurrently), `msiqtest` (Windows Installer: 32-bit
  packages into SysWOW64, product information by context, source lists,
  patch applicability; 64- and 32-bit), `chrometest` (Chromium start-up:
  read-only shared memory, handle lists, ordinal exports; 64- and
  32-bit), `svctest` (a real service: start with arguments, status
  handshake, controls, stop and delete; a LocalSystem service runs
  elevated; CopyFile keeps file times; 64- and 32-bit), `comoop`
  (cross-process COM: LocalServer32 activation, the standard marshaler
  and RPC channel, the IDispatch proxy/stub, BSTR/VARIANT marshalling;
  64- and 32-bit clients and servers), `etwtest` (event-tracing
  controllers with no sessions: StartTrace, StopTrace, ControlTrace,
  EnableTrace, QueryAllTraces, OpenTrace, ProcessTrace, CloseTrace; 64-
  and 32-bit), `qtwebtest` (the calls Qt WebEngine imports: Bluetooth
  and SDP records, Direct3D 12, WinUSB, AppContainer profiles, proxy
  resolver, security zones and more; 64- and 32-bit), `cachetest` (saved
  files of drive C: let go of and read back from the data disk: memory,
  ReadFile, a mapped view, renamed, hard-linked and emptied files,
  `shutdown /r`, `cachetest after`), `qtthemetest`
  (Windows.UI.ViewManagement UISettings and UIViewSettings as Qt reads
  them, ColorValuesChanged, Schannel's cipher suites; 64- and 32-bit),
  `samplertest` (a sampling profiler: GetThreadContext of a waiting
  thread, unwound with RtlVirtualUnwind), `hwcheck` touchpad interrupt:
  a modelled Intel GPIO controller from the ACPI tables, its interrupt
  routed, the touchpad's GpioInt pin set up and its reports read on the
  interrupt instead of polling, `proclisttest` (another program's image
  name, user and command line, as Edge Update looks for its install
  worker; 64- and 32-bit), `rawtest` (the mouse and the keyboard through
  Raw Input: device list, names and info; WM_INPUT with relative motion,
  button flags and the wheel; keys through GetRawInputBuffer with
  RIDEV_NOLEGACY; 64- and 32-bit), `libmtest` (the 32-bit C runtime's
  `_libm_sse2_*` and `_CI*` math, which MSVC-built SDL2 imports),
  `warptest` (SetCursorPos and ClipCursor: the pointer moved and
  confined by the window in front, WM_MOUSEMOVE but no WM_INPUT for a
  warp, the mouse kept in the rectangle, a background process refused;
  64- and 32-bit), `wvsetuptest` (a 300 MB file mapped whole, wer.dll's
  report API and FlsGetValue2, as the WebView2 runtime's setup needs;
  64- and 32-bit), `wvstarttest` (COM's apartment in the TEB,
  TerminateProcess on itself, FLS slots and the functions the WebView2
  runtime's browser process calls), `xpwin` (windows of different
  processes: SetParent, SetWindowPos, ShowWindow and messages across
  processes, GetDC on another process's window; 64- and 32-bit),
  `dcomptest` (Direct3D without a GPU as Chromium's software compositor
  uses it: the Basic Render Driver adapter, a WARP device,
  DirectComposition showing a swap chain, dirty-rectangle presents, a
  GPU process's window parented in another process's; 64- and 32-bit),
  `schanneltest stores` (a certificate found through a collection of
  collections, a root's extended key usages, and
  `InternetGetConnectedState` offline without a network; 64- and
  32-bit), driver packages staged in the driver store and published as
  oemN.inf from a program (`DiInstallDriver`, `SetupCopyOEMInf`), with
  `pnputil` and from an MSI (`drvtest exe`, `zip`, `msi`),
  `imgsharetest` (a second process of a running program shares its image
  pages from the image cache; 64- and 32-bit; a commit of 15/16 of the
  RAM), `chkstktest` (ntdll's stack probe, `__chkstk` on x64 and
  `_chkstk` on x86, and its half-precision conversions, which LLVM's JIT
  in Mesa looks up), `avxtest` (the AVX state: CPUID's OSXSAVE, AVX and
  F16C and XCR0 as the kernel sets them, YMM0-15 kept across context
  switches, in an exception's `CONTEXT_XSTATE` and through `NtContinue`,
  and read from a suspended thread by `GetThreadContext`), `tpwaittest`
  (thread-pool waits: a wait calls back once per signal or on its
  timeout, a callback sets its own wait again, setting a wait never
  waits for a running callback, cancelling and closing from a callback;
  64- and 32-bit), Task Manager opened with Ctrl+Shift+Esc: `winhello`
  listed, selected by typing its name and ended with Delete; the
  Performance, Details and Services views, `evtlogtest` (ReportEvent,
  ReadEventLog, EvtQuery/EvtRender/EvtSubscribe/EvtExportLog; the boot,
  service, driver-failure and crash events) and Event Viewer from the
  Start menu finding its event, Calculator (Windows Calculator's
  CalcManager engine) from the Start menu: Standard, Scientific
  (precedence, x^y, 25!) and Programmer (hexadecimal, AND) answers to
  typed expressions, `dostest` (MS-DOS 4.00 boots in a console window on
  ntvdm.exe's real-mode CPU and BIOS: COMMAND.COM's prompt, VER and DIR,
  F12 gives the screen back; a DOS .COM run from cmd by name and a DOS
  .EXE from CreateProcess read and write files on drive C: and return
  their exit code; the DOS game bootRogue draws its dungeon straight
  into video memory, takes arrow keys and ends on Esc), `kmttest` (the
  WDDM graphics kernel's D3DKMT* calls on NovaOS's Basic Render adapter,
  step 3 of docs/design/intel-gpu.md: adapters by LUID, devices,
  allocations counted as video memory and mapped by `D3DKMTLock2`,
  monitored fences signalled and waited on from the CPU, with an event
  or another thread), programs started from the Start menu (Win+S, the
  name, Enter): `winhello`, a GUI program, with no Terminal window, and
  `hello`, a console program, in a Terminal, `crash kernel`, a
  deliberate kernel fault whose serial log must show a backtrace with
  function names, and last the kernel crash report: after `crash kernel`
  and a reset, `crashes last` shows the fault's backtrace.<!-- END generated:core-tests -->

### The devices suite

`--suite devices` boots NovaOS once for each device or machine the core
boot does not have (`tests/selftest/devices/NAME/`):

| Boot | What it tests |
|---|---|
| `touch` | a virtio multi-touch screen (`touchtest`), and the USB HID parser (`usbcheck`) |
| `usbaudio` | no HD Audio card, USB speakers on xHCI, OHCI and UHCI: each speaker's WAV must hold its tones; unplugging, default device and volume kept across a restart |
| `usbheadset` | USB headsets and microphones on EHCI, xHCI, OHCI and UHCI through `tools/usbredirpeer.py` (`soundtest tone`, `record`, `capture`), including a USB Audio 2.0 headset |
| `driversearch` | `pnputil` downloads a driver package for a device with no driver by hardware ID, from recorded Microsoft Update Catalog pages (`tools/drivertest/catalog_fixture.py`) |
| `monitors` | one virtio-vga card with three outputs; monitors plugged in and out while NovaOS runs (`montest hotplug`) |
| `usbboot` | `nova.iso` written to a USB stick, with only the firmware's GOP display: starts live and writes its boot log (and a kernel fault) to the stick |
| `cdboot` | `nova.iso` as a disc in a SATA DVD drive: starts live |
| `laptop` | a laptop without S3 whose lid, battery and AC adapter are behind an embedded controller (`tests/acpi/laptop.asl`): battery, lid sleep in S0 idle and wake, then NovaOS installed from a USB stick onto an NVMe disk and started from there |
| `update` | an installed NovaOS updating itself from signed test channels, refusing bad ones, and going back after a reset ([updates.md](updates.md#the-self-test)) |
| `gamepad` | a wired Xbox 360, an Xbox One and a HID game pad (`tools/padpeer.py`): `padtest` through XInput and DirectInput 8, motors, unplugging |
| `hid` | a vendor-defined USB HID device (`tools/padpeer.py --kind vendor`): the HID class driver on its USB minidriver; `hidclasstest` through setupapi and `hid.dll` (strings, preparsed data, feature, input and output reports), Raw Input (`WM_INPUT` and the device list), unplugging |
| `camera` | a fake UVC 1.1 camera (`tools/campeer.py`): the UVC class driver; `camtest` through Media Foundation (`MFEnumDeviceSources`, the source reader in each mode and its converters, the asynchronous reader), then the Camera app's picture (`docs/screenshots/camera.png`) |
| `isocam` | a fake UVC 1.1 camera streaming isochronously in MJPEG and YUY2 (`tools/campeer.py --iso`): the UVC driver on isochronous URBs; `camtest --iso` in each mode, the MJPEG frames decoded into RGB32, NV12 and YUY2, then the Camera app showing MJPEG 640x480 |
| `pnp` | a keyboard, a hub and a USB stick plugged into xHCI and removed: `pnptest` follows the Plug and Play device tree |
| `ntfs` | an NTFS volume as drive D:: hard links across a restart, a full volume, and a reset in the middle of writing, after which NovaOS repairs the volume; `scripts/ntfs-check.py` and ntfs-3g check it from the host |
| `wdm` | a Windows kernel-mode test driver (`wdmtest.sys`) under the signing policy (loads only with test signing on; unsigned and tampered copies never), then a KMDF driver (`kmdftest.sys`) on `Wdf01000.sys` |
| `edu` | QEMU's edu PCI device, behind a PCIe root port (its interrupt routed through the bridge swizzle): a Plug and Play driver (`edutest.sys`) installed with `pnputil`, with its memory, interrupt and DMA from the PnP manager; disabled and enabled again |
| `vioser` | virtio-win's signed `vioser.sys` (KMDF) on virtio-serial: bytes to the host and back |
| `usbserial` | QEMU's FTDI FT232 USB serial adapter: FTDI's own driver found by hardware ID, the port becomes COM3 and `serialtest` loops data through it |
| `ax211` | Intel's AX211 Wi-Fi driver (`Netwtw14.sys`) downloaded by hardware ID and loaded on a stand-in PCI device; it loads its firmware and registers with NDIS, and its hardware probe fails as expected, since the device is not a real AX211 |
| `viostor` | virtio-win's signed `viostor.sys` on virtio-blk, on NovaOS's Storport host: its NTFS volume mounts as D: |
| `umdf` | a UMDF 2 driver (`umdftest.dll`) on the edu device, running in `WUDFHost.exe` |
| `kmdfedu` | a KMDF driver (`kmdfedu.sys`) on the edu device, with its memory, interrupt (line, then MSI) and DMA |
| `netkvm` | virtio-win's signed `netkvm.sys` in place of the built-in virtio-net driver, on NovaOS's NDIS host: DHCP, ping and a download |
| `devmgr` | Device Manager (`devmgmt.exe`) opened from the Start menu, then `devmgmt --check` on an e1000e card, a USB keyboard and tablet and the driverless edu device: setupapi's list against cfgmgr32's tree, classes, status and problem codes, resources (`CM_Get_First_Log_Conf`); its screenshot is `docs/screenshots/device-manager.png` |
| `wddm` | a WDDM display-only driver (`wddmdod.sys`) on the standard VGA: takes over the desktop through NovaOS's dxgkrnl host, changes mode, gives the screen back when disabled |
| `diskmgr` | a blank second disk: Disk Management (`diskmgmt.exe`), opened from the Start menu, initializes it as GPT, makes an NTFS and a FAT32 volume and moves one to T: (still T: after a restart); `scripts/ntfs-check.py`, `fsck.fat` and the files are checked from the host |

[drivers.md](drivers.md) has more on the Windows driver model these boots
exercise.

## Running the suites locally

After a build ([building.md](building.md#building)):

```bash
sudo apt install acpica-tools
python3 tools/selftest.py
python3 tools/selftest.py --suite network
python3 tools/selftest.py --suite devices
python3 tools/selftest.py --only apitest,guitest --out /tmp/st
python3 tools/compat_smoke.py
python3 tools/ci/perf.py
```

The core suite is the default. The network suite needs `node` and
`openssl` on the host. The graphics suite needs a QEMU with Venus and a
staged download folder; [building.md](building.md#tests) and the docstring
of `tools/selftest.py` give the details. `--only` takes test names or
program names, and `--out` keeps the logs and screenshots. The exit status
is the number of failed tests.

The host-side checks need no VM:

```bash
python3 -m unittest discover -s tests/tools
```

These unit tests cover the CI scripts, the corpus and download harness,
the App Store catalog, and some kernel code compiled for the host
(`tests/host/` and `tests/unit/`: console cursor handling, the Hyper-V
input ring, NTFS power-cut recovery).

## The nightly app corpus

`.github/workflows/nightly.yml` builds `main` and runs `tools/appcorpus.py`,
one VM and one job per program: the official Windows x64 releases of
<!-- BEGIN generated:corpus -->ripgrep, fd, jq, 7-Zip, MinGit (cloning a repository), Python, Node.js, .NET (German and Japanese formatting through ICU), ffmpeg (an MP4 converted to WebM), Roblox (installs; the client stops in its anti-cheat), Steam (installs and updates itself; the login window opens after the test's time), Microsoft Edge WebView2 runtime (installs; a host creates a controller, loads a page and executes JavaScript), SumatraPDF, WinMerge, KeePassXC, VLC (plays an H.264 and AAC MP4 with sound), Audacity (records 10 s from the microphone and saves the project), Inkscape, Krita (draws a stroke and saves the image), PhotoCraft, VectorCraft, EffectCraft, PdfCraft, WordCraft, Firefox, CADCraft, DeckCraft, DesignCraft, FilmCraft, GridCraft, LightCraft, SoundCraft, Notepad++, OpenTTD (free on GOG; installs with its installer and reaches its main menu), Beneath a Steel Sky on ScummVM (free on GOG; installs with its installer, skips the intro and walks), Teeworlds (full screen, through its first-start questions to its start menu, with music; Settings opened with the mouse, SDL recentring the pointer with SetCursorPos), Teeworlds joining a game on its own server (teeworlds_srv.exe on the same machine, over the loopback interface) and staying in it, OpenTyrian (Tyrian 2.1 on Direct3D 9 through DXVK, its demo, menus and full screen, with music), Blobby Volley 2 (its full screen switches the display to 800x600 on Direct3D 9 through DXVK, and back), LBreakout2 (an SDL 1.2 game drawn with GDI; its full screen switches the display to 640x480, and back), Cave Story (a DirectDraw game, on cnc-ddraw: its title screen in full screen at 640x480, then a new game), Cave Story played with a game pad (DirectInput 7 joystick): the opening talk, then Quote walks in the Start Point cave, SuperTux (an SDL 1.2 game played with a game pad, through winmm's joystick functions: its first level), PuTTY, WinSCP (a Delphi VCL program: its Login dialog), LibreOffice (Writer and Calc open, edit and save a .docx and an .xlsx), Thunderbird (opens an e-mail message), Squash the Creeps (Godot 4.7 drawing with Direct3D 12 through VKD3D-Proton), qBittorrent (a Qt 6 program: opens a .torrent), GIMP (a GTK 2 program: opens a picture), OBS Studio (Direct3D 11 through DXVK: adds a scene and saves it) and ShareX (Windows Forms on .NET 9: captures the screen to a PNG)<!-- END generated:corpus -->.
Each program is downloaded (and cached), unpacked onto drive C: or
installed with its own installer or the App Store, and driven by typed
commands, clicks and keys. The windowed programs' screenshots must match
`tests/reference/NAME.png`. Before them, NovaOS's own screens are checked:
`dir` on C: and on an NTFS drive D:, each with its own free space, and File
Explorer's This PC listing both.

The same workflow runs an SMP stress test (`smpstress scaling 0` on four
CPUs; each multi-thread figure is the best of three samples, since a
shared runner sometimes takes the emulated CPUs away for a moment and only a
real lock stays near 1x in every sample). A scheduled run posts each program's result to issue #210. The
workflow also runs on pull requests that change the corpus or what it
depends on. [ci.md](ci.md#the-nightly-app-corpus) has the schedule, the
retry rules and the reports.

To run part of the corpus locally after a build:

```bash
python3 tools/appcorpus.py --only ripgrep,jq --cache ~/.cache/novaos/appcorpus --out appcorpus-out
```

## Test programs

- **Self-test programs** in `userland/programs/`, installed in
  `C:\Programs` (and 32-bit builds in `C:\Programs\x86`). Run them from the
  Terminal; each prints "N passed, 0 failed": <!-- BEGIN generated:selftest-programs -->`abitest`, `acltest`, `adaptertest`, `afdtest`, `aligntest`, `anitest`, `apitest`, `authtest`, `avxtest`, `bmpcurtest`, `boosttest`, `cabtest`, `cachetest`, `camtest`, `chkstktest`, `chrometest`, `cliptest`, `cmdlinetest`, `comoop`, `comptest`, `comtest`, `conptytest`, `consolecursortest`, `coreapptest`, `cppeh`, `crashtest`, `crttest`, `crtthreads`, `cursortest`, `d2dctxtest`, `d3d9test`, `dcomptest`, `delaytest`, `devenumtest`, `devnotifytest`, `dhcp6test`, `digitest`, `disptest`, `dlgtest`, `dlltest`, `dltest`, `dostest`, `dpitest`, `drvtest`, `edgeupdtest`, `edutest`, `errnotest`, `etwtest`, `evtlogtest`, `filetest`, `fpstate`, `gatetest`, `gl2test`, `glgeneric`, `hidapitest`, `hidclasstest`, `httptest`, `icutest`, `imgsharetest`, `inputtest`, `kbdmoutest`, `kbdtest`, `kmdftest`, `kmttest`, `libmtest`, `linktest`, `loadtest`, `looptest`, `mmcsstest`, `montest`, `msiqtest`, `msitest`, `msxmltest`, `ndrtest`, `nlstest`, `nstest`, `ntfstest`, `overlaptest`, `padtest`, `pentest`, `pipetest`, `pnptest`, `posixtest`, `powertest`, `prioritytest`, `proclisttest`, `proctest`, `ptyscreentest`, `qttest`, `qtthemetest`, `qtwebtest`, `ramdisktest`, `rawpadtest`, `rawtest`, `regtest`, `rttest`, `samplertest`, `savetest`, `schanneltest`, `sectest`, `serialtest`, `setuptest`, `shmtest`, `smftest`, `smpstress`, `stltest`, `svctest`, `syscalltest`, `tcptabletest`, `threads`, `tlsslots`, `touchtest`, `tpsimpletest`, `tpwaittest`, `tztest`, `umdftest`, `unwindtest`, `usbcompositetest`, `usptest`, `vhidtest`, `viosertest`, `waitmigrationtest`, `warptest`, `wdmtest`, `wictest`, `winrtabitest`, `wintabtest`, `wndthreads`, `wvsetuptest`, `wvstarttest`, `xamlbasetest`, `xpwin`<!-- END generated:selftest-programs -->.
  Terminal; each prints "N passed, 0 failed": <!-- BEGIN generated:selftest-programs -->`abitest`, `acltest`, `adaptertest`, `afdtest`, `aligntest`, `anitest`, `apitest`, `authtest`, `avxtest`, `bmpcurtest`, `boosttest`, `cabtest`, `cachetest`, `camtest`, `chkstktest`, `chrometest`, `cliptest`, `cmdlinetest`, `comoop`, `comptest`, `comtest`, `conptytest`, `consolecursortest`, `coreapptest`, `cppeh`, `crashtest`, `crttest`, `crtthreads`, `cursortest`, `d2dctxtest`, `d3d9test`, `dcomptest`, `delaytest`, `devenumtest`, `devnotifytest`, `dhcp6test`, `digitest`, `disptest`, `dlgtest`, `dlltest`, `dltest`, `dostest`, `dpitest`, `drvtest`, `edgeupdtest`, `edutest`, `errnotest`, `etwtest`, `evtlogtest`, `filetest`, `fpstate`, `gatetest`, `gl2test`, `glgeneric`, `hidapitest`, `hidclasstest`, `httptest`, `icutest`, `imgsharetest`, `inputtest`, `kbdmoutest`, `kbdtest`, `kmdftest`, `kmttest`, `libmtest`, `linktest`, `loadtest`, `looptest`, `mmcsstest`, `montest`, `msiqtest`, `msitest`, `msxmltest`, `ndrtest`, `nlstest`, `nstest`, `ntfstest`, `overlaptest`, `padtest`, `pentest`, `pipetest`, `pnptest`, `posixtest`, `powertest`, `prioritytest`, `proclisttest`, `proctest`, `ptyscreentest`, `qttest`, `qtthemetest`, `qtwebtest`, `ramdisktest`, `rawpadtest`, `rawtest`, `regtest`, `rttest`, `samplertest`, `savetest`, `schanneltest`, `sectest`, `serialtest`, `setuptest`, `shmtest`, `smftest`, `smpstress`, `stltest`, `svctest`, `syscalltest`, `tcptabletest`, `threads`, `tlsslots`, `touchtest`, `tpsimpletest`, `tpwaittest`, `tztest`, `umdftest`, `unwindtest`, `usbcompositetest`, `usptest`, `vhidtest`, `viosertest`, `waitmigrationtest`, `warptest`, `wdmtest`, `wictest`, `winrtabitest`, `wintabtest`, `wndthreads`, `wvsetuptest`, `wvstarttest`, `xamlbasetest`, `xpwin`<!-- END generated:selftest-programs -->.
  [building.md](building.md#self-test-programs) has a table of what each
  covers.
- **Sound**: `soundtest` plays tones through `waveOut`, WASAPI,
  `PlaySound` and `Beep`, records through `waveIn` and WASAPI capture, lists
  the sound devices, chooses the default and sets each device's volume.
  `tools/novarun.py --wav out.wav` records what NovaOS plays, `--rec in.wav`
  feeds a WAV to its microphone, and `tools/wavcheck.py out.wav` lists each
  tone's length and pitch.
- **Disk**: `disktest write`, a restart and `disktest verify` check that
  drive C: survives a reboot.
- **GUI and interactive**: `winhello`, `guitest` (`guitest auto` drives
  its own menus, dialog, message box and property sheet and reports),
  `droptest`, `cpus` (SMP speed-up).
- **Graphics test programs** in `tools/`, built with MinGW and copied to
  `C:\Tests` by the graphics suite: `d2dtest` (Direct2D), `dwtest` and
  `dw3test` (DirectWrite), `gltest` (OpenGL), `d3dtest` (Direct3D 9/11),
  `d3d12test` (Direct3D 12) and `hlsltest` (the HLSL compiler).
  `gltest fps` and `d3dtest fps` compare direct presentation with the GDI
  way; under KVM they check bytes, not time. Frames the host did not show go
  through the GDI like the other way's, so their share is taken out before
  the "far fewer bytes" check (a slow runner shows more of them; the "frames
  shown from the host" check limits them separately).
- **Drivers**: the devices suite's test drivers (`wdmtest`, `edutest`,
  `kmdftest`, `kmdfedu`, `umdftest`) and its recorded catalog servers are
  in `tools/drivertest/`. `tools/authenticode/` makes the files
  `authtest` checks.

## On the host

- `tools/pe_imports.py PROGRAM.exe` lists the imports a Windows program
  needs that NovaOS's DLLs lack.
- `tools/sys_imports.py` does the same for a driver (`.sys`), and
  `tools/ci/check-driver-imports.py` checks the drivers NovaOS targets.
- `tools/msitest/` exercises the Windows Installer's package readers and
  SQL, and builds test packages.

## Debugging

- The serial log (COM1) has every kernel message. The Terminal's `dmesg`
  shows it, and `trace NAME` logs a program's failing system calls
  (`trace off` stops).
- A kernel fault, panic or failed assertion prints a backtrace with
  function names and offsets (the kernel carries its own symbol table);
  `crash kernel` shows one on purpose.
- A crashed program or kernel leaves a text report in `C:\NovaOS\Crashes`
  to attach to an issue (a kernel fault's report is written at the next
  start). The Terminal's `crashes` lists them and `crashes last` shows the
  newest.
