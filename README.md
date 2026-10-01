# NovaOS — Windows-Compatible Operating System

A clean-room, from-scratch x86-64 operating system that runs native Windows
executables without emulation.  The programs' own machine code runs directly
on the CPU; NovaOS supplies what they expect from Windows: the NT system-call
ABI, the Win32 API, the loader, a GUI, and the drivers underneath.  64-bit
(x64, PE32+) programs run natively, and 32-bit (x86, PE32) ones run through
NovaOS's own WoW64 layer, as on 64-bit Windows.

**Status:** Phases 1–14 are done.  NovaOS boots on UEFI machines (tested in
QEMU with OVMF), uses every CPU core, keeps its files on a SATA disk, and
runs unmodified Windows programs: 7-Zip, Git, NSIS installers, `.msi`
packages, and the Java, .NET, Node.js and Python runtimes.  It can install
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

| Program | Kind | Tested on NovaOS |
|---|---|---|
| **7-Zip 26.03** (x64) | GUI installer, file manager, `7zG`, `7z.exe` | Installs; the file manager browses, opens archives, adds and extracts with the full dialogs; Options has all six pages.  The App Store uses `7z.exe` to unpack downloads. |
| **7-Zip self-extractors** (x86) | 32-bit console and GUI SFX | Unpack an archive. |
| **NSIS installers** (x86, Modern UI) | 32-bit setup programs | Welcome, folder, progress and finish pages; files, registry, desktop and Start menu shortcuts; the uninstaller removes it all. |
| **MinGit 2.47** | Git for Windows (console) | `init`, `add`, `commit`, `log`, `status`, `diff`, `checkout -b`, `merge`, `gc`, `fsck`, and `clone`/`fetch`/`push` between local repositories. |
| **MSYS2 runtime** (MinGit's `usr\bin`) | `sh.exe` (bash), `ls`, `cat`, `wc`… | `sh -c` with pipes, `$(...)`, subshells, globbing, `fork`. |
| **Eclipse Temurin 21** | Java JRE `.msi`, JDK `.zip` | `java -version`, a threads/exceptions/files stress test, `javac` compiling a program that then runs. |
| **.NET 10** | Runtime and host from NuGet, Roslyn | `dotnet --info`, `dotnet hello.dll`, `dotnet csc.dll` compiling a C# test that passes. |
| **Node.js 24** | `.msi`, `.zip` | `node -v`, `-e`, `npm -v`, a crypto/fs/JSON/timers test script. |
| **Python 3.14** | NuGet package | `-c`, a hashlib/JSON/regex/threads/subprocess test script. |
| **ripgrep, fd, jq, fzf** | Rust (MSVC), C (MinGW), Go | Searching, walking folders, filtering, from the Terminal. |

### Built in

- **Desktop**: a Windows 11-style shell with a Start menu (live search over
  apps, settings and files), dock, tray, snapping and resizing windows,
  Alt+Tab, right-click menus, three wallpapers.
- **Apps**: Terminal, File Explorer, Notepad, Settings, Calendar, Photos,
  the **App Store** and **Install NovaOS** (Setup).
- **Web browser**: NetSurf 3.11, built from source as a Windows program,
  with HTTPS (TLS 1.3/1.2) and JavaScript.
- **Command line**: the Terminal's own commands (`dir`, `copy`, `ping`,
  `curl`, `wget`, `certutil`, `tasklist`, `trace NAME`, `vol`, `sync`…)
  and NovaOS's `cmd.exe` with batch files, plus `find`, `findstr`, `sort`,
  `more`, `timeout`, `reg`, `regsvr32` and `msiexec`.

### The App Store

