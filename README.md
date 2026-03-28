# NovaOS — Windows-Compatible Operating System

A clean-room, from-scratch x86_64 operating system designed to run native
Windows executables (PE32+) without emulation.

## Status: Phase 5 — Process Isolation & User-Mode Foundation

**What works:**

### Phase 1 — Boot & Kernel Foundation
- UEFI bootloader (PE32+ EFI application, loads kernel ELF from FAT32 ESP)
- Physical memory manager (bitmap allocator, reads UEFI memory map)
- Virtual memory (4-level paging, 64 GiB physmap, NX via EFER.NXE, CR4.PGE)
- Kernel heap (slab allocator: kmalloc/kfree/kzalloc)
- GDT/TSS with IST stacks for NMI, double fault, machine check
- IDT with all 256 handlers (CPU exceptions, APIC IRQs, INT 0x2E syscall gate)
- APIC timer at 100 Hz (legacy 8259A PIC disabled)
- Preemptive round-robin kernel scheduler (kernel threads, context switch via callee-saved regs)
- Serial console (COM1, 115200 baud) + GOP framebuffer text output
- Boots in QEMU with OVMF (UEFI firmware)

### Phase 2 — NT Kernel Personality
- Object Manager (`ObCreateObject`, `ObReferenceObject`, `ObDereferenceObject`, handle tables)
- Security Reference Monitor (`SeInitialize`, `SeCreateSystemToken`, ACL/ACE stubs)
- Process Manager (`EPROCESS`/`ETHREAD`/`KPROCESS`/`KTHREAD`, `PsCreateSystemProcess`, `PsCreateSystemThread`)
- Configuration Manager (registry hive bootstrap, `CmInitialize`)
- Syscall dispatcher: INT 0x2E (legacy) + SYSCALL/SYSRET MSR path (`KiSystemCall64`)
- NT syscall table with ~40 entries (NtAllocateVirtualMemory, NtCreateFile, etc.)

### Phase 3 — I/O, Sections, and PE Loader
- I/O Manager (`IoCreateDevice`, `IoCreateFile`, IRP dispatch: Create/Read/Write/QueryInfo)
- Virtual Memory Areas (`VMA_SPACE`, `VmaAllocate`, `VmaFree`, `VmaProtect` per-process)
- Section Objects (`MmCreateImageSection`, `MmMapImageView`, anonymous sections)
- PE32+ Loader (`LdrLoadImage`): header validation, section mapping, base relocation, import resolution
- Built-in stub DLLs: ntdll, kernel32, msvcrt, user32 (syscall thunks for import resolution)
- User-mode process creation (`PsCreateUserProcess` → IRETQ into ring-3 via `PsUserThreadEntry`)

### Phase 4 — VFS & InitRD
- VFS abstraction layer (`VfsMount`, `VfsOpen`, `VfsRead`, `VfsReadDir`, `VfsStat`)
- NT path alias resolution (`\??\C:\`, `\SystemRoot`, `\DosDevices\C:` → first mount)
- CPIO newc InitRD driver (`InitrdMount`): parses in-memory CPIO archive, mounts as `\Device\InitRD`
- Boot protocol v2: bootloader passes `initrd_base`/`initrd_size` in `BootInfo`
- File I/O via IRP dispatch: `NtReadFile`, `NtWriteFile` fully wired through VFS to InitRD

### Phase 5 — Process Isolation & User-Mode Foundation
- Per-process page tables: each user process gets its own PML4 (`paging_create_process_pt`)
- User-space mappings snapshotted into process PML4 at creation (`paging_clone_user_mappings`)
- CR3 switch on every context switch in the scheduler (`perform_switch`)
- KPCR (Kernel Processor Control Region): per-CPU GS-relative structure
  - `KPCR.KernelRsp` holds the current thread's kernel stack top for the SYSCALL path
  - Updated by the scheduler on every context switch
- SWAPGS: on SYSCALL entry, GS → KPCR; on SYSRET, GS → user TEB
- Kernel stack switch: `syscall_entry.asm` loads `gs:[KPCR_KERNEL_RSP]` before entering C
- PEB64/TEB64 allocation: `PsAllocatePebTeb` maps one page each at fixed user VAs
  - TEB.NtTib.Self initialized (GS:[0x30] self-pointer)
  - TEB.ClientId (PID/TID) and PEB.ImageBaseAddress set
- User GS base (`MSR_GS_BASE`) set to TEB VA before IRETQ so SWAPGS works on first syscall

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
| 2 | NT kernel personality (Object Manager, syscalls, registry) | ✅ **Done** |
| 3 | I/O subsystem (IRP, PE loader, section objects) | ✅ **Done** |
| 4 | VFS + InitRD (virtual file system, in-memory ramdisk) | ✅ **Done** |
| 5 | Process isolation (per-process CR3, KPCR, PEB/TEB, SWAPGS) | ✅ **Done** |
| 6 | Full user-mode (ntdll SYSCALL stubs, Win32 subsystem, CSRSS) | 🔄 Planned |
| 7 | GUI (window manager, GDI, desktop shell) | 🔄 Planned |
| 8 | Application compatibility (notepad, calc, 7-Zip) | 🔄 Planned |

## Architecture

```
os/
├── include/              # Shared headers (boot_protocol.h, types.h)
├── bootloader/           # UEFI bootloader (PE32+ EFI application)
│   ├── include/efi.h     # Minimal UEFI API headers
│   └── src/              # main.c, elf_loader.c, paging.c, console.c
├── kernel/               # NT-style kernel (freestanding ELF64)
│   ├── arch/x86_64/      # GDT, IDT, APIC, paging, CPU intrinsics
│   ├── mm/               # PMM (bitmap), VMM (slab), VMA, section objects
│   ├── hal/              # Serial driver, framebuffer console
│   ├── ke/               # Kernel executive: main, scheduler, printf, KPCR, syscall
│   ├── ob/               # Object Manager (handles, reference counting)
│   ├── ps/               # Process Manager (EPROCESS, ETHREAD, PEB, TEB)
│   ├── se/               # Security Reference Monitor
│   ├── cm/               # Configuration Manager (registry)
│   ├── io/               # I/O Manager (IRP, device objects, file objects)
│   ├── fs/               # VFS layer + InitRD CPIO driver
│   ├── ldr/              # PE32+ loader (image sections, relocations, imports)
│   ├── lib/              # Freestanding string/memory library
│   └── linker.ld         # Kernel linker script
├── cmake/                # Cross-compilation toolchain files
├── scripts/              # build.sh, run-qemu.sh, create-disk.sh
└── docs/                 # Architecture & build documentation
```

## Key Design Decisions

- **NT ABI compatibility**: syscall numbers, NTSTATUS codes, UNICODE_STRING, and structure
  offsets match Windows 10 1903 x64 so that real ntdll.dll stubs can be used unmodified.
- **Clean-room**: no GPL code; all NT API implementations are written from scratch
  using public Microsoft documentation and reverse-engineering references.
- **Single-CPU Phase 5**: KPCR is statically allocated for the boot CPU; SMP requires
  one KPCR per logical processor (Phase 7).
- **Page table isolation**: each user process has its own PML4; kernel entries (256–511)
  are shared; user entries (0–255) are snapshotted at process creation.

## License

NovaOS is MIT licensed. No GPL code is used.
All Win32 API implementations are clean-room based on public Microsoft
documentation, ReactOS reference, and Wine source study (but independently
written).
