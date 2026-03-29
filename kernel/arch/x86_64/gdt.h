/*
 * gdt.h — Global Descriptor Table (64-bit)
 *
 * In 64-bit mode most segment descriptors are ignored by the CPU,
 * but we still need:
 *  - A valid code segment (CS) for SYSCALL/SYSRET and exception handling
 *  - A data segment (DS/SS/ES) — the CPU checks the base is 0 in 64-bit mode
 *  - Separate user-mode code and data segments (DPL=3) for when we run
 *    user processes
 *  - A TSS (Task State Segment) descriptor so the CPU knows where to
 *    find the kernel stack (RSP0) when entering ring 0 from ring 3
 *
 * Segment selector layout (matches NT conventions where possible):
 *   0x00 — Null
 *   0x08 — Kernel code (ring 0, 64-bit)
 *   0x10 — Kernel data (ring 0)
 *   0x18 — User code  (ring 3, 64-bit)    ← matches Windows selector layout
 *   0x20 — User data  (ring 3)
 *   0x28 — TSS low word  (16 bytes total — System descriptor is 16 bytes)
 *   0x30 — TSS high word
 *
 * Windows uses 0x18 for user CS and 0x20 for user DS.  This is important
 * for SYSCALL/SYSRET: STAR MSR encodes these statically.
 */

#pragma once

#include "../../include/types.h"

/* Selector values */
#define GDT_NULL          0x00
#define GDT_KERNEL_CODE   0x08
#define GDT_KERNEL_DATA   0x10
#define GDT_USER_CODE     0x18   /* DPL=3: selector | 3 = 0x1B */
#define GDT_USER_DATA     0x20   /* DPL=3: selector | 3 = 0x23 */
#define GDT_TSS           0x28   /* 16 bytes — two consecutive slots */

/* RPL (Requested Privilege Level) ORed into selectors for ring-3 use */
#define RPL_RING0  0
#define RPL_RING3  3

/* User selectors with RPL set */
#define SEL_USER_CODE   (GDT_USER_CODE | RPL_RING3)
#define SEL_USER_DATA   (GDT_USER_DATA | RPL_RING3)

/* Number of regular 8-byte entries */
#define GDT_ENTRY_COUNT   7   /* null + kcode + kdata + ucode + udata + tss_lo + tss_hi */

/* Raw 64-bit GDT entry */
typedef struct __packed {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;     /* P|DPL(2)|S|Type(4) */
    uint8_t  granularity; /* G|DB|L|AVL|Limit_high(4) */
    uint8_t  base_high;
} GdtEntry64;

/* Task State Segment — 64-bit version */
typedef struct __packed {
    uint32_t reserved0;
    uint64_t rsp[3];       /* Ring 0/1/2 kernel stacks */
    uint64_t reserved1;
    uint64_t ist[7];       /* Interrupt Stack Table pointers */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;  /* I/O permission bitmap offset */
} Tss64;

/* 16-byte TSS system descriptor (two consecutive GDT slots) */
typedef struct __packed {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;       /* 0x89 = Present, Ring0, System, Type=TSS Available */
    uint8_t  flags;        /* G=0, limit high nibble */
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} TssDescriptor;

/* GDTR — loaded by LGDT instruction */
typedef struct __packed {
    uint16_t limit;
    uint64_t base;
} Gdtr;

/* IST (Interrupt Stack Table) slot assignments.
 * IST1 is used for NMI and double fault — these need a known-good stack. */
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_MACHINE_CHECK 3
#define IST_DEBUG         4

/* Per-CPU GDT state */
typedef struct {
    GdtEntry64    entries[GDT_ENTRY_COUNT];
    TssDescriptor tss_descriptor;   /* Overlaps entries[5..6] — accessed as 16 bytes */
    Tss64         tss;
} __aligned(16) CpuGdt;

/*
 * Initialize the GDT and TSS for the boot CPU.
 * Sets up kernel code/data, user code/data, and installs a TSS with
 * a dedicated double-fault stack (IST1).
 */
void gdt_init(void);

/*
 * Reload segment registers after the GDT is live.
 * Uses a far return to reload CS.
 */
void gdt_reload_segments(void);

/*
 * Update TSS.RSP0 — the kernel stack pointer used when returning from ring 3.
 * Called on every context switch so each thread gets its own kernel stack.
 */
void gdt_set_rsp0(uintptr_t rsp0);