The dock's App Store downloads the official 64-bit packages of 21 open-source
programs (Firefox, VLC, LibreOffice, GIMP, Notepad++, PuTTY…) and four
runtimes, and installs them with 7-Zip, NovaOS's Windows Installer or the
program's own setup.  Most of those programs still need more of Windows than
NovaOS has (audio, Direct3D/OpenGL, more of the GUI); the ones in the table
above are the ones verified.  See [the App Store](docs/HISTORY.md#the-app-store).

## What is inside

A one-paragraph tour; [docs/HISTORY.md](docs/HISTORY.md) has the details of
every part, phase by phase.

- **Boot**: a UEFI bootloader (a PE32+ EFI application) loads the ELF kernel
  from the EFI System Partition, or from a CD.
- **Kernel** (`kernel/`): NT-style executive: object manager and handles,
  processes and threads, virtual memory with sections and guard pages, I/O,
  registry, security tokens.  SMP with per-core scheduling and fine-grained
  locks; wait queues; APCs; pipes; the NT system-call table at Windows 10
  1903 numbers.
- **Drivers**: AHCI SATA disks, FAT16/FAT32, GPT; Intel e1000/e1000e
  network cards; PS/2 keyboard and mouse; CMOS clock; the UEFI framebuffer.
- **Networking**: lwIP (TCP/IP, DHCP, DNS), an HTTP/1.1 client, and Mbed
  TLS with the Mozilla root store.
- **Windows userland** (`userland/`): about 35 system DLLs written from
  scratch and compiled with clang for `x86_64-pc-windows-msvc`, and again
  for `i686` in `SysWOW64`: `ntdll`, `kernel32`, `msvcrt`/`ucrtbase` with
  the `api-ms-win-crt-*` API sets, `vcruntime140` (C++ exceptions),
  `user32`/`gdi32` (a real window system, controls, menus, dialogs),
  `comctl32`, `shell32`, `ole32`/`oleaut32` (COM), `advapi32`, `ws2_32`,
  `msi`, and more.
- **Program support**: the PE loader with TLS, `DllMain`, forwarders and
  API sets; x64 and x86 structured exceptions; registry saved to disk;
  COM in-process servers; drag and drop; a shared clipboard; `.lnk`
  shortcuts; Windows Installer packages.

## Quick start

Linux (Ubuntu/Debian) is the build host; on Windows use WSL2.

```bash
# Prerequisites
sudo apt install cmake nasm clang lld llvm python3 \
                 qemu-system-x86 ovmf mtools dosfstools xorriso

# Build (bootloader, kernel with the whole userland inside, nova.img)
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run in QEMU
cmake --build . --target run
```

The first build compiles the NetSurf browser (about 800 files; later builds
reuse the objects).  Set `NOVA_NO_NETSURF=1` to leave the browser out, and
`NOVA_NO_WOW64=1` to skip the 32-bit userland.  `run` attaches a second
disk, `build/nova-data.img`, where drive C: is kept across restarts and
rebuilds.  [docs/building.md](docs/building.md) covers the build, QEMU
options, putting your own programs on the disk, and debugging.

### Bootable ISO and installing on a disk

A ready-to-boot UEFI ISO, `nova.iso`, is committed at the repository root.
It is also the installation disc: booted from it, NovaOS runs live and
opens **Install NovaOS**, which writes a GPT disk with an EFI System
Partition and a data partition for drive C:.

```bash
truncate -s 1G disk.img
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -drive file=disk.img,format=raw -cdrom nova.iso
# after installing: the same command without -cdrom starts from disk.img
```

Give the machine 2 GB so downloaded installers fit in drive C: (which lives
in memory and is saved to disk).  On macOS with Homebrew QEMU, the firmware
ships with QEMU:

```bash
FW="$(brew --prefix qemu)/share/qemu/edk2-x86_64-code.fd"
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file="$FW" \
  -cdrom nova.iso -serial stdio
```

Rebuild the ISO from a fresh build with
`scripts/create-iso.sh nova.iso build/bootx64.efi build/kernel.elf`.

## Testing

There is no CI yet; testing is done in QEMU.

- **Self-test programs** in `userland/programs/`, installed in
  `C:\Programs` (and 32-bit builds in `C:\Programs\x86`).  Run them from the
  Terminal; each prints "N passed, 0 failed": `crttest`, `filetest`,
  `sectest`, `threads`, `dlltest`, `posixtest`, `apitest`, `comtest`,
  `cppeh`, `shmtest`, `pipetest`, `cliptest`, `smpstress`.  `disktest
  write`, a restart and `disktest verify` check that drive C: survives a
  reboot.
- **GUI and interactive checks**: `winhello`, `guitest`, `droptest`, `cpus`
  (SMP speed-up).
- **Real programs** are tested from a second disk image holding them
  (not in this repository), driven by a QEMU harness that types, clicks and takes screenshots.
- **On the host**: `tools/pe_imports.py PROGRAM.exe` lists the imports a
  Windows program needs that NovaOS's DLLs lack; `tools/msitest/` exercises
  the Windows Installer's package readers.
- **Debugging**: the serial log (COM1) has every kernel message; the
  Terminal's `dmesg` shows it, and `trace NAME` logs a program's failing
  system calls.

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

What comes next (graphics and OpenGL, audio, broader app coverage, the
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
│   ├── drivers/          # AHCI (SATA), e1000/e1000e
│   ├── hal/              # Serial, framebuffer, PCI, PS/2, CMOS clock
│   ├── net/              # lwIP port, HTTP client, TLS (Mbed TLS)
│   ├── gdi/              # Software renderer, fonts, ICO and PNG decoding
│   ├── wm/               # Window manager, desktop shell, input, clipboard
│   ├── apps/             # Built-in apps: Terminal, Explorer, Notepad, Settings,
│   │                     #   Calendar, Photos, App Store, Setup
│   ├── ldr/              # Early PE loader and syscall thunk pages
│   └── lib/              # Freestanding string and memory library
├── userland/             # The Windows userland, built with clang + lld-link:
│   ├── ntdll/ kernel32/ user32/ gdi32/ ...   # one directory per system DLL
│   ├── msvcrt/ crt/      # C runtime (msvcrt.dll and ucrtbase.dll), program startup
│   ├── msi/              # Windows Installer (msi.dll)
│   ├── programs/         # cmd.exe, msiexec, reg, find..., samples and self-tests
│   ├── netsurf/          # NetSurf port: fetcher, window surface, fonts
│   └── include/          # The Windows SDK headers NovaOS provides
├── third_party/          # lwIP, Mbed TLS, musl (libm), NetSurf, stb, fonts, 7-Zip installer
├── tools/                # Host tools: build_userland.py, build_netsurf.py, mkfont,
│                         #   make_icons.py, pe_imports.py, msitest/
├── scripts/              # build.sh, run-qemu.sh, create-disk.sh, create-iso.sh
├── docs/                 # Building, roadmap, feature history, Phase 1 architecture
└── nova.iso              # Prebuilt bootable/installation ISO
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
  windows, drawn from bitmaps the programs own.
- **Software rendering**: GDI is a CPU rasterizer writing the UEFI
  framebuffer, double-buffered, at integer HiDPI scale.  There is no GPU
  driver.
- **Drive C: in memory, saved to FAT**: the RAM disk is saved to a FAT32
  volume a second after each change and restored at boot.  System files
  come from the kernel image, so a new build always brings its own.
- **SMP with a shrinking big kernel lock**: the scheduler, memory,
  synchronization, sockets, the GUI and the program loader have their own
  locks; files, the registry and process creation still take the big lock
  (rules and lock order in `kernel/ke/smp.h`).
- **Kernel-helper syscalls (0x01F0–0x01FF)** are private to NovaOS's own
  DLLs and invisible to Windows programs.

## Documentation

- [docs/building.md](docs/building.md): building, running, the data disk,
  tests, debugging.
- [docs/ROADMAP.md](docs/ROADMAP.md): the compatibility strategy and what
  comes next.
- [docs/HISTORY.md](docs/HISTORY.md): what every phase added, in detail.
- [docs/phase1-architecture.md](docs/phase1-architecture.md): the boot flow,
  address-space layout and early kernel design.

## License

NovaOS is MIT licensed. The operating system (kernel, bootloader, system
DLLs, C runtime, desktop and apps) contains no GPL code; bundled third-party
code keeps its own permissive licence (lwIP: BSD 3-clause; Mbed TLS:
Apache-2.0; musl's libm: MIT; Inter and Cascadia Mono: SIL OFL 1.1; DejaVu
Sans Mono: Bitstream Vera licence; stb_truetype/stb_image: public domain or
MIT).  All Win32 API implementations are clean-room, based on public
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
