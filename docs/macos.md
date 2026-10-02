# NovaOS on a Mac

NovaOS is an x86-64 operating system, so a Mac runs it in one of three ways:

| Mac | How NovaOS runs | Speed |
|-----|-----------------|-------|
| Apple Silicon (M1 and later) | QEMU or UTM, emulating an x86-64 PC (TCG) | Slow: every instruction is translated. Booting takes minutes, not seconds. |
| Intel | QEMU or UTM with Apple's Hypervisor.framework (`-accel hvf`) | Close to native |
| Intel, booted directly | From a USB stick, on the bare hardware | Native, but few of the Mac's devices have drivers (see the end) |

Apple Silicon Macs cannot boot NovaOS on the hardware: they are not PCs and
have no x86 CPU.  Hypervisor.framework there only runs ARM guests, so
`-accel hvf` does not apply.

> **Status.** The instructions here follow the build scripts and Homebrew's
> packages, but nobody has yet run them on a real Mac.  Sections marked
> *untested* are the ones most likely to need a fix; please report what
> breaks.  The build is only verified on Linux ([building.md](building.md)).

---

## Running the ready-made ISO (no build)

CI publishes a bootable `nova.iso` built from `main`, so you can try NovaOS
without building anything.  You need QEMU, which brings the UEFI firmware
with it:

```bash
brew install qemu
mkdir nova && cd nova
curl -LO https://github.com/dean-plude/os/releases/latest/download/nova.iso
```

The firmware's variable store has to be writable, so copy it first.
Homebrew ships one store file for both 32- and 64-bit x86
(`edk2-i386-vars.fd`):

```bash
Q="$(brew --prefix qemu)/share/qemu"
cp "$Q/edk2-i386-vars.fd" /tmp/nova-vars.fd
qemu-img create -f raw disk.img 1G     # a disk to install onto / keep drive C:

qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$Q/edk2-x86_64-code.fd" \
  -drive if=pflash,format=raw,unit=1,file=/tmp/nova-vars.fd \
  -drive file=disk.img,format=raw \
  -cdrom nova.iso \
  -serial stdio
```

NovaOS boots live from the CD and opens **Install NovaOS**.  Install onto
`disk.img`, then drop `-cdrom nova.iso` to start from the disk.  Files you
create are kept on `disk.img` across restarts.

Additions to the command line:

