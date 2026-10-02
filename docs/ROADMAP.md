# NovaOS — Roadmap to a Windows-Compatible Desktop OS

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
NovaOS's own clean-room code); there is no GPU (3D runs on the CPU), and sound is
output only (no recording, MIDI, DirectSound or XAudio2); drive
C: is FAT, so there are no hard links or ACL enforcement on files (NTFS
disks can be read, as drives D:, E:, ...); and most of the App Store's catalog (Qt, GTK and
multimedia programs) does not run yet.

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
- ~~**OpenGL**: a working `opengl32.dll`~~ Done: Mesa's llvmpipe from the
  App Store is the system `opengl32.dll` (OpenGL 4.5, 64- and 32-bit); see
  [OpenGL](HISTORY.md#opengl-mesa-as-the-system-opengl32dll).
- ~~**Direct3D**, DXGI~~ Done: DXVK from the App Store is the system
  Direct3D 8–11 on Mesa's lavapipe Vulkan, through NovaOS's own
  `vulkan-1.dll`; see [Direct3D](HISTORY.md#direct3d-dxvk-on-mesas-vulkan).
- ~~**Audio**: `winmm` wave output and WASAPI over a real sound device
  (QEMU's Intel HDA)~~ Done; see [Sound](HISTORY.md#sound-intel-hd-audio-winmm-and-wasapi).
  Still open: recording (`waveIn`, capture endpoints), `dsound.dll`
  (DirectSound), XAudio2, MIDI, endpoint volume (`IAudioEndpointVolume`),
  and testing VLC and Audacity on it.
- Display: GPU-backed or at least faster blits; mode changes.
- NetSurf: SVG; redrawing pages a script changes after layout.

### Application coverage
- Bring the App Store catalog up program by program, starting with the
  "untested" portable ones (Notepad++, SumatraPDF, PuTTY, WinMerge), then
  the Qt and GTK applications (KeePassXC, Krita, Inkscape), then Firefox.
- Common dialogs: `GetOpenFileName`/`GetSaveFileName` and the
  `IFileDialog` interfaces (today they report "cancelled").
- Windows Installer: custom actions that run code, the packages' own
  dialogs (`InstallUISequence`), the `Shortcut` table, services, merge
  modules; LZX cabinets tested against real packages.
- COM type libraries (`LoadTypeLib`), the MSVC FH4 C++ exception tables.
- .NET globalization through ICU, not only NLS for English and invariant
  cultures.
- Keep the App Store's per-app compatibility notes in step with what has
  been verified.

### Kernel and API compatibility
- Processes: `CREATE_SUSPENDED` for `CreateProcess`, `CREATE_NEW_CONSOLE`
  with a console of its own, file handles that share their position with
  the processes they are handed to.
- Files: hard links, `MoveFileEx` pending renames carried out at boot,
  `RegNotifyChangeKeyValue` change events.
- Interactive MSYS2 `sh` sessions (only `sh -c` and scripts are tested).
- A console screen buffer with raw (per-key) input, so full-screen console
  programs work and `less` can be the real one (git pages through NovaOS's
  line-based `less` today).
- Move files, the registry, process creation and the console off the big
  kernel lock.
- Security: enforce tokens and ACLs on objects (`AccessCheck` already
  evaluates the DACLs it is given).

### Storage, network and hardware
- ~~NTFS read~~ Done: NTFS volumes mount read-only as drives D:, E:, ...;
  NTFS write, NVMe.
- IPv6, HTTP/2.
- ~~USB (xHCI) with HID keyboards and mice~~ Done: boot-protocol keyboards
  and mice on xHCI root ports, with hot-plug.  Still to do: USB hubs, absolute pointers
  (tablets, touch screens), report-protocol HID, mass storage.
- ACPI beyond the MADT: ~~shutdown, reboot, sleep, batteries~~ Done:
  power-off (S5), sleep (S3), reset and the fixed power button from the
  FADT; the AML interpreter (uACPI) for batteries, AC adapters,
  control-method power buttons and `_PTS`/`_WAK`.  Still to do: the lid
  switch, thermal zones, wake devices (USB keyboards), a real SCI
  interrupt and PCI interrupt routing from `_PRT`; display modes after S3
  on adapters other than the Bochs/QEMU one; HPET or TSC-deadline timers.
- Boot and test on real hardware, not only QEMU.

---

## Cross-cutting (maintain throughout)

- **Automated boot CI** (done): every pull request builds NovaOS, boots it
  under QEMU + OVMF and runs the self-tests and the OpenGL/Direct3D tests
  (`tools/selftest.py`, `.github/workflows/ci.yml`); a failing test fails
  the check.
- **Reproducible build:** CMake drives `nasm`, clang/lld and `lld-link`
  for the kernel, bootloader and Windows userland; `nova.iso` is rebuilt
  with `scripts/create-iso.sh`.
- **Debugging:** the GDB stub over QEMU (`run-debug`), the serial log,
  crash reports naming the module and offset, and the Terminal's `trace
  NAME` for a program's failing system calls, and symbolized kernel
  backtraces (the kernel embeds its symbol table; faults, panics and
  failed assertions print function+offset frames).
- **ABI conformance tests** (done): `abitest` asserts PEB/TEB/KUSER/CONTEXT
  offsets, ntdll's stub layout and every syscall number against Windows 10
  1903 x64, in CI.
- **Test corpus:** every self-test program in `userland/programs/` is a
  permanent regression test, built for x64 and x86; `tools/pe_imports.py`
  shows what a new program needs before it is tried.
- **Nightly app corpus** (done): `tools/appcorpus.py` runs ripgrep, fd, jq,
  7-Zip, MinGit, Python, Node.js and Notepad++ every night and posts a
  pass/fail table per program.

## Reality check

This is ReactOS-scale.  NovaOS already runs a meaningful set of real
command-line programs, runtimes, installers and one large GUI program
unmodified.  "Runs arbitrary commercial Windows software" remains a
long-horizon goal; the value of this plan is an ordered path where each
step produces something demonstrably working.
