# Building NovaOS

## Prerequisites

The build host is Linux (Ubuntu/Debian here); on Windows use WSL2 or a
Linux VM.  On a Mac, see [macos.md](macos.md).

```bash
sudo apt update
sudo apt install -y \
    cmake ninja-build nasm python3 \
    clang lld llvm \
    mtools dosfstools xorriso \
    qemu-system-x86 ovmf
```

What each part is for:

| Tool | Used for |
|------|----------|
| `cmake`, `ninja-build` (or make) | The build |
| `nasm` | Kernel assembly (entry, interrupt stubs, SMP trampoline, syscall entry) |
| `clang`, `lld` (`lld-link`) | **Required.** The Windows userland (`--target=x86_64-pc-windows-msvc` and `i686-pc-windows-msvc`), NetSurf, and the kernel and bootloader unless the alternatives below are installed |
| `llvm` (`llvm-rc`) | Compiling programs' resource scripts (icons, dialogs) |
| `python3` | `tools/build_userland.py`, `tools/build_netsurf.py` |
| `mtools`, `dosfstools` | `nova.img` and putting files on the data disk |
| `xorriso` | `scripts/create-iso.sh` |
| `qemu-system-x86`, `ovmf` | Running NovaOS |

### Optional compilers

- **Kernel**: CMake prefers an `x86_64-elf-gcc` cross-compiler when one is on
  `PATH` and otherwise uses clang.  To build one with crosstool-ng:

  ```bash
  git clone https://github.com/crosstool-ng/crosstool-ng
  cd crosstool-ng && ./bootstrap && ./configure --enable-local && make
  ./ct-ng x86_64-unknown-elf
  ./ct-ng build          # installs into ~/x-tools/x86_64-unknown-elf
  export PATH="$HOME/x-tools/x86_64-unknown-elf/bin:$PATH"
  ```

- **Bootloader**: `x86_64-w64-mingw32-gcc` (`gcc-mingw-w64-x86-64`) is
  used when present; otherwise clang with `lld-link`.

---

## Building

```bash
git clone https://github.com/dean-plude/os
cd os
mkdir build && cd build
cmake .. -G Ninja        # or: cmake ..  (Unix Makefiles), then make -j$(nproc)
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

To rebuild only the userland, for a quick check of a DLL:

```bash
NOVA_NO_NETSURF=1 python3 tools/build_userland.py /tmp/ul /tmp/ul/gen.c kernel/ke/syscall.h
```

### The ISO

```bash
scripts/create-iso.sh nova.iso build/bootx64.efi build/kernel.elf
```

The ISO is El Torito UEFI, no emulation: its EFI System Partition holds
`\EFI\BOOT\BOOTX64.EFI` and `\EFI\NOVA\kernel.elf`.  Booted from it, NovaOS
runs live and opens Install NovaOS (see the README).

---

## Running in QEMU

```bash
cd build
cmake --build . --target run          # or: --target run-debug (GDB on :1234)

# by hand:
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

For sound add `-device intel-hda -device hda-output` (or `hda-duplex`);
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
pull request (`.github/workflows/ci.yml`):

```bash
sudo apt install acpica-tools          # iasl, for the tables in tests/acpi/
python3 tools/selftest.py              # the core suite; exit status = failures
python3 tools/selftest.py --only apitest,guitest --out /tmp/st

# the network suite: IPv4, IPv6 and winhttp's HTTP/2 (needs node and openssl)
python3 tools/selftest.py --suite network

# the graphics suite: 7-Zip, Mesa and DXVK downloads, gltest/d3dtest builds
sudo apt install p7zip-full gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
tools/ci/stage-graphics.sh /tmp/gfx
python3 tools/selftest.py --suite graphics --gfx /tmp/gfx
```

