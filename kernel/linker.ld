/*
 * linker.ld — Kernel linker script for NovaOS (x86_64)
 *
 * Layout decisions:
 *
 *  KERNEL_VIRT_BASE = 0xFFFFFFFF80000000  (top -2 GiB of virtual address space)
 *  KERNEL_PHYS_BASE = 0x0000000000200000  (physical load address: 2 MiB)
 *
 * The ELF LMA (Load Memory Address) is the physical address where the
 * bootloader places each section.  The ELF VMA (Virtual Memory Address)
 * is where the kernel sees each section at runtime.
 *
 *   LMA = VMA - KERNEL_VIRT_BASE + KERNEL_PHYS_BASE
 *       = VMA - 0xFFFFFFFF7FE00000
 *
 * The PHYSMAP window (0xFFFF800000000000 + phys) lets the kernel access
 * all physical RAM without explicit mappings.
 *
 * Section layout:
 *   .text.entry  — kernel entry stub (must be at the very start)
 *   .text        — all other code
 *   .rodata      — read-only data
 *   .data        — read-write data
 *   .bss         — zero-initialized (BSS is zeroed by entry.asm)
 *   [stack]      — static initial stack at end of BSS
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x0000000000200000;  /* 2 MiB physical */

/* Helper macro: virtual address → physical load address */
PHYS(addr) = (addr) - KERNEL_VIRT_BASE + KERNEL_PHYS_BASE;

SECTIONS {
    /* Start at virtual 0xFFFFFFFF80200000 = VIRT_BASE + PHYS_BASE */
    . = KERNEL_VIRT_BASE + KERNEL_PHYS_BASE;

    /* ---- Code ---- */
    .text ALIGN(4096) : AT(PHYS(ADDR(.text))) {
        __text_start = .;
        *(.text.entry)      /* Entry stub must be first */
        *(.text .text.*)
        __text_end = .;
    }

    /* ---- Read-only data ---- */
    .rodata ALIGN(4096) : AT(PHYS(ADDR(.rodata))) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        *(.eh_frame_hdr)
        *(.eh_frame)
        __rodata_end = .;
    }

    /* ---- Read-write data ---- */
    .data ALIGN(4096) : AT(PHYS(ADDR(.data))) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    /* ---- BSS (zero-initialized) ---- */
    .bss ALIGN(4096) : AT(PHYS(ADDR(.bss))) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* ---- Initial kernel stack (16 KiB) ---- */
    /* Placed immediately after BSS; grows downward from __kernel_stack_top */
    . = ALIGN(4096);
    __kernel_stack_bottom = .;
    . += 0x4000;    /* 16 KiB */
    __kernel_stack_top = .;

    /* ---- Kernel end marker ---- */
    __kernel_end = .;

    /* Physical end address (for bootloader to know how much to load) */
    __kernel_phys_end = PHYS(__kernel_end);

    /* ---- Discard unwanted sections ---- */
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.gnu.version*)
        *(.gnu.hash)
        *(.dynsym)
        *(.dynstr)
        *(.dynamic)
        *(.rel.*)
        *(.rela.*)
        *(.plt)
        *(.plt.*)
    }
}
