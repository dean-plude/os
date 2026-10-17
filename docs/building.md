# Building NovaOS
<!-- The regions between "BEGIN generated" and "END generated" markers are built from fragment files by tools/docgen.py: edit those files, not the regions (CONTRIBUTING.md). -->

This page covers building NovaOS from source, running it in QEMU, the test
suites and the debugging tools.  The build host is Linux; CI builds on
Ubuntu 24.04.  On Windows, use WSL2 or a Linux VM.  On a Mac, see
[macos.md](macos.md).

If you only want to try NovaOS, you do not need to build it: the README
links the ready-made `nova.iso`.

## Prerequisites

```bash
sudo apt update
sudo apt install -y \
    git cmake ninja-build make nasm python3 \
    clang lld llvm libc++-dev gcc-mingw-w64-x86-64 \
    mtools dosfstools xorriso p7zip-full dosbox \
    qemu-system-x86 ovmf acpica-tools
```

This is the package set CI's build job installs (`tools/ci/install.sh
build`), plus QEMU, OVMF and `iasl` for running and testing.

| Package | Used for |
|---------|----------|
| `cmake`, `ninja-build` or `make` | The build.  CI uses Unix Makefiles; `scripts/build.sh` always does |
| `nasm` | Kernel assembly (entry, interrupt stubs, SMP trampoline, system-call entry) |
| `clang`, `lld` | **Required.** The Windows userland (`--target=x86_64-pc-windows-msvc` and `i686-pc-windows-msvc`, linked with `lld-link`), NetSurf, the test drivers, KMDF and UMDF, and the kernel and boot loader unless the compilers below are installed |
| `llvm` | `llvm-rc` (resource scripts: icons, dialogs), `llvm-lib`, `llvm-nm` and `llvm-readobj` |
| `libc++-dev` | libc++'s C++ headers, for HarfBuzz in `novatext.dll`.  Nothing of libc++ is linked.  `NOVA_LIBCXX=DIR` points at headers elsewhere |
| `gcc-mingw-w64-x86-64` | **Required** for the MinGW-w64 headers it pulls in (`mingw-w64-common`): `msvcp140.dll` compiles against them (`NOVA_MINGW_INCLUDE=DIR` to use others), and KMDF and UMDF compile against its WDK headers in `/usr/share/mingw-w64/include`.  Its `x86_64-w64-mingw32-gcc` also builds the boot loader |
| `python3` | The userland build (`tools/build_userland.py` and the scripts it calls) |
| `git` | Fetching Microsoft's Windows-Driver-Frameworks source (see below) |
| `p7zip-full` | **Required.** Unpacking the bundled Firefox installer and Mesa archive (`7z`, or `7zz` from the `7zip` package) |
| `mtools`, `dosfstools` | `nova.img`, copying files onto data disks, and MS-DOS's boot disk (`tools/msdos/mkdisk.py`) |
| `dosbox` | Building MS-DOS 4.00 from Microsoft's source with its own MASM, C compiler and LINK (`tools/msdos/build_msdos.py`, [dos.md](dos.md)); run headless, a build tool only |
| `xorriso` | `scripts/create-iso.sh` |
| `qemu-system-x86`, `ovmf` | Running NovaOS |
| `acpica-tools` | `iasl`, which the core self-tests use to compile the ACPI tables in `tests/acpi/` |

`ccache` is optional; CI puts `/usr/lib/ccache` first on `PATH` to speed
up rebuilds.

### Network access during the build

The build downloads four things.  Each is pinned and checked, and kept
in a cache under `~/.cache/novaos`:

| What | From | Cache | Checked by |
|------|------|-------|------------|
| Firefox 157.0 installer (`tools/bundle_firefox.py`) | `archive.mozilla.org` | `~/.cache/novaos/firefox`, or `NOVA_FIREFOX_CACHE` | Mozilla's `SHA512SUMS` |
| Mesa 24.2.4 llvmpipe, mesa-dist-win (`tools/bundle_mesa.py`) | GitHub releases | `~/.cache/novaos/mesa`, or `NOVA_MESA_CACHE` | a SHA-256 in the script |
| Windows-Driver-Frameworks at a pinned commit (`tools/kmdf/build_kmdf.py`) | GitHub, with `git fetch` | `~/.cache/novaos/wdf` | the commit id |
| MS-DOS at a pinned commit (`tools/msdos/build_msdos.py`), and the DOS files built from it | GitHub, with `git fetch` | `~/.cache/novaos/msdos` | the commit id |

Firefox's `SHA512SUMS` is fetched again on every build, so even a build
with a full cache needs to reach `archive.mozilla.org`.  Firefox and
Mesa are part of every build; there is no switch to leave them out.
[building-firefox.md](building-firefox.md) has the details for Firefox.

### Optional compilers

- **Kernel**: CMake uses an `x86_64-elf-gcc` cross compiler when one is
  on `PATH`, and clang otherwise.  To build one with crosstool-ng (it
  installs into `~/x-tools/x86_64-unknown-elf`):

  ```bash
  git clone https://github.com/crosstool-ng/crosstool-ng
  cd crosstool-ng && ./bootstrap && ./configure --enable-local && make
  ./ct-ng x86_64-unknown-elf
  ./ct-ng build
  export PATH="$HOME/x-tools/x86_64-unknown-elf/bin:$PATH"
  ```

  The toolchain's compiler is `x86_64-unknown-elf-gcc`; CMake looks for
  `x86_64-elf-gcc`, so add a link with that name if you want it used.

- **Boot loader**: `x86_64-w64-mingw32-gcc` (from `gcc-mingw-w64-x86-64`)
  is used when present, otherwise clang with `lld-link`.

---

## Building

```bash
git clone https://github.com/dean-plude/os
cd os
cmake -S . -B build -G Ninja
cmake --build build
```

Without Ninja, leave out `-G Ninja` and CMake writes Makefiles
(`make -C build -j"$(nproc)"`).  CI runs
`cmake -S . -B build -G "Unix Makefiles"` and `make -C build -j"$(nproc)"`.

The root `CMakeLists.txt` builds the kernel and the boot loader as two
separate CMake projects (ExternalProject), each with its own compiler,
then makes the disk image.  Their output goes to log files, not the
terminal.  When a sub-build fails, look in:

```
build/kernel_build-prefix/src/kernel_build-stamp/kernel_build-build-err.log
build/bootloader_build-prefix/src/bootloader_build-stamp/bootloader_build-build-err.log
```

(and the `-configure-err.log` files beside them when configuring fails).
The userland is built inside the kernel sub-build, so its errors are in
the first file.

### scripts/build.sh

`scripts/build.sh` checks for the compilers, configures `build/` with
Unix Makefiles and builds.  Options:

| Option | Effect |
|--------|--------|
| `--clean` | Delete `build/` first |
| `--jobs N` | Run `make -j N` (default: the CPU count) |
| `--no-disk` | Build only the kernel and boot loader sub-builds, not `nova.img`.  The results stay in `build/kernel_build/kernel.elf` and `build/bootloader_build/bootx64.efi` |
| `--release`, `--debug` | Set the build type passed to the sub-builds (default Debug).  This changes little: the kernel and boot loader always compile with `-O2 -g` |
| `--verbose` | `make --verbose` |

Do not mix it with a `build/` directory configured for Ninja: CMake
refuses to change the generator of an existing build directory.  Use
`--clean`, or the plain CMake commands above.

### What the build produces

```
build/
├── bootx64.efi                 # the UEFI boot loader
├── kernel.elf                  # the kernel, with debug symbols; it also carries drive C:'s system files
├── nova.img                    # a bootable FAT32 disk image (label NOVA_EFI, no partition table)
└── kernel_build/userland/      # the built DLLs and programs (x64; x86 under x86/)
```

`nova.img` (`scripts/create-disk.sh`) holds `\EFI\BOOT\BOOTX64.EFI` and
`\EFI\NOVA\kernel.elf`.  It is a sparse file of at least 128 MiB, sized
to hold the kernel and boot loader twice (room for a staged update) plus
32 MiB, rounded up to a multiple of 32 MiB.

### CMake targets

| Target | What it does |
|--------|--------------|
| (default, `all`) | `kernel.elf`, `bootx64.efi` and `nova.img` |
| `kernel_build`, `bootloader_build` | One sub-build only |
| `disk_image` | `nova.img` |
| `run` | Boot `nova.img` in QEMU with `scripts/run-qemu.sh` ([below](#running-in-qemu)) |
| `run-debug` | The same, with QEMU's GDB stub on port 1234, stopped at the first instruction |

`run` and `run-debug` exist only when CMake found `qemu-system-x86_64`
and OVMF.  CMake looks for `OVMF_CODE.fd` or `OVMF.fd` in
`/usr/share/OVMF`, `/usr/share/ovmf`, `/usr/share/qemu` and
`/usr/local/share/OVMF`, and for `OVMF_VARS.fd`.  On Ubuntu 24.04 it finds
`/usr/share/ovmf/OVMF.fd` and no separate variable store.  Set them by
hand with `-DOVMF_CODE=...` and `-DOVMF_VARS=...`.

### The Windows userland

`userland/` holds the system DLLs, the C runtimes, `cmd.exe`, `msiexec`
and the sample and test programs.  `tools/build_userland.py` compiles it
with clang and `lld-link` twice: once for x64 into `C:\Windows\System32`
and `C:\Programs`, and once for x86 into `C:\Windows\SysWOW64` and
`C:\Programs\x86`.  The same run also builds or stages:

- the NetSurf browser (`tools/build_netsurf.py`, `C:\Programs\NetSurf`);
- Firefox 157.0 (`C:\Programs\Mozilla Firefox`) and Mesa's llvmpipe
  OpenGL (`opengl32_mesa.dll` with `libgallium_wgl.dll` and
  `libglapi.dll`, in System32 and SysWOW64);
- the fonts, ICU, the trusted root certificates and the General MIDI
  sound font;
- KMDF (`Wdf01000.sys`, `WdfLdr.sys`) and UMDF (`Wudfx02000.dll`,
  `WUDFHost.exe`, `WUDFRd.sys`), built from Microsoft's MIT-licensed
  Windows-Driver-Frameworks source;
- the test packages under `C:\Tests` (Windows Installer packages, test
  drivers and their signed packages, Authenticode test files);
- firmware, when `tools/fetch_sof_firmware.py` or
  `tools/fetch_firmware.py` has fetched it (see below).

All of it is embedded in `kernel.elf` and placed on drive C: at boot.
Files of 1 MiB or more are stored zlib-compressed and inflated by the
kernel.

Wallpapers, icon sets and sample pictures are not: they are files beside
the kernel.  `userland/files` mirrors drive C: (its
`Windows/Web/Wallpaper/Beam.png` is `C:\Windows\Web\Wallpaper\Beam.png`);
`scripts/create-disk.sh` and `scripts/create-iso.sh` copy it to
`\EFI\NOVA\Files` on the boot volume (a fourth argument names another
folder), and the bootloader hands those files to the kernel at boot.  The
Beam icon set there is rendered from the brand guide's SVGs by
`tools/make_icon_set.py`.

Independent DLLs, programs and objects build concurrently, with at most
`--jobs N` (or `-j N`) compiler and linker processes at once; the default
is the CPU count.  A DLL links after the DLLs it depends on, and the
output is the same whatever `N` is.  The x64 pass, the x86 pass and
NetSurf share that budget and run side by side.  A failed command's
output is printed in one piece, headed `[x64]`, `[x86]` or `[netsurf]`.
CMake runs the script without `--jobs`, so it uses every core.

| Environment variable | Effect |
|----------|--------|
| `NOVA_NO_NETSURF=1` | Leave NetSurf out (its first build compiles about 800 files; later builds reuse them) |
| `NOVA_NO_WOW64=1` | Skip the x86 pass: no `SysWOW64`, no `C:\Programs\x86` |
| `NOVA_FIREFOX_CACHE`, `NOVA_MESA_CACHE` | Where the Firefox and Mesa downloads are kept |
| `NOVA_LIBCXX`, `NOVA_MINGW_INCLUDE` | Header locations, as in the table above |

To rebuild only the userland, for a quick check of a DLL (`--check` only
loads the DLL and program manifests, as CI's first job does):

```bash
NOVA_NO_NETSURF=1 python3 tools/build_userland.py --jobs 4 /tmp/ul /tmp/ul/gen.c kernel/ke/syscall.h
python3 tools/build_userland.py --check
```

### Adding a DLL or program

There is no central list to edit: each DLL and program registers itself,
so parallel changes add files instead of editing the same lines.

**A DLL** is a directory `userland/NAME/` with its `.c` files and a
`dll.json`:

```json
{
  "deps": ["user32", "kernel32", "ntdll"],
  "tlssup": true
}
```

| Key | Meaning |
|-----|---------|
| `deps` | The DLLs it links against.  They are built first; a cycle or a missing one stops the build |
| `base`, `base_x86` | Fixed load addresses.  Leave them out: a new DLL gets the first free 16 MiB slot from 0x7FFD00000000 (x64) and 0x97000000 (x86), and the build checks that no two DLLs overlap.  Older DLLs keep the fixed addresses in their `dll.json` |
| `sources` | Directories its `.c` files come from (default: its own) |
| `entry` | `"DllMain"` for a DLL with an entry point (a `NAME.rc` in its directory is linked in too) |
| `tlssup` | `true`: link implicit TLS support (`lib/tlssup.c`) |
| `x64_only`, `x86_only` | Build it for one architecture only |
| `ordinals` | `{"Name": 12}`: export ordinals, for DLLs programs import from by number |
| `file` | The file name when it is not `NAME.dll` (`"bthprops.cpl"`) |

When a DLL needs more (a third-party library, extra flags, a special
link), put the code in `userland/NAME/build.py`, not in
`tools/build_userland.py`.  It may define `cflags(b)` (flags for the
DLL's own sources), `objs(b, odir)` (extra objects; `b.compile_many`
compiles a library in parallel and reuses fresh objects) and
`link(b, odir, objs, deps, base)` (replaces the link).  `b` is
`tools/build_userland.py` itself, with `b.ARCH`, `b.cflags()`, `b.cc`,
`b.run` and `b.link_dll`.  `userland/secur32/build.py` (Mbed TLS) and
`userland/msvcrt/build.py` (`msvcrt.dll` and `ucrtbase.dll`) are
examples.

**A program** is `userland/programs/NAME.c` (or `.cpp`, with `NAME.rc`
for resources).  It is built 64-bit only into `C:\Programs` unless a
`NAME.json` beside it says otherwise:

| Key | Meaning |
|-----|---------|
| `x86` | `true`: also build it for 32 bits (`C:\Programs\x86`) |
| `system` | `true`: install it in `C:\Windows\System32` (and SysWOW64) |
| `libs` | More DLLs to link against, e.g. `["usp10"]` |
| `gui` | `true`: a GUI program (`/subsystem:windows`), which the shell starts without a console window |
| `msstl` | `true`: build a `.cpp` against Microsoft's STL and `msvcp140.dll`, as Visual Studio does (`userland/programs/stltest.cpp`) |
| `selftest` | `true`: list it among the self-test programs in [testing.md](testing.md) |
| `sources` | With `msstl`: more `.cpp` files compiled into it with its flags, as paths or globs from the repository root, e.g. a vendored library's (`calc.json` builds Windows Calculator's CalcManager) |
| `include`, `force_include` | With `sources`: directories searched for headers, and headers every file starts with (Visual Studio's `/FI`, for a project's forced precompiled header), from the repository root |
| `cflags` | With `msstl`: more compiler flags for its own sources, e.g. `["-D__XAML_UNITTESTS__"]` (`xamlbasetest.json`) |
| `winrt_abi` | `true`: a `.cpp` built against the Windows Runtime C++ ABI headers (`windows.ui.composition.h`, `microsoft.ui.xaml.h` ...), which the build generates without MIDL (below; `userland/programs/winrtabitest.cpp`) |
| `xaml` | `true`: a `.cpp` built against Microsoft's XAML sources as they are compiled (`third_party/microsoft-ui-xaml`, WIL, NovaOS's WRL in `userland/windows_ui_xaml/inc`, the components' forced `precomp.h`) and linked with the XAML core built so far (`xaml_components.lib`, `userland/windows_ui_xaml/build.py`) and Microsoft's unit-test stubs for the layers not built yet (`xaml_teststubs.lib`); implies `msstl` (`userland/programs/xamlbasetest.cpp`) |

### Windows Runtime ABI headers (no MIDL)

C++ code written against the Windows Runtime (`ABI::Windows::UI::Composition::ICompositor`,
`__uuidof`, `IVector<HSTRING>`, the XAML framework's own sources) includes the
headers MIDL makes from IDL and the Windows SDK ships.  NovaOS makes them with
`tools/winrt/abigen.py` (Python, a few seconds) from Windows' metadata
(`third_party/winmd`, from windows-rs) and the XAML framework's MIDL 3 IDL
(`third_party/microsoft-ui-xaml`): one `namespace.h` per namespace (its
interfaces in vtable order with their IIDs, delegates, runtime class names,
the generic instances its types use) and a `namespace.values.h` (enums,
structs, generic interface templates), on `userland/include/winrt_abi.h`.  The
userland build generates them into its include directory
(`build/userland/include/winrt-abi`) the first time a manifest with
`"winrt_abi": true` needs them.  `python3 tools/winrt/abigen.py --check` (run by
`build_userland.py --check`, so by CI's checks) compares what the IDL gives with
Microsoft's published `Microsoft.UI.Xaml.winmd`: the same interfaces, member
order and signatures.  See [design/windows-ui-xaml.md](design/windows-ui-xaml.md).

### Generated and prebuilt files

These are made by their own tools and committed, so the normal build
needs none of the tools' extra dependencies:

| File | Tool | Source |
|------|------|--------|
| `third_party/icu/` (`icu.dll` x64 and x86, `icudt77l.dat`) | `tools/build_icu.py` | ICU4C 77.1 (SHA-256 checked), cross-compiled with MinGW-w64 (`g++-mingw-w64-x86-64 g++-mingw-w64-i686`).  One DLL with ICU's C API under unversioned names, as Windows 10 ships it; the data is trimmed (no legacy code-page converters, word-break dictionaries, transliteration, unit or character names).  Rerun it only to move to another ICU release |
| `userland/kernel32/locale_data.h` | `tools/gen_locales.py` | .NET's `IcuLocaleData.cs` (MIT): the names, LCIDs, code pages and GEOIDs of the 864 locales Windows knows |
| `kernel/ke/tzdata.inc` | `tools/gen_timezones.py` | Unicode CLDR's `windowsZones.xml` and the IANA tz database through Python's `zoneinfo` (`pip install tzdata` for the old zone names CLDR uses).  Rerun it when a country changes its rules |
| `userland/include/kbdlayouts.h` | `tools/gen_keyboards.py` | xkeyboard-config through libxkbcommon (`sudo apt install xkb-data libxkbcommon0`) |

### Firmware

Release builds carry device firmware; pull requests and the rolling
`latest` build do not.  Run one of these before building to include it:

```bash
python3 tools/fetch_firmware.py        # the default firmware set (Intel graphics, Wi-Fi, Bluetooth) and the audio DSP's
python3 tools/fetch_sof_firmware.py    # only the audio DSP's Sound Open Firmware
```

The files go to `third_party/firmware` and `third_party/sof-bin`, which
are not committed.  QEMU emulates none of these devices, so nothing here
needs them.  [drivers.md](drivers.md#the-default-firmware-set) and
[hardware.md](hardware.md#the-digital-microphones-behind-the-audio-dsp)
explain what they are for.

### The ISO

```bash
scripts/create-iso.sh build/nova.iso build/bootx64.efi build/kernel.elf
```

The ISO is not committed (`*.iso` is in `.gitignore`).  CI builds it on
every run and keeps it as the `nova-iso` artifact.  Each push to `main`
that passes CI replaces `nova.iso` on the rolling `latest` pre-release
(<https://github.com/dean-plude/os/releases/download/latest/nova.iso>).
A version tag makes a release with the tested ISO, its checksums and the
update files ([releasing.md](releasing.md)); the newest one is at
<https://github.com/dean-plude/os/releases/latest/download/nova.iso>.

The ISO boots by UEFI only (El Torito, no emulation).  Its EFI System
Partition holds `\EFI\BOOT\BOOTX64.EFI`, `\EFI\NOVA\kernel.elf` and
`\EFI\NOVA\bootlog.txt` (1 MiB set aside for the boot log).  The same
partition is partition 2 of a GPT behind a protective MBR, so the ISO
written to a USB stick boots too.  Booted from either, NovaOS runs live
and opens Install NovaOS.  The boot loader tells installation media from
an installed disk by `bootlog.txt`, which the installer does not copy.

Started from a stick, the kernel writes its log into `bootlog.txt` in
place (`kernel/fs/bootlog.c`): what was logged before the stick
appeared, then the rest at most once a second, before a restart and
after a kernel fault.  To try a stick in QEMU, with only the firmware's
framebuffer for a display, as on a laptop:

```bash
cp build/nova.iso stick.img
truncate -s 2G stick.img
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/ovmf/OVMF.fd \
  -vga none -device ramfb -device qemu-xhci,id=xhci \
  -drive if=none,id=stick,format=raw,file=stick.img \
  -device usb-storage,bus=xhci.0,drive=stick -serial stdio