The core suite is `apitest`, `abitest`, `filetest`, `pipetest`, `proctest`,
`guitest auto`, `disptest`, `battery`, `soundtest tone`, `soundtest wasapi`,
`sleeptest timer`, `powertest`, `disptest 1024 768` (saves the mode), and last `crash kernel`, which halts the kernel on purpose and passes when the
serial log shows a symbolized backtrace (`KeCrashTestFault`,
`KeCrashTest`, `sys_nova_bugcheck`, ...).  A second boot on the same data
disk (drive C: as the first boot saved it) then runs `disptest saved 1024
768`: NovaOS must have come up in the saved mode.  The graphics suite types `store
install Mesa 3D` and `store install DXVK` (the archives are already in
`C:\Downloads`, so the App Store installs without a network) and then runs
`gltest` and `d3dtest`, x64 and x86, from `C:\Tests`, taking a screenshot
of each while it draws.  The network suite boots twice with a virtio-net
card: on QEMU's user network it runs `ipconfig`, `ping 10.0.2.2`, `netcat`
(Winsock over IPv4) and `httptest suite` (winhttp: HTTP/2 by ALPN, large
bodies, POST, redirects, certificate checks, chunked HTTP/1.1, the
asynchronous API) against `tools/h2server.js` with a throwaway self-signed
certificate; on an IPv6-only network made by `tools/v6peer.py` it checks
SLAAC and RDNSS (`ipconfig`), `ping -6`, `curl -6` and `netcat` over IPv6.

It boots once (about 20 s under TCG) with an HD Audio card recorded to a WAV,
the battery from `tests/acpi/battery.asl`, the lid and thermal zone from
`tests/acpi/lid-thermal.asl` (QEMU's `pc-testdev` stands in for the embedded
controller: the test writes the lid and temperature to ports 0xE8 and 0xE9
through the QEMU monitor) and a USB keyboard on an xHCI controller at
00:05.0, then types each test into the Terminal.  `powertest` asks the test
to close the lid; once NovaOS has gone to sleep the test opens it, presses a
key on the USB keyboard and wakes the machine (`system_wakeup`, as QEMU has
no USB-to-platform wake), then heats and cools the thermal zone.  A test passes when the program exits with code 0, prints no
`FAIL` line or non-zero "failed" count, and prints what the test expects;
a kernel panic stops the run.  `--out` (default `selftest-out/`) keeps the
serial log, a screenshot after each test and `sound.wav`; `--summary FILE`
appends a Markdown table and `--junit FILE` writes JUnit XML.  To add a test,
add a line to `CORE`, `GRAPHICS`, `NET4` or `NET6` in `tools/selftest.py`.

### Self-test programs

In `userland/programs/`, installed in `C:\Programs` and, as 32-bit builds,
in `C:\Programs\x86`.  Type the name in the Terminal; each prints "N passed,
0 failed".

