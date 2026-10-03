# Building NovaOS
<!-- The regions between "BEGIN generated" and "END generated" markers are built from fragment files by tools/docgen.py: edit those files, not the regions (CONTRIBUTING.md). -->

## Prerequisites

The build host is Linux (Ubuntu/Debian here); on Windows use WSL2 or a
Linux VM.  On a Mac, see [macos.md](macos.md).

```bash
sudo apt update
sudo apt install -y \
    cmake ninja-build nasm python3 \
    clang lld llvm libc++-dev \
    mtools dosfstools xorriso \
    qemu-system-x86 ovmf
```

What each part is for:

| Tool | Used for |
|------|----------|
| `cmake`, `ninja-build` (or make) | The build |
| `nasm` | Kernel assembly (entry, interrupt stubs, SMP trampoline, syscall entry) |
| `libc++-dev` | libc++'s C++ headers, for HarfBuzz in `novatext.dll` (nothing of libc++ is linked; set `NOVA_LIBCXX` to use headers elsewhere) |
| `clang`, `lld` (`lld-link`) | **Required.** The Windows userland (`--target=x86_64-pc-windows-msvc` and `i686-pc-windows-msvc`), NetSurf, and the kernel and bootloader unless the alternatives below are installed |
| `llvm` (`llvm-rc`) | Compiling programs' resource scripts (icons, dialogs) |
| `python3` | `tools/build_userland.py`, `tools/build_netsurf.py` |
| `g++-mingw-w64-x86-64`, `g++-mingw-w64-i686` | Only for `tools/build_icu.py` (rebuilding `icu.dll`) |
| `mtools`, `dosfstools` | `nova.img` and putting files on the data disk |
| `xorriso` | `scripts/create-iso.sh` |
| `qemu-system-x86`, `ovmf` | Running NovaOS |

### Optional compilers

- **Kernel**: CMake prefers an `x86_64-elf-gcc` cross-compiler when one is on
  `PATH` and otherwise uses clang.  To build one with crosstool-ng (it
  installs into `~/x-tools/x86_64-unknown-elf`):

  ```bash
  git clone https://github.com/crosstool-ng/crosstool-ng
  cd crosstool-ng && ./bootstrap && ./configure --enable-local && make
  ./ct-ng x86_64-unknown-elf
  ./ct-ng build
  export PATH="$HOME/x-tools/x86_64-unknown-elf/bin:$PATH"
  ```

- **Bootloader**: `x86_64-w64-mingw32-gcc` (`gcc-mingw-w64-x86-64`) is
  used when present; otherwise clang with `lld-link`.

---

## Building

This uses Ninja; without it, run `cmake ..` (Unix Makefiles) and then
`make -j$(nproc)` instead of the last two lines.

```bash
git clone https://github.com/dean-plude/os
cd os
mkdir build && cd build
cmake .. -G Ninja
ninja
```

`scripts/build.sh` does the same in one step (`--clean`, `--release`,
`--jobs N`, `--no-disk`).

The build produces:

```
build/
├── bootx64.efi     # UEFI bootloader
├── kernel.elf      # Kernel ELF with debug symbols; carries drive C:'s system files
├── nova.img        # Bootable FAT32 disk image (128 MiB, sparse)
└── kernel_build/userland/   # The built DLLs and programs (x64; x86 under x86/)
```

### The Windows userland

`userland/` (the system DLLs, the C runtimes, `cmd.exe`, `msiexec` and the
sample and test programs) is compiled by `tools/build_userland.py` with clang
and `lld-link`, twice: once for x64 into `C:\Windows\System32` and
`C:\Programs`, and once for x86 into `C:\Windows\SysWOW64` and
`C:\Programs\x86`.  The files, with the fonts, the sample pictures and the
NetSurf browser, are embedded in `kernel.elf` and placed on drive C: at
boot.

Environment variables:

| Variable | Effect |
|----------|--------|
| `NOVA_NO_NETSURF=1` | Leave the NetSurf browser out (the first build otherwise compiles about 800 files of it; later builds reuse them) |
| `NOVA_NO_WOW64=1` | Skip the 32-bit (x86) pass: no `SysWOW64`, no `C:\Programs\x86` |

To rebuild only the userland, for a quick check of a DLL (the second
command only loads the manifests, as CI does):