```

The devices suite's `usbboot` boot does this and reads the log back from
the stick image with mtools; its `cdboot` boot starts the ISO as a disc.

---

## Running in QEMU

The quickest way, from the build directory:

```bash
cmake --build build --target run
```

This runs `scripts/run-qemu.sh`, which starts:

- a q35 machine with 4 CPUs (`-cpu qemu64` with RDTSCP, XSAVE, XSAVEOPT,
  AVX and F16C) and **256 MiB** of memory;
- `nova.img` as the boot disk and `build/nova-data.img` as a data disk
  (an empty 256 MiB file made on the first run);
- the standard VGA adapter, no network card, an Intel HD Audio card, and
  COM1 on your terminal.

256 MiB is well below the 2 GB the [user guide](user-guide.md) asks for,
now that drive C: (held in memory) carries Firefox and Mesa.  For normal
use, the network or the App Store, run QEMU yourself:

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -drive file=build/nova.img,format=raw \
  -drive file=build/nova-data.img,format=raw \
  -serial stdio
```

Add `-accel kvm` on a Linux host with KVM.  `run-qemu.sh` can also be
called directly: `scripts/run-qemu.sh IMAGE OVMF_CODE [OVMF_VARS] [--gdb]`,
e.g. `scripts/run-qemu.sh build/nova.img /usr/share/OVMF/OVMF_CODE_4M.fd
/usr/share/OVMF/OVMF_VARS_4M.fd`.  `NOVA_AUDIO` picks its sound backend
(`pa`, `pipewire`, `alsa`, `coreaudio`, `none`, or `wav,path=out.wav`);
by default it uses PulseAudio or PipeWire when it finds one, Core Audio
on macOS, and otherwise a silent card.

The sections below list QEMU options for the devices NovaOS drives.

### Network

QEMU's default user-mode network on q35 is an e1000e (the Intel 82574L),
and it works as is.  `-nic user,model=e1000` gives the older Intel card
and `-nic user,model=virtio-net-pci` virtio-net.

### CPUs

`-smp N` sets the core count; NovaOS uses up to 16.

### USB, keyboards and game controllers

USB devices need a controller: `-device qemu-xhci` (USB 3),
`-device usb-ehci` (USB 2), `-device pci-ohci` or
`-device piix3-usb-uhci` (USB 1.1), or an ICH9 EHCI with UHCI companions
(`ich9-usb-ehci1` plus `ich9-usb-uhci1` to `3` with `masterbus=`).  Then
add devices, e.g. `-device usb-kbd -device usb-tablet`.  With
`-machine q35,i8042=off` there is no PS/2 keyboard, so typing goes over
USB.

QEMU has no game controller of its own.  Pass the host's through
(`-device usb-host,vendorid=0x045e,productid=0x028e` for a wired Xbox 360
controller), or run a simulated one:

```bash
python3 tools/padpeer.py --port 10710 --kind xbox360 &    # or xboxone, hid
```

and add `-chardev socket,id=pad,host=127.0.0.1,port=10710 -device
usb-redir,chardev=pad` (with an xHCI controller).  The peer's control
port (the port plus 100, here 10810) takes lines such as
`buttons=0x1000 lx=20000` and moves the controller.

### Sound

Add `-device intel-hda -device hda-output` (or `hda-duplex` or
`hda-micro`, which add a line in or a microphone).  To record what
NovaOS plays instead of hearing it, use
`-audiodev wav,id=snd0,path=out.wav,out.frequency=48000` with
`-device hda-output,audiodev=snd0`.

USB speakers work too: `-audiodev wav,id=usbsnd,path=usb.wav -device
qemu-xhci -device usb-audio,audiodev=usbsnd` (or on `pci-ohci` or
`piix3-usb-uhci`; QEMU's `usb-audio` is full speed only, so it does not
work on a plain `usb-ehci`).

QEMU has no USB microphone and no high-speed audio device.
`tools/usbredirpeer.py` simulates one behind a `usb-redir` device: a USB
Audio Class 1 headset or microphone, or with `--uac2` a USB Audio Class
2.0 one.  Its options:

| Option | Meaning |
|--------|---------|
| `--port N` | The TCP port QEMU connects to (required) |
| `--speed high\|full` | Default `high` |
| `--speaker FILE.wav` | Write what the speaker plays |
| `--mic HZ` | The tone the microphone hears |
| `--uac2` | USB Audio Class 2.0 |
| `--rates`, `--channels`, `--mic-channels`, `--product` | Sampling rates, channel counts, product name |
| `--feedback HZ` | An asynchronous speaker that reports, through a feedback endpoint, that it plays HZ frames a second |
| `--slow-control MS` | Answer the first control request MS late |
| `--log-times` | Log the connection's events with times |

For example, a high-speed headset on EHCI whose microphone hears 523 Hz:

```bash
python3 tools/usbredirpeer.py --port 10700 --speaker headset.wav --mic 523 &
```

then `-chardev socket,id=ur,host=127.0.0.1,port=10700 -device
usb-ehci,id=ehci -device usb-redir,chardev=ur,bus=ehci.0`.  Use
`--speed full` for the other controllers.

The newest sound output and input are the defaults.  Settings' Sound
page chooses others and sets each device's volume; both are kept in the
registry on drive C:, so they survive a restart.

### More monitors

Each further display adapter is one more monitor (`-device
secondary-vga`), and so is each output of a virtio GPU with a monitor on
it, e.g. `-vga none -device virtio-vga,max_outputs=2,id=gpu`.  QEMU
connects an output when its display window, or a VNC client on it
(`-vnc :1,display=gpu,head=1`), asks for a size.  A VNC client asking for
0 x 0 disconnects it.  Both work while NovaOS runs.

### A 3D GPU (Venus and virgl)

With QEMU 9.2 or newer and a virglrenderer built with Venus,
`-vga none -device virtio-vga-gl,venus=on,blob=on,hostmem=1G` and an
OpenGL display (`-display sdl,gl=on` or `gtk,gl=on`) give NovaOS a
virtio-gpu whose Vulkan and OpenGL run on the host's GPU.  Ubuntu 24.04's
QEMU (8.2) has no Venus; `tools/ci/build-qemu-venus.sh PREFIX` builds
QEMU 10.2 and virglrenderer with it, as CI does.

Then install **Venus** from the App Store (Runtimes).  Vulkan programs,
Direct3D programs through DXVK, and OpenGL programs run on the host's GPU;
without such a GPU they use lavapipe and llvmpipe on the CPU.  NovaOS's
`opengl32.dll` picks Mesa's virgl (`opengl32_virgl.dll`, from Venus) on
that GPU and the built-in llvmpipe (`opengl32_mesa.dll`) otherwise;
`GALLIUM_DRIVER=virgl` or `GALLIUM_DRIVER=llvmpipe` chooses one.
`tools/build_venus.py OUT` builds the App Store's `venus.7z` (Mesa's
Venus and virgl with NovaOS's back ends, `third_party/mesa-venus`) with
MinGW-w64.

### Where your files are kept

Drive C: lives in memory.  NovaOS saves every change to an NTFS or FAT
volume a second later (and before Restart and Shut down) and restores it
at the next boot.  The files installed from the OS image
(`C:\Windows\System32`, `C:\Windows\SysWOW64`, `C:\Programs`) are never
saved, so a newer image always brings its own.  NovaOS uses the first of:

1. an NTFS or FAT volume labelled `NOVADATA` on any SATA or NVMe disk
   (including the data partition Install NovaOS creates);
2. an empty disk (all zeros at the start), which it partitions (MBR, one
   partition from 1 MiB) and formats as FAT32 `NOVADATA`;
3. the FAT volume it booted from (the one holding
   `\EFI\NOVA\kernel.elf`), under `\NOVA\C`.

Install NovaOS asks which file system drive C: gets: NTFS (the default)
or FAT32.

- **On NTFS**, C: is the whole volume.  It also keeps creation times and
  each file's security descriptor, so ACLs set with `SetFileSecurity` or
  `SetNamedSecurityInfo` (or given to `CreateFile`) are enforced and
  survive a restart.  A file without a descriptor of its own inherits
  from its folders, as on Windows.
- **On FAT**, C: is the folder `\NOVA\C`.  ACLs last only until restart.
  Hard links are saved as copies and joined again at the next boot
  (`\NOVA\LINKS.TXT`).

Saving runs on its own kernel thread and holds no lock while the disk is
written: it notes what changed under the file-system lock, then writes it
out while programs carry on.  On FAT, a crash or power cut during a save
leaves each file either as it was or as it was saved, never a mix: a
file's data and cluster chain reach the disk before its directory entry,
and the clusters of a replaced or deleted file are freed only afterwards.
Clusters a cut-off save left marked as used are reclaimed at the next
boot.

`run-qemu.sh` attaches `build/nova-data.img`, so your files survive
rebuilds of `nova.img`.  In the Terminal, `vol` shows where C: is saved
and `sync` saves it now.  To put programs on the data disk from the host,
use mtools:

```bash
mmd   -i build/nova-data.img@@1M ::/NOVA/C/Tools
mcopy -i build/nova-data.img@@1M rg.exe ::/NOVA/C/Tools/
```

`@@1M` is needed because NovaOS put the volume 1 MiB in, after the
partition table it wrote.  The ISO boots from a read-only CD: attach a
disk to keep files.

### Other drives (NTFS and FAT32)

