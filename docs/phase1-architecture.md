# NovaOS — Phase 1 Architecture Document

> **Historical document.**  This describes the kernel as Phase 1 left it:
> the boot flow and address-space layout are still accurate, but much else
> has changed since (SMP with per-core run queues, a 32-bit user code
> segment for WoW64, the user-mode loader in `kernel/um/`).  For the
> current system see the [README](../README.md),
> [HISTORY.md](HISTORY.md) and [building.md](building.md).

## Overview

Phase 1 implements the foundational layer: UEFI boot, physical and virtual
memory management, interrupt handling, and a preemptive kernel scheduler.
After Phase 1 boots successfully in QEMU, you have a running kernel with
multiple threads, serial + framebuffer output, and a working interrupt
infrastructure ready for NT personality layering.

---

## System Boot Flow

```
Power on
  └─> UEFI Firmware (OVMF)
        └─> Loads /EFI/BOOT/BOOTX64.EFI from ESP
              └─> Bootloader (bootx64.efi)
                    1. Locates kernel.elf on the ESP
                    2. Parses ELF64, allocates physical pages at LMA
                    3. Queries GOP for framebuffer
                    4. Finds ACPI RSDP in EFI config table
                    5. Builds 4-level page tables:
                       - Identity map: [0..4GiB) → [0..4GiB)
                       - Physmap:    [PHYSMAP_BASE..+64GiB) → [0..64GiB)
                       - Kernel:     [KERN_VIRT..+size) → [KERN_PHYS..+size)
                    6. Allocates boot stack (16 KiB)
                    7. Gets UEFI memory map
                    8. Calls ExitBootServices()
                    9. Loads new CR3 (paging active)
                   10. Jumps to kernel virtual entry
              └─> Kernel (kernel.elf)
                    entry.asm (_start):
                      - Sets static BSS stack
                      - Zeroes .bss
                      - Calls KiSystemStartup(BootInfo*)
                    ke/main.c (KiSystemStartup):
                      - Serial init
                      - Framebuffer init
                      - PMM init (bitmap from memory map)
                      - Paging init (take ownership, enable NX)
                      - VMM init (slab heap)
                      - GDT reload (TSS + IST stacks)
                      - IDT init (256 gates, exception handlers)
                      - APIC init (disable 8259A, start timer)
                      - Scheduler init + test threads
                      - STI (interrupts live)
                      - Main idle loop
```

---

## Virtual Address Space Layout

```
0x0000000000000000  ┌─────────────────────────────────┐
                    │         User Space               │
                    │     (128 TiB — Phase 4+)         │
0x00007FFFFFFFFFFF  └─────────────────────────────────┘
0x0000800000000000  ┌─────────────────────────────────┐  ← Canonical hole
                    │    Non-canonical address gap     │
0xFFFF7FFFFFFFFFFF  └─────────────────────────────────┘
0xFFFF800000000000  ┌─────────────────────────────────┐  ← PHYSMAP_BASE
                    │   Direct Physical Map (64 GiB)   │
                    │   phys[0..64GiB) → virt here     │
0xFFFF87FFFFFFFFFF  └─────────────────────────────────┘
                    (gap)
0xFFFFFFFF80000000  ┌─────────────────────────────────┐  ← KERNEL_VIRT_BASE
                    │  .text.entry  (entry.asm)        │  0xFFFFFFFF80200000
                    │  .text        (all kernel code)  │
                    │  .rodata                         │
                    │  .data                           │
                    │  .bss + stack                    │
0xFFFFFFFFFFFFFFFF  └─────────────────────────────────┘
```

The physmap window lets the kernel access any physical address as
`PHYSMAP_BASE + phys_addr` without needing to set up new page table entries.
This is the same design used by Linux (`PAGE_OFFSET`) and is critical for
performance (no TLB misses for common kernel data access patterns).

---

## Memory Manager

### Physical Memory Manager (PMM)

