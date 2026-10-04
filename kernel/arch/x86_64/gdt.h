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
 * Segment selector layout:
 *   0x00 — Null
 *   0x08 — Kernel code (ring 0, 64-bit)
 *   0x10 — Kernel data (ring 0)
 *   0x18 — User data  (ring 3)
 *   0x20 — User code  (ring 3, 64-bit)
 *   0x28 — TSS low word  (16 bytes total — System descriptor is 16 bytes)
 *   0x30 — TSS high word
 *
 * User data MUST sit directly below user code: SYSRETQ loads
 * SS = STAR[63:48] + 8 and CS = STAR[63:48] + 16, so with STAR[63:48] =
 * 0x10 we get SS = 0x18|3 and CS = 0x20|3.
 */

#pragma once

#include "../../include/types.h"

/* Selector values: Windows' x64 layout (KGDT64_* in the WDK's ntddk.h),
 * which programs see and depend on.  The user selectors are 0x23 (32-bit
 * code), 0x2B (data and stack), 0x33 (64-bit code) and 0x53 (the 32-bit
 * TEB that FS addresses); code that far-jumps to 0x23 to run 32-bit code
 * in a 64-bit process ("heaven's gate" in reverse, as Roblox's Hyperion
 * does) and back to 0x33 finds what it expects.  SYSCALL/SYSRET force the
 * order: kernel CS, then kernel SS 8 above it; user 32-bit CS, then SS 8
 * above it, then 64-bit CS 16 above it (syscall.c). */
#define GDT_NULL          0x00
#define GDT_KERNEL_CODE   0x10   /* KGDT64_R0_CODE */
#define GDT_KERNEL_DATA   0x18   /* KGDT64_R0_DATA */
#define GDT_USER_CODE32   0x20   /* KGDT64_R3_CMCODE, DPL=3, 32-bit (compatibility mode): 0x23 */
#define GDT_USER_DATA     0x28   /* KGDT64_R3_DATA,   DPL=3: selector | 3 = 0x2B */
#define GDT_USER_CODE     0x30   /* KGDT64_R3_CODE,   DPL=3: selector | 3 = 0x33 */
#define GDT_TSS           0x40   /* KGDT64_SYS_TSS: 16 bytes, two consecutive slots */
#define GDT_USER_TEB32    0x50   /* KGDT64_R3_CMTEB,  DPL=3: 0x53, based at a 32-bit thread's TEB */

/* RPL (Requested Privilege Level) ORed into selectors for ring-3 use */
#define RPL_RING0  0
#define RPL_RING3  3

/* User selectors with RPL set */
#define SEL_USER_CODE   (GDT_USER_CODE | RPL_RING3)
#define SEL_USER_DATA   (GDT_USER_DATA | RPL_RING3)
#define SEL_USER_CODE32 (GDT_USER_CODE32 | RPL_RING3)
#define SEL_USER_TEB32  (GDT_USER_TEB32 | RPL_RING3)

/* SYSCALL loads SS from kernel CS + 8; SYSRET loads SS from 32-bit CS + 8
 * and 64-bit CS from 32-bit CS + 16 */
_Static_assert(GDT_KERNEL_DATA == GDT_KERNEL_CODE + 8, "SYSCALL selector order");
_Static_assert(GDT_USER_DATA == GDT_USER_CODE32 + 8 && GDT_USER_CODE == GDT_USER_CODE32 + 16,
               "SYSRET selector order");

/* Number of regular 8-byte entries: 0x00-0x68, Windows' limit of 0x6F
 * (0x08 and 0x60-0x68 are left empty) */
#define GDT_ENTRY_COUNT   14

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
    TssDescriptor tss_descriptor;   /* Copied over entries[8..9] (GDT_TSS) */
    Tss64         tss;
} __aligned(16) CpuGdt;

/*
 * Initialize the GDT and TSS for the boot CPU.
 * Sets up kernel code/data, user code/data, and installs a TSS with
 * a dedicated double-fault stack (IST1).
 */
void gdt_init(void);
CpuGdt *gdt_boot(void);

/* Build and load a GDT + TSS for the calling CPU; the four IST stacks are
 * EXCEPTION_STACK_SIZE bytes each. */
#define EXCEPTION_STACK_SIZE 0x4000
void gdt_init_cpu(CpuGdt *g, uint8_t *double_fault_stack, uint8_t *nmi_stack,
                  uint8_t *machine_check_stack, uint8_t *debug_stack);

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

/*
 * Point this CPU's 0x53 descriptor at a 32-bit thread's TEB (FS's base
 * also comes from MSR_IA32_FSBASE; the descriptor serves a program that
 * reloads FS).  Called on every switch to a user thread.
 */
void gdt_set_teb32(uint32_t base);