Every partition of every disk (in an MBR or GPT, or a whole disk) is a
volume, as on Windows: `\Device\HarddiskVolumeN` (drive C:'s is 1) and
`\\?\Volume{GUID}\`.  Each NTFS and FAT volume becomes a drive of its
own, D:, E:, and so on in the order found, unless Disk Management gave
it another letter or none: the letters chosen are kept in
`HKLM\SYSTEM\MountedDevices`, as Windows keeps them, and come back at
the next boot.  Programs, the Terminal and File Explorer can list, open,
run, write, create, rename and delete files there.  FAT volumes on fixed
disks are writable; FAT volumes on USB sticks are mounted read-only.
NovaOS's own volumes (the one C: is saved to, the boot files) are left
alone: Disk Management shows them but does not format them or change
their letters.

- Creating, renaming and deleting reach the disk at once; a file's new
  contents when the program closes it, or after a quiet second.
- Compressed and sparse files are read but not rewritten (they show as
  read-only).  Encrypted files are not read.
- A write that would need more clusters than the volume has free fails
  with `ERROR_DISK_FULL`, and the file keeps what it had.
- A volume Windows left hibernated (Fast Startup included), with a chkdsk
  pending, or with transactions in its `$LogFile` is mounted read-only,
  as ntfs-3g does.

There is no journal.  `$LogFile` is emptied when a volume is mounted,
and each change is complete on disk when it returns, so Windows and
`ntfsfix` find the volume clean.  While changes are being written the
volume is marked dirty (`$Volume`'s flag, cleared a quiet second after
the last change and at shutdown).  A volume cut off half-way (power
loss, reset) is therefore found dirty at the next mount.  NovaOS writes
each change in an order that a cut can only leave in states the MFT
records can repair (allocation bitmaps out of step with the records, a
directory index behind its files).  It repairs those, clears the mark and
mounts the volume read-write ("was cut off while being written: checked,
..." in the log).  Anything else, such as a half-written record or a
cluster two files claim, keeps the volume read-only for chkdsk on
Windows.  A rewritten file gets new clusters before it frees its old
ones, so a cut leaves it whole, old or new.

`scripts/ntfs-dirty.py IMAGE [OFFSET_BYTES]` says whether a volume is
marked dirty.

To make and check a test disk (needs `ntfs-3g`):

```bash
scripts/make-ntfs-disk.sh build/nova-ntfs.img build
qemu-system-x86_64 ... -drive format=raw,file=build/nova-ntfs.img
```

`make-ntfs-disk.sh` makes a 128 MiB disk with one NTFS partition labelled
`NOVATEST`, holding what `drivetest` checks (and `hello.exe` and
`testdll.dll` from the build).  Run `drivetest` in the Terminal (and
`linktest D:\LinkTest`, which leaves two names of one file), shut down,
and check the disk on the host:

```bash
scripts/check-ntfs-disk.sh build/nova-ntfs.img
```

It runs `ntfsfix -n`, `ntfssecaudit -a` and `scripts/ntfs-check.py` (a
chkdsk-style check of the bitmaps, MFT records, directory indexes, link
counts and `$Secure`), and checks the files `drivetest` left behind.

---

## Tests

Tests run inside NovaOS under QEMU.  [testing.md](testing.md) says what
each suite and program covers, and [ci.md](ci.md) how CI runs them.  This
section says how to run them yourself.

### Running the self-tests

`tools/selftest.py` boots `build/nova.img`, types each test into the
Terminal and checks the output.  Its exit status is the number of failed
tests.

```bash
python3 tools/selftest.py                                  # the core suite
python3 tools/selftest.py --only apitest,guitest --out /tmp/st
python3 tools/selftest.py --suite devices --list           # print a suite's tests
```

| Option | Meaning |
|--------|---------|
| `--suite core\|graphics\|network\|devices` | The suite (default `core`) |
| `--only NAME,...` | Only these tests, by test name or by the command's first word.  In the network and devices suites, a boot with none of them chosen is skipped |
| `--img PATH` | The image (default `build/nova.img`) |
| `--out DIR` | Where the serial log, a screenshot after each test and the sound recordings go (default `selftest-out/`) |
| `--summary FILE` | Append a Markdown table |
| `--junit FILE` | Write JUnit XML |
| `--gfx DIR` | The graphics suite's files (below) |
| `--list` | Print the suite's tests and exit |

A test passes when the program exits with code 0 within its time limit,
prints no `FAIL` line and no non-zero "N failed" count, and prints what
the test expects.  A kernel panic stops the run.

| Suite | Boots | Needs on the host |
|-------|-------|-------------------|
| `core` | One (and the restarts some tests ask for), with an HD Audio card whose microphone hears 523 Hz, the ACPI tables in `tests/acpi/` and a USB keyboard on xHCI | `iasl` (`acpica-tools`); for the recording tests, `pulseaudio`, `pulseaudio-utils` and QEMU's PulseAudio backend (`qemu-system-gui` on Ubuntu).  Without PulseAudio the recording tests fail and the rest run |
| `network` | Three: virtio-net on QEMU's user network (IPv4), virtio-net on an IPv6-only network made by `tools/v6peer.py`, and an Intel e1000e | `node` and `openssl` (for `tools/h2server.js`) |
| `devices` | One per device the core boot does not have (`tests/selftest/devices/NAME/`) | `ntfs-3g` (`mkntfs`, `ntfscat`), `mtools`, `p7zip-full`, and network access: some boots download the Windows driver packages they install (virtio-win, FTDI, Intel Wi-Fi) |
| `graphics` | One, with a 3D virtio-gpu (Venus) and a second monitor | A QEMU with Venus and an OpenGL display, and the staged files (below) |

The devices suite's boots are `touch`, `usbaudio`, `usbheadset`,
`driversearch`, `monitors`, `usbboot`, `cdboot`, `laptop`, `update`,
`gamepad`, `hid`, `pnp`, `ntfs`, `wdm`, `edu`, `vioser`, `usbserial`, `ax211`,
`viostor`, `umdf`, `kmdfedu`, `netkvm` and `wddm`.  The docstring at the
top of `tools/selftest.py` describes each one.

### Accelerator: KVM or TCG

`tools/novarun.py` (and so `tools/selftest.py` and `tools/appcorpus.py`)
starts QEMU with `-accel kvm` when `/dev/kvm` is readable and writable,
and with TCG otherwise.  `NOVARUN_ACCEL=tcg` or `NOVARUN_ACCEL=kvm`
forces one.  The CPU model is `qemu64` with the same features under both
(SSSE3 to SSE4.2, POPCNT, RDTSCP, XSAVE, XSAVEOPT, AVX and F16C), so the
kernel's AVX state handling is tested either way.

In CI, `tools/ci/enable-kvm.sh` opens `/dev/kvm` to the runner and sets
`NOVARUN_ACCEL`.  The CI workflow's test jobs fall back to TCG with a
warning when KVM is not usable; the nightly app corpus and SMP stress
jobs require KVM unless started by hand with `allow_tcg`.  Each job's summary says which accelerator its VMs used.

### The graphics suite

The graphics suite needs a QEMU with Venus, an OpenGL display and a
directory of staged files: 7-Zip, the Mesa 3D and DXVK archives the App
Store lists, `venus.7z` and `vkd3d-proton.7z`, and the test programs
(`gltest`, `d3dtest`, `d3d12test`, `hlsltest`, `d2dtest`, `dwtest`,
`dw3test`).  On a machine without a screen, run it under `xvfb-run`:

```bash
sudo apt install p7zip-full gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 \
  g++-mingw-w64-x86-64 g++-mingw-w64-i686 mingw-w64-tools \
  ninja-build pkg-config libglib2.0-dev libpixman-1-dev libsdl2-dev libepoxy-dev \
  libgbm-dev libdrm-dev libvulkan-dev libpng-dev glslang-tools bison flex \
  python3-mako python3-yaml mesa-vulkan-drivers seabios ipxe-qemu xvfb xauth
pip install --user 'meson>=1.5' pycotap
sudo install -d -o "$USER" /opt/qv
tools/ci/build-qemu-venus.sh /opt/qv
tools/ci/stage-graphics.sh /tmp/gfx
NOVARUN_QEMU=/opt/qv/bin/qemu-system-x86_64 LD_LIBRARY_PATH=/opt/qv/lib/x86_64-linux-gnu \
  xvfb-run -a -s '-screen 0 1280x1024x24' python3 tools/selftest.py --suite graphics --gfx /tmp/gfx
```

`stage-graphics.sh` builds Venus (`tools/build_venus.py`) and
VKD3D-Proton (`tools/build_vkd3d_proton.py`, which needs `widl` from
`mingw-w64-tools`) unless `VENUS_7Z` and `VKD3D_7Z` name archives already
built.  Its second argument (default `DIR/cache`) keeps the downloads.

`NOVARUN_QEMU` names the QEMU `tools/novarun.py` runs (default
`qemu-system-x86_64` from `PATH`).  When a `virtio-vga-gl` or
`virtio-gpu-gl-pci` is among its devices it opens an SDL window with
OpenGL (`NOVARUN_GL_DISPLAY` changes the display option), and no window
otherwise.

**When a graphics test hangs.**  CI's graphics job runs two helpers
beside the suite, and they work locally too:

- `tools/ci/host_watch.py OUTDIR &` (needs `gdb` and `sudo`) writes
  `host-watch.log` every 15 s: the serial log's size and the CPU time and
  thread count of QEMU and of virglrenderer's render server processes.
  When the serial log has been silent for six minutes it writes
  `host-hang-N.txt` (each host thread's state, wait channel and CPU use,
  gdb backtraces of QEMU and each render process, the guest's CPU
  registers with the kernel function each RIP is in, its local APIC timer
  and the kernel lock `g_bkl`, the render server's processes, the host
  kernel's messages about crashed or killed processes, and
  `info virtio-queue-status`) and `serial-hang-N.log`.
- `tools/ci/host_profile.py OUTDIR &` prints a `host profile:` line for
  each turn of `gltest fps` (also kept in `host-profile.log`): the turn's
  wall time and the CPU time per second of each group of QEMU's threads.

Set `VIRGL_LOG_LEVEL=debug` and
`VIRGL_LOG_FILE=OUTDIR/virglrenderer-%PID%.log` for virglrenderer's own
logs.  How to read them: a QEMU whose threads all wait while its CPU
time stands still has lost a wake-up; one that spins is busy.  A request
the guest sent that QEMU's used index has not caught up with was taken
and never answered by virglrenderer.  In the guest, Ctrl+Alt+F12 logs the
virtio-gpu control queue (`[VGPU]` lines), and the kernel logs
`[WATCHDOG] the kernel lock has been held for ...` by itself after three
seconds.  The self-tests stop at the first test after which the machine
answers neither Ctrl+Alt+F12 nor Ctrl+C, instead of waiting out every
later test's time limit.

### Adding a self-test

Add a file to `tests/selftest/core/` (or `graphics/`, `network4/`,
`devices/NAME/`, ...).  Tests run in file-name order, so the number
prefix places it.  In the core suite the restart is at 130, and
`crash kernel`, which halts the kernel on purpose, is at 900 with
`restart=True` (the machine is then reset); only tests that check what
the crash left come after it, at 905.

```python
# tests/selftest/core/166-mytest.py
DOC = '`mytest` (what it covers)'      # its entry in testing.md's core-suite list
TESTS = [
    Test('mytest', 'mytest', [r'mytest: \d+ passed, 0 failed']),
]
```

A new self-test program also adds a row to the table below: one file in
`docs/selftests/` ([CONTRIBUTING.md](../CONTRIBUTING.md)).  Run
`python3 tools/selftest.py --list` to check the suite loads.

### Self-test programs

These are in `userland/programs/`, installed in `C:\Programs` and, for
those built for 32 bits too, in `C:\Programs\x86`.  Type the name in the
Terminal; each prints "N passed, 0 failed".  Each row of this table is a
file in `docs/selftests/`.

<!-- BEGIN generated:selftest-table -->

| Program | Covers |
|---|---|
| `crttest` | The C runtime |
| `filetest` | Files and directories; delete on close (`FILE_FLAG_DELETE_ON_CLOSE`, `FileDispositionInfo`, `DeleteFile` of an open file: deleted when the last handle closes, meanwhile `ERROR_ACCESS_DENIED`; a running program's file is refused); `RegNotifyChangeKeyValue` (values, subkeys, subtrees, deleted keys, synchronous); `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)`.  `filetest install`, a restart and `filetest installed` check that a running program replaced at boot |
| `linktest` | Hard links: `CreateHardLink`, the link count and file id by every name, a write or attribute change by one name seen by the others (open handles too), a link in another folder renamed there, deleting names down to the last, a file replaced under a linked name, `FILE_SUPPORTS_HARD_LINKS`, every name listed by `FindFirstFileNameW`/`FindNextFileNameW` (`FileHardLinkInformation`, the folders opened by `OpenFileById`'s `FILE_OPEN_BY_FILE_ID`).  `linktest restarted [DIR]` after the restart checks the three names it left (one in another folder) are still one file, its link count and names whole before that folder is read.  The core suite runs both on drive C:; the devices suite's "ntfs" boot runs them on a fresh NTFS drive D: with a restart between, then checks the volume offline (`scripts/ntfs-check.py`, `ntfscat`) |
| `sectest` (x64) | Hostile system calls refused (kernel pointers, bogus handles, bad descriptors); tokens and object security through the native API: a restricted or deny-only impersonation token is refused a protected named event |
| `threads` | Threads, synchronization, SEH |
| `dlltest` | DLL loading, TLS, `DllMain`; a DLL whose `DllMain` returns FALSE stops the program at start with 0xC0000142; exports Windows numbers (uxtheme, d2d1) and what LibreOffice imports at start |
| `posixtest` | The POSIX layer in msvcrt |
| `apitest` | kernel32, advapi32, bcrypt, shell32, shlwapi, psapi, user32/gdi32, the registry; placeholders and write-copy views, ntdll lists and bit maps, urlmon MIME sniffing |
| `consolecursortest` | Classic console screen buffers: cursor position and visibility, cell reads/writes, fills, scrolling, attributes, resizing, independent buffers, and invalid handles/coordinates; x64 and x86. |
| `ptyscreentest` | A full-screen program (alternate screen, colours, wide characters, erasing, scroll regions, inserted and deleted lines, wrapping, cell writes, fills and scrolling) drawn in a console window and through a pseudo console: both runs read back the same cells, and the pseudo console's output drawn by a host gives them too. |
| `conptytest` | A terminal host runs an interactive `cmd.exe` through `CreatePseudoConsole`: typed commands are echoed and edited with Backspace, Ctrl+C drops a line, VT key sequences (arrows, Delete, F-keys) reach a raw-input program as keys, `ResizePseudoConsole` while `cmd.exe` runs gives programs the new size and draws the screen again, the host's drawing matches the pseudo console's cells, and `exit` ends `cmd.exe` and the output pipe. |
| `abitest` (x64) | The binary interface against Windows 10 1903 x64: TEB, PEB, process parameters, loader lists, `KUSER_SHARED_DATA`, `CONTEXT` and `EXCEPTION_RECORD` offsets, ntdll's stub bytes and every system-call number (`nt1903_services.h`), raw `syscall`s |
| `syscalltest` (x64) | The system-call table as code that calls the kernel without ntdll sees it (Roblox's Hyperion): ntdll's `Zw` exports ranked by address give each service its Windows 10 1903 number, and the kernel answers that number (and `STATUS_INVALID_SYSTEM_SERVICE`, not the wrong service, for one it lacks) |
| `comtest` | ole32/oleaut32, `IShellLink` |
| `tlbtest` | COM type libraries: `LoadTypeLib` on `testdll.dll`'s embedded library, `ITypeLib`/`ITypeInfo`/`ITypeComp`, registration, `ITypeInfo::Invoke`, `DispCallFunc`, `CreateStdDispatch` |
| `cppeh` | C++ exceptions and RTTI |
| `unwindtest` (x64) | `RtlUnwindEx` as MinGW's C++ runtimes (LLVM's libunwind, GCC's libgcc) use it: a throw caught two frames up through a collided unwind to the landing pad with an unfilled CONTEXT (no trap flag, the frame's registers, RAX and RDX for the landing pad), a cleanup frame in between, and `_Unwind_Resume`'s unwind from no handler; and as Microsoft's own `vcruntime140` uses it: a catch block run by a consolidating unwind's callback (`STATUS_UNWIND_CONSOLIDATE`), and an exception raised inside that catch block reaching the catching frame's handler and then the caller's `__except`; an `__except` inside a `__try`/`__finally` of the same frame (the `__finally` runs once, after, also when the `__except` body raises again, as Microsoft's C++ runtime re-raises a rethrow); an unwind started inside a frame handler to a frame below the one being unwound (the C++ runtime's `_local_unwind` in its own frame handler) lands there and the outer unwind carries on |
| `stltest` | The C++ standard library (`msvcp140.dll`, `msvcp140_1`, `msvcp140_atomic_wait`) built as Visual Studio builds a program: strings, containers, streams and locales, exceptions and `exception_ptr`, threads, mutexes, condition variables, `std::async`, atomic waits, `pmr`, `std::filesystem` and `fstream`, `to_chars`, `std::format`, `std::regex`; `msvcp140.dll`'s version (14.42, which OBS Studio checks is 14.40 or later) |
| `rttest` | The UCRT's C99 complex functions with MSVC's `_Dcomplex`/`_Fcomplex` (as NumPy calls them), `_cprintf`/`_cputs`, the DLL search directories: `AddDllDirectory`, `RemoveDllDirectory`, `SetDllDirectory` (a child started with `_spawnl` inherits its directory and the C runtime descriptors, here a pipe's write end by number, and gets its arguments as they are), and the secure scanf as MSVC's headers inline `sscanf_s` (`__stdio_common_vsscanf` with `_CRT_INTERNAL_SCANF_SECURECRT`: sizes after `%s`, `%c` and `%[`, none after `%*s`, too long a string failing with the buffer emptied) |
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
| `pipetest` | Pipes, inherited handles, `cmd /c`, `_popen`, overlapped I/O, each end's process and session ids |
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
| `tpsimpletest` | Warms the loader's one-time contention event, then runs 5120 one-shot thread-pool callbacks, checking their context and calling convention and that the process handle count does not grow; x64 and x86 |
| `powertest` | The lid, a thermal zone and processor throttling (`GetPwrCapabilities`, `ThermalInformation`, `ProcessorInformation`, `LastSleepTime`/`LastWakeTime`): closing the lid sleeps (its event and its wake come through a GPE block device), and heating the zone past its passive trip point throttles CPU 0 to its deepest T-state (`MhzLimit` 25%) until it cools; needs `tests/acpi/lid-thermal.asl` and the self-test's help (see above) |
| `waitmigrationtest` | Yielding workers repeatedly perform timed wait-any and wait-all operations on unsignaled events, exercising scheduler migration and waiter registration; x64 and x86 |
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
| `soundtest info` (the device IDs, device 0 the default, and the preferred IDs), `tone` (`rate=N` for another rate than 22.05 kHz), `float`, `wasapi`, `godot` (WASAPI the way Godot's driver plays: `IAudioClient3`, `InitializeSharedAudioStream`, a polling thread), `both`, `play FILE`, `ding`, `msgbeep`, `beep` | Sound output (needs an HD Audio card; see below) |
| `soundtest record FILE [MS]`, `mme FILE [MS] [STALL]`, `capture FILE [MS]`, `volume` | Recording through `waveIn` (`mme`: as PortAudio's MME host, which Audacity uses, records, its thread held up STALL ms a second; an input overflow fails) and WASAPI capture into a WAV, and `IAudioEndpointVolume` (needs a card with an input) |
| `soundtest dsound [HZ] [MS]`, `dscapture FILE [MS]` | DirectSound: a streaming buffer refilled at position notifications, a static one at twice its frequency, and DirectSoundCapture (made with `CoCreateInstance`) recording into a WAV |
| `soundtest endpoints`, `default out\|in NAME`, `level out\|in [LEVEL]`, `wovolume`, `dsenum` (and `dev=NAME` on the other commands) | The sound devices: the WASAPI endpoints and the default, choosing the default as Settings' Sound page does, each device's own endpoint volume, a program's `waveOutSetVolume` on one device ID, and DirectSound's device list; `dev=NAME` plays on or records from a named device through `waveOut`, `waveIn`, WASAPI and DirectSound |
| `xa2test [HZ] [MS]`, `xa2test devices [NAME HZ MS]` | XAudio2 2.9 (source and mastering voices, voice and engine callbacks, end of stream, a volume meter in an effect chain), XAudio2 2.7 made with `CoCreateInstance` (a looped buffer through a submix voice), and X3DAudio panning; `devices` lists XAudio2 2.7's devices and plays through 2.9 on the one named |
| `miditest [DIR]` | MIDI: the `midiOut` device and a GM reset sent with `midiOutLongMsg`, then a flute note; a `midiStream` buffer with a tempo and time division, `MOM_POSITIONCB` and `MOM_DONE` callbacks and the stream position; and a MIDI file written to DIR and played through the MCI sequencer (`open`, `status length`, `play wait`, `status mode`) |
| `disktest write`, restart, `disktest verify` | Drive C: surviving a reboot |
| `httptest suite HTTPS-BASE HTTP-BASE` | winhttp against `tools/h2server.js`: HTTP/2 by ALPN, a 300 KB body, POST, a redirect, an untrusted certificate refused, chunked HTTP/1.1, the asynchronous API.  `httptest [-2] [-k] [-a] URL` fetches one URL |
| `netcat [-4\|-6] [-p PORT] HOST [PATH]` | Winsock: `getaddrinfo`, IPv4 or IPv6 sockets, an HTTP/1.0 GET |
| `looptest` | Winsock over the loopback interface: a socket pair over 127.0.0.1 and ::1 (port 0, `getsockname`, a non-blocking connect, `getpeername` after it, data sent before `accept`, the accepted socket inheriting non-blocking mode), `shutdown(SD_BOTH)` with unread data, closing a listener with a queued connection, `localhost`, `wsock32.dll`'s Winsock 1.1 ordinals, and socket options: `SO_RCVBUF`, `SO_SNDBUF`, `SO_REUSEADDR`, `SO_KEEPALIVE`, `SO_BROADCAST`, `TCP_NODELAY`, `SO_RCVTIMEO`, `SO_SNDTIMEO`, `SO_LINGER`/`SO_DONTLINGER` and `IP_TTL` read back what was set, `SO_TYPE`/`SO_ERROR`/`SO_ACCEPTCONN` answer, and five change behaviour: `TCP_NODELAY` sends the second of two small writes at once (under 30 ms, against Nagle's wait for the delayed ACK), `SO_RCVTIMEO` and `SO_SNDTIMEO` of 300 ms end a blocked `recv` and `send` with `WSAETIMEDOUT`, `SO_LINGER {1, 0}` resets the connection on close, and `SO_REUSEADDR` lets two sockets share a port; `connect` on a UDP socket (`send` and `recv` use the peer, `getsockname` names 127.0.0.1, a stranger's datagram is dropped), `IPV6_V6ONLY` (on in a new IPv6 socket, so `[::]` binds the port an IPv4 socket has on `0.0.0.0`; turned off, `[::]` gets a datagram sent to 127.0.0.1; no option of an IPv4 socket), and `WSADuplicateSocket` into a child process that makes the socket with `WSASocket(FROM_PROTOCOL_INFO)` and sends on it after the parent closed its copy |
| `dltest [-n N] [-t S] [-w] HOST PORT BYTES` | A long download over N connections at once (up to 16), read through `select()` as libcurl's multi interface reads them, from `tools/h2server.js`'s `/stream/BYTES/SEED`; every byte is checked against the pattern.  `-w` also does what an installer does meanwhile: writes each download to a file in `C:\Temp\dltest` while two threads map files over and over.  It fails if the transfer as a whole makes no progress for S seconds (default 5), and prints the throughput and the longest pause |
| `overlaptest` | Overlapped sockets on an I/O completion port over 127.0.0.1, as Python's asyncio uses them: `AcceptEx`, `ConnectEx` and the other extension functions from `WSAIoctl`, an `AcceptEx` and a `WSARecv` pending until a connection or data comes, `CancelIoEx`, closing a socket with a receive pending, and `WSAIoctl`'s `FIONREAD` (the bytes waiting, as Qt asks before every read) and `SIOCATMARK`; the `AcceptEx` and `GetAcceptExSockaddrs` it uses are `wsock32`'s, by ordinal, as Steam imports them |
| `dhcp6test` | A stateful DHCPv6 lease (RFC 8415) through `GetAdaptersAddresses`, 64- and 32-bit, on the network suite's IPv6 network: the leased address with `Dhcp` prefix and suffix origins, finite valid and preferred lifetimes and a /128 on-link prefix, beside the SLAAC address; the adapter's `Dhcpv6Server` (link-local, scoped), `Dhcpv6ClientDuid` (a DUID-LL of its MAC), `Dhcpv6Iaid` and `IP_ADAPTER_IPV6_MANAGE_ADDRESS_CONFIG`; `DnsSuffix` and the `FirstDnsSuffix` list from the domain search list; and the DHCPv6 DNS server |
| `loadtest` | Winsock under a browser's or a game client's load: 200 loopback connections (401 sockets) open at once in one process, one `select` over all 200 (a program's own `FD_SETSIZE` of 1024) and `WSAPoll` over all 200, a `WSAEventSelect` registration on each socket whose event fires on data, 200 new sockets registering again after `closesocket` ended the old ones, 400 connections opened and closed one after another (each leaves a TIME-WAIT behind), 100 UDP sockets open at once, and 24 parallel HTTP downloads of 300,000 bytes from `tools/h2server.js`, checked byte for byte; on the old limits it failed with `WSAENOBUFS` after 8 connections |
| `msitest` | Windows Installer: `MsiDatabaseApplyTransform` and `TRANSFORMS=` (a transform for another product refused with 1624), `msiexec /p` and `/uninstall` of a patch (1642 when its product is missing), rollback of a package that fails half-way (files put back, new files, folders and keys removed, its rollback custom action run; `DISABLEROLLBACK` keeps what was done), an automatic service that `services.exe` starts at the next boot, and (`msitest script`) JScript and VBScript custom actions of every source type setting properties, reading them back and writing files, and a failing script failing its install.  The packages come from `tools/msitest/mkpkg.py` (`C:\Tests\Msi`) |
| `gl2test` | OpenGL 2 and later with nothing installed: NovaOS's `opengl32.dll` hands the program to the built-in Mesa llvmpipe, which says OpenGL 3.0 or later and "llvmpipe"; the OpenGL 2 shader functions come from `wglGetProcAddress`, a GLSL vertex and fragment shader compile and link, a triangle drawn with them reads back green inside and black outside, `SwapBuffers` shows it in the window, and `wglCreateContextAttribsARB` makes an OpenGL 3.3 core-profile context; a frame drawn into a child window through the DC taken before the window moved shows where the window is now, as VLC's OpenGL video output needs (64- and 32-bit) |
| `glgeneric` | With no OpenGL driver (`GALLIUM_DRIVER=generic`: Mesa's llvmpipe is built in), `opengl32.dll` falls back to its own OpenGL 1.1, as Windows does without a display driver's OpenGL: a double-buffered RGBA pixel format, a context made current on a window, `glGetString` saying "GDI Generic" and 1.1, `glClear` read back with `glReadPixels`, and `SwapBuffers`; and 2D drawing as ScummVM does it: a texture uploaded and updated, a textured quad from vertex arrays through an orthographic projection, blending (the edge two triangles share blended once), and a scissored `glBegin`/`glEnd` quad (64- and 32-bit) |
| `cmdlinetest` | Long command lines: `cmdlinetest len=N sum=S ARG...` checks that `GetCommandLineW`/`GetCommandLineA` hold all N characters and that the CRT's `argv` (quoted paths with spaces, escaped quotes, trailing backslashes) hashes to S and ends in `END`; `cmdlinetest spawn` starts itself with `CreateProcessW` (1,000, 8,191 and 32,766 characters, with characters outside ASCII), `CreateProcessA` and `cmd.exe /c` (8,000), and checks that 32,767 characters fail with `ERROR_FILENAME_EXCED_RANGE`; and starts a copy of itself from an unquoted path with spaces with no program name (`CreateProcessW` tries the path at each space, as Windows does), suspended in a job whose completion port must report `JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO` once it exits (as Godot's console launcher starts its game) |
| `crashtest` | Crash reports: starts `crash.exe` (from its own folder, so the 32-bit build starts the 32-bit one), checks it ended with `STATUS_ACCESS_VIOLATION`, and reads its report in `C:\NovaOS\Crashes`: program and PID, exception, `crash.exe+offset`, address 0, return addresses, the module list and the end of the kernel's log |
| `fpstate` | The floating-point state a thread runs with: x87 control word `0x27F` and MXCSR `0x1F80` on the main thread and on a new thread whatever its creator set, each thread's own state kept across context switches, `RtlCaptureContext` saving it into `FltSave` and `RtlRestoreContext` putting it back (MXCSR from `CONTEXT.MxCsr`), the state unchanged after an SEH unwind, inexact SSE and x87 results without a fault, an unmasked x87 divide by zero caught as `EXCEPTION_FLT_DIVIDE_BY_ZERO`, and an unmasked SSE one where the CPU traps (KVM, real hardware) (64- and 32-bit) |
| `tztest` | Time zones: the zone in effect and its offset in `GetLocalTime` against `GetSystemTime` and in the C runtime's `localtime`; then zones from the list by name (`GetTimeZoneInformationForYear`) at known instants: Berlin and Sydney in summer and winter, the minute Berlin and Los Angeles spring forward, Tokyo and India, each converted back with `TzSpecificLocalTimeToSystemTime` |
| `kbdtest` | Keyboard layouts: the layout in effect (`GetKeyboardLayout`, `GetKeyboardLayoutName`); US, French AZERTY and German by handle (`ToUnicodeEx`, `MapVirtualKeyEx`, `VkKeyScanEx`, a dead circumflex, `LoadKeyboardLayout`) and the registry's `Keyboard Layouts` list; `kbdtest 00000407` checks German in effect (Y and Z swapped, ö, AltGr+Q and AltGr+E, the dead acute accent, Ctrl+A, key names, `SPI_GETDEFAULTINPUTLANG`, `Preload`), and `us` after it puts US back with `SPI_SETDEFAULTINPUTLANG` and waits for the system to follow |
| `edgeupdtest` | The APIs Microsoft Edge Update (the WebView2 runtime's installer) calls: Task Scheduler 2.0 (`ITaskService` connects; a task registered from its XML is a file in `C:\Windows\System32\Tasks`, reads back by name, path and XML, disables and enables, runs its `<Exec>` action, is listed, and is deleted; malformed XML, `TASK_CREATE` of an existing task and `TASK_UPDATE` of a missing one fail as on Windows), `CryptProtectData`/`CryptUnprotectData` with the user's and the machine's key (the wrong entropy and a changed blob fail), `UrlCombineW` on RFC 3986's examples, `UrlEscapeA`, `UrlUnescapeA`, `PackageFamilyNameFromFullName`, `PackageIdFromFullName`, `GetPackagesByPackageFamily`, `WTSEnumerateSessionsW`, `MakeAbsoluteSD`, `IsDeviceRegisteredWithManagement`, `NetGetAadJoinInformation`, `CoRegisterPSClsid` and `CoGetCallContext` (64- and 32-bit); and cmd's `type` showing a UTF-16 file as text |
| `ramdisktest` | Files on drive C: (kept in memory) take the memory their contents need, measured as C:'s free space: 33 MiB written in 64 KiB appends takes about 33 MiB once closed (not the 64 MiB buffer it grew into), `SetEndOfFile` to 72 MiB takes 72 MiB (not 128 MiB) and reads back as zeros past what was written, two files appended in turn, a file made shorter and longer again and a closed file appended to keep their contents, deleting the files gives the memory back, and programs are told the machine's real memory: `GlobalMemoryStatusEx`'s total is `NtQuerySystemInformation`'s physical pages, its free memory is C:'s free space and falls by what the files take (it said 512 MB total, 256 MB free) |
| `setuptest` | What installers need: the desktop user's token is the limited half of an administrator's and UAC is on (`CheckElevationEnabled`, the `EnableLUA` policy value, which NSIS's UAC plug-in reads), ShellExecuteEx's `runas` and a `requireAdministrator` manifest (`setupadmin.exe`) start a program elevated (`TokenElevation`, Administrators enabled, high integrity) while `CreateProcess` does not; `msftedit.dll`'s `RICHEDIT50W` and `riched20.dll`'s `RichEdit20W`/`RichEdit20A` take RTF by `EM_STREAMIN`, `EM_SETTEXTEX` and `WM_SETTEXT` and answer `EM_GETTEXTEX`, `EM_GETTEXTRANGE`, `EM_FINDTEXTEXW` and `EM_STREAMOUT`; `VarAdd` ... `VarCmp`; `AddFontResource` of a bare file name finds the font in the Fonts folder; `TaskDialogIndirect` shows custom buttons (command links) and returns the one pressed; the ShellLink object's `IPropertyStore` keeps an AppUserModelID; `CryptProtectMemory` and `RtlEncryptMemory` round-trip; `setuptest bigfile` writes a 300 MB file on C: |
| `msxmltest` | MSXML (`msxml6.dll`, 64- and 32-bit): `CLSIDFromProgID` and `CoCreateInstance` for `Msxml2.DOMDocument`, `Msxml2.DOMDocument.6.0`, the free-threaded document, `XMLHTTP` and `SAXXMLReader`; `loadXML` and `load` of a WiX Burn manifest read back with `selectSingleNode`/`selectNodes` (XPath with `SelectionNamespaces`, and MSXML 3's XSL Patterns, where Burn's unprefixed `Payload` or `Chain/MsiPackage` match elements in the manifest's default namespace and prefixes match as written, while MSXML 6's XPath matches neither), `getElementsByTagName`, attributes and `text`; a document built element by element, as Edge Update builds its requests, serialised by `xml` and `save`; `parseError` codes, lines and reasons; UTF-16 and windows-1252 input; MSXML 3's and 6's defaults (`ProhibitDTD`, `resolveExternals`, whitespace); `importNode`; a SAX parse; the 6.30 version resource |
| `cabtest` | `cabinet.dll`'s File Decompression Interface as installers use it (WiX Burn extracts the Visual C++ Redistributable's payloads with it): `FDIIsCabinet` reads a cabinet's header (folders, files, set ID, whether a next cabinet follows) and refuses a text file; `FDICopy` extracts an MSZIP and an LZX cabinet (window 2^16, x86 call translation on, a file spanning three data blocks) through the caller's own open/read/write/seek functions, with `fdintCABINET_INFO`, `fdintCOPY_FILE` (sizes, dates) and `fdintCLOSE_FILE_INFO` and the caller's `pvUser`; a file the caller skips is not written and the next still comes out; a cabinet 4,321 bytes into another file (Burn's attached container) extracts through the caller's offset; a set of two cabinets whose folder (and a data block) goes on into the second asks `fdintNEXT_CABINET` once and extracts the files of both; the second cabinet alone reports `fdintPARTIAL_FILE`; a missing, foreign or damaged cabinet and a caller's abort fail with `FDIERROR_CABINET_NOT_FOUND`, `FDIERROR_NOT_A_CABINET`, `FDIERROR_CORRUPT_CABINET` and `FDIERROR_USER_ABORT`; 64- and 32-bit |
| `authtest` | Authenticode, with test files `tools/authenticode/mktests.py` signs under a test root the test adds to the `ROOT` store and removes again: `WinVerifyTrust` (`WINTRUST_ACTION_GENERIC_VERIFY_V2`, as Steam's service calls it) accepts SHA-256 and SHA-1 signatures, a 32-bit program, a nested SHA-256 signature, and an expired signer saved by a PKCS #9 countersignature or an RFC 3161 token from within its validity; it refuses a changed code byte and a damaged signature (`TRUST_E_BAD_DIGEST`), a root nobody trusts (`CERT_E_UNTRUSTEDROOT`, also before the root is added and after it is removed), an expired signer without or with a late timestamp (`CERT_E_EXPIRED`), a TLS server certificate (`CERT_E_WRONG_USAGE`), an unsigned program (`TRUST_E_NOSIGNATURE`), a text file (`TRUST_E_SUBJECT_FORM_UNKNOWN`), an unknown action, and a revocation check, which fails offline (`CERT_E_REVOCATION_FAILURE`); `WINTRUST_SIGNATURE_SETTINGS` counts and verifies nested signatures; the provider state (`WTHelperProvDataFromStateData`, `WTHelperGetProvSignerFromChain`, `WTHelperGetProvCertFromChain`) gives the signer's chain and the timestamp's time; `CryptQueryObject` opens the embedded signature, `CryptMsgGetParam` its signer, `CertGetSubjectCertificateFromStore` the certificate, `CertGetNameString` and `CertNameToStr` the names; `CryptCATAdminCalcHashFromFileHandle` gives the Authenticode SHA-1 and no catalog holds it; `CertVerifyCertificateChainPolicy` with the Microsoft root policy (as Edge Update checks its packages) refuses the test root, accepts Microsoft's code signing CA 2024 chained to the 2011 application root only with `MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG`, and always accepts the 2010 root; 64- and 32-bit |
| `gatetest` (x64) | Windows' x64 segment layout and the self-inspection anti-cheat code (Roblox's Hyperion) leans on: CS 0x33, SS/DS/ES/GS 0x2B and FS 0x53, also in `RtlCaptureContext`; a far jump to 0x23 runs 32-bit code (`dec eax`, not a REX prefix) in the 64-bit program and a far jump to 0x33 comes back; 32-bit code at 0xFFFFFFFE wraps to address 0 and faults, and the handler sees an instruction-fetch access violation at 0 with `SegCs` 0x23 and resumes in 64-bit code; `NtGetContextThread` on the calling thread returns the registers its system call left with (RBX and R12 set before the call, RIP in ntdll's stub, debug registers clear, `ContextFlags` kept) and `SetThreadContext` on it resumes at the CONTEXT; `__fastfail` ends a child with `STATUS_STACK_BUFFER_OVERRUN` without running its handlers |
| `msiqtest` | Windows Installer queries as bootstrappers (WiX Burn) make them, from a 64- and a 32-bit process: a 32-bit package (Template `Intel`) puts its `SystemFolder` file in `SysWOW64` and a 64-bit one in `System32`, whichever process runs the engine; `MsiGetProductInfoEx` by context (`VersionString`, `State`, `PackageName`, `PackageCode`, `LocalPackage`; the length alone, `ERROR_MORE_DATA`, 1605 in a context the product is not in, 1608 for an unknown property, bad contexts refused) and the ANSI form; `MsiEnumProductsEx` (all contexts, one product, per-machine with no SID); the source list the install registered and `MsiSourceListAddSourceEx` placing, moving and appending network sources and a URL, `GetInfo`/`SetInfo` (`PackageName`, `LastUsedSource`, `LastUsedType`) and `ClearSource`; `MsiDetermineApplicablePatches` and `MsiDeterminePatchSequence` on a patch file and applicability XML (1642 for a patch for another product, 1635 for a missing one); `MsiEnumPatchesEx`, `MsiGetPatchInfoEx` and a patch's own source list on a patched product; an upgrade package whose Upgrade table removes the version below its range and does not take it for a later one (its block action stays off); removal taking the registration and source list with it |
| `ndrtest` | rpcrt4's NDR engine, in one process: `ndrtestps.dll`, a proxy/stub DLL made with widl, registers itself (`DllRegisterServer`, `NdrDllRegisterProxy`), is found again through `CoGetPSClsid` and `CoGetClassObject` (`NdrDllGetClassObject`), and makes an interface stub around a test object and a stubless proxy joined by a channel that hands each request to the stub's `Invoke` (`NdrStubCall2`).  Calls carry strings, conformant arrays in and out, structures by value, `[in, out]` and with an embedded string, conformant structures, `[unique]` pointers, `hyper`, `double` and `float` arguments in registers and on the stack, `[out]` arrays the proxy allocates, a base interface in another proxy file (forwarding proxy and delegating stub), the server's HRESULT and a crash in the server (`RPC_E_SERVERFAULT`); every buffer goes back to the channel and `DllCanUnloadNow` agrees once everything is released.  64- and 32-bit |
| `regtest` | machine-wide 32- and 64-bit registry views are isolated, `WOW6432Node` exposes the 32-bit view, base-key handles retain an explicitly selected view across relative opens, and `Classes` is shared; then six threads and four waves of four child processes open and close a shared key, create and delete their own subkeys, delete keys others hold open (`ERROR_KEY_DELETED` on those handles), duplicate key handles within a process and into a child, set, read and delete the values of one key at once, enumerate while subkeys come and go, leave change notifications pending on keys they close, and end (the children) with keys and watches still open, while a thread waits synchronously for changes; the children inherit a key handle; afterwards the shared key is whole and the tree deletes cleanly; 64- and 32-bit |
| `d3d9test` | NovaOS's own `d3d9.dll`, 64- and 32-bit (graphics suite): it is the system folder's and has every export Windows' has; before DXVK is installed `Direct3DCreate9` and `Direct3DCreate9Ex` give NovaOS's object: an adapter per monitor, named by the display card's PCI vendor and device IDs (not 0, which Qt's GPU blocklist takes for "Standard VGA" and turns OpenGL off for), with `\\.\DISPLAY1`, the current mode and the 32-bit modes, the monitor and an adapter LUID, but no device (`CheckDeviceType`, `GetDeviceCaps` and `CreateDevice` give `D3DERR_NOTAVAILABLE`); `Direct3DCreate9On12` and `Direct3DShaderValidatorCreate9` return NULL, and the PIX markers count open events with no profiler attached (`D3DPERF_GetStatus` 0); with the App Store's DXVK (`d3d9_dxvk.dll`) the calls reach DXVK, whose `IDirect3D9` lists an adapter |
| `svctest` | A real service through the service control manager, installed as Steam's is (a name with spaces, `"path" /service`): `CreateService`, `StartService` with two arguments that `ServiceMain` receives after the service's name, `StartServiceCtrlDispatcher`, `RegisterServiceCtrlHandlerEx` with its context, `SetServiceStatus` from `SERVICE_START_PENDING` to `SERVICE_RUNNING`, `QueryServiceStatusEx` (its own process id), `ERROR_SERVICE_ALREADY_RUNNING`, interrogate, a user-defined control, a control the service does not accept refused with `ERROR_INVALID_SERVICE_CONTROL` before it reaches the handler, stop (the process ends), `DeleteService`; the program run by hand, and a program the service starts before it reports itself running, both get `ERROR_FAILED_SERVICE_CONTROLLER_CONNECT`; the service (a `LocalSystem` one) runs elevated though the test is not; `CopyFile` keeps the source's last-write time, size and read-only attribute; 64- and 32-bit |
| `chrometest` | What Chromium (Steam's browser) needs before it starts its child processes: a section created with an empty DACL, named or not, cannot be duplicated from a read-only handle to `FILE_MAP_WRITE` (`ERROR_ACCESS_DENIED`) while a writable handle can, `DUPLICATE_SAME_ACCESS` keeps a handle read-only, `NtQueryObject` reports each handle's own `GrantedAccess` (the read-only section handle's is `FILE_MAP_READ | SECTION_QUERY`; a file opened `GENERIC_READ` has `FILE_GENERIC_READ` and no write right, which Chromium's browser `CHECK`s), a section without a descriptor allows the wider duplicate, and the named one cannot be opened by name; `NtQuerySection` gives the size and no `SEC_IMAGE`; `CreateProcess` with `STARTUPINFOEX` and `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` gives the child the listed handles (it reads the shared text through them and cannot write it) and not an inheritable event left off the list; the attribute list's size and errors; the ordinal exports Chromium imports by number (shlwapi `IsOS` 437 and `QISearch` 219, oleaut32 `VarUI4FromStr`, `VarBstrCat`, `VarBstrCmp`, uxtheme `DrawThemeBackgroundEx` 47, shell32 `SHChangeNotifyRegister` 2 and `SHChangeNotifyDeregister` 4) and a register/deregister round trip; `api-ms-win-power-base-l1-1-0.dll` loads `powrprof.dll`; `GetFileVersionInfo` finds `kernelbase.dll` and `kernel32.dll` by bare name and reads Windows 10 build 18362 from them; `EvtQuery` of the System log returns no events, a bookmark and a subscription after it (no events come), `EvtOpenLog` and `EvtGetLogInfo` (no records) and `EvtOpenPublisherMetadata` (no metadata); `GetFirmwareType` (UEFI), `GetConsoleDisplayMode`, `CertControlStore`, `CertCompareCertificateName` and `CryptVerifyCertificateSignatureEx` on the root store's certificates; WinHTTP's proxy resolver needs an asynchronous session and completes later with "autodetection failed"; 64- and 32-bit |
| `comoop` | COM between processes: `CoCreateInstance` with `CLSCTX_LOCAL_SERVER` starts `comserver.exe` from its `LocalServer32` key with `-Embedding`; the server registers its classes with `CoRegisterClassObject` (`REGCLS_SUSPENDED`, then `CoResumeClassObjects`) and the client gets a proxy.  Calls go through ole32's standard marshaler (OBJREFs) and named-pipe channel, `ndrtestps.dll`'s NDR proxies and stubs and oleaut32's `IDispatch` proxy/stub: strings, arrays, a crash in the server, `QueryInterface` across the process and object identity, a dual interface with `BSTR` and `VARIANT` arguments, an interface pointer passed to the server that calls back into the client's apartment, `GetIDsOfNames` and `Invoke` with `BSTR`, by-reference, `SAFEARRAY`, `VT_DISPATCH` and other `VARIANT` arguments and `EXCEPINFO`; the server exits once everything is released, and a second one serves `CoGetClassObject`'s class factory (`LockServer`, `CreateInstance`).  64- and 32-bit clients, each against both servers |
| `etwtest` | advapi32's event-tracing controller and consumer functions as Windows answers them with no logging session running and none able to start: `StartTrace` checks its properties block (`ERROR_BAD_LENGTH`, `ERROR_INVALID_PARAMETER`) and then fails with `ERROR_NO_SYSTEM_RESOURCES`; `StopTrace`, `ControlTrace`, `QueryTrace`, `FlushTrace`, `UpdateTrace`, `EnableTrace` and `EnableTraceEx2` find no session (`ERROR_WMI_INSTANCE_NOT_FOUND`); `QueryAllTraces` lists none; `OpenTrace` opens a real-time consumer that `ProcessTrace` finds no session for and fails on a missing log file; `CloseTrace` closes a handle once; Steam's service's stop-then-start sequence; 64- and 32-bit |
| `evtlogtest` | The event log: the EventLog service is running; `ReportEventW` and `ReportEventA` from a registered source (category, strings, binary data) read back with `ReadEventLog` (backwards, seek, a too-small buffer, ANSI), `BackupEventLog` and `OpenBackupEventLog`; the same event through `EvtQuery`, `EvtRender` (XML and system values) and `EvtFormatMessage`, a bad XPath (`ERROR_EVT_INVALID_QUERY`), `EvtSubscribe` signalled by a new event; the System log's boot (12), EventLog start (6005) and service (7036) events; a kernel driver service that fails to start (7045, then 7000 with its reason); `crash.exe` logged as Application Error 1000; channel enumeration and `EvtExportLog` to `.evtx` |
| `qtwebtest` | The calls Qt WebEngine (Chromium, in GOG GALAXY's client) imports, 64- and 32-bit: `bthprops.cpl` finds no Bluetooth radio or device (`ERROR_NO_MORE_ITEMS`, `ERROR_REVISION_MISMATCH` for a wrong `dwSize`) and parses an SDP service record (a string, a 128-bit UUID, the RFCOMM channel inside the protocol list, a missing attribute, a cut-off record); `d3d12.dll` has no device (`DXGI_ERROR_UNSUPPORTED`) or debug layer, with `D3D12CreateDevice` and `D3D12GetDebugInterface` at ordinals 101 and 102; no WinUSB device, HID report descriptor or device instance to open; `SetEnvironmentStringsW` replaces the environment and refuses a string without `=`; `TreeResetNamedSecurityInfoW` leaves everything below a folder inheriting only the new entry, with its progress callback; `NetShareEnum` (no shares; another machine `ERROR_BAD_NETPATH`); the interface's name, LUID and GUID; `DnsQueryEx` with and without a completion routine; `WSAAccept` rejecting and accepting through its condition function; `SetArcDirection` (an arc drawn clockwise); `CreateAppContainerProfile` and `DeleteAppContainerProfile`; `CryptVerifyCertificateSignatureEx` (a root's own signature, from its certificate and from its public key; another key `NTE_BAD_SIGNATURE`); WinHTTP's proxy resolver ending with `ERROR_WINHTTP_AUTODETECTION_FAILED` through the callback; urlmon's security zones and `GetSecurityId`; `_ultow_s` |
| `cachetest` | Drive C:'s saved files let go of when memory runs short and read back from the data disk when wanted, as Windows drops cached file pages: `NtSetSystemInformation(SystemMemoryListInformation)` saves (`MemoryFlushModifiedList`) and lets go (`MemoryPurgeStandbyList`, with `SeProfileSingleProcessPrivilege`, so the program runs as administrator), `GlobalMemoryStatusEx` shows the memory come back, the files read back as they were through `ReadFile` and a mapped view, opening one for its details reads nothing back, files renamed, hard-linked and emptied while only on the disk read back as they should once saved and let go of again; after `shutdown /r` C: is restored without reading its large files and `cachetest after` reads them back |
| `qtthemetest` | What Qt's Windows platform plugin (in GOG GALAXY's client) asks of the Windows Runtime, 64- and 32-bit: `Windows.UI.ViewManagement.UISettings` activates through its factory and `RoActivateInstance` with `IUISettings` to `IUISettings6`; `GetColorValue` gives a white background and black text, or the reverse, for the app mode (`AppsUseLightTheme`) and the accent shades from `AccentPalette` (the default blue without it); `UIElementColor`, scroll bar sizes, caret and double-click times; `AdvancedEffectsEnabled` follows `EnableTransparency`; `ColorValuesChanged` is raised when the app mode or accent changes, not after removal, and handlers go with the last reference; `UIViewSettings`' `GetForWindow` says `Mouse`; an unknown class is `REGDB_E_CLASSNOTREG`; `BCryptEnumContextFunctions` lists Schannel's cipher suites |
| `proclisttest` | Finding another running program by its name, owner and command line, as Microsoft Edge Update finds its install worker, 64- and 32-bit: `EnumProcesses` and the Tool Help snapshot; `GetProcessImageFileName` (a `\Device\HarddiskVolume1\` path) turned into a drive path with `QueryDosDevice`; `QueryFullProcessImageName` (Win32 and native); `GetModuleFileNameEx` and `GetModuleBaseName` of another process; its token's user; `ProcessIdToSessionId`; `IsWow64Process` and `IsWow64Process2`; its command line read from its PEB with `ReadProcessMemory` and through `NtQueryInformationProcess(ProcessCommandLineInformation)` |
| `libmtest` | The 32-bit C runtime's math entry points that MSVC-built x86 code calls instead of the C names: `_libm_sse2_*_precise` (arguments and result in SSE registers) and `_CI*` (on the x87 stack), against the C functions |
| `wvsetuptest` | What the WebView2 runtime's own setup (Chromium's `setup.exe`) needs to unpack its archive and report its crashes, 64- and 32-bit: a 300 MB file mapped whole and read-only through a duplicate of its handle (the view reads the file's head, tail and the zeros between; a read-only mapping larger than the file is refused with a real error, not `ERROR_INVALID_FUNCTION`), an empty file's mapping refused with `ERROR_FILE_INVALID`; `wer.dll`'s report API (`WerReportCreate`, `WerReportSetParameter`, `WerReportAddFile`, `WerReportAddDump`, `WerReportSubmit` answering `WerDisabled`, `WerReportCloseHandle`, bad arguments refused); `FlsGetValue2` leaving the last error alone. The 32-bit run leaves out the large file |
| `warptest` | `SetCursorPos` and `ClipCursor`, as games recentre and confine the pointer: with its window in front the test moves the pointer (`GetCursorPos` reads it back, the window gets `WM_MOUSEMOVE` at that point but no `WM_INPUT`, `SetPhysicalCursorPos` too), off the desktop it stops at the edges; `ClipCursor` moves it inside the rectangle, `GetClipCursor` returns it, `SetCursorPos` and the PS/2 mouse (moved 300 left and up, all of it still in Raw Input) stop at its edges, `ClipCursor(NULL)` lets it go; a process in the background is refused (`ERROR_ACCESS_DENIED`) until its own window comes to the front, which ends the other's confinement.  64- and 32-bit |
| `wvstarttest` | What the WebView2 runtime's browser process (`msedgewebview2.exe`) and its loader need to start: COM's apartment published in the TEB (`ReservedForOle`: STA, MTA, gone after `CoUninitialize`); `TerminateProcess` on the process itself ending it with its code and no FLS callback (a child run); 300 FLS slots; `GetAddrInfoExW`; `RtlIpv4StringToAddressExW` and `RtlIpv6StringToAddressExW` (ports, a scope id, junk refused); `LdrLockLoaderLock` and its try-only form; `CryptFindOIDInfo` by OID and name; the performance counter provider API; `GetDllDirectoryW`; `GetPhysicallyInstalledSystemMemory`; `FindPackagesByPackageFamily` (none) and `AppPolicyGetThreadInitializationType` |
| `xpwin` | Windows of different processes working together, as WebView2's host, browser and GPU processes need: `SetParent` of another process's hidden window into a child window (taking effect when it is first shown), `SetWindowPos`, `MoveWindow`, `ShowWindow` and `SetWindowLong` on it, `SendMessage`, `SendMessageTimeout`, `WM_COPYDATA`, `GetWindowText` and `GetClassName` across processes, posted messages both ways, what the embedded window's own process sees (`GetParent`, `GetAncestor`, its size and place), a third process drawing into it with `GetDC` and `BitBlt`, and into a child window of another process's window (Steam puts Chromium's window inside its own SDL window), clipped to it, the window following its host and parent as they move, hide and show, and `SetParent(NULL)` |
| `dcomptest` | Direct3D without a GPU, as Chromium's software compositor draws (WebView2's, Steam's and Galaxy's browsers): `dxgi.dll`'s one adapter, the Microsoft Basic Render Driver (`0x1414`/`0x8c`, a software adapter with no outputs), `EnumWarpAdapter` and `EnumAdapterByLuid`, no hardware device without DXVK, a WARP device (feature level 9_1, refused at ANGLE's levels) and its adapter, a DirectComposition target and visual showing a swap chain made for composition, frames drawn into a mapped staging texture, copied and presented with dirty rectangles, a child visual's offset, a swap chain on a window and `D3D11CreateDeviceAndSwapChain`, and the GPU process's compositing window (a child of its own hidden window) parented in another process's window and drawn into; 64-bit (the 32-bit run checks the adapter and the parenting).  `dcomptest dxvk` (graphics suite, DXVK on Venus) checks that programs' factory and hardware devices are DXVK's on the GPU and that ANGLE's WARP renderer (feature level 9_3 and up) is DXVK's too, while a WARP device at 9_1 stays the software device on the Basic Render Driver, and runs the same composition, window and parenting checks through DXVK's factory, which still makes that device's swap chains; and a composition swap chain on the GPU device (both widths): its buffer a texture of that device, cleared and copied into on the GPU, presented whole and with a dirty rectangle, shown by a DirectComposition visual, and its `IDXGISwapChain3` color space; and Chromium's hardware DirectComposition path (64-bit on the software device too): `DCompositionCreateDevice3` giving `IDCompositionDesktopDevice` and `IDCompositionDevice3`, a visual (`IDCompositionVisual3`), a surface whose `BeginDraw` hands a texture of the device at the update rectangle's offset, drawn whole and in part and shown at `Commit`, a rectangle clip; and on the GPU the factory of the device's adapter (`GetAdapter`, `GetParent`) making composition swap chains, the adapter still DXVK's (`IDXGIDXVKAdapter`).  `dcomptest life` (graphics suite, both widths) runs Chromium's GPU composition through its lifecycle: a composition swap chain on the GPU resized with its window (`ResizeBuffers`; the last frame shown until the next present; no size refused) and a virtual surface resized with its pixels kept; a target, its visual, the DirectComposition device, the Direct3D device and the window released while another thread presents into the swap chain; and the GPU process's window composited on the GPU inside the browser's window, seen from the browser's process, resized by the browser and its process killed while presenting, after which its window is gone and the browser draws its own again; `dxvk` and `life` keep every frame the GDI way (`NOVA_PRESENT=gdi`), since they read frames back with `GetPixel`.  `dcomptest direct` (graphics suite, both widths, DXVK on Venus) checks the GPU composition: a composition swap chain on the GPU device, its target's only content, composited on the GPU into a swap chain of the device's on the window and shown straight from the host while the window is alone (nine frames in ten, and a screenshot of the screen showing its four quadrants), through the GDI while another window covers it (none from the host; the window's pixels hold the frame), from the host again once that window has gone, and the GDI way again once a swap chain of another device joins the tree (no frame from the host, and the screen shows the yellow frame drawn that way, not the host's last one); and the GPU process's window, embedded in the browser's, shown straight from the host |
| `schanneltest` | TLS through SSPI as Qt's Schannel backend (GOG Galaxy) uses it, against the network suite's HTTPS server: with manual certificate checks asked for, `InitializeSecurityContext` returns the manual-validation and mutual-authentication flags; `SECPKG_ATTR_CIPHER_INFO` names the suite (`TLS_...`), `SECPKG_ATTR_REMOTE_CERT_CONTEXT` gives the server's certificate with its chain in the context's store, and `CertGetCertificateChain` on it reports the self-signed root untrusted; a request goes out through `EncryptMessage` and the page comes back through `DecryptMessage`.  `schanneltest stores`: crypt32 finds a certificate through a collection held by a collection (as Chromium's `TrustStoreWin` looks up issuers), each store's certificates once; `CertGetEnhancedKeyUsage` on a root without the extension reports every usage (none listed, `CRYPT_E_NOT_FOUND`); and `InternetGetConnectedState` reports online with an address and offline without a network; 64- and 32-bit |
| `drvtest` | Windows driver packages, the three ways they arrive.  `drvtest exe`, as a setup program does: `SetupOpenInfFile` and `SetupGetLineText` read the INF (quotes off, `[Strings]` substituted, a missing line `ERROR_LINE_NOT_FOUND`, a file without `[Version]` refused); `DiInstallDriver` copies the INF, its catalog and its `SourceDisksFiles` to `DriverStore\FileRepository\novadrv.inf_<arch>_<hash>` and publishes it as one `C:\Windows\INF\oemN.inf`; `SetupCopyOEMInf` with `SP_COPY_NOOVERWRITE` finds it published (`ERROR_FILE_EXISTS` and its name) and a second `DiInstallDriver` publishes nothing more; `SetupGetInfDriverStoreLocation` names the store copy; `UpdateDriverForPlugAndPlayDevices` finds no device for `ROOT\NOVADRV` (`ERROR_NO_SUCH_DEVINST`); a package with its `.sys` missing is refused with nothing published; `DiUninstallDriver` removes the INF and the store copy.  `drvtest zip`, as from an unpacked .zip: `pnputil /add-driver *.inf /install` (added, its published name, no device took it), again (up to date), `/enum-drivers` (original name, provider, class, version), `/delete-driver` (and a second delete fails).  `drvtest msi`: `msiexec` installs `driver.msi`, whose deferred custom action runs `pnputil /add-driver` on the INF it installed; the package is in the store; it uninstalls |
| `pnptest` | The Plug and Play device tree (the devices suite's "pnp" boot): `pnptest list` prints every device node and checks the tree (one root, each present node's parent present, instance IDs unique, hardware IDs given); `pnptest arrival`/`removal PREFIX` follow the arrival and removal events of a USB keyboard, a hub and a USB stick the test plugs in and takes out through QEMU's monitor, which must leave their nodes behind as devices not present |
| `devnotifytest` | Devices arriving and leaving as Windows programs hear of it (the devices suite's "pnp" boot, after `pnptest`): the PlugPlay service running and its `HKLM\SYSTEM\CurrentControlSet\Enum` keys; `RegisterDeviceNotificationW` on a message-only window for `GUID_DEVINTERFACE_USB_DEVICE` and for an open handle on a keyboard's HID interface (the link kbdhid publishes), and `RegisterDeviceNotificationA` on a hidden window for all interface classes; `DBT_DEVICEARRIVAL`/`DBT_DEVICEREMOVECOMPLETE` with `DEV_BROADCAST_DEVICEINTERFACE` (wide and ANSI names), `DBT_DEVTYP_HANDLE` on device removal, a USB stick's drive letter as `DEV_BROADCAST_VOLUME`, `DBT_DEVNODES_CHANGED`; `CM_Register_Notification` for an interface class and for every device instance, as the test plugs in and takes out a USB keyboard and a FAT USB stick |
| `devenumtest` | The device tree as Windows programs walk it (the devices suite's "pnp" boot, after `devnotifytest`), with a USB keyboard the test plugs in: cfgmgr32's `CM_Locate_DevNode`, `CM_Get_Child`/`Sibling`/`Parent`/`Depth`, `CM_Get_DevNode_Status`, registry and `DEVPKEY` properties, device ID lists filtered by enumerator, service, class, bus relations and presence, a phantom found only with `CM_LOCATE_DEVNODE_PHANTOM`, the keyboard interface list and `CM_Open_DevNode_Key`; setupapi's `SetupDiGetClassDevs` by setup class, enumerator and interface class, `SetupDiOpenDeviceInfo`/`SetupDiOpenDeviceInterface` by name and `SetupDiOpenDevRegKey` |
| `imgsharetest` | The image cache, 64- and 32-bit: a program changes its writable data, makes a read-only page writable and changes it, loads `advapi32.dll`, then starts a copy of itself while it runs.  The copy's pages come from the cache: its data, zeroed data and read-only page are as the file has them, its imports work, and `LoadLibrary` of a DLL the first process holds works too.  The 64-bit build also commits fifteen sixteenths of the RAM at once (past the old 7/8 cap) and is refused more than the RAM.  `mem` then counts the loads that reused a kept image |
| `ntfstest` | An NTFS drive D: full and cut off (the devices suite's "ntfs" boot, after `linktest restarted`): `ntfstest fill DIR` grows a file until `WriteFile` fails with `ERROR_DISK_FULL`, reads back what was written, finds the free space under 2 MB, a second file refused by `WriteFile` and `SetEndOfFile` alike, and the space back once the file is deleted; `ntfstest churn DIR` leaves `kept.txt` and rewrites four 4 MB files without end while the test resets the machine; `ntfstest cut DIR` in the next boot (NovaOS found D: marked dirty, checked and repaired it) finds `kept.txt` whole, each file one whole generation, old or new, and D: taking a new file.  Offline, `scripts/ntfs-check.py` passes the disk after filling and after the cut with no errors |
| `chkstktest` | ntdll's helpers for code built without a C runtime, 64- and 32-bit: the stack probe (`__chkstk` on x64, which leaves RSP alone; `_chkstk` on x86, which returns with ESP lowered by EAX, as MSVC's) on a thread of its own with a 1 MB stack, for sizes from 100 bytes to 256 KB, keeping the registers it must; the half-precision conversions LLVM's JIT calls (`__extendhfsf2`, `__extendhfdf2`, `__truncsfhf2`, `__truncdfhf2`) on normals, subnormals, infinities, NaN, ties to even and a double that must round once |
| `wdmtest` | A Windows kernel-mode driver, `wdmtest.sys` (`tools/drivertest/wdmtest.c`, built by `tools/drivertest/mksys.py` with clang and lld-link `/driver`), test-signed by `mksys.py`'s test certificate, which the devices suite's "wdm" boot loads with `sc create WdmTest type= kernel` and `sc start` once test signing is on (before that it is refused with 577, as are an unsigned and a tampered copy even with it; `pnputil` stages the catalog-signed `wdmcat` package and refuses `wdmbad`, whose `.sys` its catalog does not list, and `wdmcat.sys` loads by its installed catalog): `\\.\WdmTest` opens, `WriteFile` and `ReadFile` round-trip bytes the driver keeps (end of file before any write), a buffered control code reverses bytes (too small an output buffer: `ERROR_INSUFFICIENT_BUFFER`), a neither-I/O one works on the program's own buffers, an unknown one fails, direct-I/O ones read and fill a buffer over four pages (committed, never touched) through its MDL and fail on a bad buffer, the driver's own checks of the kernel API pass (the IRQL in `CR8` and the exports, a DPC queued under a spin lock running when it is let go, an in-stack queued spin lock, `ExAllocatePoolWithTag` and `ExAllocatePool2`, an MDL for pool, an event, a semaphore, a 20 ms timer, a fast mutex, `MmGetSystemRoutineAddress`), a request the driver returns `STATUS_PENDING` for completes from a timer's DPC at `DISPATCH_LEVEL`, and the driver reports the `DRIVER_OBJECT`, `DEVICE_OBJECT`, `IRP` and `FILE_OBJECT` it sees (Windows' types and sizes, `UserMode`, its `DriverName`, `RegistryPath` and `ServiceKeyName`, the `FileName` of `\\.\WdmTest\sub`); `wdmtest absent`, after `sc stop`, finds the device gone, and the unload log names the pool block the driver left |
| `avxtest` | The AVX state the kernel saves (64-bit): CPUID's OSXSAVE, AVX and F16C and XCR0's x87, SSE and AVX bits as a program sees them, `GetEnabledXStateFeatures` and `IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE)`; an F16C round trip; an 8 MiB `HeapAlloc` block 64-byte aligned and loaded with `vmovaps`; four threads on two CPUs each holding its own values in YMM0-15 across time slices; a `ud2` whose vectored handler finds YMM0-15 in its `CONTEXT_XSTATE` CONTEXT (FltSave and `LocateXStateFeature`), clobbers them, and gets them back through `NtContinue`; `GetThreadContext` with `CONTEXT_XSTATE` on a suspended thread |
| `edutest` | A Windows Plug and Play driver, `edutest.sys` (`tools/drivertest/edutest.c` and `edutest.inf`, built by `tools/drivertest/mksys.py`), for QEMU's `edu` PCI device, which the devices suite's "edu" boot installs with `pnputil /add-driver C:\Tests\Edu\edutest.inf /install` once test signing is on: the driver's device interface is listed by `CM_Get_Device_Interface_List` and opens, the driver reports what `IRP_MN_START_DEVICE` gave it (edu's identification and liveness registers through the mapped 1 MiB BAR, one memory and one level-triggered interrupt resource, the vector's IRQL, a line-based connection, a version 3 DMA adapter), the device computes 10! and 12! with an interrupt each (the ISR at the interrupt's IRQL, its DPC at `DISPATCH_LEVEL`), and 4096 and 1500 bytes go to the device through a scatter/gather list and back into a common buffer and through map registers, every address within edu's 28 bits; `edutest veto` finds disabling the device refused while it is open (`CR_REMOVE_VETOED`), `edutest notify` registers the open handle for a window (`RegisterDeviceNotification` with a `DEV_BROADCAST_HANDLE`) on a thread of its own and finds the PnP manager asking it before the driver: its `BROADCAST_QUERY_DENY` to `DBT_DEVICEQUERYREMOVE` vetoes disabling (`CR_REMOVE_VETOED`, then `DBT_DEVICEQUERYREMOVEFAILED`, the driver never asked), and once it agrees, closing its handle first, the driver accepts and the device is disabled (`DBT_DEVICEREMOVEPENDING`), and `edutest absent`, after `pnputil /disable-device`, finds the interface gone; the devices suite's "kmdfedu" boot runs the same checks against `kmdfedu.sys` (`tools/drivertest/kmdfedu.c` and `kmdfedu.inf`, built by `tools/kmdf/build_kmdf.py`), the KMDF counterpart in `C:\Tests\KmdfEdu`, which says so in its answer: `edutest` then checks the interrupt object `WdfInterruptGetInfo` describes (message-signalled exactly when the resources carry an MSI message, the same vector and IRQL) in place of the WDM connection details, and its DMA goes through KMDF's scatter/gather and packet transactions and a common buffer |
| `kmdftest` | A KMDF driver, `kmdftest.sys` (`tools/drivertest/kmdftest.c`, built by `tools/kmdf/build_kmdf.py` against the KMDF 1.33 headers with the client stub `tools/kmdf/stub.c`), on Microsoft's own framework: the devices suite's "wdm" boot, once test signing is on, installs it with `sc create KmdfTest type= kernel` and `sc start` loads `WdfLdr.sys` (the export driver it imports), which starts `Wdf01000.sys` (built from Microsoft's Windows-Driver-Frameworks source) and binds the driver to KMDF 1.33; `\\.\KmdfTest` opens (`EvtDeviceFileCreate`), `WriteFile` and `ReadFile` round-trip bytes through its default queue's `EvtIoWrite` and `EvtIoRead`, a buffered control code reverses bytes in `EvtIoDeviceControl` (too small an output buffer fails, an unknown code gives `ERROR_INVALID_FUNCTION`), and the driver reports what it saw of the framework (`WdfDriverCreate` for a non-PnP driver, `WdfDriverIsVersionAvailable` for 1.33, a control device with a typed context and a sequential queue, callbacks at `PASSIVE_LEVEL`, `WdfDriverGetRegistryPath`); `kmdftest absent`, after `sc stop` (its `EvtDriverUnload`), finds the device gone |
| `viosertest` | virtio-win's serial driver, `vioser.sys` from the virtio-win 0.1.302 release (`tools/drivertest/vioser.py` downloads it and checks its SHA-256), on QEMU's `virtio-serial-pci` with a `virtserialport` named `org.novaos.test.0` whose other end is `tools/drivertest/serialpeer.py` on the host: the devices suite's "vioser" boot, once test signing is on (for the KMDF framework it runs on), installs it with `pnputil /add-driver C:\Tests\Vioser\vioser.inf /install`, and the driver starts on the PCI device and reports its port as a child that starts raw; `viosertest` finds the port's `GUID_VIOSERIAL_PORT` interface with `CM_Get_Device_Interface_List`, opens `\\.\Global\org.novaos.test.0`, and sends a line and then 64 KiB to the host, which returns each byte XORed with 0x5A, checking every byte that comes back; after a restart the driver starts at boot and the exchange passes again |
| `umdftest` | A UMDF 2 driver, `umdftest.dll` (`tools/drivertest/umdftest.c`, built by `tools/umdf/build_umdf.py` against the UMDF 2.33 headers with NovaOS's stub `tools/umdf/stub.c`), on QEMU's `edu` device: the devices suite's "umdf" boot, once test signing is on (for the test-signed reflector), installs its package with `pnputil /add-driver C:\Tests\Umdf\umdftest.inf /install`; the PnP manager starts the reflector `WUDFRd.sys` on the device, the UMDF service starts `WUDFHost.exe`, which loads `Wudfx02000.dll` (built from Microsoft's Windows-Driver-Frameworks source) and the driver (`DriverEntry`, `EvtDriverDeviceAdd`, `EvtDevicePrepareHardware` with the device's memory BAR and interrupt, `EvtDeviceD0Entry`); `umdftest` finds the driver's interface with `CM_Get_Device_Interface_List`, opens it (`EvtDeviceFileCreate`), round-trips bytes through the default queue's `EvtIoWrite` and `EvtIoRead`, has `EvtIoDeviceControl` reverse a buffer (an unknown code fails), and checks what the driver saw: UMDF 2.33, its own host process, this program as the requestor, the 1 MiB BAR and interrupt, a typed device context and its UMDF service key as the registry path; `pnputil /disable-device` removes the device in the host (which exits), `/enable-device` starts a new host, and after a restart the driver starts at boot |
| `tpwaittest` | Thread-pool waits (`CreateThreadpoolWait`): a wait calls back once when its event is set (`WAIT_OBJECT_0`) or when its timeout passes (`WAIT_TIMEOUT`), a cancelled one never; a callback sets its own wait again 100 times; a thread holding a lock the running callback waits for sets the wait again without waiting for the callback (the pattern UMDF's work items use), and the wait set meanwhile fires; a callback closes its own wait; x64 and x86 |
| `coreapptest` | The UWP app model's core (`twinapi.appcore.dll`), 64- and 32-bit: `RoGetActivationFactory` finds `Windows.ApplicationModel.Core.CoreApplication` and `Windows.UI.Core.CoreWindow` through the registry's `ActivatableClassId` keys (an unknown class is `REGDB_E_CLASSNOTREG`); `CoreApplication.Run` calls the view's `CreateView`, `Initialize`, `SetWindow`, `Load`, the launch's `Activated` (kind Launch, the command line as `Arguments`), `Run` and `Uninitialize` in order; the `CoreWindow` (class `Windows.UI.Core.CoreWindow`, its HWND through `ICoreWindowInterop`) is hidden until `Activate`, raises `VisibilityChanged`, `KeyDown`, `CharacterReceived`, `KeyUp` and `SizeChanged` (in DIPs) from its messages and `Closed` on `CoreApplication.Exit`; `GetForCurrentThread` gives it on its thread only; the dispatcher's `ProcessEvents` and `RunAsync` from another thread (run on the window's thread, its `IAsyncAction` completing); the pointer events from the mouse's messages (`PointerEntered`; `PointerPressed` for the first button, a second button as `PointerMoved`, `PointerReleased` for the last, with the `PointerPoint`'s position in DIPs, `PointerId` 1, a mouse `PointerDevice`, contact, buttons and `PointerUpdateKind`; `PointerWheelChanged` with `MouseWheelDelta`); a Windows.UI.Composition target on the CoreWindow (`CreateTargetForCurrentView`) shows a sprite; 64-bit: a Direct3D 11 (WARP) swap chain from `CreateSwapChainForCoreWindow` presents into the window |
| `comptest` | Windows.UI.Composition (`dcomp.dll`), 64- and 32-bit: `RoActivateInstance` makes a `Compositor` through the registry's `ActivatableClassId` key; a desktop window target (`ICompositorDesktopInterop::CreateDesktopWindowTarget`) shows a container's sprite visuals with colour brushes, checked in the window's pixels: the window's background where no visual is, offsets, `Opacity` 0.5 over another visual and over the background, `AnchorPoint`, `RotationAngleInDegrees` 90, an `InsetClip`, `Scale` 2 inside an offset container, `IsVisible` false, `InsertBelow`, `Remove` and `RemoveAll`; `Parent`, `Compositor`, the sprites' interfaces and runtime class name; 64-bit: a `CompositionGraphicsDevice` on a WARP Direct3D 11 device (`ICompositorInterop::CreateGraphicsDevice`), a drawing surface drawn through `ICompositionDrawingSurfaceInterop` (`BeginDraw`'s texture, `EndDraw`, `Resize`) and shown by a surface brush stretched (`Fill`) and centred at its own size (`None`); and a graphics device on an `ID2D1Device` of that device, whose drawing surface's `BeginDraw` hands out an `ID2D1DeviceContext`, cleared with it and shown |
| `d2dctxtest` | Direct2D 1.1's devices and device contexts (`d2d1.dll`), 64-bit (the software Direct3D device is): `D2D1CreateFactory` giving `ID2D1Factory1` and its `ID2D1Multithread`; an `ID2D1Device` on a WARP Direct3D 11 device (`ID2D1Factory1::CreateDevice`, `D2D1CreateDevice`) and its device context (`CreateDeviceContext`, an `ID2D1RenderTarget` too, `GetDevice`, `IsDxgiFormatSupported`); a bitmap made with `D2D1_BITMAP_PROPERTIES1` (`TARGET`) as the context's target (`SetTarget`, `GetTarget`, `GetOptions`), drawn into and read through a `CPU_READ` copy's `Map` (and `Map` refused without it); 192 DPI in DIPs and in `D2D1_UNIT_MODE_PIXELS`; a render-target texture's `IDXGISurface` as a target (`CreateBitmapFromDxgiSurface`, its options from the texture's), the drawing read back from the texture, a second `BeginDraw` keeping what it holds, and `D2D1CreateDeviceContext` on the surface; DirectComposition on the `ID2D1Device` (`DCompositionCreateDevice2`): a surface whose `BeginDraw` hands out an `ID2D1DeviceContext`, drawn whole and in an update rectangle at its offset, shown in a window at `Commit`, and an `IDCompositionSurfaceFactory` on the device whose surfaces draw the same way |
| `winrtabitest` | The Windows Runtime C++ ABI headers `tools/winrt/abigen.py` generates without MIDL, 64- and 32-bit: IIDs Windows publishes, from its metadata (`ICompositor`), a generic instance's pinterface signature (`IIterable<HSTRING>`), the lifted `Microsoft.UI.Composition` and the XAML framework's IDL (`IUIElement`, `IButton`, `IButtonFactory`, a classic COM interop interface), `RuntimeClass_`/`InterfaceName_` names and enum values; then the Compositor (`dcomp.dll`) driven only through the generated interfaces: a sprite's size, offset, opacity, visibility, rotation and anchor point, a colour brush set as its brush, a container's children (`InsertAtTop`, `Count`, `Parent`, `Remove`) and `ICompositionObject::Compositor`, so the headers' method order matches the implementation |
| `xamlbasetest` | The first of Microsoft's XAML framework built for NovaOS (step 32.3): the XAML core's lowest layer compiled from Microsoft's sources as XAML is compiled (`xaml_components.lib`: `xstring_ptr`, `XStringBuilder`, `DoubleUtil`, `bit_vector`, the resource qualifiers a `ResourceDictionary` picks by window size, runtime-enabled features with their overrides, the dependency locator), XAML's type metadata (`Button` by index with its name, full name and base types down to `DependencyObject`, assignability, `Grid` by full name, `FrameworkElement.Width` and the attached `Grid.Row`, properties found by name on `Button` including inherited ones and the `Name` directive), XAML's collections (a `DependencyObjectCollection` filled, searched, removed from and emptied, its items parented to it, a `CollectionMoveView` replaying a move), and NovaOS's WRL the sources are written on: `Make`, `RuntimeClass` (QueryInterface, `IAgileObject`, a cloaked interface, a classic COM class), `GetRuntimeClassName`, weak references resolving while alive and to null after, `Callback`, `EventSource` adding, invoking and removing handlers, `HString` and `HStringReference` |
| `camtest` | A USB camera through Media Foundation (the devices suite's "camera" boot, a fake UVC 1.1 camera on NovaOS's UVC class driver): `MFEnumDeviceSources` (friendly name, symbolic link, no audio capture sources), the media source's presentation descriptor and type handler, the source reader's native types, frames read in YUY2 640x480 and 320x240 (at 30 and 15 frames a second) and NV12, and converted to RGB32, NV12 and YUY2, each checked against the test card's colour bars and frame number; a second reader refused while the first streams; the asynchronous reader and `Flush` (x86) ; `camtest --iso` (the "isocam" boot, a fake camera streaming isochronously): the same on MJPEG 640x480 and 320x240 and YUY2 160x120, the MJPEG frames read as JPEG images and decoded into RGB32, NV12 and YUY2 |
| `digitest` | USB touch screens, pens and precision touchpads through the HID class driver (the devices suite's "touch" boot, 64-bit): a USB digitizer (`tools/padpeer.py --kind digitizer`) whose touch screen, pen, touchpad and configuration collections run on `hiddigi` and whose mouse collection runs on `mouhid`, under `HidUsb`; Raw Input lists the digitizer collections as HID devices and `GetPointerDevices` a pen; a touch gives `WM_POINTERDOWN`/`UP` (`PT_TOUCH`) where it touched, the pen `PT_PEN` with its pressure (`GetPointerPenInfo`), a finger on the touchpad (switched to touchpad mode at boot) moves the pointer, a tap clicks, a tap then a touch drags with the left button held until the finger lifts, two fingers scroll (`WM_MOUSEWHEEL`), a two-finger flick keeps scrolling after the fingers lift and a touch stops it, and unplugging takes the collections and the pen away |
| `dostest` | The DOS host (steps 31.2 to 31.4): `ntvdm.exe` boots MS-DOS 4.00 in the Terminal's console window on its own real-mode CPU and BIOS; at `COMMAND.COM`'s prompt the test types `VER` and `DIR /W` as key records and reads "MS-DOS Version 4.00" and the listing off the screen; F12 ends `ntvdm.exe` and the Terminal's screen comes back.  Then DOS programs started as Windows programs: `DOSRW` written into a long-named folder on C: as `dosrw.com` and `dosrwexe.exe`; `cmd /c dosrw IN.TXT` finds the `.COM` by name and the kernel runs it in `ntvdm.exe`, which reads `IN.TXT` and writes `OUT.TXT` through DOS's drive C:, returns exit code 42 through `cmd`, leaves its message on the screen and gives the console back; `CreateProcess` on the `.EXE` (an `MZ` image) does the same.  Last, `C:\Tests\Dos\ROGUE.COM` (bootRogue, a DOS game that writes video memory directly) must show its hero and HP, take eight arrow-key moves and end on Esc with exit code 0 |
| `hidapitest` | hidapi's own Windows backend (hidapi 0.15.0, `third_party/hidapi`, built unchanged) on the devices suite's "hid" boot device: cfgmgr32's `CM_Get_Device_Interface_PropertyW` (`DEVPKEY_Device_InstanceId` sized with `CR_BUFFER_SMALL` first as hidapi asks, the interface's class GUID and enabled state, `CR_NO_SUCH_DEVICE_INTERFACE` for a path no device has), the collection's parent `USB\VID_1209&PID_0008\NOVA0002` (its serial number as its instance ID) and its `USB\Class_03` compatible IDs, `DEVPKEY_Device_BusTypeGuid` and a shared `DEVPKEY_Device_ContainerId`; `hid_enumerate` reporting the device once as `HID_API_BUS_USB` with its IDs, usage, interface and release numbers, manufacturer, product and serial number (and every HID device on a known bus); `hid_open` by serial number, `hid_get_device_info`, the string getters, a feature report set and read back, `hid_write` and the echo through `hid_read_timeout`, a timed-out read (64-bit and x86) |
| `hidclasstest` | A HID collection the kernel's HID class driver runs (the devices suite's "hid" boot, a vendor-defined USB HID device on NovaOS's USB HID minidriver): setupapi's and `CM_Get_Device_Interface_List`'s HID interfaces, `HidD_GetAttributes`, the manufacturer, product, serial number and indexed strings, `HidD_GetPreparsedData` and `HidP_GetCaps`, `IOCTL_HID_GET_COLLECTION_INFORMATION` through `DeviceIoControl`, `HidD_SetFeature` and `GetFeature`, output reports by `WriteFile` and `HidD_SetOutputReport` read back by `ReadFile` on two handles, `HidD_GetInputReport`, `HidD_Get`/`SetNumInputBuffers`, `HidD_FlushQueue`; overlapped I/O as hidapi does it (a `ReadFile` left pending whose event times out and then completes with the next report, `CancelIo`, `CancelIoEx`, an overlapped `WriteFile` and `DeviceIoControl(IOCTL_HID_GET_FEATURE)`, `CloseHandle` ending a pending read); Raw Input: the collection in `GetRawInputDeviceList` under its interface path, `RIDI_DEVICEINFO` and `RIDI_PREPARSEDDATA`, `WM_INPUT_DEVICE_CHANGE` and an echoed report as `WM_INPUT` (x86).  `hidclasstest unplug` (64-bit) then has the test unplug the device: a read fails, the interface leaves the lists, and Raw Input sends `GIDC_REMOVAL` and drops the device |
| `kbdmoutest` | USB keyboards and mice through the HID class driver and the `kbdhid` and `mouhid` class drivers, as on Windows (the devices suite's "devmgr" boot, 64-bit): the boot's QEMU keyboard and tablet on `kbdhid` (class Keyboard) and `mouhid` (class Mouse) under `HidUsb`; then a second USB keyboard and tablet plugged in arrive the same way, Raw Input lists none of them as a HID device, moving the tablet moves the pointer, typing on the keyboard reaches the console, and unplugging them takes their collections away |
| `usbcompositetest` | A composite USB device split into one function per driver (the devices suite's "composite" boot: a keyboard and two vendor-defined HID functions behind one address): the device's node (`USB\COMPOSITE`, "USB Composite Device"), a started child per interface with `USB\VID_1209&PID_000A&REV_0114&MI_nn` and `USB\Class_03&...` IDs and its driver (`HidUsb` on every one), the keyboard's collection on `kbdhid` (hardware ID `HID_DEVICE_SYSTEM_KEYBOARD`) and the `HID\VID_1209&PID_000A&MI_nn` collections under them; through `hid.dll` each function's usage, an output report echoed only by the function written to, `HidD_GetInputReport` and a feature report per function (x86).  `usbcompositetest unplug` (64-bit) then has the test unplug the device: a read fails, both interfaces leave the HID list and the device's node goes |
| `vhidtest` | A third-party KMDF HID minidriver (the devices suite's "vhidmini" boot: Microsoft's `vhidmini2` sample, built unchanged, on a root-enumerated device under NovaOS's `mshidkmdf`). `vhidtest create` registers `ROOT\HIDCLASS\0000` with hardware ID `root\vhidmini` as `devcon install` does (`SetupDiCreateDeviceInfo`, `DIF_REGISTERDEVICE`; problem code 28 until a driver is installed). Then, 32- and 64-bit: the node started with `vhidmini` in its `LowerFilters`, the HID collection in setupapi's and cfgmgr32's interface lists, `HidD_GetAttributes` (DEED:FEED, version 0101), the four strings, caps (usage FF00:1, input 2, output 8, feature 10 bytes), a feature report with the attributes, an output report read back as an input report, a `WriteFile` report coming back through `ReadFile`, a `SET_ATTRIBUTES` feature report changing the attributes, and Raw Input listing it.  `vhidtest gone` checks the device is gone after `pnputil /remove-device` |
| `wictest` | The Windows Imaging Component (`windowscodecs.dll`), 64- and 32-bit: the imaging factory through `CoCreateInstance` and `WICCreateImagingFactory_Proxy`; PNG (with its DPI), BMP, JPEG, GIF (two frames) and ICO (a DIB and a PNG entry) decoded from memory streams and through `CreateDecoderFromStream`'s sniffing; the format converter to 32bppPBGRA, 24bppRGB, 8bppGray, 1bppIndexed and 128bppRGBAFloat; the scaler, clipper and flip-rotator; palettes (predefined and from a bitmap); bitmaps from memory, sources, sections, HBITMAPs and HICONs, with locks; decoder and pixel-format component info and enumeration; the `_Proxy` exports WPF calls |

<!-- END generated:selftest-table -->

`crash` writes through a NULL pointer: only it dies, and it leaves a
report in `C:\NovaOS\Crashes`.  `crash kernel` crashes the kernel on
purpose (`NtNovaBugCheck`) to show the backtrace.

Interactive programs: `winhello` and `guitest` (windows, menus, dialogs,
property sheets; `guitest auto` drives them itself and reports, as CI
runs it), `droptest` (drag and drop; its targets list each dropped
file's size, or "missing"), `cpus` (SMP speed-up), and `hello`, `mandel`,
`primes`, `wc` and `guess`.

### Boot-time self-tests

The kernel tests itself while it boots and prints the results to the
serial log: `[PROBE]` (user-pointer validation) and `[PSTEST]`
(`PsGetCurrentThread` on bare kernel threads).  `grep -a 'passed,'
serial.log` lists them; each should end "0 failed".

### Real programs: the app corpus

`tools/appcorpus.py` runs unmodified Windows programs, one file per
program in `tests/appcorpus/` ([testing.md](testing.md) lists them).  It
downloads each program's official release into a cache
(`build/appcorpus-cache` by default, kept between runs), unpacks it into
`C:\Apps`, boots NovaOS once (6 GB of memory, a 12 GB data disk) and types
each program's commands.  A command passes as a self-test does: exit code
0 and the expected output.  Windowed programs run last, one at a time,
and their screenshots must match `tests/reference/NAME.png` (at most 3% of
pixels may differ).  The exit status counts failed and unrun programs.

It needs on the host: `p7zip-full`, `python3-pil` (Pillow), `openssl`
(the HTTPS server Firefox loads a page from), `ntfs-3g` (`mkntfs`, for
drive D:), `ffmpeg` (VLC's test video), `cmake` and
`gcc-mingw-w64-x86-64` (to build PuTTY), and, for the programs that need
sound, `pulseaudio`, `pulseaudio-utils` and `qemu-system-gui`.  Without
PulseAudio those programs are skipped, which is not a failure.

```bash
sudo apt install p7zip-full python3-pil ntfs-3g ffmpeg cmake gcc-mingw-w64-x86-64 \
  pulseaudio pulseaudio-utils qemu-system-gui
