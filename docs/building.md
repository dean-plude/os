# Building NovaOS
<!-- The regions between "BEGIN generated" and "END generated" markers are built from fragment files by tools/docgen.py: edit those files, not the regions (CONTRIBUTING.md). -->

## Prerequisites

The build host is Linux (Ubuntu/Debian here); on Windows use WSL2 or a
Linux VM.  On a Mac, see [macos.md](macos.md).

```bash
sudo apt update
sudo apt install -y \
    cmake ninja-build nasm python3 \
    clang lld llvm libc++-dev gcc-mingw-w64-x86-64 \
    mtools dosfstools xorriso \
    qemu-system-x86 ovmf acpica-tools
```

That is everything the build, `cmake --build . --target run` and the
self-tests need; it was last checked on Ubuntu 24.04 (October 2026) by
following this page from the top.

What each part is for:

| Tool | Used for |
|------|----------|
| `cmake`, `ninja-build` (or make) | The build |
| `nasm` | Kernel assembly (entry, interrupt stubs, SMP trampoline, syscall entry) |
| `libc++-dev` | libc++'s C++ headers, for HarfBuzz in `novatext.dll` (nothing of libc++ is linked; set `NOVA_LIBCXX` to use headers elsewhere) |
| `clang`, `lld` (`lld-link`) | **Required.** The Windows userland (`--target=x86_64-pc-windows-msvc` and `i686-pc-windows-msvc`), NetSurf, and the kernel and bootloader unless the alternatives below are installed |
| `llvm` (`llvm-rc`) | Compiling programs' resource scripts (icons, dialogs) |
| `python3` | `tools/build_userland.py`, `tools/build_netsurf.py` |
| `mingw-w64-common` (comes with `gcc-mingw-w64-x86-64`) | **Required.** MinGW-w64's C and Windows headers, which `msvcp140.dll` (Microsoft's STL, `userland/msvcp140/build.py`) compiles against; set `NOVA_MINGW_INCLUDE` to use headers elsewhere |
| `g++-mingw-w64-x86-64`, `g++-mingw-w64-i686` | Only for `tools/build_icu.py` (rebuilding `icu.dll`) |
| `mtools`, `dosfstools` | `nova.img` and putting files on the data disk |
| `xorriso` | `scripts/create-iso.sh` |
| `qemu-system-x86`, `ovmf` | Running NovaOS |
| `acpica-tools` (`iasl`) | Only for the core self-tests, which compile the ACPI tables in `tests/acpi/` |

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

Independent DLLs, programs and objects build concurrently, with at most
`--jobs N` (or `-j N`) compiler and linker processes running at once; the
default is the number of CPUs and `--jobs 1` builds one step at a time.  A
DLL still links after the DLLs it depends on, and the files and their order
in the image are the same whatever `N` is.  The 64-bit pass, the 32-bit
pass and NetSurf's objects run side by side on that one budget (NetSurf links
once the 64-bit import libraries it needs exist), so none of them waits for
another to finish.  A failed command's output is printed in one piece, headed
by its pass: `[x64]`, `[x86]` or `[netsurf]`.  CMake runs the script without
the flag, so the kernel build uses every core.

Environment variables:

| Variable | Effect |
|----------|--------|
| `NOVA_NO_NETSURF=1` | Leave the NetSurf browser out (the first build otherwise compiles about 800 files of it; later builds reuse them) |
| `NOVA_NO_WOW64=1` | Skip the 32-bit (x86) pass: no `SysWOW64`, no `C:\Programs\x86` |

To rebuild only the userland, for a quick check of a DLL (the second
command only loads the manifests, as CI does):

```bash
NOVA_NO_NETSURF=1 python3 tools/build_userland.py --jobs 4 /tmp/ul /tmp/ul/gen.c kernel/ke/syscall.h
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
  `entry` (`"DllMain"`), `x64_only`, `x86_only` (built for SysWOW64 alone), `ordinals` (`{"Name": 12}`, for
  DLLs programs import from by number), and `file` for a DLL whose file
  name is not `NAME.dll` (`"bthprops.cpl"`, a Control Panel item).
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
  ["usp10"]` links more DLLs, `"msstl": true` builds a `.cpp` against
  Microsoft's STL headers and `msvcp140.dll`, as Visual Studio builds a
  program (`userland/programs/stltest.cpp`), and `"selftest": true` lists
  it among the README's self-test programs.

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
- **The time zone table** (`kernel/ke/tzdata.inc`):
  `tools/gen_timezones.py` generates it from Unicode CLDR's
  `windowsZones.xml` (the Windows zone names and their cities) and the
  IANA tz database through Python's `zoneinfo` (`pip install tzdata` has
  the old zone names CLDR uses).  Run it again when a country changes its
  rules.
- **The keyboard layouts** (`userland/include/kbdlayouts.h`, read by the
  kernel and user32): `tools/gen_keyboards.py` generates them from
  xkeyboard-config (MIT/X11) through libxkbcommon
  (`sudo apt install xkb-data libxkbcommon0`).

### The ISO

```bash
scripts/create-iso.sh nova.iso build/bootx64.efi build/kernel.elf
```