```bash
NOVA_NO_NETSURF=1 python3 tools/build_userland.py /tmp/ul /tmp/ul/gen.c kernel/ke/syscall.h
python3 tools/build_userland.py --check
```

### Adding a DLL or program

There is no central list to edit: each DLL registers itself, so changes
made in parallel add files rather than collide on the same lines.

- **A DLL** is a directory `userland/NAME/` with its `.c` files and a
  `dll.json`:

  ```json
  {
    "deps": ["user32", "kernel32", "ntdll"],
    "tlssup": true
  }
  ```

  `deps` are the DLLs it links against (they are built first; a cycle or a
  missing one stops the build).  Leave `base` and `base_x86` out: a new
  DLL gets the first free 16 MiB slot at 0x7FFD00000000 (x64) and
  0x97000000 (x86), and the build checks that no two DLLs overlap.  The
  older DLLs keep the fixed addresses written in their `dll.json`.  Other
  keys: `sources` (directories its `.c` files come from, default its own),
  `entry` (`"DllMain"`), `x64_only`, and `ordinals` (`{"Name": 12}`, for
  DLLs programs import from by number).
- **When a DLL needs more** (a third-party library, extra flags, a
  special link), put the code in `userland/NAME/build.py`, not in
  `tools/build_userland.py`: it may define `cflags(b)` (flags for the
  DLL's own sources), `objs(b, odir)` (extra objects; `b.compile_many`
  compiles a library in parallel and reuses fresh objects) and `link(b,
  odir, objs, deps, base)` (replaces the link; `b` is
  `tools/build_userland.py` itself, with `b.ARCH`, `b.cflags()`, `b.cc`,
  `b.run`, `b.link_dll`).  `userland/secur32/build.py` (Mbed TLS) and
  `userland/msvcrt/build.py` (msvcrt.dll and ucrtbase.dll) are examples.
- **A program** is `userland/programs/NAME.c` (or `.cpp`, and `NAME.rc`
  for resources).  It goes to `C:\Programs`, 64-bit only, unless
  `NAME.json` says otherwise: `{"x86": true}` builds it for 32 bits as
  well, `"system": true` installs it in `C:\Windows\System32`, `"libs":
  ["usp10"]` links more DLLs, and `"selftest": true` lists it among the
  README's self-test programs.

Two pieces are built by their own tools and committed, so the normal build
needs neither:

