/*
 * idt.h — Interrupt Descriptor Table (x86_64)
 *
 * We define 256 IDT entries:
 *   0–19:   CPU exceptions (defined by Intel/AMD)
 *   20–31:  Reserved CPU exceptions
 *   32–47:  Legacy PIC IRQs (remapped; we disable PIC and use APIC)
 *   48–63:  Reserved for APIC MSI vectors
 *   0x80:   NT syscall vector (int 0x2E — matches NT ABI)
 *   0x81–0xFF: Additional OS-defined vectors
 *
 * Exception frame layout on the stack when an ISR is invoked:
 *
 *   [RSP+40] SS        (only pushed on ring change)
 *   [RSP+32] RSP_user  (only pushed on ring change)
 *   [RSP+24] RFLAGS
 *   [RSP+16] CS
 *   [RSP+8]  RIP
 *   [RSP+0]  Error code (or 0 for exceptions without one)
 *
 * Our assembly stubs push all GPRs after the error code, giving a
 * full InterruptFrame structure.
 */

#pragma once

#include "../../include/types.h"

/* Exception vector numbers */
#define EXC_DIVIDE_ERROR          0
#define EXC_DEBUG                 1
#define EXC_NMI                   2
#define EXC_BREAKPOINT            3
#define EXC_OVERFLOW              4
#define EXC_BOUND_RANGE           5
#define EXC_INVALID_OPCODE        6
#define EXC_DEVICE_NOT_AVAILABLE  7
#define EXC_DOUBLE_FAULT          8
#define EXC_COPROCESSOR_SEGMENT   9   /* Legacy, not generated on modern CPUs */
#define EXC_INVALID_TSS           10
#define EXC_SEGMENT_NOT_PRESENT   11
#define EXC_STACK_FAULT           12
#define EXC_GENERAL_PROTECTION    13
#define EXC_PAGE_FAULT            14
/* 15 is reserved */
#define EXC_X87_FP                16
#define EXC_ALIGNMENT_CHECK       17
#define EXC_MACHINE_CHECK         18
#define EXC_SIMD_FP               19
#define EXC_VIRTUALIZATION        20
#define EXC_CONTROL_PROTECTION    21
/* 22-27 reserved */
#define EXC_HYPERVISOR_INJECTION  28
#define EXC_VMM_COMM              29
#define EXC_SECURITY              30
/* 31 reserved */

/* IRQ base (APIC remapped to avoid conflict with exceptions) */
#define IRQ_BASE       0x30    /* IRQs start at vector 0x30 (48) */
#define IRQ_TIMER      (IRQ_BASE + 0)
#define IRQ_KEYBOARD   (IRQ_BASE + 1)
#define IRQ_SPURIOUS   0xFF

/* NT syscall vector */
#define VECTOR_SYSCALL  0x2E

/* Total IDT entries */
#define IDT_ENTRIES 256

/* IDT gate types */
#define IDT_TYPE_INTERRUPT 0xE   /* Clears IF on entry */
#define IDT_TYPE_TRAP      0xF   /* Preserves IF */

/* Raw 16-byte IDT gate descriptor */
typedef struct __packed {
    uint16_t  offset_low;    /* bits 0–15 of handler address */
    uint16_t  selector;      /* Code segment selector */
    uint8_t   ist;           /* IST index (0 = use current RSP) */
    uint8_t   type_attr;     /* P=1 | DPL | 0 | Type */
    uint16_t  offset_mid;    /* bits 16–31 */
    uint32_t  offset_high;   /* bits 32–63 */
    uint32_t  reserved;
} IdtGate;

/* IDTR — loaded by LIDT instruction */
typedef struct __packed {
    uint16_t limit;
    uint64_t base;
} Idtr;

/*
 * Register file saved by the ISR stub before calling the C handler.
 * Layout must match the push order in isr_stubs.asm.
 *
 * Note: The CPU pushes SS/RSP/RFLAGS/CS/RIP (and optional error code)
 * automatically.  We push the rest (GPRs + segment regs).
 */
typedef struct __packed {
    /* GPRs (pushed by our stub, in this order) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;

    /* Vector number (pushed by stub) */
    uint64_t vector;

    /* Error code (pushed by CPU for some exceptions, else stub pushes 0) */
    uint64_t error_code;

    /* CPU-pushed exception frame */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;      /* Only valid on ring-change (ring 3 → ring 0) */
    uint64_t ss;       /* Only valid on ring-change */
} InterruptFrame;

/* Initialize the IDT and load IDTR. */
void idt_init(void);
/* Load the IDT on the calling CPU (the other CPUs share it). */
void idt_load(void);

/*
 * C-level interrupt/exception dispatch.
 * Called from the assembly stubs with a pointer to the frame on the stack.
 */
void interrupt_dispatch(InterruptFrame *frame);

/* A page fault inside a user-memory copy: send the copy to its error
 * return (probe.c).  False if the fault wasn't one of those. */
bool UserCopyFixup(InterruptFrame *f);

/* Install a custom gate (for drivers adding IRQ handlers, etc.) */
void idt_set_gate(uint8_t vector, uintptr_t handler, uint8_t ist, uint8_t dpl);