**Algorithm**: Two-level bitmap allocator

```
Bitmap: 1 bit per 4KiB page (1=used, 0=free)
        64 pages per uint64_t word
        Walking: skip fully-allocated words with a single compare
```

**Initialization**:
1. Find highest physical address from boot memory map
2. Allocate bitmap at first suitable conventional memory region
3. Mark everything used
4. Mark CONVENTIONAL regions free
5. Re-mark: kernel image, bitmap itself, first 1 MiB as used

**Performance**: O(N/64) allocation worst case. Acceptable for Phase 1.
Phase 2 replaces with a buddy allocator for O(log N) contiguous allocations.

### Virtual Memory Manager (VMM)

**Kernel heap**: Slab allocator

```
Slab sizes: 8, 16, 32, 64, 128, 256, 512, 1024, 2048 bytes
Each slab:  One 4KiB page, header at page start, objects following
Free list:  Embedded in free objects (first 8 bytes = next pointer)
Large (>2KB): Direct PMM allocation + header page for size tracking
```

The heap lives entirely in the physmap window — no new page table entries
needed. This is efficient for Phase 1 but means kernel data isn't protected
from accidental overwrites. Phase 2 will add guard pages.

---

## Interrupt Architecture

### GDT Layout

| Selector | Description           | DPL |
|----------|-----------------------|-----|
| 0x00     | Null                  | —   |
| 0x08     | Kernel Code (64-bit)  | 0   |
| 0x10     | Kernel Data           | 0   |
| 0x18     | User Code (64-bit)    | 3   |
| 0x20     | User Data             | 3   |
| 0x28     | TSS (16-byte entry)   | 0   |

The SYSCALL/SYSRET instruction reads STAR MSR to determine CS/SS selectors.
Our layout (0x08/0x10 kernel, 0x18/0x20 user) matches Windows NT's layout
exactly, which simplifies STAR MSR configuration for Phase 4.

### IDT Gate Assignments

