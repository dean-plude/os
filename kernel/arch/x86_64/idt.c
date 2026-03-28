/*
 * idt.c — Interrupt Descriptor Table initialization and dispatch
 *
 * We register all 256 stubs from isr_stubs.asm and then handle
 * interrupts/exceptions in interrupt_dispatch().
 */

#include "idt.h"
#include "gdt.h"
#include "cpu.h"
#include "apic.h"
#include "../../hal/serial.h"
#include "../../ke/printf.h"
#include "../../ke/syscall.h"

/* Assembly stub address table (defined in isr_stubs.asm) */
extern uintptr_t isr_stub_table[IDT_ENTRIES];

/* The static IDT */
static IdtGate idt[IDT_ENTRIES] __aligned(16);

/* -----------------------------------------------------------------------
 * idt_set_gate
 * ----------------------------------------------------------------------- */
void idt_set_gate(uint8_t vector, uintptr_t handler, uint8_t ist, uint8_t dpl)
{
    IdtGate *g = &idt[vector];
    g->offset_low  = (uint16_t)(handler & 0xFFFF);
    g->offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    g->offset_high = (uint32_t)(handler >> 32);
    g->selector    = GDT_KERNEL_CODE;
    g->ist         = ist & 0x7;
    g->type_attr   = (uint8_t)(0x80 | (dpl << 5) | IDT_TYPE_INTERRUPT);
    g->reserved    = 0;
}

/* Trap gate (preserves IF) */
static void set_trap_gate(uint8_t vector, uintptr_t handler, uint8_t ist, uint8_t dpl)
{
    IdtGate *g = &idt[vector];
    g->offset_low  = (uint16_t)(handler & 0xFFFF);
    g->offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    g->offset_high = (uint32_t)(handler >> 32);
    g->selector    = GDT_KERNEL_CODE;
    g->ist         = ist & 0x7;
    g->type_attr   = (uint8_t)(0x80 | (dpl << 5) | IDT_TYPE_TRAP);
    g->reserved    = 0;
}

/* -----------------------------------------------------------------------
 * idt_init
 * ----------------------------------------------------------------------- */
void idt_init(void)
{
    /* Register all 256 stubs as interrupt gates (clears IF on entry). */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate((uint8_t)i, isr_stub_table[i], 0, 0);
    }

    /* Exceptions that need dedicated stacks (via IST) */
    idt_set_gate(EXC_DOUBLE_FAULT,   isr_stub_table[EXC_DOUBLE_FAULT],
                 IST_DOUBLE_FAULT, 0);
    idt_set_gate(EXC_NMI,            isr_stub_table[EXC_NMI],
                 IST_NMI, 0);
    idt_set_gate(EXC_MACHINE_CHECK,  isr_stub_table[EXC_MACHINE_CHECK],
                 IST_MACHINE_CHECK, 0);
    idt_set_gate(EXC_DEBUG,          isr_stub_table[EXC_DEBUG],
                 IST_DEBUG, 0);

    /* Breakpoint and int3 — allow ring 3 to trigger (DPL=3) */
    set_trap_gate(EXC_BREAKPOINT, isr_stub_table[EXC_BREAKPOINT], 0, 3);

    /* NT syscall (int 0x2E) — DPL=3 so userspace can invoke it */
    set_trap_gate(VECTOR_SYSCALL,  isr_stub_table[VECTOR_SYSCALL],  0, 3);

    /* Load the IDTR */
    Idtr idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)(uintptr_t)idt,
    };
    __asm__ volatile ("lidt %0" : : "m"(idtr) : "memory");

    kprintf("[IDT] Initialized: %d gates, IDTR base=0x%016lx\n",
            IDT_ENTRIES, idtr.base);
}

/* -----------------------------------------------------------------------
 * Exception names (for diagnostic output)
 * ----------------------------------------------------------------------- */
static const char *exception_names[32] = {
    [0]  = "Divide Error (#DE)",
    [1]  = "Debug (#DB)",
    [2]  = "NMI",
    [3]  = "Breakpoint (#BP)",
    [4]  = "Overflow (#OF)",
    [5]  = "Bound Range (#BR)",
    [6]  = "Invalid Opcode (#UD)",
    [7]  = "Device Not Available (#NM)",
    [8]  = "Double Fault (#DF)",
    [9]  = "Coprocessor Segment Overrun",
    [10] = "Invalid TSS (#TS)",
    [11] = "Segment Not Present (#NP)",
    [12] = "Stack-Segment Fault (#SS)",
    [13] = "General Protection Fault (#GP)",
    [14] = "Page Fault (#PF)",
    [15] = "Reserved (15)",
    [16] = "x87 FP Exception (#MF)",
    [17] = "Alignment Check (#AC)",
    [18] = "Machine Check (#MC)",
    [19] = "SIMD FP Exception (#XF)",
    [20] = "Virtualization Exception (#VE)",
    [21] = "Control Protection (#CP)",
};