python3 tools/appcorpus.py
python3 tools/appcorpus.py --only ripgrep,jq --out /tmp/ac
python3 tools/appcorpus.py --only NovaOS,SumatraPDF,WinMerge --update-reference
```

`--only` takes the names the files give their `App(...)` (`ripgrep`,
`7-Zip`, `Notepad++`, `Cave Story (game pad)`, ...).  `--update-reference`
rewrites the reference screenshots after an intended change.  To add a
program, add a file to `tests/appcorpus/` that defines `APP = App(...)`
and `DOC`; windowed programs (`gui=True`) are numbered from 850 so they
run last.

The nightly workflow (`.github/workflows/nightly.yml`) runs each program
in its own job, plus `smpstress scaling 0` on 4 CPUs, checked by
`tools/ci/check-smpstress.py` (see [ci.md](ci.md)).

### Trying things by hand: novarun.py

`tools/novarun.py` boots a throwaway snapshot of `build/nova.img` with a
fresh FAT32 data disk, opens the Terminal and types each command,
printing its output:

```bash
python3 tools/novarun.py ver 'dir C:\Programs'
python3 tools/novarun.py --put 'nvim-win64=C:\Apps\nvim' 'cd C:\Apps\nvim\bin' \
    '!type nvim --clean t.txt\n' '!wait 40' '!type ihello\e:wq\n' '!done 60' 'type t.txt'