- **ICU** (`third_party/icu`): `icu.dll` for x64 and x86, one DLL holding
  ICU's C API under unversioned names as Windows 10's does, and the ICU
  data both read from `C:\Windows\Globalization\ICU\icudt77l.dat`.
  `tools/build_icu.py` downloads the ICU4C 77.1 source (checking its
  SHA-256), builds ICU's host tools, cross-compiles ICU with MinGW-w64
  (`sudo apt install g++-mingw-w64-x86-64 g++-mingw-w64-i686`) and trims the
  data (legacy code-page converters, word-break dictionaries,
  transliteration, unit names and character names: 18 MB instead of 31;
  .NET's answers for all its cultures are the same with either).  Run it
  again only to move to another ICU release.  `build_userland.py` embeds
  files of 1 MiB or more (ICU's, NetSurf) zlib-compressed; the kernel
  inflates them onto drive C: at boot.
- **kernel32's locale table** (`userland/kernel32/locale_data.h`):
  `tools/gen_locales.py` generates it from .NET's `IcuLocaleData.cs` (MIT),
  .NET's record of the names, LCIDs, code pages and GEOIDs of the 864
  locales Windows knows.

### The ISO

```bash
scripts/create-iso.sh nova.iso build/bootx64.efi build/kernel.elf
```

The ISO is not committed (`*.iso` is in `.gitignore`).  CI builds it on
every pull request and keeps it as the run's `nova-iso` artifact, and each
push to `main` that passes CI replaces `nova.iso` on the `latest` release:
<https://github.com/dean-plude/os/releases/latest/download/nova.iso>.

The ISO is El Torito UEFI, no emulation: its EFI System Partition holds
`\EFI\BOOT\BOOTX64.EFI` and `\EFI\NOVA\kernel.elf`.  Booted from it, NovaOS
runs live and opens Install NovaOS (see the README).

---

## Running in QEMU

From the build directory (`--target run-debug` instead also starts the GDB
stub on port 1234):

```bash
cd build
cmake --build . --target run
```

or by hand, from the top of the repository:

```bash
./scripts/run-qemu.sh build/nova.img /usr/share/OVMF/OVMF_CODE.fd
```

`run-qemu.sh` starts a q35 machine with 4 CPUs and 256 MiB of memory, no
network card, the serial port (COM1) on your terminal, `nova.img` as the
boot disk and `build/nova-data.img` as the data disk.  For the network, the
App Store or large programs, run QEMU yourself with more memory and a NIC:

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -drive file=build/nova.img,format=raw \
  -drive file=build/nova-data.img,format=raw \
  -serial stdio
```

QEMU's default user-mode network (an e1000e on q35) works out of the box;
add `-nic user,model=e1000` to test the older card or
`-nic user,model=virtio-net-pci` for virtio-net.  `-smp N` sets the core
count (up to 16).

USB devices need a controller: `-device qemu-xhci` (USB 3), `-device
usb-ehci` (USB 2), `-device pci-ohci` or `-device piix3-usb-uhci` (USB
1.1), or an ICH9 EHCI with UHCI companions (`ich9-usb-ehci1` plus
`ich9-usb-uhci1..3` with `masterbus=`), then e.g. `-device usb-kbd
-device usb-tablet`.  With `-machine q35,i8042=off` there is no PS/2
keyboard, so typing goes over USB.

For sound add `-device intel-hda -device hda-output` (or `hda-duplex` or
`hda-micro`, which add a line in or a microphone to record from);
`-audiodev wav,id=snd0,path=out.wav,out.frequency=48000` with
`-device hda-output,audiodev=snd0` records it instead of playing it.
`cmake --build . --target run` adds the card, playing through the host's
PulseAudio or PipeWire when it finds one; set `NOVA_AUDIO` (`pa`,
`pipewire`, `alsa`, `none`, `wav,path=out.wav`) to pick QEMU's backend.

### Where your files are kept

Drive C: lives in memory, and NovaOS saves every change to an NTFS or FAT
volume a second later (and before Restart / Shut down), restoring it at
the next boot.  The files installed from the OS image (`C:\Windows\System32`,
`C:\Windows\SysWOW64`, `C:\Programs`) are never saved, so a newer image
always brings its own.  It uses the first of:

1. an NTFS or FAT16/FAT32 volume labelled `NOVADATA` (on any SATA or NVMe
   disk, including the data partition Install NovaOS creates);
2. an empty disk (all zeros at the start), which it formats as FAT32 `NOVADATA`;
3. the boot disk itself, under `\NOVA\C`.

Install NovaOS asks which file system drive C: gets: NTFS (the default)
or FAT32.  On NTFS, C: is the whole volume, and it keeps creation times
and each file's security descriptor too, so ACLs set with
`SetFileSecurity` or `SetNamedSecurityInfo` (or given to `CreateFile`) are
enforced when files are opened, deleted and renamed, and survive a
restart.  A file without a descriptor of its own inherits from its
folders, as on Windows; the new volume's root gives the user full control
of what they create.  On FAT, C: is the folder `\NOVA\C`, and ACLs last
only until restart.  Files from the OS image keep no descriptor across a
restart either way.

`scripts/run-qemu.sh` attaches `build/nova-data.img` (256 MiB, created on
first run), so your files survive rebuilds of `nova.img`.  In the Terminal,
`vol` shows where C: is saved and `sync` saves it now.  Programs can be put
on the disk from the host with mtools, e.g.:

```bash
mmd   -i build/nova-data.img@@1M ::/NOVA/C/Tools
mcopy -i build/nova-data.img@@1M rg.exe ::/NOVA/C/Tools/
```

(`@@1M`: the volume starts 1 MiB in, after the partition table.)  The ISO
boots from a CD, which is read-only: attach a disk to keep files.

### Other drives (NTFS)

Every NTFS volume on an attached disk (a partition in an MBR or GPT, or a
whole disk) becomes a drive of its own, D:, E:, ... in the order found.
Programs, the Terminal and File Explorer list, open, run, write, create,
rename and delete files there.  Creating, renaming and deleting reach the
disk at once; a file's new contents when the program closes it, or after
a quiet second.  Compressed and sparse files are read but not rewritten
(they show as read-only); encrypted files are not read.  A file is read
into memory when it is opened, so the largest one that opens is 256 MiB.

There is no journal: `$LogFile` is emptied when a volume is mounted and
each change is complete on disk when it returns, so Windows and
`ntfsfix` find the volume clean.  A volume Windows left hibernated (Fast
Startup included), with a chkdsk pending or with transactions in its
`$LogFile` is mounted read-only, as ntfs-3g does.

For a test disk (needs `ntfs-3g`):

```bash
scripts/make-ntfs-disk.sh build/nova-ntfs.img build
qemu-system-x86_64 ... -drive format=raw,file=build/nova-ntfs.img
```

then run `drivetest` in the Terminal, shut down, and check the disk on the
host:

```bash
scripts/check-ntfs-disk.sh build/nova-ntfs.img
```

which runs `ntfsfix -n`, `ntfssecaudit -a` and `scripts/ntfs-check.py` (a
chkdsk-style check of the bitmaps, MFT records, directory indexes, link
counts and `$Secure`) and checks the files drivetest left behind.

---

## Tests

Tests run inside NovaOS under QEMU.  `tools/selftest.py` boots
`build/nova.img` and runs the regression gate, the same one CI runs on every
pull request (`.github/workflows/ci.yml`).  The core suite needs `iasl`
(from `acpica-tools`) for the tables in `tests/acpi/`; its exit status is the
number of failures:

```bash
sudo apt install acpica-tools
python3 tools/selftest.py
python3 tools/selftest.py --only apitest,guitest --out /tmp/st
```

The graphics suite downloads 7-Zip, Mesa and DXVK and builds
gltest/d3dtest/d2dtest:

```bash
sudo apt install p7zip-full gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
tools/ci/stage-graphics.sh /tmp/gfx
python3 tools/selftest.py --suite graphics --gfx /tmp/gfx
```

The network suite tests IPv4, IPv6 and winhttp's HTTP/2, and needs `node`
and `openssl`:

```bash
python3 tools/selftest.py --suite network
```

The core suite is `apitest`, `abitest`, `filetest`, `pipetest`, `proctest`,
`sectest`, `acltest` (x64 and x86), `guitest auto`, `disptest`, `icutest` (x64 and x86), `comtest`,
`tlbtest` (x64 and x86), `usptest` (x64 and x86), `cppeh`, `battery`, `soundtest tone`,
`soundtest wasapi`, `soundtest record`, `soundtest capture`, `soundtest volume`,
`sleeptest timer`, `powertest`, `disptest 1024 768` (saves the mode),
`filetest install` (an installer that must replace a running program
schedules it for the next boot), a restart that must report `Pending file
operations at boot: 2 done, 0 failed` and come up in the saved mode
(`disptest saved 1024 768`), `filetest installed`, and
last `crash kernel`, which halts the kernel on purpose and passes when the
serial log shows a symbolized backtrace (`KeCrashTestFault`,
`KeCrashTest`, `sys_nova_bugcheck`, ...).  The graphics suite first runs
`d2dtest`, x64 and x86: it checks geometry computations, draws a scene into
a DC render target and compares it with `d2dref.bmp`, the image
`tools/d2dtest/reference.py` draws with Skia (`pip install skia-python`;
re-run it when the scene changes), then shows the scene in a window.  It
then types `store
install Mesa 3D` and `store install DXVK` (the archives are already in
`C:\Downloads`, so the App Store installs without a network) and then runs
`gltest` and `d3dtest`, x64 and x86, from `C:\Tests`, taking a screenshot
of each while it draws.  The network suite (`tests/selftest/network4` and
`network6`) boots twice with a virtio-net
card: on QEMU's user network it runs `ipconfig`, `ping 10.0.2.2`, `netcat`
(Winsock over IPv4) and `httptest suite` (winhttp: HTTP/2 by ALPN, large
bodies, POST, redirects, certificate checks, chunked HTTP/1.1, the
asynchronous API) against `tools/h2server.js` with a throwaway self-signed
certificate, then `looptest` (socket pairs over 127.0.0.1 and ::1,
`localhost`); on an IPv6-only network made by `tools/v6peer.py` it checks
SLAAC and RDNSS (`ipconfig`), `ping -6`, `curl -6` and `netcat` over IPv6.

It boots once (about 20 s under TCG) with an HD Audio card recorded to a WAV
whose microphone hears a 523 Hz tone (through a private PulseAudio server,
so the host needs `pulseaudio`, `pulseaudio-utils` and QEMU's PulseAudio
backend, `qemu-system-gui` on Ubuntu; without them the recording tests
fail and the rest run), the battery from `tests/acpi/battery.asl`, the
lid and thermal zone from `tests/acpi/lid-thermal.asl` (QEMU's `pc-testdev`
stands in for the embedded controller: the test writes the lid and
temperature to ports 0xE8 and 0xE9 through the QEMU monitor) and a USB
keyboard on an xHCI controller at 00:05.0, then types each test into the
Terminal.  `powertest` asks the test to close the lid; once NovaOS has gone
to sleep the test opens it, presses a key on the USB keyboard and wakes the
machine (`system_wakeup`, as QEMU has no USB-to-platform wake), then heats
and cools the thermal zone.  A test passes when the program exits with code 0, prints no
`FAIL` line or non-zero "failed" count, and prints what the test expects;
a kernel panic stops the run.  `--out` (default `selftest-out/`) keeps the
serial log, a screenshot after each test, `sound.wav` and the two
recordings (`rec.wav`, `cap.wav`); `--summary FILE`
appends a Markdown table and `--junit FILE` writes JUnit XML.

To add a test, add a file to `tests/selftest/core/` (or `graphics/`):
tests run in file-name order, so the number prefix places it (the restart
is at 130 and `crash kernel`, which halts NovaOS, stays last at 900).

```python
# tests/selftest/core/140-mytest.py
DOC = '`mytest` (what it covers)'      # its entry in README's core-suite list
TESTS = [
    Test('mytest', 'mytest', [r'mytest: \d+ passed, 0 failed']),
]
```

`python3 tools/selftest.py --list` prints the suite.  The nightly app
corpus works the same way: one file per program in `tests/appcorpus/`
defining `APP = App(...)` and `DOC` (Notepad++ stays last at 900).

### Self-test programs

In `userland/programs/`, installed in `C:\Programs` and, as 32-bit builds,
in `C:\Programs\x86`.  Type the name in the Terminal; each prints "N passed,
0 failed".  Each row of this table is a file in `docs/selftests/` (a new
program adds one; see [CONTRIBUTING.md](../CONTRIBUTING.md)).

<!-- BEGIN generated:selftest-table -->

| Program | Covers |
|---|---|
| `crttest` | The C runtime |
| `filetest` | Files and directories; `RegNotifyChangeKeyValue` (values, subkeys, subtrees, deleted keys, synchronous); `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)`.  `filetest install`, a restart and `filetest installed` check that a running program replaced at boot |
| `sectest` (x64) | Hostile system calls refused (kernel pointers, bogus handles, bad descriptors); tokens and object security through the native API: a restricted or deny-only impersonation token is refused a protected named event |
| `threads` | Threads, synchronization, SEH |
| `dlltest` | DLL loading, TLS, `DllMain` |
| `posixtest` | The POSIX layer in msvcrt |
| `apitest` | kernel32, advapi32, bcrypt, shell32, shlwapi, psapi, user32/gdi32, the registry |
| `abitest` (x64) | The binary interface against Windows 10 1903 x64: TEB, PEB, process parameters, loader lists, `KUSER_SHARED_DATA`, `CONTEXT` and `EXCEPTION_RECORD` offsets, ntdll's stub bytes and every system-call number (`abitest_nt1903.h`), raw `syscall`s |
| `comtest` | ole32/oleaut32, `IShellLink` |
| `tlbtest` | COM type libraries: `LoadTypeLib` on `testdll.dll`'s embedded library, `ITypeLib`/`ITypeInfo`/`ITypeComp`, registration, `ITypeInfo::Invoke`, `DispCallFunc`, `CreateStdDispatch` |
| `cppeh` | C++ exceptions and RTTI |
| `usptest` | Uniscribe on HarfBuzz: Arabic and Devanagari itemized, shaped (contextual forms, ligatures, reordering) and placed with the Noto fonts, and GDI `ExtTextOut` drawing complex text exactly as `ScriptStringOut` does; `usptest bmp FILE` saves sample lines as a bitmap |
| `shmtest` | Named and file-backed shared memory between processes |
| `pipetest` | Pipes, inherited handles, `cmd /c`, `_popen`, overlapped I/O |
| `proctest` | `CreateProcess` flags: `CREATE_SUSPENDED`, `CREATE_NEW_CONSOLE` (`GetConsoleProcessList`), file positions shared with children and duplicates |
| `cliptest` | The clipboard and the OLE clipboard, across two processes |
| `disptest` | Display modes: `EnumDisplaySettings`, `ChangeDisplaySettings`, `WM_DISPLAYCHANGE`, a window that 800x600 shrinks growing back to its size and place, `CDS_UPDATEREGISTRY` saving the mode in the registry.  `disptest W H` switches and saves; `disptest saved W H` checks the mode after a restart |
| `icutest` | The system ICU (`icu.dll`) as .NET loads it: German and Japanese names, numbers, currencies, dates, the Japanese calendar, collation, case, time-zone ids, IDNA, normalization, 8 threads at once; then kernel32's `GetLocaleInfoEx`, LCIDs and locale enumeration for those locales |
| `battery` | AC power and batteries (`GetSystemPowerStatus`, `SystemBatteryState`); CI expects the battery in `tests/acpi/battery.asl` |
| `sleeptest timer` | How late `Sleep(1)`, `Sleep(5)` and a 1 ms wait timeout end, idle and with a busy thread on every CPU; passes when the 95th percentile under load is 1 ms or less and none ends early.  Plain `sleeptest` sleeps (S3) instead |
| `powertest` | The lid and a thermal zone (`GetPwrCapabilities`, `ThermalInformation`, `LastSleepTime`/`LastWakeTime`): closing the lid sleeps; needs `tests/acpi/lid-thermal.asl` and the self-test's help (see above) |
| `guitest auto` | user32 and comctl32: menus, accelerators, edit and list boxes, a resource dialog, a message box, a property sheet |
| `smpstress` (x64) | Locks, events, semaphores, memory, handles and starting processes from many threads, then file and registry throughput on one CPU and on all (`smpstress scaling 3` fails below 3x; `smpstress throughput [X [files\|registry [many\|N]]]` measures only; run with `tools/novarun.py --smp 4`) |
| `acltest` | Access checks against DACLs (`AccessCheck`) for our token and restricted, write-restricted and deny-only ones; `CheckTokenMembership`, impersonation; a named event with a DACL refused to a restricted token; file ACLs on drive C:: denied writes, deletes and renames (and reads for a restricted token), inheritance, `CreateFile` with a descriptor.  It leaves `C:\AclTest\kept.txt` and, run again after a restart, checks it kept its DACL (C: on NTFS) |
| `drivetest` | Drive D: (NTFS: reading, then writing, renaming, deleting), with the disk from `scripts/make-ntfs-disk.sh`; then `scripts/check-ntfs-disk.sh` on the host |
| `soundtest info`, `tone`, `float`, `wasapi`, `both`, `play FILE`, `ding`, `msgbeep`, `beep` | Sound output (needs an HD Audio card; see below) |
| `soundtest record FILE [MS]`, `capture FILE [MS]`, `volume` | Recording through `waveIn` and WASAPI capture into a WAV, and `IAudioEndpointVolume` (needs a card with an input) |
| `disktest write`, restart, `disktest verify` | Drive C: surviving a reboot |
| `httptest suite HTTPS-BASE HTTP-BASE` | winhttp against `tools/h2server.js`: HTTP/2 by ALPN, a 300 KB body, POST, a redirect, an untrusted certificate refused, chunked HTTP/1.1, the asynchronous API.  `httptest [-2] [-k] [-a] URL` fetches one URL |
| `netcat [-4\|-6] [-p PORT] HOST [PATH]` | Winsock: `getaddrinfo`, IPv4 or IPv6 sockets, an HTTP/1.0 GET |
| `looptest` | Winsock over the loopback interface: a socket pair over 127.0.0.1 and ::1 (port 0, `getsockname`, a non-blocking connect, data sent before `accept`), closing a listener with a queued connection, `localhost` |

<!-- END generated:selftest-table -->

`crash` writes through a NULL pointer (only it dies); `crash kernel`
crashes the kernel on purpose (`NtNovaBugCheck`) to show the backtrace.

Interactive ones: `winhello` and `guitest` (windows, menus, dialogs,
property sheets; `guitest auto` drives them itself and reports, as CI runs it), `droptest` (drag and drop; its targets list each dropped file's size, or "missing"), `cpus` (SMP speed-up), and
`hello`, `mandel`, `primes`, `wc`, `guess`.

### Boot-time self-tests

The kernel tests itself while it boots and prints the results to the serial
log: `[PROBE]` (user-pointer validation) and `[PSTEST]`
(`PsGetCurrentThread` on bare kernel threads).  `grep -a 'passed,'
serial.log` lists them; each should end "0 failed".

### Real programs

`tools/appcorpus.py` is the nightly app corpus
(`.github/workflows/nightly.yml`, which also runs on pull requests that
change it): it downloads the official Windows x64 releases into a cache,
unpacks them into `C:\Apps`, boots once and runs each one's commands.
The same run then boots on 4 CPUs for `smpstress scaling 0` and records
each scaling figure in the night's table.  A shared runner may not run the
emulated CPUs side by side at all, so rather than the 3x target
`tools/ci/check-smpstress.py` fails the run on a hang or a failed test,
and, when bare system calls scale 2x or more in the same run, on files or
the registry scaling less than half as well (what work under one big lock
would do).

| Program | Checks |
|---|---|
| ripgrep 14.1.1 | `--version`, a search |
| fd 10.2.0 | `--version`, finding `*.txt` |
| jq 1.7.1 | `--version`, a filter over a JSON file |
| 7-Zip 26.03 | `7z a`, `7z t` |
| MinGit 2.51.0 | `git clone` of a bare repository, `log` (through the `less` pager), `status` |
| Python 3.14.0 (NuGet package) | `-c` with `json` and `sys` |
| Node.js 24.9.0 | `-v`, `-e` |
| .NET 10.0.12 (runtime from NuGet, with the 8.0 host) | `--list-runtimes`; `tests/dotnet/culturetest.dll` formats German and Japanese through ICU |
| NovaOS's own screens | `dir C:\` and `dir D:\` (an empty NTFS disk made with `mkntfs`) name their drive and give its own free space (`dir.png`); `start explorer` shows This PC with both drives, matching `tests/reference/this-pc.png` |
| Notepad++ 8.8.3 (portable) | opens a file; the screenshot (tab bar and status bar drawn) must match `tests/reference/notepad++.png` (at most 3% of pixels differ) |

It needs 7-Zip's installer, Pillow, and `mkntfs` (for drive D:).  The exit
status is the number of programs that failed; `--update-reference` rewrites
the reference screenshots after an intended change:

```bash
sudo apt install p7zip-full python3-pil ntfs-3g
python3 tools/appcorpus.py
python3 tools/appcorpus.py --only ripgrep,jq --out /tmp/ac
python3 tools/appcorpus.py --only NovaOS,Notepad++ --update-reference
```

A command passes as a self-test does (exit code 0, the output expected).
To add a program, add an `App` to `APPS`.  Other third-party programs (the
installers, Java, Roslyn) and test scripts such as `cmdtest.bat` for
`cmd.exe` are tried by hand with `tools/novarun.py`: copy a program onto
the data disk with `--put` and type its commands.

Full-screen and interactive programs (Neovim, an MSYS2 `sh` session) are
driven with `!type`, which types without waiting for the command to end
(`\n` is Enter, `\e` Esc), and `!done N`, which waits up to N seconds for
it to end:

```bash
python3 tools/novarun.py --put 'nvim-win64=C:\Apps\nvim' 'cd C:\Apps\nvim\bin' \
    '!type nvim --clean t.txt\n' '!wait 40' '!type ihello\e:wq\n' '!done 60' 'type t.txt'
```

`!reboot` restarts NovaOS (`shutdown /r`; drive C: on the data disk is
kept) and opens the Terminal again, for what must survive a restart:

```bash
python3 tools/novarun.py 'filetest install' '!reboot' 'filetest installed'
```

### Sound

`soundtest` plays sine tones through each path.  To check what came out,
record it and measure it (`wavcheck.py` prints each tone's start, length,
level and pitch):

```bash
python3 tools/novarun.py --wav /tmp/out.wav 'C:\Programs\soundtest.exe tone 440 1000' \
    'C:\Programs\soundtest.exe wasapi 523 800'
python3 tools/wavcheck.py /tmp/out.wav
```

To test recording, `--rec FILE.wav` gives the card a microphone that hears
FILE over and over (a private PulseAudio server with two null sinks, so the
guest records and plays in real time; with `--wav` too, the playback is
saved from the second sink).  Copy the recording off the data disk and
check it (`wavcheck.py` exits 0 when the tone is there):

```bash
python3 tools/novarun.py --keep /tmp/rec --rec tone523.wav 'soundtest record C:\rec.wav 3000'
mcopy -i /tmp/rec/data.img ::/NOVA/C/rec.wav /tmp/
python3 tools/wavcheck.py /tmp/rec.wav --tone 523 2500
```

### Network

`--net` gives the guest an e1000e card on QEMU's user network; the host is
`10.0.2.2`, so a test server on the host is reachable from programs:

```bash
python3 tools/novarun.py --net --put 'DIR=C:\Apps\x' 'cd C:\Apps\x' \
    'ffmpeg.exe -tls_verify 0 -i https://10.0.2.2:8443/tone.wav -f null -'
```

For IPv6, `tools/v6peer.py` is a whole IPv6-only network (a router with
SLAAC and RDNSS, DNS for `nova6.test`, an HTTP server) that QEMU reaches
through a datagram netdev, so the host needs no IPv6 of its own:

```bash
python3 tools/v6peer.py &
python3 tools/novarun.py --extra '-netdev dgram,id=v6,local.type=inet,local.host=127.0.0.1,local.port=10601,remote.type=inet,remote.host=127.0.0.1,remote.port=10600 -device virtio-net-pci,netdev=v6' \
    ipconfig 'ping -6 nova6.test' 'curl -6 http://nova6.test/'
```

`tools/h2server.js CERT KEY` (Node) serves HTTPS with HTTP/2 on port 8443
and plain HTTP on 8080 for `httptest` (`httptest -2 -k https://10.0.2.2:8443/hello`).

### On the host

- `tools/pe_imports.py PROGRAM.exe ...` lists every DLL and function a
  Windows program imports that NovaOS's DLLs do not provide (by default it
  reads the DLLs in `build/kernel_build/userland`).