/* Dump the exception frame for debugging */
static void dump_frame(InterruptFrame *f)
{
    kprintf("  RIP=%016lx  CS=%04lx  RFLAGS=%016lx\n",
            f->rip, f->cs, f->rflags);
    kprintf("  RSP=%016lx  SS=%04lx\n", f->rsp, f->ss);
    kprintf("  RAX=%016lx  RBX=%016lx  RCX=%016lx  RDX=%016lx\n",
            f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("  RSI=%016lx  RDI=%016lx  RBP=%016lx\n",
            f->rsi, f->rdi, f->rbp);
    kprintf("  R8 =%016lx  R9 =%016lx  R10=%016lx  R11=%016lx\n",
            f->r8, f->r9, f->r10, f->r11);
    kprintf("  R12=%016lx  R13=%016lx  R14=%016lx  R15=%016lx\n",
            f->r12, f->r13, f->r14, f->r15);
}

/* -----------------------------------------------------------------------
 * Kernel-mode page fault handler (placeholder — VMM not yet wired in)
 * ----------------------------------------------------------------------- */
static void handle_page_fault(InterruptFrame *f)
{
    uint64_t cr2 = read_cr2();

    /*
     * Error code bits:
     *   0: P  — page was present (else not present)
     *   1: W  — write access (else read)
     *   2: U  — user-mode access (else supervisor)
     *   3: R  — reserved bit set in PTE
     *   4: I  — instruction fetch
     *   5: PK — protection key violation
     */
    bool present   = !!(f->error_code & 1);
    bool write     = !!(f->error_code & 2);
    bool user      = !!(f->error_code & 4);
    bool reserved  = !!(f->error_code & 8);
    bool ifetch    = !!(f->error_code & 16);

    kprintf("\n=== PAGE FAULT ===\n");
    kprintf("  Fault address (CR2): 0x%016lx\n", cr2);
    kprintf("  Access: %s %s %s%s%s\n",
            user    ? "user"      : "kernel",
            write   ? "write"     : "read",
            present ? "(present)" : "(not present)",
            reserved ? " [RESERVED PTE BIT]" : "",
            ifetch   ? " [INSTRUCTION FETCH]" : "");
    dump_frame(f);

    kprintf("  Error code: 0x%lx\n", f->error_code);

    /* If this is a kernel-mode fault we can't recover — kernel bug */
    if (!user) {
        kprintf("KERNEL PAGE FAULT — halting\n");
        cpu_halt_forever();
    }

    /* TODO Phase 2+: deliver EXCEPTION_ACCESS_VIOLATION to user process */
    kprintf("USER PAGE FAULT — process termination not yet implemented\n");
    cpu_halt_forever();
}

/* -----------------------------------------------------------------------
 * interrupt_dispatch — called from isr_common in isr_stubs.asm
 * ----------------------------------------------------------------------- */
void interrupt_dispatch(InterruptFrame *frame)
{
    uint64_t vector = frame->vector;

    /* ---- CPU Exceptions (0–31) ---- */
    if (vector < 32) {
        const char *name = (vector < 22 && exception_names[vector])
                           ? exception_names[vector]
                           : "Unknown Exception";

        if (vector == EXC_PAGE_FAULT) {
            handle_page_fault(frame);
            return;
        }

        if (vector == EXC_BREAKPOINT || vector == EXC_DEBUG) {
            kprintf("[DBG] Breakpoint/debug trap at RIP=0x%016lx\n", frame->rip);
            /* For INT3: advance past the CC byte so we don't loop */
            if (vector == EXC_BREAKPOINT) frame->rip++;
            return;
        }

        kprintf("\n=== EXCEPTION #%lu: %s ===\n", vector, name);
        kprintf("  Error code: 0x%lx\n", frame->error_code);
        dump_frame(frame);

        if (vector == EXC_DOUBLE_FAULT) {
            kprintf("DOUBLE FAULT — system halted\n");
            cpu_halt_forever();
        }

        /* For unhandled kernel exceptions, panic */
        if (!(frame->cs & 3)) {  /* ring 0 */
            kprintf("Unhandled kernel exception — halting\n");
            cpu_halt_forever();
        }
        /* TODO: signal user process */
        cpu_halt_forever();
        return;
    }

    /* ---- Spurious APIC interrupt ---- */
    if (vector == IRQ_SPURIOUS) {
        /* Do NOT send EOI for spurious */
        return;
    }

    /* ---- Timer interrupt (APIC local timer, vector IRQ_TIMER) ---- */
    if (vector == IRQ_TIMER) {
        apic_eoi();
        /* TODO Phase 1 final: call scheduler tick */
        return;
    }

    /* ---- NT Syscall (int 0x2E) ---- */
    if (vector == VECTOR_SYSCALL) {
        /* Dispatch via the NT syscall table.
         * Windows NT ABI: RAX=num, RCX=arg1, RDX=arg2, R8=arg3, R9=arg4 */
        frame->rax = (uint64_t)KiSystemCallDispatch(
            frame->rax,
            frame->rcx,
            frame->rdx,
            frame->r8,
            frame->r9);
        return;
    }

    /* ---- All other IRQs ---- */
    if (vector >= IRQ_BASE) {
        apic_eoi();
        /* TODO: dispatch to registered IRQ handlers */
        return;
    }

    kprintf("[IRQ] Unhandled vector %lu\n", vector);
}