python3 tools/novarun.py 'filetest install' '!reboot' 'filetest installed'
```

| Option | Meaning |
|--------|---------|
| `--put HOSTDIR=C:\Dir` | Copy a host file or folder onto drive C: (repeatable) |
| `--mem MiB`, `--smp N` | Memory (2048) and CPUs (2) |
| `--timeout S` | Time limit per command (120) |
| `--data-mb N` | Data disk size (1024) |
| `--keep DIR` | Keep the serial log, data disk (`data.img`) and screenshots |
| `--img PATH` | The boot image |
| `--wav PATH` | An HD Audio card whose output is recorded to PATH |
| `--rec PATH` | An HD Audio card whose microphone hears PATH over and over (needs PulseAudio) |
| `--net` | An e1000e on QEMU's user network; the host is `10.0.2.2` |
| `--display NAME` | The display adapter (`std`, `cirrus`, `vmware`, `qxl`, `virtio`, or a device such as `bochs-display`) |
| `--monitors N` | N - 1 more monitors (`secondary-vga`) |
| `--extra 'ARGS'` | More QEMU arguments |

Special commands:

| Command | Effect |
|---------|--------|
| `!type TEXT` | Type without waiting for a command to end (`\n` Enter, `\e` Esc), for full-screen and interactive programs |
| `!done N` | Wait up to N seconds for the running command to end |
| `!wait N` | Wait N seconds |
| `!bg CMD` | Type a command and leave it running |
| `!keys a b ctrl-c` | Press QEMU key names |
| `!click X Y` | Left click at a logical screen point |
| `!shot NAME.png` | Save a screenshot (one per monitor) |
| `!reboot` | `shutdown /r` (the data disk keeps drive C:) and open the Terminal again |

`NOVARUN_GDB=1` starts QEMU's GDB stub on port 1234.

### Testing sound by hand

`soundtest` plays sine tones through each path.  To check what came out,
record it and measure it; `tools/wavcheck.py` prints each tone's start,
length, level and pitch:

```bash
python3 tools/novarun.py --wav /tmp/out.wav 'soundtest tone 440 1000' 'soundtest wasapi 523 800'
python3 tools/wavcheck.py /tmp/out.wav
```

To test recording, `--rec FILE.wav` gives the card a microphone that
hears FILE over and over, through a private PulseAudio server.  Copy the
recording off the data disk and check it:

```bash
python3 tools/novarun.py --keep /tmp/rec --rec tone523.wav 'soundtest record C:\rec.wav 3000'
mcopy -i /tmp/rec/data.img ::/NOVA/C/rec.wav /tmp/
python3 tools/wavcheck.py /tmp/rec.wav --tone 523 2500
```

`wavcheck.py --tone HZ MS` exits 0 when the file holds that tone for that
long, `--sound MS` when it holds that much sound of any kind (a game's
music), and `--gaps HZ` lists places where a steady tone jumps.

### Testing the network by hand

`--net` gives the guest an e1000e on QEMU's user network.  The host is
`10.0.2.2`, so programs can reach a test server on the host:

```bash
python3 tools/novarun.py --net --put 'DIR=C:\Apps\x' 'cd C:\Apps\x' \
    'ffmpeg.exe -tls_verify 0 -i https://10.0.2.2:8443/tone.wav -f null -'
