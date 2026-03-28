# NovaOS — Windows-Compatible Operating System

A clean-room, from-scratch x86_64 operating system designed to run native
Windows executables (PE32+) without emulation.

## Status: Phase 1 — Boot & Kernel Foundation

**What works:**
- UEFI bootloader (PE32+ EFI application)
- Physical memory manager (bitmap allocator)
- Virtual memory (4-level paging, physmap, NX)
- Kernel heap (slab allocator)
- GDT/TSS with IST stacks
- IDT with all 256 handlers (exceptions, APIC IRQs, int 0x2E syscall)
- APIC timer at 100 Hz (disables legacy 8259A PIC)
- Preemptive round-robin scheduler (kernel threads)
- Serial console (COM1) + GOP framebuffer text output
- Boots in QEMU with OVMF (UEFI firmware)

## Quick Start

```bash
# Install prerequisites
sudo apt install cmake nasm gcc-mingw-w64-x86-64 qemu-system-x86 ovmf mtools dosfstools

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run
cmake --build . --target run
```

## Phase Roadmap

| Phase | Focus | Status |
|-------|-------|--------|
| 1 | Boot & kernel foundation | ✅ **Done** |
| 2 | NT kernel personality (Object Manager, syscalls, registry) | 🔄 Planned |
| 3 | I/O subsystem (IRP, drivers, NTFS) | 🔄 Planned |
| 4 | Win32 subsystem (PE loader, kernel32, ntdll) | 🔄 Planned |
| 5 | GUI (window manager, GDI) | 🔄 Planned |
| 6 | Application compatibility (notepad, calc, 7-Zip) | 🔄 Planned |

## Architecture

See [docs/phase1-architecture.md](docs/phase1-architecture.md) for detailed
design documentation, data structures, and boot flow.

See [docs/building.md](docs/building.md) for cross-compilation instructions.

## Repository Structure

```
os/
├── include/              # Shared headers (boot_protocol.h)
├── bootloader/           # UEFI bootloader (PE32+ EFI application)
│   ├── include/efi.h     # Minimal UEFI API headers
│   └── src/              # main.c, elf_loader.c, paging.c, console.c
├── kernel/               # NT-style kernel (freestanding ELF64)
│   ├── arch/x86_64/      # GDT, IDT, APIC, paging, CPU intrinsics
│   ├── mm/               # PMM (bitmap) + VMM (slab heap)
│   ├── hal/              # Serial driver, framebuffer console
│   ├── ke/               # Kernel executive: main, scheduler, printf
│   ├── lib/              # Freestanding string/memory library
│   └── linker.ld         # Kernel linker script
├── cmake/                # Cross-compilation toolchain files
├── scripts/              # build.sh, run-qemu.sh, create-disk.sh
└── docs/                 # Architecture & build documentation
```

## License

NovaOS is MIT licensed. No GPL code is used.
All Win32 API implementations are clean-room based on public Microsoft
documentation, ReactOS reference, and Wine source study (but independently
written).