| Vectors   | Purpose                              |
|-----------|--------------------------------------|
| 0–31      | CPU exceptions (#DE, #PF, #GP, etc.) |
| 32–47     | APIC hardware IRQs (remapped PIC)    |
| 0x30      | APIC timer (scheduler tick)          |
| 0x2E      | NT syscall (int 0x2E)                |
| 0xFF      | APIC spurious interrupt              |

**IST (Interrupt Stack Table)** entries:
- IST1: Double fault (#DF) — dedicated stack so stack overflow doesn't compound
- IST2: NMI — guaranteed clean stack
- IST3: Machine check (#MC)
- IST4: Debug (#DB)

### Exception Dispatch

The assembly stubs in `isr_stubs.asm` use a macro-generated approach:
256 stubs, each pushing a dummy error code (for exceptions without one) and
the vector number, then jumping to the common handler which saves all GPRs
and calls `interrupt_dispatch()`.

---

## APIC Timer and Scheduler

### APIC Timer Calibration

The APIC timer frequency is proportional to the CPU bus clock, which varies
by CPU model. We calibrate against the PIT (8254 timer) channel 2:

1. Set PIT channel 2 for a 10ms one-shot count
2. Start APIC timer counting down from 0xFFFFFFFF
3. Wait for PIT channel 2 to expire
4. APIC ticks counted in 10ms = calibration value

### Scheduler Design

**Algorithm**: Round-robin with fixed 20ms time quantum

```
Ready queue: Circular doubly-linked list
Thread states: RUNNING, READY, WAITING, SUSPENDED, DEAD
Context switch: Save callee-saved GPRs + RFLAGS to ThreadContext
                Load next thread's ThreadContext
                Update TSS.RSP0 for next thread
```

**Initial thread setup** (for newly created threads):
The initial stack has the thread_trampoline address as a "return address".
When `context_switch()` does its `ret`, it jumps to `thread_trampoline`.
The entry function pointer is in R12 (callee-saved), and the argument in R13.

---

## Key Data Structures

### BootInfo (boot_protocol.h)
Passed from bootloader to kernel. Contains memory map, framebuffer
descriptor, ACPI RSDP pointer, and kernel placement information.

### Thread (ke/scheduler.h)
```c
typedef struct Thread {
    struct Thread  *next, *prev;  // Ready queue linkage
    uint64_t        tid;          // Thread ID
    ThreadState     state;
    ThreadContext   context;      // Saved registers (for context switch)
    void           *kernel_stack; // Stack base
    uint8_t         priority;     // 0-31 (NT-compatible)
    void           *process;      // Owner process (Phase 2)
} Thread;
```

### InterruptFrame (arch/x86_64/idt.h)
Complete saved register state at interrupt entry. Laid out to match
the CPU's automatic push + our assembly stub's pushes.

---

## Build System

The build uses CMake with two separate sub-builds (via ExternalProject):

```
Root CMakeLists.txt
├── ExternalProject: kernel_build
│   └── kernel/CMakeLists.txt
│       ├── C compiler: x86_64-elf-gcc OR clang -target x86_64-unknown-none-elf
│       ├── ASM: NASM (elf64 format)
│       └── Linker: custom linker.ld
└── ExternalProject: bootloader_build
    └── bootloader/CMakeLists.txt
        ├── C compiler: x86_64-w64-mingw32-gcc OR clang -target x86_64-windows
        └── Produces: PE32+ EFI application
```

---

## Testing

### Phase 1 Smoke Tests

After boot, the kernel creates three test threads:
1. `thread_a` — 5 yield cycles, tests scheduler interleaving
2. `thread_b` — 5 yield cycles, runs concurrently with thread_a
3. `mem_test` — allocates/verifies/frees 16 objects of various sizes

Expected serial output:
```
NovaOS Bootloader v0.1
...
[NovaOS] Serial console active
[PMM] Memory map: N entries
[PMM] Free: X MiB / 256 MiB
[PAGING] NX enabled
[GDT] GDT/TSS initialized
[IDT] Initialized: 256 gates
[APIC] APIC timer started at 100 Hz
[SCHED] Scheduler initialized
[Thread A] iteration 0 (TID=2)
[Thread B] iteration 0 (TID=3)
[Thread A] iteration 1 (TID=2)
...
[MemTest] All allocations OK
```

### QEMU GDB Debugging

```bash
# Terminal 1: start QEMU in debug mode
./scripts/run-qemu.sh build/nova.img /usr/share/OVMF/OVMF_CODE.fd --gdb

# Terminal 2: connect GDB
gdb build/kernel.elf \
    -ex 'target remote :1234' \
    -ex 'hbreak KiSystemStartup' \
    -ex 'continue'
```

---

## Known Limitations (Phase 1)

Everything Phase 1 left out has since been built:

| Component | At Phase 1 | Landed in |
|-----------|------------|-----------|
| Object Manager | Not implemented | Phase 2 |
| NT syscall dispatcher | Stub only | Phase 2 (table), Phase 9 (real services) |
| Registry | Not implemented | Phase 2 (bootstrap), Phase 10 (real, saved to disk) |
| IRP-based I/O | Not implemented | Phase 3 |
| NTFS driver | Not implemented | Phase 18 (other drives, read and write; drive C: is still FAT, see [ROADMAP.md](ROADMAP.md)) |
| PE loader | Not implemented | Phase 3, ring 3 in Phase 9 |
| Win32 API (kernel32, ntdll) | Not implemented | Phase 9 onward |
| Window Manager / GDI | Not implemented | Phase 7, Win32 window system in Phase 12 |
| Winsock / TCP stack | Not implemented | Phase 8 (lwIP), Phase 9 (ws2_32) |
| SMP (multi-core) | Single CPU only | Phase 11 |
| 32-bit WoW64 | Not planned | Phase 13 |