The ISO is not committed (`*.iso` is in `.gitignore`).  CI builds it on
every pull request and keeps it as the run's `nova-iso` artifact, and each
push to `main` that passes CI replaces `nova.iso` on the `latest` build
(<https://github.com/dean-plude/os/releases/download/latest/nova.iso>).
A version tag makes a release with the tested ISO, its checksums and the
update files ([releasing.md](releasing.md)); the newest one is always at
<https://github.com/dean-plude/os/releases/latest/download/nova.iso>.

The ISO is El Torito UEFI, no emulation: its EFI System Partition holds
`\EFI\BOOT\BOOTX64.EFI`, `\EFI\NOVA\kernel.elf` and `\EFI\NOVA\bootlog.txt`
(1 MiB set aside for the boot log).  The same ESP is partition 2 of a GPT
behind a protective MBR, so the ISO written to a USB stick starts too.
Booted from either, NovaOS runs live and opens Install NovaOS (see the
README); the bootloader tells the installation media from an installed
disk by `bootlog.txt`, which the installer does not copy.  Started from a
stick, the kernel writes its log into `bootlog.txt` in place
(`kernel/fs/bootlog.c`): what was logged since boot when the stick
appears, then the rest at most once a second, before a restart and after
a kernel fault.  To try the stick in QEMU, with only the firmware's
framebuffer for a display as on a laptop:

```bash
cp nova.iso stick.img
truncate -s 2G stick.img
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/ovmf/OVMF.fd \
  -vga none -device ramfb -device qemu-xhci,id=xhci \
  -drive if=none,id=stick,format=raw,file=stick.img \
  -device usb-storage,bus=xhci.0,drive=stick -serial stdio
```

The devices suite's `usbboot` boot does this and reads the log back
from the stick image with mtools; `cdboot` starts from the ISO as a
disc.

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

QEMU's default user-mode network (an e1000e on q35, the 82574L) works out of the box;
add `-nic user,model=e1000` to test the older card or
`-nic user,model=virtio-net-pci` for virtio-net.  `-smp N` sets the core
count (up to 16).

USB devices need a controller: `-device qemu-xhci` (USB 3), `-device
usb-ehci` (USB 2), `-device pci-ohci` or `-device piix3-usb-uhci` (USB
1.1), or an ICH9 EHCI with UHCI companions (`ich9-usb-ehci1` plus
`ich9-usb-uhci1..3` with `masterbus=`), then e.g. `-device usb-kbd
-device usb-tablet`.  With `-machine q35,i8042=off` there is no PS/2
keyboard, so typing goes over USB.  QEMU has no game controller of its
own: pass the host's through (`-device
usb-host,vendorid=0x045e,productid=0x028e` for a wired Xbox 360
controller), or run `tools/padpeer.py --port 10710 --kind xbox360`
(or `xboxone`, `hid`) and add `-chardev
socket,id=pad,host=127.0.0.1,port=10710 -device
usb-redir,chardev=pad`; the peer takes lines such as `buttons=0x1000
lx=20000` on port 10810 and moves the controller.

For sound add `-device intel-hda -device hda-output` (or `hda-duplex` or
`hda-micro`, which add a line in or a microphone to record from);
`-audiodev wav,id=snd0,path=out.wav,out.frequency=48000` with
`-device hda-output,audiodev=snd0` records it instead of playing it.
`cmake --build . --target run` adds the card, playing through the host's
PulseAudio or PipeWire when it finds one; set `NOVA_AUDIO` (`pa`,
`pipewire`, `alsa`, `none`, `wav,path=out.wav`) to pick QEMU's backend.
USB speakers work too: `-audiodev wav,id=usbsnd,path=usb.wav
-device qemu-xhci -device usb-audio,audiodev=usbsnd` (or on `pci-ohci` or
`piix3-usb-uhci`; QEMU's `usb-audio` is full speed only, so not on a
plain `usb-ehci`).  The newest sound output and input are the defaults
(Settings' Sound page chooses others and sets each device's volume, and
programs can pick a device); the chosen default and the volumes are kept
in the registry on drive C:, so they hold after a restart.  QEMU has no USB microphone and no high-speed audio device:
`tools/usbredirpeer.py` is one (a USB Audio Class 1 headset or microphone
behind a `usb-redir` device, or with `--uac2` a USB Audio Class 2.0 one:
a programmable clock behind a clock selector, 24-bit samples and, at
high speed, a packet every microframe; `--rates`, `--channels`,
`--mic-channels` and `--product` give it other sampling rates, channel
counts and a name, and `--feedback HZ` makes its speaker asynchronous,
on its own clock, saying through a feedback endpoint that it plays HZ
frames a second), e.g. a high-speed headset on EHCI whose
microphone hears 523 Hz:

```bash
python3 tools/usbredirpeer.py --port 10700 --speaker headset.wav --mic 523 &
```

then `-chardev socket,id=ur,host=127.0.0.1,port=10700 -device
usb-ehci,id=ehci -device usb-redir,chardev=ur,bus=ehci.0` (`--speed full`
for a full-speed one on the other controllers; the speaker's sound goes
to `headset.wav`).

Laptops whose built-in microphones sit behind Intel's audio DSP (Tiger
Lake to Raptor Lake, as on the reference ThinkPad) need the DSP's Sound
Open Firmware built in; `python3 tools/fetch_sof_firmware.py` before the
build downloads it into `third_party/sof-bin`, which is not committed
([hardware.md](hardware.md#the-digital-microphones-behind-the-audio-dsp)).
QEMU has no such DSP, so nothing here needs it.

More monitors: each further display adapter is one (`-device
secondary-vga`), and so is each output of a virtio GPU with a monitor on
it, e.g. `-vga none -device virtio-vga,max_outputs=2,id=gpu`.  QEMU
connects an output when its display window or a VNC client on it
(`-vnc :1,display=gpu,head=1`) asks for a size, and a VNC client asking
for 0 x 0 disconnects it, while NovaOS runs.

A 3D GPU: with QEMU 9.2 or newer and a virglrenderer built with Venus,
`-vga none -device virtio-vga-gl,venus=on,blob=on,hostmem=1G` and an
OpenGL display (`-display sdl,gl=on` or `gtk,gl=on`) give NovaOS a
virtio-gpu whose Vulkan and OpenGL run on the host's GPU.  Install
**Venus** from the App Store (Runtimes), next to Mesa 3D and DXVK: Vulkan
programs, Direct3D ones through DXVK, and OpenGL ones then run there
(`d3dtest` prints `D3D9 adapter  Virtio-GPU Venus (...)`, `gltest`
`GL_RENDERER virgl (...)`), and without such a GPU they keep using
lavapipe and llvmpipe.  NovaOS's `opengl32.dll` picks Mesa's virgl
(`opengl32_virgl.dll`, from Venus) on that GPU and Mesa 3D's llvmpipe
(`opengl32_mesa.dll`) otherwise; `GALLIUM_DRIVER=virgl` or
`GALLIUM_DRIVER=llvmpipe` chooses one.  `tools/ci/build-qemu-venus.sh
PREFIX` builds such a QEMU (Ubuntu 24.04's has no Venus), and
`tools/build_venus.py OUT` builds the App Store's `venus.7z` (Mesa's
Venus and virgl with NovaOS's back ends, `third_party/mesa-venus`) with
MinGW-w64.

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
only until restart; its root has the same permissions as a new NTFS
volume's (Windows' for `C:\`), which folders without their own inherit.  Files from the OS image keep no descriptor across a
restart either way.

Saving runs on its own kernel thread and keeps no lock while the disk is
written: it notes what changed under the file-system lock (well under a
millisecond; a changed file's contents are lent to the save, and a program
writing to the file meanwhile gets its own copy), then writes it out while
programs, the desktop and the keyboard carry on.  On FAT, a crash or power
cut during a save leaves each file either as it was or as it was saved,
never half of each: a file's data and its cluster chain reach the disk
before its directory entry, and the clusters of a replaced or deleted file
are freed only after the entries that no longer use them are on the disk.

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

then run `drivetest` (and `linktest D:\LinkTest`, which leaves two names
of one file) in the Terminal, shut down, and check the disk on the host:

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

`tools/novarun.py` (and so `tools/selftest.py`) starts QEMU with `-accel kvm`
when `/dev/kvm` is readable and writable, and with TCG otherwise.  Set
`NOVARUN_ACCEL=tcg` or `NOVARUN_ACCEL=kvm` to force one.  The CPU model stays
`qemu64` with the same feature flags under both.  CI runs the test VMs under
KVM: each job's `tools/ci/enable-kvm.sh` step opens `/dev/kvm` to the runner
user and sets `NOVARUN_ACCEL=kvm` (history entry "Kernel under KVM" has the
timings).  The app corpus job fails at that step, with a message, when the runner has no
usable `/dev/kvm` (start the workflow by hand with the `allow_tcg` input for a
deliberate TCG run); the boot-test and graphics jobs, which are required
checks, fall back to TCG with a warning on every event so main and pull
requests never go red on the runner alone.  Every job's summary and the corpus table say which accelerator
the test VMs used.

The graphics suite downloads 7-Zip, Mesa and DXVK, builds Venus
(`tools/build_venus.py`) and gltest/d3dtest/d2dtest/dwtest/dw3test, and runs on a
QEMU with Venus (`tools/ci/build-qemu-venus.sh`; without one the first
monitor is a standard VGA and the Venus tests fail) with an OpenGL display,
so on a machine without a screen it runs under `xvfb-run`:

```bash
sudo apt install p7zip-full gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 g++-mingw-w64-x86-64 g++-mingw-w64-i686 \
  ninja-build pkg-config libglib2.0-dev libpixman-1-dev libsdl2-dev libepoxy-dev libgbm-dev libdrm-dev \
  libvulkan-dev libpng-dev glslang-tools bison flex python3-mako python3-yaml mesa-vulkan-drivers seabios ipxe-qemu xvfb
pip install --user 'meson>=1.5' pycotap
sudo install -d -o "$USER" /opt/qv
tools/ci/build-qemu-venus.sh /opt/qv
tools/ci/stage-graphics.sh /tmp/gfx
NOVARUN_QEMU=/opt/qv/bin/qemu-system-x86_64 LD_LIBRARY_PATH=/opt/qv/lib/x86_64-linux-gnu \
  xvfb-run -a -s '-screen 0 1280x1024x24' python3 tools/selftest.py --suite graphics --gfx /tmp/gfx
```

When a graphics test hangs, the guest can only say that it waits for the
host, so CI's graphics job runs `tools/ci/host_watch.py graphics-out &` beside
the suite (needs `gdb` and `sudo`) and sets `VIRGL_LOG_LEVEL=debug` and
`VIRGL_LOG_FILE=graphics-out/virglrenderer-%PID%.log`.  The `graphics-out`
artifact then holds `host-watch.log` (every 15 s: the guest's serial log size,
and the CPU time and thread count of QEMU and of virglrenderer's render server
processes), `virglrenderer-PID.log` (one per render process), and, once the
guest's serial log has been silent for six minutes, `host-hang-N.txt`: every
host thread's state, wait channel and CPU use over five seconds, and gdb's
backtrace of QEMU and of each render process, then what QEMU's monitor says
about the guest itself: every CPU's registers (with the kernel function its RIP
is in, named from `build/kernel.elf`), its local APIC timer, and the kernel's
big lock (`g_bkl`); `serial-hang-N.log` is the whole serial log so far (a job
cut off by its time limit never reaches the end of the suite, where it is
normally kept).  A QEMU whose threads all wait and whose CPU time stands still
has lost a wake-up; one that spins is busy.  Two CPUs that both sit in HLT with
their timers ticking, the serial log silent and no answer to Ctrl+Alt+F12, are
waiting for the kernel lock: the kernel then logs `[WATCHDOG] the kernel lock
has been held for ...` itself after three seconds (who holds it, what each CPU
runs) and lets go of a lock whose holder no CPU runs (`[SMP] Bug: ...`).  The
self-tests stop at the first test after which the machine answers neither
Ctrl+Alt+F12 nor Ctrl+C, instead of waiting out every later test's limit.
A program stuck in a GPU wait (the host never answered a fenced request) can be
stopped like any other: the wait checks every 50 ms.  Ctrl+Alt+F12 also logs
the virtio-gpu control queue (`[VGPU]` lines: requests taken and answered,
fenced requests still out and the timeline value each sets), and
`host-hang-N.txt` lists the render server's processes (the Venus worker of the
running test is one; none means it died), the host kernel's messages about
crashed or killed processes, and QEMU's own view of the queues
(`info virtio-queue-status`): a request the guest sent that QEMU's used index
has not caught up with was taken and never answered by virglrenderer.

`NOVARUN_QEMU` names the QEMU that `tools/novarun.py` runs (default
`qemu-system-x86_64` from `PATH`); it opens an SDL window with OpenGL
(`NOVARUN_GL_DISPLAY` to change it) when a `virtio-vga-gl` or
`virtio-gpu-gl-pci` is among its devices, and no window otherwise.

The network suite tests IPv4, IPv6 and winhttp's HTTP/2 on virtio-net,
then IPv4 again on an Intel e1000e, and needs `node` and `openssl`:

```bash
python3 tools/selftest.py --suite network
```

The devices suite boots once for each device that would get in the core
boot's way (a touch screen takes QEMU's mouse buttons from the PS/2
mouse, and USB speakers are heard instead of the HD Audio card): a
virtio multi-touch screen for `touchtest`, which QEMU's
`input-send-event` touches where the program asks; and, with no HD Audio
card, QEMU `usb-audio` speakers, each recorded to its own WAV (kept in
`--out` as `usb1.wav` to `usb3.wav`).  The first is on an xHCI controller
from boot; the test plugs the second into an OHCI and the third into a
UHCI controller while NovaOS runs, plays `soundtest tone` after each, then
unplugs the third and plays again, which the second must hear.  Each
speaker's WAV must hold its tones and nothing else: a speaker another one
took over from has to go quiet.  Then the first speaker is made the
default and set to half volume (the second stays at full), and after
`shutdown /r` it must be the default again, although both attach at
boot, and still play at half volume.  A third boot has no HD Audio card and a
high-speed USB headset on an EHCI controller (`tools/usbredirpeer.py`
behind a `usb-redir` device; its speaker writes `headset.wav`, its
microphone hears 523 Hz): `soundtest tone` must sound in `headset.wav`
alone, and `soundtest record` and `capture` must record the microphone's
tone.  The test then plugs full-speed USB microphones (more
`usbredirpeer.py`s, each hearing its own tone) into an xHCI, an OHCI and
a UHCI controller, records after each (the newest microphone must be
heard), unplugs the UHCI one and records the OHCI one again.  Then it
plugs a high-speed USB Audio 2.0 headset (`usbredirpeer.py --uac2`,
writing `uac2.wav`, its microphone hearing 988 Hz) into the xHCI
controller: `soundtest tone` must sound in `uac2.wav` alone, `soundtest
record` and `capture` must hear 988 Hz, and once it is unplugged
recording must go back to the OHCI microphone.  Then a USB Audio 2.0
surround headset whose clock offers only 44.1 kHz (`usbredirpeer.py
--rates 44100 --channels 6 --mic-channels 4`, writing `surround.wav`):
the tone must sound at its pitch in its front two channels with the
other four silent, and its microphone's 1175 Hz must be recorded at its
pitch.  Last for sound, a full-speed USB speaker (`spk.wav`) for the
device picker: with it the default, `soundtest ... dev=NAME` must play
on (or record from) the named device through `waveOut`, `waveIn` and
WASAPI, and `soundtest default out|in NAME` (what Settings' Sound page
does) must move the default.  Then each device's own volume (`soundtest
level`: a quarter on the surround headset leaves the others at full, and
its tone must sound a quarter as loud; `soundtest wovolume`: a program's
`waveOutSetVolume` on one device ID), DirectSound's device list
(`soundtest dsenum`) and playing and recording on a device named by its
GUID, XAudio2's device list and a mastering voice on a named device
(`xa2test devices`), and, after `shutdown /r`, the surround headset
still the default output (although the speaker attaches again too) and
still at a quarter.  Before the restart a 44.1 kHz tone (`soundtest tone
... rate=44100`) on the 44.1 kHz surround headset must arrive sample for
sample (nothing converted: the mixer runs at the device's rate); after
it, the default output and input must be `waveOut` and `waveIn` device
0, with `DRVM_MAPPER_PREFERRED_GET` naming device 0, and device 0 must
follow the default when it moves.  Then two asynchronous speakers
(`usbredirpeer.py --feedback`): a full-speed USB Audio 1.0 one saying it
plays 48,500 frames a second (10.14 feedback) and a high-speed USB Audio
2.0 one saying 47,600 (16.16): NovaOS must send 48.5 frames a packet to
the first and 5.95 to the second on average, not the nominal 48 and 6.
Last, one
`virtio-vga` card with three outputs and a monitor only on the first, for `montest hotplug`: the test
connects a monitor to the second and third outputs and disconnects them
again while NovaOS runs, through a VNC server QEMU has on each (an RFB
`SetDesktopSize` asks for a monitor of that size there; 0 x 0 takes it
away).  The "laptop" boot stands in for the reference ThinkPad: QEMU
without `\_S3` (`ICH9-LPC.disable_s3=1`), `tests/acpi/laptop.asl` (an
embedded controller holding the lid, a battery and the AC adapter, which
NovaOS serves with its model of one because QEMU emulates none, and an
LPS0 device), the ISO on a USB stick, an empty NVMe disk and only the
firmware's GOP.  `battery` must read the battery through the controller;
the test closes the lid (pc-testdev port `0xE8`), NovaOS must sleep in
low-power S0 idle, and opening it must wake it; then `install nvme0n1`
installs NovaOS on the NVMe disk, and after `shutdown /r` it must start
from that disk and add its firmware boot entry
([install-and-power.md](install-and-power.md)).  The "gamepad" boot has
three USB game controllers on an xHCI controller, each
`tools/padpeer.py` behind a `usb-redir` device: a wired Xbox 360
controller, an Xbox One controller and a HID game pad.  `padtest`
checks what XInput and DirectInput 8 list; at each step it prints, the
test sets a controller's buttons, triggers, sticks and hat through the
peer's control port, and `padtest` must read them through both APIs
(and DirectInput's buffered events); the motors it sets must reach the
two Xbox controllers (the peers' logs), and the Xbox 360 one is
unplugged while it runs.  The 32-bit `padtest still` then reads the two
left.  To run the suite:

```bash
python3 tools/selftest.py --suite devices
```

The core suite is `apitest`, `abitest`, `filetest`, `pipetest`, `proctest`,
`sectest`, `acltest` (x64 and x86), `guitest auto`, `disptest`, `dpitest` (per-monitor DPI), `icutest` (x64 and x86), `comtest`,
`tlbtest` (x64 and x86), `usptest` (x64 and x86), `delaytest` (x64 and x86), `cppeh`, `battery`, `soundtest tone`,
`soundtest wasapi`, `soundtest record`, `soundtest capture`, `soundtest volume`,
`sleeptest timer`, `powertest`, `disptest 1024 768` (saves the mode),
`filetest install` (an installer that must replace a running program
schedules it for the next boot), a restart that must report `Pending file
operations at boot: 2 done, 0 failed` and come up in the saved mode
(`disptest saved 1024 768`), `filetest installed`, and
`crash kernel`, which halts the kernel on purpose and passes when the
serial log shows a symbolized backtrace (`KeCrashTestFault`,
`KeCrashTest`, `sys_nova_bugcheck`, ...); the machine is then reset, and
last the next start must have moved the fault's report into
`C:\NovaOS\Crashes` (`crashes last` shows the backtrace).  The graphics suite first runs
`d2dtest`, x64 and x86: it checks geometry computations, draws a scene into
a DC render target and compares it with `d2dref.bmp`, the image
`tools/d2dtest/reference.py` draws with Skia (`pip install skia-python`;
re-run it when the scene changes), then shows the scene in a window.  Next
`dwtest`, x64 and x86, lays out Latin, Arabic and Devanagari in one line
with DirectWrite from a Latin-only font and checks the fallback fonts, the
shaping, the direction and the drawing, and shows the line in a window.
`dw3test`, x64 and x86, goes through DirectWrite's Windows 10 font model
as Chromium and Skia use it: `IDWriteFactory2` and `IDWriteFactory3`, the
system font set and face references, matching by full and PostScript
name, set builders and collections made from sets, `IDWriteFont3` and
`IDWriteFontFace3`, the system font fallback and a built one, grayscale
glyph run analysis and rendering params.  It then types `store
install Mesa 3D`, `store install DXVK` and `store install Venus` (the
archives are already in `C:\Downloads`, so the App Store installs without
a network) and then runs `gltest` (on virgl and, with
`GALLIUM_DRIVER=llvmpipe`, on llvmpipe) and `d3dtest` (on Venus:
the first monitor is a 3D virtio-gpu, `virtio-vga-gl,venus=on`, and the
test expects the Venus adapter), x64 and x86, from `C:\Tests`, taking a
screenshot of each while it draws, and `d3dtest angle`, which brings
Direct3D 11 up the way ANGLE (Chromium's GPU process) does: the screen's DC
as the EGL display (`WindowFromDC` must find the desktop window), the first
adapter that is not Microsoft's, a device from feature levels 11.1 to 9.3,
`IDXGIDevice2`, the adapter's description, factory and driver version, the
feature and format queries, a DXGI 1.2 swap chain and a WARP device.  `d3dtest shared` (on lavapipe, which its `VK_DRIVER_FILES` picks)
shares a Direct3D 11 texture by NT handle as Chromium shares its frames
with Qt WebEngine: one device clears it, a second opens the handle
(`OpenSharedResource1`) and reads the colour, the first reads the second's
write, a child process opens an inherited copy of the handle and
reads it too, and a shared fence (`ID3D11Fence`) signalled on either
device completes on the other.  `gltest fps 10` draws an OpenGL
scene that keeps the rasterizer busy (64 blended quads over a 640x480
window) for 10 s on virgl and 10 s on llvmpipe, each in a child process
whose `GALLIUM_DRIVER` names the driver, and passes when virgl draws more
frames per second (under TCG: 12.6 to 15.9 against 0.24).  Last, `d3dtest fps 10` draws a
Direct3D 9 scene that keeps the rasterizer busy (64 blended quads over a
640x480 window) for 10 s on Venus and 10 s on lavapipe, each in a child
process whose `VK_DRIVER_FILES` names the driver, and passes when Venus
draws more frames per second (under TCG: 14 to 17 against about 0.13).  The
graphics boot has a second monitor (a QEMU
`secondary-vga`): between the installs and `gltest` it runs `montest 2`,
which checks the monitor calls and layout changes; when it asks, the test
pushes the pointer across onto the second monitor, and the screenshot is
one PNG per monitor (`montest.png`, `montest-2.png`).  Then `nstest`
starts NetSurf on a page with an SVG image, an inline `<svg>`, a list a
script builds, an SVG without a size and an iframe holding a frameset
page: the test checks the SVGs' colours and sizes (the unsized one at the
default 300 x 150), the list items and both frames on the screen, clicks the page's box (a script changes it) and
checks the page was redrawn with it changed and the rest still there (`nstest-before.png`,
`nstest-after.png`), then closes NetSurf with Alt+F4.  `nstest` then
opens four pages a script changes twelve times (a long page, iframes,
positioned boxes, floats), each twice: laid out from the changed box, and
with `NETSURF_LAYOUT_CHECK=1` comparing every third such layout box by box
with a full one; the test compares the two runs' screenshots
(`nstest-PAGE-incremental.png`, `nstest-PAGE-check.png`) above NetSurf's
status bar.  Then `store scroll
bar` runs `store open` and checks, from the `[STORE] view:` lines the App
Store logs when its view changes, that All apps has a vertical scroll bar
and that the wheel, Home, Page Down, End, a click on the bar's down arrow,
one in its trough and a drag of its thumb each scroll the list, then
closes the Store with Esc.  Last, `explorer
scroll bars` opens File Explorer on `C:\Windows\System32` (more files than
fit) and checks, from the `[EXPLORER]` lines it logs when its view
changes, that the list has a vertical scroll bar and that the wheel, Page
Down, End, Home, a click on the bar's down arrow and one in its trough
each scroll the list.  The network suite (`tests/selftest/network4` and
`network6`) boots twice with a virtio-net
card: on QEMU's user network it runs `ipconfig`, `ping 10.0.2.2`, `netcat`
(Winsock over IPv4) and `httptest suite` (winhttp: HTTP/2 by ALPN, large
bodies, POST, redirects, certificate checks, chunked HTTP/1.1, the
asynchronous API) against `tools/h2server.js` with a throwaway self-signed
certificate, then `looptest` (socket pairs over 127.0.0.1 and ::1,
`localhost`), `loadtest` (hundreds of sockets open at once, parallel
downloads) and `dltest -w` (eight 32 MB downloads at once from
`tools/h2server.js`'s `/stream` while the files are written and mapped,
as an installer does; it fails if the transfer stops for 5 s); on an IPv6-only network made by `tools/v6peer.py` it checks
SLAAC and RDNSS (`ipconfig`), `ping -6`, `curl -6` and `netcat` over IPv6.
A third boot (`tests/selftest/network-e1000e`) has QEMU's e1000e (the
82574L) instead of virtio-net: the boot log must show the PHY's ID, its
auto-negotiation and the link up at 1000 Mb/s; the test pulls the link
and plugs it back with QEMU's `set_link` (`ipconfig` shows the media
disconnected, then the address again), runs `ping`, sleeps and wakes the
machine with `sleeptest` and `ping`s again, then runs the IPv4 tests
above on it.  [ethernet.md](ethernet.md) says what this covers of the
I219 that real PCs have.

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
is at 130; `crash kernel`, which halts NovaOS, is at 900 with `restart=True`,
which resets the machine, and only tests that check what the crash left
come after it, at 905).

```python
# tests/selftest/core/140-mytest.py
DOC = '`mytest` (what it covers)'      # its entry in README's core-suite list
TESTS = [
    Test('mytest', 'mytest', [r'mytest: \d+ passed, 0 failed']),
]
```

`python3 tools/selftest.py --list` prints the suite.  The nightly app
corpus works the same way: one file per program in `tests/appcorpus/`
defining `APP = App(...)` and `DOC` (the windowed programs, `gui=True`,
stay last, from 850).

### Self-test programs

In `userland/programs/`, installed in `C:\Programs` and, as 32-bit builds,
in `C:\Programs\x86`.  Type the name in the Terminal; each prints "N passed,
0 failed".  Each row of this table is a file in `docs/selftests/` (a new
program adds one; see [CONTRIBUTING.md](../CONTRIBUTING.md)).

<!-- BEGIN generated:selftest-table -->

| Program | Covers |
|---|---|
| `crttest` | The C runtime |
| `filetest` | Files and directories; delete on close (`FILE_FLAG_DELETE_ON_CLOSE`, `FileDispositionInfo`, `DeleteFile` of an open file: deleted when the last handle closes, meanwhile `ERROR_ACCESS_DENIED`; a running program's file is refused); `RegNotifyChangeKeyValue` (values, subkeys, subtrees, deleted keys, synchronous); `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)`.  `filetest install`, a restart and `filetest installed` check that a running program replaced at boot |
| `linktest` | Hard links: `CreateHardLink`, the link count and file id by every name, a write or attribute change by one name seen by the others (open handles too), a link in another folder renamed there, deleting names down to the last, a file replaced under a linked name, `FILE_SUPPORTS_HARD_LINKS`.  `linktest restarted` after the restart checks a linked pair is still one file |
| `sectest` (x64) | Hostile system calls refused (kernel pointers, bogus handles, bad descriptors); tokens and object security through the native API: a restricted or deny-only impersonation token is refused a protected named event |
| `threads` | Threads, synchronization, SEH |
| `dlltest` | DLL loading, TLS, `DllMain`; a DLL whose `DllMain` returns FALSE stops the program at start with 0xC0000142 |
| `posixtest` | The POSIX layer in msvcrt |
| `apitest` | kernel32, advapi32, bcrypt, shell32, shlwapi, psapi, user32/gdi32, the registry |
| `abitest` (x64) | The binary interface against Windows 10 1903 x64: TEB, PEB, process parameters, loader lists, `KUSER_SHARED_DATA`, `CONTEXT` and `EXCEPTION_RECORD` offsets, ntdll's stub bytes and every system-call number (`nt1903_services.h`), raw `syscall`s |
| `syscalltest` (x64) | The system-call table as code that calls the kernel without ntdll sees it (Roblox's Hyperion): ntdll's `Zw` exports ranked by address give each service its Windows 10 1903 number, and the kernel answers that number (and `STATUS_INVALID_SYSTEM_SERVICE`, not the wrong service, for one it lacks) |
| `comtest` | ole32/oleaut32, `IShellLink` |
| `tlbtest` | COM type libraries: `LoadTypeLib` on `testdll.dll`'s embedded library, `ITypeLib`/`ITypeInfo`/`ITypeComp`, registration, `ITypeInfo::Invoke`, `DispCallFunc`, `CreateStdDispatch` |
| `cppeh` | C++ exceptions and RTTI |
| `unwindtest` (x64) | `RtlUnwindEx` as MinGW's C++ runtimes (LLVM's libunwind, GCC's libgcc) use it: a throw caught two frames up through a collided unwind to the landing pad with an unfilled CONTEXT (no trap flag, the frame's registers, RAX and RDX for the landing pad), a cleanup frame in between, and `_Unwind_Resume`'s unwind from no handler; and as Microsoft's own `vcruntime140` uses it: a catch block run by a consolidating unwind's callback (`STATUS_UNWIND_CONSOLIDATE`), and an exception raised inside that catch block reaching the catching frame's handler and then the caller's `__except`; an `__except` inside a `__try`/`__finally` of the same frame (the `__finally` runs once, after, also when the `__except` body raises again, as Microsoft's C++ runtime re-raises a rethrow) |
| `stltest` | The C++ standard library (`msvcp140.dll`, `msvcp140_1`, `msvcp140_atomic_wait`) built as Visual Studio builds a program: strings, containers, streams and locales, exceptions and `exception_ptr`, threads, mutexes, condition variables, `std::async`, atomic waits, `pmr`, `std::filesystem` and `fstream`, `to_chars`, `std::format`, `std::regex` |
| `rttest` | The UCRT's C99 complex functions with MSVC's `_Dcomplex`/`_Fcomplex` (as NumPy calls them), `_cprintf`/`_cputs`, and the DLL search directories: `AddDllDirectory`, `RemoveDllDirectory`, `SetDllDirectory` |
| `crtthreads` | The per-thread pieces beyond `errno`: `FlsAlloc` callbacks run once on each value when its thread ends (on that thread), on every live thread's value at `FlsFree`, and on a fiber's own values at `DeleteFiber`; a new fiber starts with no values; FLS slots leave the TLS slots alone; `_configthreadlocale` gives a thread its own `setlocale` names in `msvcrt.dll` and `ucrtbase.dll` while the process's change underneath; `getenv`/`_wgetenv` results stay whole while another thread rewrites the variable, and values past 512 bytes come back (64- and 32-bit) |
| `errnotest` | The C runtime's per-thread data in `msvcrt.dll` and `ucrtbase.dll`: six threads fail in different ways (`fopen` of a missing file, an overflowing `strtol`, `_set_errno`) and each still reads its own `errno` and `_doserrno` afterwards, along with its own `strtok` position, `gmtime` buffer and `rand` seed; new threads start with `errno` 0; reading `errno` leaves `GetLastError` alone (64- and 32-bit) |
| `smftest` | The C++17 special math functions of `<cmath>` (`msvcp140_2.dll`: the STL's `special_math.cpp` over Boost.Math), 64- and 32-bit: Bessel and Neumann functions, elliptic integrals, `beta`, `expint`, `riemann_zeta`, Legendre, Laguerre and Hermite polynomials, their `f` and `l` forms, against known values, and `EDOM` with NaN outside the domain |
| `tlsslots` | `TlsAlloc` hands out 1088 indexes (the TEB's 64 and 1024 expansion slots); four threads start at zero in all of them and keep their own values; a freed and reallocated index reads zero in every thread, a TEB slot and an expansion slot alike; past 1087 is invalid.  A child copy that returns from `main`, and one that calls `ExitProcess`, runs its main thread's `FlsAlloc` callbacks once each before `DLL_PROCESS_DETACH`, and a still-running thread's value when the detach frees the index (64- and 32-bit) |
| `usptest` | Uniscribe on HarfBuzz: Arabic and Devanagari itemized, shaped (contextual forms, ligatures, reordering) and placed with the Noto fonts, and GDI `ExtTextOut` drawing complex text exactly as `ScriptStringOut` does; `usptest bmp FILE` saves sample lines as a bitmap |
| `dlgtest` | The common file dialogs without showing them: `GetOpenFileName`/`GetSaveFileName` argument checks, the `IFileOpenDialog`/`IFileSaveDialog` objects' options, folders, file types, file name and events; `dlgtest open`, `multi`, `save`, `ifd`, `ifdsave` and `folder` show each dialog for a look |
| `wndthreads` | A thread makes a child window (and a grandchild) of the program's window: a message sent to it from the main thread is answered; `DestroyWindow` on the parent returns when the child's thread ends on the child's `WM_DESTROY`, and when the child's thread had already ended, and both windows are gone after it; a message sent to the window of an ended thread returns 0 at once.  A watchdog ends the program if a step hangs (64- and 32-bit) |
| `delaytest` | the DLLs Firefox delay-loads: urlmon (`CreateUri`, `CoInternetParseUrl`), winspool.drv, credui, dhcpcsvc, d3dcompiler_47, d3d11 |
| `qttest` | What Qt programs (KeePassXC) need: `GetGlyphOutline` metrics, gray bitmaps and outlines; `HSTRING`s built in Windows' layout by the caller, as C++/WinRT does; `Windows.Security.Credentials.KeyCredentialManager` activating with Windows Hello not supported; `SetSecurityInfo` on `GetCurrentProcess()` |
| `shmtest` | Named and file-backed shared memory between processes; a reserved section committed through its own view and mapped into a suspended child near ntdll (`MapViewOfFile3`, `NtMapViewOfSection` with a process handle), as Firefox's launcher does |
| `pipetest` | Pipes, inherited handles, `cmd /c`, `_popen`, overlapped I/O |
| `proctest` | `CreateProcess` flags: `CREATE_SUSPENDED`, `CREATE_NEW_CONSOLE` (`GetConsoleProcessList`), file positions shared with children and duplicates |
| `cliptest` | The clipboard and the OLE clipboard, across two processes |
| `disptest` | Display modes: `EnumDisplaySettings`, `ChangeDisplaySettings`, `WM_DISPLAYCHANGE`, a window that 800x600 shrinks growing back to its size and place, `CDS_UPDATEREGISTRY` saving the mode in the registry, and a child in `CDS_FULLSCREEN` that, as SDL goes full screen, takes its window's DC before the switch and blits a frame through it after (the mode ends with the child).  `disptest W H` switches and saves; `disptest saved W H` checks the mode after a restart |
| `cursortest` | System pointers: every `IDC_*` cursor loads with its own image and hot spot, the desktop draws each one over a window (the busy ring turning), `SetSystemCursor` replaces the I-beam for every program and `SPI_SETCURSORS` puts it back, and a program's 32 x 32 cursor is sent at the display's scale.  `cursortest show N` keeps a window of cells, one pointer each, up for N seconds |
| `montest` | More than one monitor: `EnumDisplayMonitors`, `GetMonitorInfo`, `MonitorFromPoint`/`Rect`/`Window`, `EnumDisplayDevices`, `EnumDisplaySettings` and `ChangeDisplaySettingsEx` for `\\.\DISPLAY2` (moving it with `DM_POSITION`, saved in the registry), `SM_*VIRTUALSCREEN`, a window maximized on the second monitor, and the pointer crossing onto it.  `montest hotplug` (the devices suite's "monitors" boot) has the test plug monitors into a virtio-vga's second and third outputs and unplug them: the monitor calls follow, `WM_DISPLAYCHANGE` comes each time, and a window and the pointer on a monitor that goes move to one that is left.  `montest list` prints the monitors |
| `bmpcurtest` | Pointers made from a program's bitmaps, as GTK makes them: DIB sections of 1, 4, 8 and 16 bits per pixel (`GetObject`, drawing on them, the program's bits, `Get`/`SetDIBColorTable`), a cursor from a 32-bit image with alpha and a 1-bit mask, `CreateCursor`'s AND and XOR planes and a monochrome mask, and the copy set as the class cursor that the desktop then shows.  `bmpcurtest show N` keeps a window with that pointer up for N seconds |
| `dpitest` | Per-monitor DPI.  Per-monitor aware (v2) by its manifest, it sets the primary monitor to 192 DPI for DPI-aware programs (`NtNovaGuiCtl` `CTL_SET_DPI`, what Settings > Display does) and back to 96, and checks `WM_DPICHANGED` (`wParam` and the suggested rectangle), `GetDpiForMonitor`, `GetDpiForWindow`, `GetDpiForSystem`, `AdjustWindowRectExForDpi`, `GetSystemMetricsForDpi`, the awareness calls, and its window's and monitor's rectangles in physical pixels; two children started with `__COMPAT_LAYER` (`DpiUnaware`, `HighDpiAware`) check that an unaware process keeps 96 DPI and logical pixels and a system-aware one sees 192 DPI everywhere.  At 192 DPI it measures user32's own parts (list box items, combo box field, scroll bar, menu bar, dialog units and dialog size) in a window of its own thread against one made by a thread in the unaware context, checks `GetSystemMetrics`, `DEFAULT_GUI_FONT` and `SPI_GETNONCLIENTMETRICS` at the system DPI, the `DPI_AWARENESS_CONTEXT_*` pseudo-handles and that a window procedure runs with its window's context; the children do the same with a thread of the other awareness.  It also measures comctl32's controls in both windows (header height, tree view item height and indent, toolbar padding, status bar, tab and list view row heights), checks that a window with default-font controls and a per-monitor v2 dialog made at 96 DPI rescale when the monitor goes to 192 and back (list box, combo box and edit line heights, a tree view's items, the dialog's control places and font), and that `GetWindowRect`, `GetClientRect`, `ClientToScreen`, `ScreenToClient`, `MapWindowPoints`, `SetWindowPos` and `WindowFromPoint` convert between the per-monitor-aware thread and the unaware window, and the other way from an unaware context.  With a second monitor at 96 DPI it also moves its window there and back (`WM_DPICHANGED` each way) |
| `nstest` | The NetSurf browser (graphics suite): it writes a page with an SVG image, an `<svg>` element written inline in the HTML, a list a script builds, an SVG image without a width or height, an iframe showing a frameset page and a box a script changes, and starts NetSurf on it; the test sees the SVG's filled and outlined shapes, the inline SVG's shapes at the size its `width`, `height` and `viewBox` give, the three list items, the unsized SVG at the default 300 x 150 CSS pixels and both frames on the screen, clicks the box (its script changes its style attribute and adds text after the page was laid out) and sees the page redrawn with the changed box and the images, list and frames still there (only the body's boxes were built again and the iframe kept its window), then closes NetSurf, which must exit normally; then four pages a script changes twelve times (a long page of 1,500 paragraphs, iframes, positioned boxes, floats) are each shown twice, laid out from the changed box and with NetSurf checking every third such layout box by box against a full one: no check may find a difference, the two runs' screenshots must be identical above the status bar, and on the long page the layout from the changed box must take less than half the full layout's time |
| `samplertest` (x64) | a sampling profiler's view of a thread waiting in a system call (Chromium's GPU process runs one): `GetThreadContext` of the suspended thread gives R12-R15 as it waits with them and Rbp as its frame function's frame pointer, and `RtlLookupFunctionEntry` and `RtlVirtualUnwind` unwind it from there through the stub and the Rbp frame to its caller without faulting |
| `icutest` | The system ICU (`icu.dll`) as .NET loads it: German and Japanese names, numbers, currencies, dates, the Japanese calendar, collation, case, time-zone ids, IDNA, normalization, 8 threads at once; then kernel32's `GetLocaleInfoEx`, LCIDs and locale enumeration for those locales |
| `nlstest` | `GetDateFormat`, `GetTimeFormat`, `GetNumberFormat` and `GetCurrencyFormat` (A, W, Ex) in German, Japanese and English against what Windows prints: default formats, pictures, `NUMBERFMT`/`CURRENCYFMT`, flags, rounding and errors.  `nlstest user` sets the user locale with `intl.exe` and checks that new processes format that way; `nlstest set NAME` and `after-restart NAME` check it lasts across a restart |
| `nlstest calendars` | The locales' calendars: `LOCALE_ICALENDARTYPE`, `GetCalendarInfo` (A, W, Ex), `EnumCalendarInfo`, Japanese eras, Buddhist, Taiwan, Tangun, Hebrew, Hijri, Um Al Qura and Persian dates, `DATE_USE_ALT_CALENDAR`, `EnumDateFormats` and `EnumTimeFormats`; then `GetDurationFormat`.  `nlstest override` sets the user's overrides with `SetLocaleInfo` and checks them here and in a new process |
| `battery` | AC power and batteries (`GetSystemPowerStatus`, `SystemBatteryState`); CI expects the battery in `tests/acpi/battery.asl` |
| `boosttest` (x64 and x86) | NT's wake-up priority boosts: how soon a thread waiting on an event, a semaphore, a condition variable or a thread message runs once another thread wakes it, idle and with a busy thread of the same base priority on every CPU; passes when the 95th percentile under load is 2 ms or less, the woken thread's priority (`NtQueryInformationThread`) is its base plus the increment (+1, +2 for a message; +2 more as the Terminal's console program, the foreground process; one level may have decayed already), it is back at its base after 200 ms of running, and the busy threads ran meanwhile |
| `prioritytest` (x64 and x86) | `SetThreadPriority` and `SetPriorityClass`: for each class from IDLE to HIGH and each level from `THREAD_PRIORITY_IDLE` to `TIME_CRITICAL`, the class and level read back and the thread's actual priority (`NtQueryInformationThread`) is NT's (class base plus level, within 1-15); `REALTIME_PRIORITY_CLASS` without the privilege is HIGH; the boost switches read back.  Then, with wake-up boosts off and a busy NORMAL thread on every CPU, an event-woken `THREAD_PRIORITY_HIGHEST` thread must run within 2 ms, and sooner than a NORMAL one, which waits for a busy thread's time slice to end.  Run from the Terminal, it must be the foreground process (`ProcessPriorityClass`) without a window of its own.  Time slices: with twice as many busy NORMAL threads as CPUs timing themselves, its threads must run at least 40 ms at a time and at least twice as long as a background copy's (60 ms against 20 ms).  Then the foreground boost, as the Terminal's console program and again with its own window active: `ProcessPriorityClass` must say Foreground, and with a background copy of itself running a busy `THREAD_PRIORITY_HIGHEST` thread on every CPU, its own event-woken NORMAL thread (8 + 1 + 2) must run within 3 ms while the background copy's (8 + 1) waits at least 100 ms.  `prioritytest net` (network suite): with a busy `HIGH_PRIORITY_CLASS` thread on every CPU, 40 one-byte round trips over a 127.0.0.1 TCP connection must each take at most 250 ms (the network thread runs above programs) |
| `mmcsstest` (x64 and x86) | The Multimedia Class Scheduler (`avrt.dll`): a TIME_CRITICAL thread registered for "Pro Audio" runs at 18 (`NtQueryInformationThread`), at 16 after `AvSetMmThreadPriority(AVRT_PRIORITY_LOW)`, at 18 again at CRITICAL, and at 15 after `AvRevertMmThreadCharacteristics`; "Games" runs at 16; an unknown task fails with `ERROR_INVALID_TASK_NAME`; no privilege is needed.  Then, with a busy TIME_CRITICAL thread on every CPU (as high as wake-up boosts lift a foreground program's window threads), an "Audio" thread woken by an event every 5 ms must run within 2 ms (95th percentile), and sooner than a plain TIME_CRITICAL thread, which waits for a busy thread's time slice.  Then a "Pro Audio" thread spins on every CPU for a second: a NORMAL thread sleeping 1 ms at a time must never wait more than 250 ms (the 80% budget; without it, the whole second) |
| `sleeptest timer` | How late `Sleep(1)`, `Sleep(5)`, a 1 ms wait timeout, a 1 ms waitable timer, a 5 ms periodic one, a 1 ms timer's completion routine and a 1 ms timer queue timer end, idle and with a busy thread on every CPU; passes when the 95th percentile under load is 1 ms or less and none ends early.  Also reports how soon a thread waiting on an event runs once another sets it.  Plain `sleeptest` sleeps (S3) instead |
| `savetest` | Writes 32 MiB to `C:\Temp` (or `savetest MIB`), then for 12 seconds, while NovaOS saves drive C: to its disk, times `GetProcessHandleCount` (the big kernel lock), `GetMonitorInfo` (the desktop lock) and `GetFileAttributes` (the file-system lock) on three threads; passes when none waited 250 ms and `GetFileAttributes` under 100 ms (the desktop's redraws take the file-system lock only around what they read from files).  The self-test also checks the kernel's `[PERSIST] Saved` line: the save held the file-system lock under 20 ms |
| `powertest` | The lid and a thermal zone (`GetPwrCapabilities`, `ThermalInformation`, `LastSleepTime`/`LastWakeTime`): closing the lid sleeps; needs `tests/acpi/lid-thermal.asl` and the self-test's help (see above) |
| `guitest auto` | user32 and comctl32: menus, accelerators, edit and list boxes, a resource dialog, a message box, a property sheet |
| `inputtest` | Mouse side buttons (`WM_XBUTTONDOWN`/`UP`, `WM_APPCOMMAND` back and forward), the horizontal wheel (`WM_MOUSEHWHEEL`) and the volume keys (`VK_VOLUME_*`, `WM_APPCOMMAND`): a USB mouse plugged in for the test, the PS/2 mouse and the USB keyboard, driven by the self-test (see above); `WM_ACTIVATEAPP` before `WM_NCACTIVATE` when the program's window comes up over another program's; and `GetKeyState` reading 0xFF80 for a key that is down |
| `touchtest` | Multi-touch: `WM_TOUCH` with `GetTouchInputInfo` in a `RegisterTouchWindow` window, `WM_POINTERDOWN`/`UP`, `GetPointerInfo`, `GetPointerType`, `GetPointerFrameTouchInfo` and the mouse messages `DefWindowProc` makes of them in another (with the touch signature `0xFF515780` in `GetMessageExtraInfo`), `SM_DIGITIZER`, `GetPointerDevices` listing the screen as `POINTER_DEVICE_TYPE_TOUCH`; needs the devices suite's virtio multi-touch screen and its help (see above) |
| `wintabtest` | Pen tablets through `wintab32.dll`, loaded as GTK loads it: no tablet without a pen; with a synthetic pen (`CreateSyntheticPointerDevice`), one device with a pen and an eraser cursor, a context opened as GTK opens it getting `WT_PROXIMITY`, `WT_PACKET` (position, pressure, relative button changes) and `WT_CSRCHANGE`, the pen moving the pointer and clicking, and an absolute-mode context read with `WTQueuePacketsEx`, `WTDataPeek` and `WTPacketsGet` |
| `pentest` | A pen as Windows 8+ programs see it, without Wintab: a synthetic pen over two windows gets `WM_POINTERENTER`, `WM_POINTERUPDATE` while hovering, `WM_POINTERDOWN` / `UP` for the tip and `WM_POINTERLEAVE` when it moves on or goes out of range; `GetPointerType` says `PT_PEN` and `GetPointerPenInfo` carries pressure, tilt, rotation, the barrel button and the eraser; a window that handles the messages gets no mouse messages, one that leaves them to `DefWindowProc` gets `WM_LBUTTONDOWN` / `UP` with the pen signature (`0xFF515700`) in `GetMessageExtraInfo`; touching an inactive window sends `WM_POINTERACTIVATE` first; over a window's own title bar the pen gets `WM_NCPOINTERUPDATE` / `DOWN` / `UP` with `HTCAPTION` (`DefWindowProc`: `WM_NCLBUTTONDOWN` / `UP`) and `PA_NOACTIVATE` keeps the active window; `GetPointerDevices` lists the pen (and a touch screen exactly when there is one); with `EnableMouseInPointer` the test moves the mouse over two windows and away, and each gets `WM_POINTERENTER` / `LEAVE` for pointer 1 (`PT_MOUSE`) |
| `rawtest` | The mouse and the keyboard through Raw Input: `GetRawInputDeviceList` lists one mouse and one keyboard, their names (mouse and keyboard interface paths, W and A), `RIDI_DEVICEINFO`; `RegisterRawInputDevices` with `RIDEV_DEVNOTIFY` (the mouse's arrival) and `RIDEV_NOLEGACY` (no key messages); as the test moves the PS/2 mouse 30 right and 20 down, clicks, turns the wheel and right-clicks, `WM_INPUT` read with `GetRawInputData` carries relative motion, `RI_MOUSE_*` button flags in order and 120 for the wheel's notch; A, Right and Alt+X come through `GetRawInputBuffer` as `RAWKEYBOARD` blocks (make codes, `RI_KEY_BREAK`, `RI_KEY_E0`, virtual keys, `WM_SYSKEYDOWN` with Alt); `RIDEV_REMOVE`.  64- and 32-bit |
| `padtest` | Game controllers (the devices suite's "gamepad" boot and its help): XInput (`XInputGetState`, `XInputGetStateEx` with the guide button, `XInputGetCapabilities`/`Ex`, `XInputGetKeystroke`, `XInputSetState` reaching the motors, `XInputGetBatteryInformation`, a controller unplugged), DirectInput 8 (`EnumDevices` for game controllers, keyboards and all, `CoCreateInstance` + `Initialize`, `DIPROP_GUIDANDPATH`, `VIDPID`, `RANGE`, `BUFFERSIZE`, `EnumObjects`, `GetDeviceState` with `DIJOYSTATE`, buffered `GetDeviceData`, `DIERR_INPUTLOST`) on a wired Xbox 360, an Xbox One and a HID game pad.  `padtest still` (x86) reads the state the test left; `padtest list` prints both APIs' controllers |
| `rawpadtest` | Raw Input and `hid.dll` on game pads (the devices suite's "gamepad" boot, after `padtest`): `GetRawInputDeviceList`, `GetRawInputDeviceInfo` (`RIDI_DEVICENAME`, `RIDI_DEVICEINFO`, `RIDI_PREPARSEDDATA`), `RegisterRawInputDevices` with `RIDEV_DEVNOTIFY`, `WM_INPUT_DEVICE_CHANGE` arrivals and a removal, `WM_INPUT` and `GetRawInputData`, `GetRawInputBuffer`; `HidP_GetCaps`, button and value caps, link collections, `HidP_GetUsages`, `GetUsageValue`, `GetScaledUsageValue`, `GetData`, building reports with `InitializeReportForID`, `SetUsages`, `SetUsageValue`, and the `HIDP_STATUS_*` errors; setupapi's and `CM_Get_Device_Interface_List`'s HID interfaces; `CreateFile` on a HID path, an overlapped `ReadFile` finishing with the next report, `HidD_GetAttributes`, `GetProductString`, `GetPreparsedData`, `GetInputReport`, on an Xbox One controller and a HID game pad.  `rawpadtest still` (x86) checks the HID game pad left |
| `smpstress` (x64) | Locks, events, semaphores, memory, handles, starting processes and random bytes (high-, normal- and low-priority threads on the one entropy pool) from many threads, then file and registry throughput on one CPU and on all (`smpstress scaling 3` fails below 3x; `smpstress throughput [X [files\|registry [many\|N]]]` measures only; run with `tools/novarun.py --smp 4`) |
| `acltest` | Access checks against DACLs (`AccessCheck`) for our token and restricted, write-restricted and deny-only ones; `CheckTokenMembership`, impersonation; a named event with a DACL refused to a restricted token; file ACLs on drive C:: denied writes, deletes and renames (and reads for a restricted token), inheritance, `CreateFile` with a descriptor; `SetEntriesInAcl` (GRANT, SET, DENY, REVOKE, trustees by SID and by name, canonical order), `GetExplicitEntriesFromAcl`, `LookupAccountName`, SDDL both ways, and a WiX Burn style package cache: a root secured with `SetEntriesInAcl`, a folder made in it as the elevated (linked) token and reset to inherit with `SetNamedSecurityInfo`; drive C:'s root DACL (Windows' for `C:\`) and its inheritance, and an entry added to a folder's inherited DACL as Chromium's setup does (`SetEntriesInAcl` + `SetNamedSecurityInfo`), after which folders can still be made in it.  It leaves `C:\AclTest\kept.txt` and, run again after a restart, checks it kept its DACL (C: on NTFS) |
| `drivetest` | Drive D: (NTFS: reading, then writing, renaming, deleting), with the disk from `scripts/make-ntfs-disk.sh`; then `scripts/check-ntfs-disk.sh` on the host |
| `soundtest info` (the device IDs, device 0 the default, and the preferred IDs), `tone` (`rate=N` for another rate than 22.05 kHz), `float`, `wasapi`, `both`, `play FILE`, `ding`, `msgbeep`, `beep` | Sound output (needs an HD Audio card; see below) |
| `soundtest record FILE [MS]`, `mme FILE [MS] [STALL]`, `capture FILE [MS]`, `volume` | Recording through `waveIn` (`mme`: as PortAudio's MME host, which Audacity uses, records, its thread held up STALL ms a second; an input overflow fails) and WASAPI capture into a WAV, and `IAudioEndpointVolume` (needs a card with an input) |
| `soundtest dsound [HZ] [MS]`, `dscapture FILE [MS]` | DirectSound: a streaming buffer refilled at position notifications, a static one at twice its frequency, and DirectSoundCapture (made with `CoCreateInstance`) recording into a WAV |
| `soundtest endpoints`, `default out\|in NAME`, `level out\|in [LEVEL]`, `wovolume`, `dsenum` (and `dev=NAME` on the other commands) | The sound devices: the WASAPI endpoints and the default, choosing the default as Settings' Sound page does, each device's own endpoint volume, a program's `waveOutSetVolume` on one device ID, and DirectSound's device list; `dev=NAME` plays on or records from a named device through `waveOut`, `waveIn`, WASAPI and DirectSound |
| `xa2test [HZ] [MS]`, `xa2test devices [NAME HZ MS]` | XAudio2 2.9 (source and mastering voices, voice and engine callbacks, end of stream, a volume meter in an effect chain), XAudio2 2.7 made with `CoCreateInstance` (a looped buffer through a submix voice), and X3DAudio panning; `devices` lists XAudio2 2.7's devices and plays through 2.9 on the one named |
| `miditest [DIR]` | MIDI: the `midiOut` device and a GM reset sent with `midiOutLongMsg`, then a flute note; a `midiStream` buffer with a tempo and time division, `MOM_POSITIONCB` and `MOM_DONE` callbacks and the stream position; and a MIDI file written to DIR and played through the MCI sequencer (`open`, `status length`, `play wait`, `status mode`) |
| `disktest write`, restart, `disktest verify` | Drive C: surviving a reboot |
| `httptest suite HTTPS-BASE HTTP-BASE` | winhttp against `tools/h2server.js`: HTTP/2 by ALPN, a 300 KB body, POST, a redirect, an untrusted certificate refused, chunked HTTP/1.1, the asynchronous API.  `httptest [-2] [-k] [-a] URL` fetches one URL |
| `netcat [-4\|-6] [-p PORT] HOST [PATH]` | Winsock: `getaddrinfo`, IPv4 or IPv6 sockets, an HTTP/1.0 GET |
| `looptest` | Winsock over the loopback interface: a socket pair over 127.0.0.1 and ::1 (port 0, `getsockname`, a non-blocking connect, `getpeername` after it, data sent before `accept`, the accepted socket inheriting non-blocking mode), `shutdown(SD_BOTH)` with unread data, closing a listener with a queued connection, `localhost`, `wsock32.dll`'s Winsock 1.1 ordinals, and socket options: `SO_RCVBUF`, `SO_SNDBUF`, `SO_REUSEADDR`, `SO_KEEPALIVE`, `SO_BROADCAST`, `TCP_NODELAY`, `SO_RCVTIMEO`, `SO_SNDTIMEO`, `SO_LINGER`/`SO_DONTLINGER` and `IP_TTL` read back what was set, `SO_TYPE`/`SO_ERROR`/`SO_ACCEPTCONN` answer, and five change behaviour: `TCP_NODELAY` sends the second of two small writes at once (under 30 ms, against Nagle's wait for the delayed ACK), `SO_RCVTIMEO` and `SO_SNDTIMEO` of 300 ms end a blocked `recv` and `send` with `WSAETIMEDOUT`, `SO_LINGER {1, 0}` resets the connection on close, and `SO_REUSEADDR` lets two sockets share a port; `connect` on a UDP socket (`send` and `recv` use the peer, `getsockname` names 127.0.0.1, a stranger's datagram is dropped), and `WSADuplicateSocket` into a child process that makes the socket with `WSASocket(FROM_PROTOCOL_INFO)` and sends on it after the parent closed its copy |
| `dltest [-n N] [-t S] [-w] HOST PORT BYTES` | A long download over N connections at once (up to 16), read through `select()` as libcurl's multi interface reads them, from `tools/h2server.js`'s `/stream/BYTES/SEED`; every byte is checked against the pattern.  `-w` also does what an installer does meanwhile: writes each download to a file in `C:\Temp\dltest` while two threads map files over and over.  It fails if the transfer as a whole makes no progress for S seconds (default 5), and prints the throughput and the longest pause |
| `overlaptest` | Overlapped sockets on an I/O completion port over 127.0.0.1, as Python's asyncio uses them: `AcceptEx`, `ConnectEx` and the other extension functions from `WSAIoctl`, an `AcceptEx` and a `WSARecv` pending until a connection or data comes, `CancelIoEx`, and closing a socket with a receive pending; the `AcceptEx` and `GetAcceptExSockaddrs` it uses are `wsock32`'s, by ordinal, as Steam imports them |
| `loadtest` | Winsock under a browser's or a game client's load: 200 loopback connections (401 sockets) open at once in one process, one `select` over all 200 (a program's own `FD_SETSIZE` of 1024) and `WSAPoll` over all 200, a `WSAEventSelect` registration on each socket whose event fires on data, 200 new sockets registering again after `closesocket` ended the old ones, 400 connections opened and closed one after another (each leaves a TIME-WAIT behind), 100 UDP sockets open at once, and 24 parallel HTTP downloads of 300,000 bytes from `tools/h2server.js`, checked byte for byte; on the old limits it failed with `WSAENOBUFS` after 8 connections |
| `msitest` | Windows Installer: `MsiDatabaseApplyTransform` and `TRANSFORMS=` (a transform for another product refused with 1624), `msiexec /p` and `/uninstall` of a patch (1642 when its product is missing), rollback of a package that fails half-way (files put back, new files, folders and keys removed, its rollback custom action run; `DISABLEROLLBACK` keeps what was done), an automatic service that `services.exe` starts at the next boot, and (`msitest script`) JScript and VBScript custom actions of every source type setting properties, reading them back and writing files, and a failing script failing its install.  The packages come from `tools/msitest/mkpkg.py` (`C:\Tests\Msi`) |
| `glgeneric` | With no OpenGL driver installed (before the App Store's Mesa 3D), `opengl32.dll` falls back to its own OpenGL 1.1, as Windows does without a display driver's OpenGL: a double-buffered RGBA pixel format, a context made current on a window, `glGetString` saying "GDI Generic" and 1.1, `glClear` read back with `glReadPixels`, and `SwapBuffers`; and 2D drawing as ScummVM does it: a texture uploaded and updated, a textured quad from vertex arrays through an orthographic projection, blending (the edge two triangles share blended once), and a scissored `glBegin`/`glEnd` quad (64- and 32-bit) |
| `cmdlinetest` | Long command lines: `cmdlinetest len=N sum=S ARG...` checks that `GetCommandLineW`/`GetCommandLineA` hold all N characters and that the CRT's `argv` (quoted paths with spaces, escaped quotes, trailing backslashes) hashes to S and ends in `END`; `cmdlinetest spawn` starts itself with `CreateProcessW` (1,000, 8,191 and 32,766 characters, with characters outside ASCII), `CreateProcessA` and `cmd.exe /c` (8,000), and checks that 32,767 characters fail with `ERROR_FILENAME_EXCED_RANGE` |
| `crashtest` | Crash reports: starts `crash.exe` (from its own folder, so the 32-bit build starts the 32-bit one), checks it ended with `STATUS_ACCESS_VIOLATION`, and reads its report in `C:\NovaOS\Crashes`: program and PID, exception, `crash.exe+offset`, address 0, return addresses, the module list and the end of the kernel's log |
| `fpstate` | The floating-point state a thread runs with: x87 control word `0x27F` and MXCSR `0x1F80` on the main thread and on a new thread whatever its creator set, each thread's own state kept across context switches, `RtlCaptureContext` saving it into `FltSave` and `RtlRestoreContext` putting it back (MXCSR from `CONTEXT.MxCsr`), the state unchanged after an SEH unwind, inexact SSE and x87 results without a fault, an unmasked x87 divide by zero caught as `EXCEPTION_FLT_DIVIDE_BY_ZERO`, and an unmasked SSE one where the CPU traps (KVM, real hardware) (64- and 32-bit) |
| `tztest` | Time zones: the zone in effect and its offset in `GetLocalTime` against `GetSystemTime` and in the C runtime's `localtime`; then zones from the list by name (`GetTimeZoneInformationForYear`) at known instants: Berlin and Sydney in summer and winter, the minute Berlin and Los Angeles spring forward, Tokyo and India, each converted back with `TzSpecificLocalTimeToSystemTime` |
| `kbdtest` | Keyboard layouts: the layout in effect (`GetKeyboardLayout`, `GetKeyboardLayoutName`); US, French AZERTY and German by handle (`ToUnicodeEx`, `MapVirtualKeyEx`, `VkKeyScanEx`, a dead circumflex, `LoadKeyboardLayout`) and the registry's `Keyboard Layouts` list; `kbdtest 00000407` checks German in effect (Y and Z swapped, ö, AltGr+Q and AltGr+E, the dead acute accent, Ctrl+A, key names, `SPI_GETDEFAULTINPUTLANG`, `Preload`), and `us` after it puts US back with `SPI_SETDEFAULTINPUTLANG` and waits for the system to follow |
| `edgeupdtest` | The APIs Microsoft Edge Update (the WebView2 runtime's installer) calls: Task Scheduler 2.0 (`ITaskService` connects; a task registered from its XML is a file in `C:\Windows\System32\Tasks`, reads back by name, path and XML, disables and enables, runs its `<Exec>` action, is listed, and is deleted; malformed XML, `TASK_CREATE` of an existing task and `TASK_UPDATE` of a missing one fail as on Windows), `CryptProtectData`/`CryptUnprotectData` with the user's and the machine's key (the wrong entropy and a changed blob fail), `UrlCombineW` on RFC 3986's examples, `UrlEscapeA`, `UrlUnescapeA`, `PackageFamilyNameFromFullName`, `PackageIdFromFullName`, `GetPackagesByPackageFamily`, `WTSEnumerateSessionsW`, `MakeAbsoluteSD`, `IsDeviceRegisteredWithManagement`, `NetGetAadJoinInformation`, `CoRegisterPSClsid` and `CoGetCallContext` (64- and 32-bit); and cmd's `type` showing a UTF-16 file as text |
| `ramdisktest` | Files on drive C: (kept in memory) take the memory their contents need, measured as C:'s free space: 33 MiB written in 64 KiB appends takes about 33 MiB once closed (not the 64 MiB buffer it grew into), `SetEndOfFile` to 72 MiB takes 72 MiB (not 128 MiB) and reads back as zeros past what was written, two files appended in turn, a file made shorter and longer again and a closed file appended to keep their contents, deleting the files gives the memory back, and programs are told the machine's real memory: `GlobalMemoryStatusEx`'s total is `NtQuerySystemInformation`'s physical pages, its free memory is C:'s free space and falls by what the files take (it said 512 MB total, 256 MB free) |
| `setuptest` | What installers need: the desktop user's token is the limited half of an administrator's, ShellExecuteEx's `runas` and a `requireAdministrator` manifest (`setupadmin.exe`) start a program elevated (`TokenElevation`, Administrators enabled, high integrity) while `CreateProcess` does not; `msftedit.dll`'s `RICHEDIT50W` and `riched20.dll`'s `RichEdit20W`/`RichEdit20A` take RTF by `EM_STREAMIN`, `EM_SETTEXTEX` and `WM_SETTEXT` and answer `EM_GETTEXTEX`, `EM_GETTEXTRANGE`, `EM_FINDTEXTEXW` and `EM_STREAMOUT`; `VarAdd` ... `VarCmp`; `AddFontResource` of a bare file name finds the font in the Fonts folder; `TaskDialogIndirect` shows custom buttons (command links) and returns the one pressed; the ShellLink object's `IPropertyStore` keeps an AppUserModelID; `CryptProtectMemory` and `RtlEncryptMemory` round-trip; `setuptest bigfile` writes a 300 MB file on C: |
| `msxmltest` | MSXML (`msxml6.dll`, 64- and 32-bit): `CLSIDFromProgID` and `CoCreateInstance` for `Msxml2.DOMDocument`, `Msxml2.DOMDocument.6.0`, the free-threaded document, `XMLHTTP` and `SAXXMLReader`; `loadXML` and `load` of a WiX Burn manifest read back with `selectSingleNode`/`selectNodes` (XPath with `SelectionNamespaces`, and MSXML 3's XSL Patterns, where Burn's unprefixed `Payload` or `Chain/MsiPackage` match elements in the manifest's default namespace and prefixes match as written, while MSXML 6's XPath matches neither), `getElementsByTagName`, attributes and `text`; a document built element by element, as Edge Update builds its requests, serialised by `xml` and `save`; `parseError` codes, lines and reasons; UTF-16 and windows-1252 input; MSXML 3's and 6's defaults (`ProhibitDTD`, `resolveExternals`, whitespace); `importNode`; a SAX parse; the 6.30 version resource |
| `cabtest` | `cabinet.dll`'s File Decompression Interface as installers use it (WiX Burn extracts the Visual C++ Redistributable's payloads with it): `FDIIsCabinet` reads a cabinet's header (folders, files, set ID, whether a next cabinet follows) and refuses a text file; `FDICopy` extracts an MSZIP and an LZX cabinet (window 2^16, x86 call translation on, a file spanning three data blocks) through the caller's own open/read/write/seek functions, with `fdintCABINET_INFO`, `fdintCOPY_FILE` (sizes, dates) and `fdintCLOSE_FILE_INFO` and the caller's `pvUser`; a file the caller skips is not written and the next still comes out; a cabinet 4,321 bytes into another file (Burn's attached container) extracts through the caller's offset; a set of two cabinets whose folder (and a data block) goes on into the second asks `fdintNEXT_CABINET` once and extracts the files of both; the second cabinet alone reports `fdintPARTIAL_FILE`; a missing, foreign or damaged cabinet and a caller's abort fail with `FDIERROR_CABINET_NOT_FOUND`, `FDIERROR_NOT_A_CABINET`, `FDIERROR_CORRUPT_CABINET` and `FDIERROR_USER_ABORT`; 64- and 32-bit |
| `authtest` | Authenticode, with test files `tools/authenticode/mktests.py` signs under a test root the test adds to the `ROOT` store and removes again: `WinVerifyTrust` (`WINTRUST_ACTION_GENERIC_VERIFY_V2`, as Steam's service calls it) accepts SHA-256 and SHA-1 signatures, a 32-bit program, a nested SHA-256 signature, and an expired signer saved by a PKCS #9 countersignature or an RFC 3161 token from within its validity; it refuses a changed code byte and a damaged signature (`TRUST_E_BAD_DIGEST`), a root nobody trusts (`CERT_E_UNTRUSTEDROOT`, also before the root is added and after it is removed), an expired signer without or with a late timestamp (`CERT_E_EXPIRED`), a TLS server certificate (`CERT_E_WRONG_USAGE`), an unsigned program (`TRUST_E_NOSIGNATURE`), a text file (`TRUST_E_SUBJECT_FORM_UNKNOWN`), an unknown action, and a revocation check, which fails offline (`CERT_E_REVOCATION_FAILURE`); `WINTRUST_SIGNATURE_SETTINGS` counts and verifies nested signatures; the provider state (`WTHelperProvDataFromStateData`, `WTHelperGetProvSignerFromChain`, `WTHelperGetProvCertFromChain`) gives the signer's chain and the timestamp's time; `CryptQueryObject` opens the embedded signature, `CryptMsgGetParam` its signer, `CertGetSubjectCertificateFromStore` the certificate, `CertGetNameString` and `CertNameToStr` the names; `CryptCATAdminCalcHashFromFileHandle` gives the Authenticode SHA-1 and no catalog holds it; `CertVerifyCertificateChainPolicy` with the Microsoft root policy (as Edge Update checks its packages) refuses the test root, accepts Microsoft's code signing CA 2024 chained to the 2011 application root only with `MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG`, and always accepts the 2010 root; 64- and 32-bit |
| `gatetest` (x64) | Windows' x64 segment layout and the self-inspection anti-cheat code (Roblox's Hyperion) leans on: CS 0x33, SS/DS/ES/GS 0x2B and FS 0x53, also in `RtlCaptureContext`; a far jump to 0x23 runs 32-bit code (`dec eax`, not a REX prefix) in the 64-bit program and a far jump to 0x33 comes back; 32-bit code at 0xFFFFFFFE wraps to address 0 and faults, and the handler sees an instruction-fetch access violation at 0 with `SegCs` 0x23 and resumes in 64-bit code; `NtGetContextThread` on the calling thread returns the registers its system call left with (RBX and R12 set before the call, RIP in ntdll's stub, debug registers clear, `ContextFlags` kept) and `SetThreadContext` on it resumes at the CONTEXT; `__fastfail` ends a child with `STATUS_STACK_BUFFER_OVERRUN` without running its handlers |
| `msiqtest` | Windows Installer queries as bootstrappers (WiX Burn) make them, from a 64- and a 32-bit process: a 32-bit package (Template `Intel`) puts its `SystemFolder` file in `SysWOW64` and a 64-bit one in `System32`, whichever process runs the engine; `MsiGetProductInfoEx` by context (`VersionString`, `State`, `PackageName`, `PackageCode`, `LocalPackage`; the length alone, `ERROR_MORE_DATA`, 1605 in a context the product is not in, 1608 for an unknown property, bad contexts refused) and the ANSI form; `MsiEnumProductsEx` (all contexts, one product, per-machine with no SID); the source list the install registered and `MsiSourceListAddSourceEx` placing, moving and appending network sources and a URL, `GetInfo`/`SetInfo` (`PackageName`, `LastUsedSource`, `LastUsedType`) and `ClearSource`; `MsiDetermineApplicablePatches` and `MsiDeterminePatchSequence` on a patch file and applicability XML (1642 for a patch for another product, 1635 for a missing one); `MsiEnumPatchesEx`, `MsiGetPatchInfoEx` and a patch's own source list on a patched product; an upgrade package whose Upgrade table removes the version below its range and does not take it for a later one (its block action stays off); removal taking the registration and source list with it |
| `ndrtest` | rpcrt4's NDR engine, in one process: `ndrtestps.dll`, a proxy/stub DLL made with widl, registers itself (`DllRegisterServer`, `NdrDllRegisterProxy`), is found again through `CoGetPSClsid` and `CoGetClassObject` (`NdrDllGetClassObject`), and makes an interface stub around a test object and a stubless proxy joined by a channel that hands each request to the stub's `Invoke` (`NdrStubCall2`).  Calls carry strings, conformant arrays in and out, structures by value, `[in, out]` and with an embedded string, conformant structures, `[unique]` pointers, `hyper`, `double` and `float` arguments in registers and on the stack, `[out]` arrays the proxy allocates, a base interface in another proxy file (forwarding proxy and delegating stub), the server's HRESULT and a crash in the server (`RPC_E_SERVERFAULT`); every buffer goes back to the channel and `DllCanUnloadNow` agrees once everything is released.  64- and 32-bit |
| `regtest` | registry keys under concurrency: six threads and four waves of four child processes open and close a shared key, create and delete their own subkeys, delete keys others hold open (`ERROR_KEY_DELETED` on those handles), duplicate key handles within the process and into a child, set, read and delete the values of one key at once, enumerate while subkeys come and go, leave change notifications pending on keys they close, and end (the children) with keys and watches still open, while a thread waits synchronously for changes; the children inherit a key handle; afterwards the shared key is whole and the tree deletes cleanly; 64- and 32-bit |
| `d3d9test` | NovaOS's own `d3d9.dll`, 64- and 32-bit (graphics suite): it is the system folder's and has every export Windows' has; before DXVK is installed `Direct3DCreate9` and `Direct3DCreate9Ex` give NovaOS's object: an adapter per monitor, named by the display card's PCI vendor and device IDs (not 0, which Qt's GPU blocklist takes for "Standard VGA" and turns OpenGL off for), with `\\.\DISPLAY1`, the current mode and the 32-bit modes, the monitor and an adapter LUID, but no device (`CheckDeviceType`, `GetDeviceCaps` and `CreateDevice` give `D3DERR_NOTAVAILABLE`); `Direct3DCreate9On12` and `Direct3DShaderValidatorCreate9` return NULL, and the PIX markers count open events with no profiler attached (`D3DPERF_GetStatus` 0); with the App Store's DXVK (`d3d9_dxvk.dll`) the calls reach DXVK, whose `IDirect3D9` lists an adapter |
| `svctest` | A real service through the service control manager, installed as Steam's is (a name with spaces, `"path" /service`): `CreateService`, `StartService` with two arguments that `ServiceMain` receives after the service's name, `StartServiceCtrlDispatcher`, `RegisterServiceCtrlHandlerEx` with its context, `SetServiceStatus` from `SERVICE_START_PENDING` to `SERVICE_RUNNING`, `QueryServiceStatusEx` (its own process id), `ERROR_SERVICE_ALREADY_RUNNING`, interrogate, a user-defined control, a control the service does not accept refused with `ERROR_INVALID_SERVICE_CONTROL` before it reaches the handler, stop (the process ends), `DeleteService`; the program run by hand, and a program the service starts before it reports itself running, both get `ERROR_FAILED_SERVICE_CONTROLLER_CONNECT`; `CopyFile` keeps the source's last-write time, size and read-only attribute; 64- and 32-bit |
| `chrometest` | What Chromium (Steam's browser) needs before it starts its child processes: a section created with an empty DACL, named or not, cannot be duplicated from a read-only handle to `FILE_MAP_WRITE` (`ERROR_ACCESS_DENIED`) while a writable handle can, `DUPLICATE_SAME_ACCESS` keeps a handle read-only, `NtQueryObject` reports each handle's own `GrantedAccess` (the read-only section handle's is `FILE_MAP_READ | SECTION_QUERY`; a file opened `GENERIC_READ` has `FILE_GENERIC_READ` and no write right, which Chromium's browser `CHECK`s), a section without a descriptor allows the wider duplicate, and the named one cannot be opened by name; `NtQuerySection` gives the size and no `SEC_IMAGE`; `CreateProcess` with `STARTUPINFOEX` and `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` gives the child the listed handles (it reads the shared text through them and cannot write it) and not an inheritable event left off the list; the attribute list's size and errors; the ordinal exports Chromium imports by number (shlwapi `IsOS` 437 and `QISearch` 219, oleaut32 `VarUI4FromStr`, `VarBstrCat`, `VarBstrCmp`, uxtheme `DrawThemeBackgroundEx` 47, shell32 `SHChangeNotifyRegister` 2 and `SHChangeNotifyDeregister` 4) and a register/deregister round trip; `api-ms-win-power-base-l1-1-0.dll` loads `powrprof.dll`; `GetFileVersionInfo` finds `kernelbase.dll` and `kernel32.dll` by bare name and reads Windows 10 build 18362 from them; `EvtQuery` of the System log returns no events; `GetFirmwareType` (UEFI), `GetConsoleDisplayMode`, `CertControlStore`, `CertCompareCertificateName` and `CryptVerifyCertificateSignatureEx` on the root store's certificates; WinHTTP's proxy resolver needs an asynchronous session and completes later with "autodetection failed"; 64- and 32-bit |
| `comoop` | COM between processes: `CoCreateInstance` with `CLSCTX_LOCAL_SERVER` starts `comserver.exe` from its `LocalServer32` key with `-Embedding`; the server registers its classes with `CoRegisterClassObject` (`REGCLS_SUSPENDED`, then `CoResumeClassObjects`) and the client gets a proxy.  Calls go through ole32's standard marshaler (OBJREFs) and named-pipe channel, `ndrtestps.dll`'s NDR proxies and stubs and oleaut32's `IDispatch` proxy/stub: strings, arrays, a crash in the server, `QueryInterface` across the process and object identity, a dual interface with `BSTR` and `VARIANT` arguments, an interface pointer passed to the server that calls back into the client's apartment, `GetIDsOfNames` and `Invoke` with `BSTR`, by-reference, `SAFEARRAY`, `VT_DISPATCH` and other `VARIANT` arguments and `EXCEPINFO`; the server exits once everything is released, and a second one serves `CoGetClassObject`'s class factory (`LockServer`, `CreateInstance`).  64- and 32-bit clients, each against both servers |
| `etwtest` | advapi32's event-tracing controller and consumer functions as Windows answers them with no logging session running and none able to start: `StartTrace` checks its properties block (`ERROR_BAD_LENGTH`, `ERROR_INVALID_PARAMETER`) and then fails with `ERROR_NO_SYSTEM_RESOURCES`; `StopTrace`, `ControlTrace`, `QueryTrace`, `FlushTrace`, `UpdateTrace`, `EnableTrace` and `EnableTraceEx2` find no session (`ERROR_WMI_INSTANCE_NOT_FOUND`); `QueryAllTraces` lists none; `OpenTrace` opens a real-time consumer that `ProcessTrace` finds no session for and fails on a missing log file; `CloseTrace` closes a handle once; Steam's service's stop-then-start sequence; 64- and 32-bit |
| `qtwebtest` | The calls Qt WebEngine (Chromium, in GOG GALAXY's client) imports, 64- and 32-bit: `bthprops.cpl` finds no Bluetooth radio or device (`ERROR_NO_MORE_ITEMS`, `ERROR_REVISION_MISMATCH` for a wrong `dwSize`) and parses an SDP service record (a string, a 128-bit UUID, the RFCOMM channel inside the protocol list, a missing attribute, a cut-off record); `d3d12.dll` has no device (`DXGI_ERROR_UNSUPPORTED`) or debug layer, with `D3D12CreateDevice` and `D3D12GetDebugInterface` at ordinals 101 and 102; no WinUSB device, HID report descriptor or device instance to open; `SetEnvironmentStringsW` replaces the environment and refuses a string without `=`; `TreeResetNamedSecurityInfoW` leaves everything below a folder inheriting only the new entry, with its progress callback; `NetShareEnum` (no shares; another machine `ERROR_BAD_NETPATH`); the interface's name, LUID and GUID; `DnsQueryEx` with and without a completion routine; `WSAAccept` rejecting and accepting through its condition function; `SetArcDirection` (an arc drawn clockwise); `CreateAppContainerProfile` and `DeleteAppContainerProfile`; `CryptVerifyCertificateSignatureEx` (a root's own signature, from its certificate and from its public key; another key `NTE_BAD_SIGNATURE`); WinHTTP's proxy resolver ending with `ERROR_WINHTTP_AUTODETECTION_FAILED` through the callback; urlmon's security zones and `GetSecurityId`; `_ultow_s` |
| `cachetest` | Drive C:'s saved files let go of when memory runs short and read back from the data disk when wanted, as Windows drops cached file pages: `NtSetSystemInformation(SystemMemoryListInformation)` saves (`MemoryFlushModifiedList`) and lets go (`MemoryPurgeStandbyList`, with `SeProfileSingleProcessPrivilege`, so the program runs as administrator), `GlobalMemoryStatusEx` shows the memory come back, the files read back as they were through `ReadFile` and a mapped view, opening one for its details reads nothing back, files renamed, hard-linked and emptied while only on the disk read back as they should once saved and let go of again; after `shutdown /r` C: is restored without reading its large files and `cachetest after` reads them back |
| `qtthemetest` | What Qt's Windows platform plugin (in GOG GALAXY's client) asks of the Windows Runtime, 64- and 32-bit: `Windows.UI.ViewManagement.UISettings` activates through its factory and `RoActivateInstance` with `IUISettings` to `IUISettings6`; `GetColorValue` gives a white background and black text, or the reverse, for the app mode (`AppsUseLightTheme`) and the accent shades from `AccentPalette` (the default blue without it); `UIElementColor`, scroll bar sizes, caret and double-click times; `AdvancedEffectsEnabled` follows `EnableTransparency`; `ColorValuesChanged` is raised when the app mode or accent changes, not after removal, and handlers go with the last reference; `UIViewSettings`' `GetForWindow` says `Mouse`; an unknown class is `REGDB_E_CLASSNOTREG`; `BCryptEnumContextFunctions` lists Schannel's cipher suites |
| `proclisttest` | Finding another running program by its name, owner and command line, as Microsoft Edge Update finds its install worker, 64- and 32-bit: `EnumProcesses` and the Tool Help snapshot; `GetProcessImageFileName` (a `\Device\HarddiskVolume1\` path) turned into a drive path with `QueryDosDevice`; `QueryFullProcessImageName` (Win32 and native); `GetModuleFileNameEx` and `GetModuleBaseName` of another process; its token's user; `ProcessIdToSessionId`; `IsWow64Process` and `IsWow64Process2`; its command line read from its PEB with `ReadProcessMemory` and through `NtQueryInformationProcess(ProcessCommandLineInformation)` |
| `libmtest` | The 32-bit C runtime's math entry points that MSVC-built x86 code calls instead of the C names: `_libm_sse2_*_precise` (arguments and result in SSE registers) and `_CI*` (on the x87 stack), against the C functions |
| `wvsetuptest` | What the WebView2 runtime's own setup (Chromium's `setup.exe`) needs to unpack its archive and report its crashes, 64- and 32-bit: a 300 MB file mapped whole and read-only through a duplicate of its handle (the view reads the file's head, tail and the zeros between; a read-only mapping larger than the file is refused with a real error, not `ERROR_INVALID_FUNCTION`), an empty file's mapping refused with `ERROR_FILE_INVALID`; `wer.dll`'s report API (`WerReportCreate`, `WerReportSetParameter`, `WerReportAddFile`, `WerReportAddDump`, `WerReportSubmit` answering `WerDisabled`, `WerReportCloseHandle`, bad arguments refused); `FlsGetValue2` leaving the last error alone. The 32-bit run leaves out the large file |
| `warptest` | `SetCursorPos` and `ClipCursor`, as games recentre and confine the pointer: with its window in front the test moves the pointer (`GetCursorPos` reads it back, the window gets `WM_MOUSEMOVE` at that point but no `WM_INPUT`, `SetPhysicalCursorPos` too), off the desktop it stops at the edges; `ClipCursor` moves it inside the rectangle, `GetClipCursor` returns it, `SetCursorPos` and the PS/2 mouse (moved 300 left and up, all of it still in Raw Input) stop at its edges, `ClipCursor(NULL)` lets it go; a process in the background is refused (`ERROR_ACCESS_DENIED`) until its own window comes to the front, which ends the other's confinement.  64- and 32-bit |
| `wvstarttest` | What the WebView2 runtime's browser process (`msedgewebview2.exe`) and its loader need to start: COM's apartment published in the TEB (`ReservedForOle`: STA, MTA, gone after `CoUninitialize`); `TerminateProcess` on the process itself ending it with its code and no FLS callback (a child run); 300 FLS slots; `GetAddrInfoExW`; `RtlIpv4StringToAddressExW` and `RtlIpv6StringToAddressExW` (ports, a scope id, junk refused); `LdrLockLoaderLock` and its try-only form; `CryptFindOIDInfo` by OID and name; the performance counter provider API; `GetDllDirectoryW`; `GetPhysicallyInstalledSystemMemory`; `FindPackagesByPackageFamily` (none) and `AppPolicyGetThreadInitializationType` |

<!-- END generated:selftest-table -->

`crash` writes through a NULL pointer (only it dies, and it leaves a
report in `C:\NovaOS\Crashes`); `crash kernel` crashes the kernel on
purpose (`NtNovaBugCheck`) to show the backtrace.

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
| SumatraPDF 3.4.6 (the official 32-bit build, from the npm package `pdf-to-printer`) | opens a PDF the script generates; the screenshot must match `tests/reference/sumatrapdf.png` (at most 3% of pixels differ, for every screenshot) |
| WinMerge 2.16.50 | compares `hello.txt` with `hello2.txt`; the screenshot must match `tests/reference/winmerge.png` |
| VLC 3.0.21 (the 32-bit PortableApps package, unpacked with 7-Zip) | loops the MP4 the ffmpeg test made (30 s of SMPTE colour bars with a 440 Hz tone) with its Qt interface, screenshot once the colour bars show in its window; the screenshot must match `tests/reference/vlc.png`, and the sound NovaOS played (`sound.wav` in `--out`) must hold the tone |
| Audacity 3.7.4 (the official 64-bit zip) | through its first-run dialogs, records 10 s of the microphone's 523 Hz tone, stops and saves the project; the screenshot must match `tests/reference/audacity.png` and `C:\Apps\rec10.aup3` must exist |
| Firefox 157.0 (Mozilla's full installer, the App Store's download) | `store install Firefox`: the Store unpacks the installer from `C:\Downloads` with 7-Zip (staged in `C:\Programs\7-Zip`), as its Install button does; then Firefox loads a page from an HTTPS server the script runs on the host (https://10.0.2.2:8443/, a certificate from a CA made for the run with `openssl` and trusted through Firefox's `distribution\policies.json`); the screenshot must match `tests/reference/firefox.png` |
| Teeworlds 0.7.5 (the official 64-bit zip, the App Store's download) | `store install Mesa 3D` and `store install Teeworlds`; the game starts in full screen, Enter answers its two first-start questions, and its start menu must match `tests/reference/teeworlds.png`; the sound NovaOS played while it ran must hold at least 5 s of sound (`App(sound=(None, 5000))`: any sound, here its menu music, counted between its first test starting and its last one ending) |
| OpenTyrian 2.1.20260913 (the official 64-bit zip with the freeware Tyrian 2.1 data, the App Store's download) | `store install Mesa 3D`, `store install DXVK` and `store install OpenTyrian`; the game draws with Direct3D 9 (SDL2's renderer) through DXVK, its demo plays in a window, Alt+Enter switches it to full screen, and Enter goes through its menus (one-player game, episode 1, normal), pressed only once the screen stands still (the game fades between menus and takes no keys meanwhile), to its game menu, which must match `tests/reference/opentyrian.png`; the sound NovaOS played while it ran must hold at least 5 s of sound (its music) |
| Blobby Volley 2 1.1.1 (the official 32-bit zip, the App Store's download) | `store install Mesa 3D`, `store install DXVK` and `store install Blobby Volley 2`; started from its folder, the game opens in an 800x600 window and the keyboard goes Options, Graphic Options, Fullscreen Mode, OK: SDL switches the display to 800x600 and draws with Direct3D 9 in exclusive full screen through DXVK; the screenshot must be 800x600 and, back in the main menu, match `tests/reference/blobby volley 2.png`; after Alt+F4 ends the game, `sysinfo` must report the display back at 2560x1600 |
| LBreakout2 2.6.5 (the official 64-bit zip, the App Store's download) | `store install LBreakout2`; started from its folder, the game (SDL 1.2, drawn with GDI) opens in a 640x480 window and 'f' switches it to full screen: SDL changes the display to 640x480 and the window to `WS_POPUP`, so the desktop's frame goes; the screenshot must be 640x480 and match `tests/reference/lbreakout2.png`; after Alt+F4 ends the game, `sysinfo` must report the display back at 2560x1600 |
| Cave Story 1.0.0.6 (Aeon Genesis' English translation, 32-bit, the App Store's download) | `store install Cave Story`; started from its folder, the game asks DirectDraw for 640x480 at 16 bits and NovaOS's `ddraw.dll` (cnc-ddraw) switches the display to 640x480; the title screen must show at 640x480, Z (held) starts a new game, and once its opening line is written the screenshot must match `tests/reference/cave story.png`; after Alt+F4 ends the game, `sysinfo` must report the display back at 2560x1600 |
| Notepad++ 8.8.3 (portable) | opens a file; the screenshot (tab bar and status bar drawn) must match `tests/reference/notepad++.png` |
| PuTTY 0.81 (built from the source release with MinGW, kept in the cache) | a raw connection to an echo server the script runs on the host (10.0.2.2:2323); the line typed must reach the server, and the screenshot must match `tests/reference/putty.png` |

The windowed programs run last, one at a time (each takes the keyboard and
is closed with Alt+F4 before the next).  The next one starts only once
every process the last one started has ended (any still running after
two minutes is stopped with `taskkill`) and the Terminal answers again.
When it does not (a program that failed keeps the keyboard, as VLC does
with its error box when its file is missing), the script opens a new
Terminal from Start, stops the program from there and carries on, so one
failure does not fail every program after it.  After Firefox, `store close` closes the App Store window its install
opened.  After each program the script types `mem` and prints the
machine's free memory and what drive C: takes: how many files and bytes
it holds, how much of that is in memory, and how much has been let go of
so far.  C: lives in memory, but the contents of its saved files are let
go of when memory runs short and read back from the data disk when
wanted, and the 2.3 GB of programs copied to `C:\Apps` before the first
one starts are restored without being read.  The machine has 6 GB of
memory and a 12 GB data disk: Roblox, Steam and WebView2 install about
2.7 GB more, and the data disk saves all of drive C:.  Building PuTTY needs `cmake` and
`gcc-mingw-w64-x86-64`.

It needs 7-Zip's installer, Pillow, `openssl` (for Firefox's test
server), `mkntfs` (for drive D:) and, for the two programs that need
sound, PulseAudio and QEMU's PulseAudio backend, `qemu-system-gui` on
Ubuntu (NovaOS then boots with a microphone that hears a tone and its
output recorded, as the core self-tests do; without PulseAudio the
programs that need sound are skipped, not failed).  The exit
status is the number of programs that failed; `--update-reference` rewrites
the reference screenshots after an intended change:

```bash
sudo apt install p7zip-full python3-pil ntfs-3g pulseaudio pulseaudio-utils qemu-system-gui
python3 tools/appcorpus.py
python3 tools/appcorpus.py --only ripgrep,jq --out /tmp/ac
python3 tools/appcorpus.py --only NovaOS,SumatraPDF,WinMerge,VLC,Audacity,Notepad++,PuTTY --update-reference
```

A command passes as a self-test does (exit code 0, the output expected).
To add a program, add a file to `tests/appcorpus/`.  Other third-party programs (the
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
check it (`wavcheck.py` exits 0 when the tone is there; `--sound MS`
instead exits 0 when the file holds MS milliseconds of any sound, such
as a game's music):

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
and plain HTTP on 8080 for `httptest` (`httptest -2 -k https://10.0.2.2:8443/hello`)
and `dltest` (`dltest -n 8 -w 10.0.2.2 8080 33554432`: eight 32 MB downloads
at once from `/stream/BYTES/SEED`).