```

For IPv6, `tools/v6peer.py` is a whole IPv6-only network (a router with
SLAAC and RDNSS, a stateful DHCPv6 server leasing `fd00:6e6f:7661::100`,
DNS for `nova6.test`, an HTTP server) that QEMU reaches
through a datagram netdev, so the host needs no IPv6 of its own:

```bash
python3 tools/v6peer.py &
python3 tools/novarun.py --extra '-netdev dgram,id=v6,local.type=inet,local.host=127.0.0.1,local.port=10601,remote.type=inet,remote.host=127.0.0.1,remote.port=10600 -device virtio-net-pci,netdev=v6' \
    ipconfig 'ping -6 nova6.test' 'curl -6 http://nova6.test/'
```

`node tools/h2server.js CERT KEY` serves HTTPS with HTTP/2 on port 8443
and plain HTTP on 8080, for `httptest` (`httptest -2 -k
https://10.0.2.2:8443/hello`) and `dltest` (`dltest -n 8 -w 10.0.2.2 8080
33554432`: eight 32 MB downloads at once from `/stream/BYTES/SEED`).

### Host-side tools

- `tools/pe_imports.py PROGRAM.exe ...` lists every DLL and function a
  Windows program imports that NovaOS's DLLs do not provide (by default
  it reads the DLLs in `build/kernel_build/userland`), mapping API sets
  as the kernel's loader does.  `--exports DLL` prints a DLL's exports.
- `tools/sys_imports.py DRIVER.sys` does the same for a kernel-mode
  driver's imports ([drivers.md](drivers.md)).
- `tools/msitest/hosttest.c` dumps a Windows Installer package's tables
  and cabinets with the same readers `msi.dll` uses (build instructions
  in the file); `tools/msitest/make_package.sh` builds a test package
  with msitools.
- `tools/make_cabtest_data.py` writes `userland/programs/cabtest_data.h`,
  the MSZIP, LZX and two-cabinet set `cabtest` extracts;
  `--write-cabs DIR` writes them as files, to check with
  `cabextract -t` or `7z t` after a change.
- `tools/authenticode/mktests.py OUT PE64 PE32` makes `authtest`'s signed
  files (the build runs it on `hello.exe`; they go to
  `C:\Tests\Authenticode`).  A test certificate authority from fixed
  seeds, in plain Python (`tools/authenticode/pki.py`), signs copies of
  the programs the ways `WinVerifyTrust` must accept or refuse.  To check
  them on the host: `openssl x509 -inform der -in OUT/testroot.cer -out
  root.pem`, then `osslsigncode verify -CAfile root.pem -TSA-CAfile
  root.pem -in OUT/signed.exe`.
- `tools/d2dtest/reference.py` draws `d2dtest`'s reference image with
  Skia (`pip install skia-python`); rerun it when the scene changes.
- `tests/tools/` holds host-side unit tests, run in CI with
  `python3 -m unittest discover -s tests/tools -v`.  One of them,
  `tests/tools/test_ntfs_cut.py`, runs the real `kernel/fs/ntfs.c` on a
  RAM disk (`tests/host/ntfs_cut.c`), cuts it off after each disk write
  in turn, and requires every cut disk to come back writable, with every
  file whole, and to pass `scripts/ntfs-check.py`.

---

## Debugging

- **Serial log**: every kernel message goes to COM1 (your terminal under
  `run`, or `serial.log` in a test's `--out` or `--keep` directory).  In
  NovaOS, the Terminal's `dmesg` shows it.
- **`devices`** in the Terminal lists the PCI devices and the driver each
  one has (missing drivers in red).  On a real PC this is the first thing
  to check ([hardware.md](hardware.md)).
- **Program crashes** are logged with the faulting module and offset, the
  process's exit code, and `OutputDebugString` output.
- **Crash reports**: a program that crashes leaves
  `C:\NovaOS\Crashes\NAME-YYYYMMDD-HHMMSS-PID.txt` (the exception, module
  and offset, the address it touched, the return addresses on its stack,
  its modules and the end of the kernel's log), and the Terminal names
  the file under the crash line.  A kernel fault writes its log and
  backtrace into `\NOVA\PANIC.TXT` on the FAT disk that keeps drive C:
  (64 KiB set aside at boot), and the next start moves it to
  `C:\NovaOS\Crashes\kernel-YYYYMMDD-HHMMSS.txt`.  The Terminal's
  `crashes` lists the reports and `crashes last` shows the newest.  At
  most 100 are kept; delete old ones to make room.
- **`trace NAME`** in the Terminal logs the failing system calls (with
  file names) of the program called NAME, each with its process id.
  `trace +NAME` logs every call; `trace off` stops it.
- **`profile on [DELAY LENGTH]`** samples every CPU at each timer tick (100
  a second; from DELAY ticks on, for LENGTH ticks), and **`profile`**
  prints the result: how time split between programs, the kernel,
  waiting for the big kernel lock and idle; the busiest kernel functions
  and their callers; the busiest 64-byte lines of program code (look the
  address up in the DLL's `.map` file in the build); the system calls
  made; and the desktop's frames.  For example:
  `tools/novarun.py --smp 4 "profile on 550 100" "smpstress throughput 0
  files many" profile`.  With a LENGTH, the report also goes to the
  serial log when the window closes, so a program that takes the
  keyboard needs no `profile` command: `tools/novarun.py --keep out
  "profile on 50 6000" "start browser" "!wait 100"`, then
  `grep PROF out/serial.log`.