- `tools/msitest/hosttest.c` dumps a Windows Installer package's tables
  and cabinets with the same readers `msi.dll` uses (build instructions in
  the file); `tools/msitest/make_package.sh` builds a test package with
  msitools.

---

## Debugging

- **Serial log**: every kernel message goes to COM1 (your terminal under
  `run`).  In NovaOS, the Terminal's `dmesg` shows it.
- **Program crashes** are logged with the faulting module and offset, the
  process's exit code, and `OutputDebugString` output.
- **`trace NAME`** in the Terminal logs the failing system calls (with file
  names) of the program called NAME, each with its process id; `trace
  +NAME` logs every call, not only the failing ones; `trace off` stops it.
- **`profile on [DELAY LENGTH]`** in the Terminal samples every CPU at each
  timer tick (from DELAY ticks on, for LENGTH ticks; 100 a second), and
  **`profile`** prints the result: how the time split between programs, the
  kernel, waiting for the big kernel lock and idle; the busiest kernel
  functions and their callers; the busiest 64-byte lines of program code
  (look the address up in a DLL's `.map` next to it in the build); and the
  system calls made (3xx: an interrupt under the big lock).  For example
  `tools/novarun.py --smp 4 "profile on 550 100" "smpstress throughput 0
  files many" profile`.

### GDB

In one terminal, start QEMU with the GDB stub (it waits for GDB):

```bash
./scripts/run-qemu.sh build/nova.img /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/OVMF/OVMF_VARS.fd --gdb
```

In a second, attach GDB and set a hardware breakpoint at the kernel's C
entry:

```bash
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) hbreak KiSystemStartup
(gdb) continue
```

---

## Troubleshooting

### "No suitable kernel compiler found"

Install clang: `sudo apt install clang lld llvm`, or build x86_64-elf-gcc as
described above.

### "OVMF not found"

```bash
sudo apt install ovmf
ls /usr/share/OVMF/ /usr/share/ovmf/
cmake -DOVMF_CODE=/path/to/OVMF_CODE.fd ..
```

Newer distributions name the files `OVMF_CODE_4M.fd` and `OVMF_VARS_4M.fd`.

### Kernel crashes on boot

Break at `KiSystemStartup` in GDB and check:
1. Is the BootInfo magic correct? (`0x4E4F564100424F4F`)
2. Is the physmap VA correct? (`0xFFFF800000000000`)
3. Is the CR3 value reasonable? (a physical address below 4 GiB)

### Serial output not appearing in QEMU

Use `-serial stdio`, not `-serial null`.  `run-qemu.sh` does this.