| Program | Covers |
|---------|--------|
| `crttest` | The C runtime |
| `filetest` | Files and directories |
| `sectest` | Sections and memory |
| `threads` | Threads, synchronization, SEH |
| `dlltest` | DLL loading, TLS, `DllMain` |
| `posixtest` | The POSIX layer in msvcrt |
| `apitest` | kernel32, advapi32, bcrypt, shell32, shlwapi, psapi, user32/gdi32, the registry |
| `abitest` (x64) | The binary interface against Windows 10 1903 x64: TEB, PEB, process parameters, loader lists, `KUSER_SHARED_DATA`, `CONTEXT` and `EXCEPTION_RECORD` offsets, ntdll's stub bytes and every system-call number (`abitest_nt1903.h`), raw `syscall`s |
| `comtest` | ole32/oleaut32, `IShellLink` |
| `cppeh` | C++ exceptions and RTTI |
| `shmtest` | Named and file-backed shared memory between processes |
| `pipetest` | Pipes, inherited handles, `cmd /c`, `_popen`, overlapped I/O |
| `proctest` | `CreateProcess` flags: `CREATE_SUSPENDED`, `CREATE_NEW_CONSOLE` (`GetConsoleProcessList`), file positions shared with children and duplicates |
| `cliptest` | The clipboard and the OLE clipboard, across two processes |
| `disptest` | Display modes: `EnumDisplaySettings`, `ChangeDisplaySettings`, `WM_DISPLAYCHANGE`, a window that 800x600 shrinks growing back to its size and place, `CDS_UPDATEREGISTRY` saving the mode in the registry.  `disptest W H` switches and saves; `disptest saved W H` checks the mode after a restart |
| `battery` | AC power and batteries (`GetSystemPowerStatus`, `SystemBatteryState`); CI expects the battery in `tests/acpi/battery.asl` |
| `sleeptest timer` | How late `Sleep(1)`, `Sleep(5)` and a 1 ms wait timeout end, idle and with a busy thread on every CPU; passes when the 95th percentile under load is 1 ms or less and none ends early.  Plain `sleeptest` sleeps (S3) instead |
| `powertest` | The lid and a thermal zone (`GetPwrCapabilities`, `ThermalInformation`, `LastSleepTime`/`LastWakeTime`): closing the lid sleeps; needs `tests/acpi/lid-thermal.asl` and the self-test's help (see above) |
| `guitest auto` | user32 and comctl32: menus, accelerators, edit and list boxes, a resource dialog, a message box, a property sheet |
| `smpstress` (x64) | Locks, events, semaphores and memory from many threads |
| `acltest` | Access checks against DACLs (`AccessCheck`), and file ACLs on drive C:: denied writes, deletes and renames, inheritance, `CreateFile` with a descriptor; it leaves `C:\AclTest\kept.txt` and, run again after a restart, checks it kept its DACL (C: on NTFS) |
| `drivetest` | Drive D: (NTFS: reading, then writing, renaming, deleting), with the disk from `scripts/make-ntfs-disk.sh`; then `scripts/check-ntfs-disk.sh` on the host |
| `soundtest info`, `tone`, `float`, `wasapi`, `both`, `play FILE`, `ding`, `msgbeep`, `beep` | Sound output (needs an HD Audio card; see below) |
| `disktest write`, restart, `disktest verify` | Drive C: surviving a reboot |
| `httptest suite HTTPS-BASE HTTP-BASE` | winhttp against `tools/h2server.js`: HTTP/2 by ALPN, a 300 KB body, POST, a redirect, an untrusted certificate refused, chunked HTTP/1.1, the asynchronous API.  `httptest [-2] [-k] [-a] URL` fetches one URL |
| `netcat [-4\|-6] [-p PORT] HOST [PATH]` | Winsock: `getaddrinfo`, IPv4 or IPv6 sockets, an HTTP/1.0 GET |

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

| Program | Checks |
|---|---|
| ripgrep 14.1.1 | `--version`, a search |
| fd 10.2.0 | `--version`, finding `*.txt` |
| jq 1.7.1 | `--version`, a filter over a JSON file |
| 7-Zip 26.03 | `7z a`, `7z t` |
| MinGit 2.51.0 | `git clone` of a bare repository, `log` (through the `less` pager), `status` |
| Python 3.14.0 (NuGet package) | `-c` with `json` and `sys` |
| Node.js 24.9.0 | `-v`, `-e` |
| Notepad++ 8.8.3 (portable) | opens a file; the screenshot must match `tests/reference/notepad++.png` (at most 3% of pixels differ) |

```bash
sudo apt install p7zip-full python3-pil     # 7-Zip's installer, Pillow
python3 tools/appcorpus.py                   # exit status = programs that failed
python3 tools/appcorpus.py --only ripgrep,jq --out /tmp/ac
python3 tools/appcorpus.py --only Notepad++ --update-reference   # after an intended change
```

A command passes as a self-test does (exit code 0, the output expected).
To add a program, add an `App` to `APPS`.  Other third-party programs (the
installers, Java, .NET) and test scripts such as `cmdtest.bat` for
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

### Sound

`soundtest` plays sine tones through each path.  To check what came out,
record it and measure it:

```bash
python3 tools/novarun.py --wav /tmp/out.wav 'C:\Programs\soundtest.exe tone 440 1000' \
    'C:\Programs\soundtest.exe wasapi 523 800'
python3 tools/wavcheck.py /tmp/out.wav     # each tone: start, length, level, pitch
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

### GDB

```bash
# Terminal 1: start QEMU with the GDB stub (waits for GDB)
./scripts/run-qemu.sh build/nova.img /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/OVMF/OVMF_VARS.fd --gdb

# Terminal 2
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) hbreak KiSystemStartup     # Hardware breakpoint at the kernel's C entry
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