### GDB

Start QEMU with the GDB stub; it waits for GDB before running anything:

```bash
cmake --build build --target run-debug
# or: scripts/run-qemu.sh build/nova.img /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_VARS_4M.fd --gdb
```

In a second terminal, attach GDB and set a hardware breakpoint at the
kernel's C entry point:

```bash
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) hbreak KiSystemStartup
(gdb) continue
```

---

## Troubleshooting

### "No suitable kernel compiler found"

CMake found neither `x86_64-elf-gcc` nor `clang`.  Install clang
(`sudo apt install clang lld llvm`), or build a cross compiler as
described under [Optional compilers](#optional-compilers).

### The build fails with no useful message

The kernel and boot loader sub-builds log to files.  Look at the end of
`build/kernel_build-prefix/src/kernel_build-stamp/kernel_build-build-err.log`
(CI prints its last 100 lines when the build fails).  Common causes:

- `7z` not found: install `p7zip-full`.
- A download failed (Firefox, Mesa, or the Windows-Driver-Frameworks
  fetch): check that the machine can reach `archive.mozilla.org` and
  `github.com`.
- `libc++'s headers not found` or `MinGW-w64's headers not found`:
  install `libc++-dev` and `gcc-mingw-w64-x86-64`.

### No `run` target, or "OVMF not found"

```bash
sudo apt install ovmf
ls /usr/share/OVMF/ /usr/share/ovmf/
cmake -S . -B build -DOVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd -DOVMF_VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
```

Ubuntu 24.04 names the split files `OVMF_CODE_4M.fd` and
`OVMF_VARS_4M.fd`; `/usr/share/ovmf/OVMF.fd` is the combined image.

### NovaOS does not boot, or stops early

Give it at least 2 GB of memory (`run-qemu.sh` gives 256 MiB).  Read the
serial log: the kernel logs its start-up to COM1.  To step
through the start, break at `KiSystemStartup` in GDB and check that the
boot information's magic is `0x4E4F564100424F4F` ("NOVA\0BOO",
`include/boot_protocol.h`) and that physical memory is mapped at
`0xFFFF800000000000`.

### No serial output in QEMU

Use `-serial stdio`, not `-serial null`.  `run-qemu.sh` does this.
