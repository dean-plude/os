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
add `-nic user,model=e1000` to test the older card.  `-smp N` sets the core
count (up to 16).

For sound add `-device intel-hda -device hda-output` (or `hda-duplex`);
`-audiodev wav,id=snd0,path=out.wav,out.frequency=48000` with
`-device hda-output,audiodev=snd0` records it instead of playing it.
`cmake --build . --target run` adds the card, playing through the host's
PulseAudio or PipeWire when it finds one; set `NOVA_AUDIO` (`pa`,
`pipewire`, `alsa`, `none`, `wav,path=out.wav`) to pick QEMU's backend.

### Where your files are kept

Drive C: lives in memory, and NovaOS saves every change to a FAT volume a
second later (and before Restart / Shut down), restoring it at the next
boot.  The files installed from the OS image (`C:\Windows\System32`,
`C:\Windows\SysWOW64`, `C:\Programs`) are never saved, so a newer image
always brings its own.  It uses the first of:

1. a FAT16/FAT32 volume labelled `NOVADATA` (on any SATA disk, including
   the data partition Install NovaOS creates);
2. an empty disk (all zeros at the start), which it formats as FAT32 `NOVADATA`;
3. the boot disk itself, under `\NOVA\C`.

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
They are read-only: programs, the Terminal and File Explorer can list,
open, copy from and run what is there, and writes fail with "the media is
write protected".  Compressed and sparse files are read; encrypted files
are not.  A file is read into memory when it is opened, so the largest one
that opens is 256 MiB.  For a test disk (needs `ntfs-3g`):

```bash
scripts/make-ntfs-disk.sh build/nova-ntfs.img build
qemu-system-x86_64 ... -drive format=raw,file=build/nova-ntfs.img
```

then run `drivetest` in the Terminal.

---

## Tests

There is no CI; tests run inside NovaOS under QEMU.

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
| `comtest` | ole32/oleaut32, `IShellLink` |
| `cppeh` | C++ exceptions and RTTI |
| `shmtest` | Named and file-backed shared memory between processes |
| `pipetest` | Pipes, inherited handles, `cmd /c`, `_popen`, overlapped I/O |
| `cliptest` | The clipboard and the OLE clipboard, across two processes |
| `smpstress` (x64) | Locks, events, semaphores and memory from many threads |
| `acltest` | Access checks against DACLs (`AccessCheck`) |
| `drivetest` | Drive D: (read-only NTFS), with the disk from `scripts/make-ntfs-disk.sh` |
| `soundtest info`, `tone`, `float`, `wasapi`, `both`, `play FILE`, `ding`, `msgbeep`, `beep` | Sound output (needs an HD Audio card; see below) |
| `disktest write`, restart, `disktest verify` | Drive C: surviving a reboot |

Interactive ones: `winhello` and `guitest` (windows, menus, dialogs,
property sheets), `droptest` (drag and drop), `cpus` (SMP speed-up), and
`hello`, `mandel`, `primes`, `wc`, `guess`.

### Real programs

Third-party programs (7-Zip, MinGit, the language runtimes, installers),
and test scripts such as `cmdtest.bat` for `cmd.exe`, are tested from a
second disk image holding them (not in this repository), driven by a QEMU harness
that types Terminal commands, clicks, drags and takes screenshots.  Copy a
program onto the data disk as above and run it from the Terminal.

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
  names) of the program called NAME; `trace off` stops it.

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