### On the host

- `tools/pe_imports.py PROGRAM.exe ...` lists every DLL and function a
  Windows program imports that NovaOS's DLLs do not provide (by default it
  reads the DLLs in `build/kernel_build/userland`), mapping API sets as the
  kernel's loader does (`api-ms-win-core-synch-*` to `kernelbase`, with
  `kernel32` and `kernelbase` standing in for each other;
  `api-ms-win-power-*` to `powrprof`).
- `tools/msitest/hosttest.c` dumps a Windows Installer package's tables
  and cabinets with the same readers `msi.dll` uses (build instructions in
  the file); `tools/msitest/make_package.sh` builds a test package with
  msitools.
- `tools/make_cabtest_data.py` writes `userland/programs/cabtest_data.h`,
  the MSZIP, LZX and two-cabinet set `cabtest` extracts (it has a small LZX
  encoder of its own); `--write-cabs DIR` writes them as files, to check
  with `cabextract -t` or `7z t` after a change.
- `tools/authenticode/mktests.py OUT PE64 PE32` makes `authtest`'s signed
  files (`C:\Tests\Authenticode`; the build runs it on `hello.exe`): a test
  certificate authority from fixed seeds, in plain Python
  (`tools/authenticode/pki.py`), signs copies of the programs the ways
  `WinVerifyTrust` must accept or refuse (`tools/authenticode/sign.py`
  writes Authenticode signatures, PKCS #9 and RFC 3161 timestamps and
  nested signatures).  `osslsigncode verify -CAfile root.pem -TSA-CAfile
  root.pem -in OUT/signed.exe` checks them on the host, with the root from
  `openssl x509 -inform der -in OUT/testroot.cer -out root.pem`.
  It also copies Microsoft's code signing CA 2024
  (`tools/authenticode/mspca2024.cer`, a public certificate) for
  `authtest`'s check of the Microsoft root chain policy.

---

## Debugging

- **Serial log**: every kernel message goes to COM1 (your terminal under
  `run`).  In NovaOS, the Terminal's `dmesg` shows it.
- **`devices`** in the Terminal lists the PCI devices and the driver
  each one has (missing drivers in red); on a real PC this is the first
  thing to check ([hardware.md](hardware.md)).
- **Program crashes** are logged with the faulting module and offset, the
  process's exit code, and `OutputDebugString` output.
- **Crash reports**: a program that crashes leaves
  `C:\NovaOS\Crashes\NAME-YYYYMMDD-HHMMSS-PID.txt` (the exception, the
  module and offset, the address it touched, the return addresses on its
  stack, its modules and the end of the kernel's log), and the Terminal
  names the file under the crash line.  A kernel fault writes its log and
  backtrace into `\NOVA\PANIC.TXT` on the disk that keeps drive C: (FAT;
  64 KiB set aside at boot, written with the disk driver alone), and the
  next start moves it to `C:\NovaOS\Crashes\kernel-YYYYMMDD-HHMMSS.txt`.
  The Terminal's `crashes` lists the reports and `crashes last` shows the
  newest; attach the file to an issue.  At most 100 are kept: delete old
  ones to make room.
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
