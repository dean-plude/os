# Building NovaOS

## Prerequisites

### Required Tools

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install -y \
    cmake ninja-build \
    nasm \
    gcc-mingw-w64-x86-64 \   # UEFI bootloader compiler
    qemu-system-x86 \        # For running
    ovmf \                   # UEFI firmware (OVMF)
    mtools dosfstools         # For disk image creation
```

### Kernel Compiler Options

The kernel needs a freestanding x86_64 C compiler. Choose one:

**Option A: x86_64-elf-gcc (recommended for production builds)**

This is a cross-compiler that produces bare-metal ELF. Build with crosstool-ng:

```bash
# Install crosstool-ng
git clone https://github.com/crosstool-ng/crosstool-ng
cd crosstool-ng
./bootstrap
./configure --enable-local
make

# Configure for x86_64-elf
./ct-ng x86_64-unknown-elf
./ct-ng menuconfig
# Set: Paths → CT_PREFIX_DIR = ${HOME}/x-tools/${CT_TARGET}
./ct-ng build
export PATH="$HOME/x-tools/x86_64-unknown-elf/bin:$PATH"
```

**Option B: Clang (easier, works out of the box)**

```bash
sudo apt install clang lld
```

CMake auto-detects which is available (x86_64-elf-gcc is preferred).

---

## Building

```bash
# Clone the repo (if not already done)
git clone https://github.com/dean-plude/os
cd os

# Configure
mkdir build && cd build
cmake .. -G Ninja

# Build everything (bootloader + kernel + disk image)
ninja

# Or with make:
cmake .. -G "Unix Makefiles"
make -j$(nproc)
```

The build produces:
```
build/
├── bootx64.efi     # UEFI bootloader
├── kernel.elf      # Kernel ELF (with debug symbols)
├── kernel.sym      # Stripped symbol file
└── nova.img        # Bootable FAT32 disk image
```

---

## Running in QEMU

```bash
# Automatic (using CMake target):
cd build
cmake --build . --target run

# Manual:
./scripts/run-qemu.sh build/nova.img /usr/share/OVMF/OVMF_CODE.fd
```

The serial output appears on your terminal (COM1 → stdio).

---

## Debugging with GDB

```bash
# Terminal 1: Start QEMU with GDB stub
./scripts/run-qemu.sh build/nova.img /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/OVMF/OVMF_VARS.fd --gdb

# Terminal 2: Connect GDB
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) hbreak KiSystemStartup     # Hardware breakpoint at kernel C entry
(gdb) hbreak pmm_init            # Break at memory manager init
(gdb) continue

# Useful GDB commands:
(gdb) info registers             # Show all registers
(gdb) x/20gx $rsp                # Dump 20 qwords at stack pointer
(gdb) bt                         # Backtrace
(gdb) disassemble                # Disassemble current function
(gdb) p *current_thread          # Print scheduler state
```

---

## Cross-Compilation from Windows

On Windows, use WSL2 (Ubuntu) or a Linux VM. The build system is identical.

Alternatively, if you have LLVM/Clang for Windows and MinGW-w64:

```powershell
# PowerShell (Windows)
cmake .. -G "Ninja" -DCMAKE_C_COMPILER=clang
ninja
```

---

## Troubleshooting

### "No suitable kernel compiler found"

Install clang: `sudo apt install clang lld`

Or build x86_64-elf-gcc as described above.

### "OVMF not found"

```bash
sudo apt install ovmf
# OVMF files are usually at:
ls /usr/share/OVMF/
ls /usr/share/ovmf/
```

Pass the path explicitly:
```bash
cmake -DOVMF_CODE=/path/to/OVMF_CODE.fd ..
```

### Kernel crashes on boot

Enable GDB debugging and break at `KiSystemStartup`. Check:
1. Is the BootInfo magic correct? (`0x4E4F564100424F4F`)
2. Is the physmap VA correct? (`0xFFFF800000000000`)
3. Is the CR3 value reasonable? (Should be a physical address < 4GiB)

### Serial output not appearing in QEMU

Make sure you're using `-serial stdio` and not `-serial null`.
The `run-qemu.sh` script handles this automatically.