| Add | For |
|-----|-----|
| `-accel hvf -cpu host` | **Intel Macs only**: hardware virtualization.  Leave it out on Apple Silicon. |
| `-device intel-hda -device hda-output,audiodev=snd0 -audiodev coreaudio,id=snd0` | Sound through the Mac's speakers |
| `-nic user,model=e1000e` | Network (q35's default NIC is already an e1000e; this just makes it explicit) |
| `-display cocoa,zoom-to-fit=on` | Scale the window on a Retina screen |
| `-smp 8` | More cores (up to 16).  Under emulation more cores help less than on Linux/KVM. |

On Apple Silicon, give QEMU time: the firmware alone takes a while before
the NovaOS boot logo appears, and the serial log in the terminal shows that
it is progressing.  `-smp` higher than 4 rarely helps there.

### UTM (graphical) — *untested*

[UTM](https://mac.getutm.app) wraps QEMU in a Mac app.

1. **Create a New Virtual Machine** → **Emulate** (on Apple Silicon; on an
   Intel Mac choose **Virtualize**) → **Other**.
2. Boot device: **CD/DVD Image**, and pick the downloaded `nova.iso`.
3. Architecture **x86_64**, System **Standard PC (Q35 + ICH9)**, memory
   2048 MB, 4 cores.
4. Create a 1 GB drive.  Before saving, tick **Open VM Settings** and set:
   - **System → UEFI Boot**: on.
   - **Drives**: interface **IDE** for the disk (on Q35 that is the AHCI
     SATA controller, the one NovaOS has a driver for; VirtIO and NVMe
     disks are not seen).
   - **Display**: emulated display card **VGA** (`VGA`, the Bochs adapter
     NovaOS sets resolutions on), not `virtio-gpu`.
   - **Network**: emulated card **e1000e** (or **e1000**).
   - **Sound**: **Intel HD Audio**.
5. Start it, install to the disk, then eject the ISO.

---

## Building on macOS — *untested*

The build needs a cross toolchain, not Xcode's: Apple's `clang` has no
`lld-link`, and Apple's `ld` and `ar` cannot link the kernel's ELF objects.
The kernel and bootloader are therefore built with GNU cross compilers from
Homebrew, and the Windows userland with Homebrew's LLVM.

### Prerequisites

Install the [Xcode command line tools](https://developer.apple.com/xcode/resources/)
(`xcode-select --install`) and [Homebrew](https://brew.sh), then:

```bash
brew install cmake ninja nasm python \
             llvm lld \
             x86_64-elf-gcc mingw-w64 \
             mtools dosfstools xorriso coreutils \
             qemu
```

| Package | Used for |
|---------|----------|
| `llvm`, `lld` | `clang`, `clang++`, `lld-link`, `llvm-rc`, `llvm-nm`: the Windows userland and NetSurf |
| `x86_64-elf-gcc` | The kernel (CMake prefers it over clang) |
| `mingw-w64` | The UEFI bootloader (`x86_64-w64-mingw32-gcc`) |
| `nasm` | Kernel assembly |
| `mtools`, `dosfstools` | `nova.img` (`mkfs.fat` / `mkdosfs` and `mformat`, `mcopy`) |
| `xorriso` | `scripts/create-iso.sh` |
| `coreutils` | GNU `truncate`, `stat -c` and `nproc`, which the scripts use and macOS lacks |
| `qemu` | Running NovaOS, and its UEFI firmware |

Homebrew's LLVM is "keg-only" (not on `PATH`, so Apple's `clang` would be
found instead), and the GNU coreutils install with a `g` prefix.  Put both
first on `PATH` in the shell you build from:

```bash
export PATH="$(brew --prefix llvm)/bin:$(brew --prefix lld)/bin:$(brew --prefix coreutils)/libexec/gnubin:$PATH"
which clang lld-link truncate      # should all be under $(brew --prefix)
```

### Building

```bash
git clone https://github.com/dean-plude/os && cd os
mkdir build && cd build
Q="$(brew --prefix qemu)/share/qemu"
cmake .. -G "Unix Makefiles" \
    -DOVMF_CODE="$Q/edk2-x86_64-code.fd" \
    -DOVMF_VARS="$Q/edk2-i386-vars.fd"
make -j"$(sysctl -n hw.ncpu)"
```

`scripts/build.sh` works too, with the same `PATH`.  The `OVMF_*` options
are needed because CMake only looks for the firmware where Linux
distributions put it.  The output is the same as on Linux: `bootx64.efi`,
`kernel.elf` and `nova.img` in `build/`.  `NOVA_NO_NETSURF=1` and
`NOVA_NO_WOW64=1` work as described in [building.md](building.md); the first
build with NetSurf compiles about 800 files.

Errors from the userland build land in
`build/kernel_build-prefix/src/kernel_build-stamp/kernel_build-build-err.log`
(similarly `bootloader_build-...` for the bootloader), not on the terminal.

What is most likely to go wrong:

- **A different `clang` was picked up.**  `which clang` must print Homebrew's.
  Delete `build/` after fixing `PATH`, since CMake caches the compiler.
- **`mkdosfs` not found, so no `nova.img`.**  Homebrew's `dosfstools` puts
  `mkfs.fat` in `$(brew --prefix)/sbin`; make sure that is on `PATH` (it is
  with `brew shellenv`).
- **The kernel or bootloader sub-build fails to configure.**  They are
  separate CMake projects configured for the host, and on macOS CMake may
  add Apple-only flags to them.  The `-configure-err.log` beside the build
  log shows the reason; the Linux route below avoids it entirely.

### If the native build fails: build in Linux

The supported build host is Linux, and a Linux container on the Mac runs it
unchanged.  With Docker Desktop, [OrbStack](https://orbstack.dev) or
`colima`:

```bash
docker run --rm -it -v "$PWD":/src -w /src ubuntu:24.04 bash -c '
  apt-get update &&
  DEBIAN_FRONTEND=noninteractive apt-get install -y \
      cmake make nasm clang lld llvm python3 mtools dosfstools xorriso &&
  NOVA_NO_NETSURF=1 scripts/build.sh'
```

On Apple Silicon the container is ARM Linux, which is fine: every NovaOS
compiler is a cross compiler.  The results land in `build/` on the Mac.
Start them with the `qemu-system-x86_64` command below rather than `make
run`, since that build directory was configured inside the container.

### Running your build

```bash
cd build
make run        # scripts/run-qemu.sh with the firmware found above
```

`run-qemu.sh` uses 256 MiB, no network, sound through Core Audio, and the
`nova-data.img` data disk beside `nova.img` (it needs GNU `truncate` the
first time, from the `PATH` above).  For more memory, the network or HVF,
run QEMU yourself:

```bash
Q="$(brew --prefix qemu)/share/qemu"
cp "$Q/edk2-i386-vars.fd" /tmp/nova-vars.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$Q/edk2-x86_64-code.fd" \
  -drive if=pflash,format=raw,unit=1,file=/tmp/nova-vars.fd \
  -drive file=build/nova.img,format=raw \
  -drive file=build/nova-data.img,format=raw \
  -serial stdio
# Intel Mac: add  -accel hvf -cpu host
```

GDB: Homebrew's `gdb` does not run on Apple Silicon, but a remote debugger
does not need to: `brew install x86_64-elf-gdb`, start QEMU with
`make run-debug`, then `x86_64-elf-gdb build/kernel.elf -ex 'target remote :1234'`.
LLDB (`lldb build/kernel.elf`, then `gdb-remote 1234`) also works with
QEMU's stub.

Copying programs onto the data disk with `mmd`/`mcopy` works the same as on
Linux ([building.md](building.md#where-your-files-are-kept)).

---

## On a real Intel Mac — *untested*

NovaOS boots on UEFI PCs, and Intel Macs (2006–2020) have UEFI firmware, so
it should start from a USB stick.  Expect a minimal system, though: the
Mac's own devices are mostly ones NovaOS has no driver for.

### Making the USB stick

`nova.iso` is a hybrid image, so it can be written straight to a stick.
**This erases the stick**; check the disk number carefully.

```bash
diskutil list                          # find the stick, e.g. /dev/disk4
diskutil unmountDisk /dev/disk4
sudo dd if=nova.iso of=/dev/rdisk4 bs=4m
diskutil eject /dev/disk4
```

### Booting

- Macs with a **T2 chip** (most 2018–2020 models): first boot into macOS
  Recovery (hold ⌘R), open **Utilities → Startup Security Utility**, and set
  **No Security** and **Allow booting from external media**.
- Hold **Option (⌥)** while the Mac starts, and pick the **EFI Boot** disk.

### What to expect

| Part | Expected |
|------|----------|
| Display | Works at the resolution the firmware set (the UEFI framebuffer); no resolution changes, which need the Bochs adapter QEMU emulates |
| External USB keyboard and mouse | Should work (xHCI and USB HID drivers) |
| Built-in keyboard and trackpad | Only on older models that wire them over USB internally; 2016 and later MacBooks use SPI, which NovaOS cannot drive. Use an external USB keyboard and mouse. |
| Internal SSD | Not seen on NVMe Macs (2016 and later): NovaOS has only an AHCI (SATA) driver. Older SATA Macs may see it, but **do not run Install NovaOS on a Mac whose disk you need**: it repartitions the disk. |
| Keeping files | With no usable disk, drive C: lives in memory and is lost at shutdown |
| Network | None: Mac Ethernet and Wi-Fi are Broadcom or Aquantia, and NovaOS drives only Intel e1000/e1000e cards |
| Sound | Unknown; Macs use Intel HD Audio controllers with Cirrus Logic codecs, which the HD Audio driver may or may not set up. T2 Macs route audio through the T2 and will be silent. |
| Battery, power button | Read through ACPI; may work |
| Multiple cores | Should work |

To go back to macOS, restart; NovaOS changes nothing on the Mac's disk
unless you install it.
