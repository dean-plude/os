/*
 * idt.c — Interrupt Descriptor Table initialization and dispatch
 *
 * We register all 256 stubs from isr_stubs.asm and then handle
 * interrupts/exceptions in interrupt_dispatch().
 */

#include "idt.h"
#include "../../ke/prof.h"
#include "../../um/um.h"
#include "gdt.h"
#include "cpu.h"
#include "apic.h"
#include "../../hal/serial.h"
#include "../../ke/printf.h"
#include "../../ke/syscall.h"
#include "../../ke/scheduler.h"
#include "../../ke/sleep.h"
#include "../../ps/ps.h"
#include "../../hal/ps2.h"
#include "../../hal/ioapic.h"
#include "../../ke/kpcr.h"
#include "../../ke/smp.h"
#include "../../ke/probe.h"
#include "../../ke/ksym.h"
#include "../../fs/bootlog.h"

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

    /* Breakpoint and int3 — allow ring 3 to trigger (DPL=3).  Interrupt
     * gates (IF cleared): an interrupt arriving before isr_common's swapgs
     * would otherwise run with the program's GS as its KPCR. */
    idt_set_gate(EXC_BREAKPOINT, isr_stub_table[EXC_BREAKPOINT], 0, 3);

    /* NT syscall (int 0x2E) — DPL=3 so userspace (32-bit programs) can
     * invoke it; entered with interrupts off, like SYSCALL */
    idt_set_gate(VECTOR_SYSCALL,  isr_stub_table[VECTOR_SYSCALL],  0, 3);

    /* __fastfail (int 0x29, code in RCX) — DPL=3 as on Windows: the program
     * ends at once with STATUS_STACK_BUFFER_OVERRUN, no handler runs */
    idt_set_gate(VECTOR_FASTFAIL, isr_stub_table[VECTOR_FASTFAIL], 0, 3);

    idt_load();
    kprintf("[IDT] Initialized: %d gates, IDTR base=0x%016lx\n",
            IDT_ENTRIES, (uint64_t)(uintptr_t)idt);
}

/* Load the (shared) IDT on the calling CPU */
void idt_load(void)
{
    Idtr idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)(uintptr_t)idt,
    };
    __asm__ volatile ("lidt %0" : : "m"(idtr) : "memory");
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
    PKPCR k = KiGetCurrentKpcr();
    Thread *t = (Thread *)k->CurrentThread;
    if (t)
        kprintf("  CPU %u, thread '%s' (TID %lu), kernel stack %p-%p\n", k->CpuNumber, t->name,
                t->tid, t->kernel_stack, (void *)((uintptr_t)t->kernel_stack + t->stack_size));
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
 * terminate_faulting_user_thread
 *
 * A ring-3 thread raised an exception nothing else handles (programs'
 * exceptions normally go to their SEH handlers, UmUserException).
 * Instead of halting the machine, end its program or mark its process as
 * exited and retire the faulting thread: it never returns to user mode,
 * and the scheduler switches to other work.  Only called for faults taken
 * in ring 3.
 * ----------------------------------------------------------------------- */
static void __attribute__((noreturn))
terminate_faulting_user_thread(NTSTATUS status, InterruptFrame *f, uint64_t addr)
{
    if (sched_current()->um)                  /* a kernel/um program: end it */
        UmFault(status, f->rip, addr);
    PEPROCESS proc = PsGetCurrentProcess();
    if (proc) PsTerminateProcess(proc, status);

    Thread *t = sched_current();
    kprintf("[IDT] Terminating faulting user thread '%s' (TID %lu), status 0x%x\n",
            t->name, t->tid, (UINT32)status);
    sched_exit_current();
}

/* NTSTATUS reported as the exit status for an unhandled user exception */
static NTSTATUS exception_status(uint64_t vector)
{
    switch (vector) {
    case EXC_DIVIDE_ERROR:     return STATUS_INTEGER_DIVIDE_BY_ZERO;
    case EXC_INVALID_OPCODE:   return STATUS_ILLEGAL_INSTRUCTION;
    case EXC_GENERAL_PROTECTION:
    case EXC_PAGE_FAULT:       return STATUS_ACCESS_VIOLATION;
    default:                   return STATUS_UNSUCCESSFUL;
    }
}

/* -----------------------------------------------------------------------
 * Page fault handler
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

    if (user && sched_current()->um)          /* program bug: no kernel dump */
        terminate_faulting_user_thread(STATUS_ACCESS_VIOLATION, f, cr2);

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
        KsymBacktrace(f->rip, f->rbp, f->rsp);
        kprintf("KERNEL PAGE FAULT — halting\n");
        BootLogPanic();
        UmCrashKernel();                    /* the report, for C:\NovaOS\Crashes at the next start */
        cpu_halt_forever();
    }

    /* (a Windows program's faults never get here: dispatch hands them to
     * its SEH handlers, UmUserException) */
    terminate_faulting_user_thread(STATUS_ACCESS_VIOLATION, f, cr2);
}

/* -----------------------------------------------------------------------
 * interrupt_dispatch — called from isr_common in isr_stubs.asm
 * ----------------------------------------------------------------------- */
static void dispatch(InterruptFrame *frame)
{
    uint64_t vector = frame->vector;

    /* ---- __fastfail: the end of the program (Windows doesn't dispatch it) ---- */
    if (vector == VECTOR_FASTFAIL && (frame->cs & 3) && sched_current()->um)
        UmFaultAt(0xC0000409u, frame->rip - 2, frame->rcx, frame->rsp);

    /* ---- CPU Exceptions (0–31) ---- */
    if (vector < 32) {
        /* A program's exception goes to its own handlers (SEH) */
        if ((frame->cs & 3) && sched_current()->um && vector != 2 && vector != 8 && vector != 18) {
            UmUserException(frame, vector == EXC_PAGE_FAULT ? read_cr2() : 0);
            return;
        }
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

        if ((frame->cs & 3) && sched_current()->um)
            terminate_faulting_user_thread(exception_status(vector), frame, 0);

        kprintf("\n=== EXCEPTION #%lu: %s ===\n", vector, name);
        kprintf("  Error code: 0x%lx\n", frame->error_code);
        dump_frame(frame);

        if (!(frame->cs & 3)) KsymBacktrace(frame->rip, frame->rbp, frame->rsp);
        if (vector == EXC_DOUBLE_FAULT) {
            kprintf("DOUBLE FAULT — system halted\n");
            cpu_halt_forever();
        }

        /* For unhandled kernel exceptions, panic */
        if (!(frame->cs & 3)) {  /* ring 0 */
            kprintf("Unhandled kernel exception — halting\n");
            BootLogPanic();                 /* (a PC without a serial port: the USB stick NovaOS started from) */
            UmCrashKernel();
            cpu_halt_forever();
        }
        /* (a program's exceptions went to its SEH handlers above,
         * UmUserException; what gets here from ring 3 is an NMI or a
         * machine check, or a thread that is not a program's) */
        terminate_faulting_user_thread(exception_status(vector), frame, 0);
    }

    /* ---- Spurious APIC interrupt ---- */
    if (vector == IRQ_SPURIOUS) {
        /* Do NOT send EOI for spurious */
        return;
    }

    /* ---- Timer interrupt (APIC local timer, vector IRQ_TIMER) ---- */
    if (vector == IRQ_TIMER) {
        apic_eoi();
        /* Drive the clock (and keyboard/mouse polling, see sched_tick) and
         * preemptive scheduling: may context-switch to another
         * thread; control returns here (on this thread's stack) before the
         * ISR epilogue performs IRETQ. */
        sched_tick();
        return;
    }

    /* ---- NT Syscall (int 0x2E) ---- */
    if (vector == VECTOR_SYSCALL && (frame->cs & ~3ULL) == GDT_USER_CODE32) {
        /* A 32-bit program (ntdll's 32-bit layer): EAX = number, EDX =
         * a block of 64-bit arguments laid out like the 64-bit stack
         * (1-4 in its first slots, 5+ from offset 0x28, see um_stack_arg).
         * The result comes back in EDX:EAX. */
        UINT64 blk = frame->rdx & 0xFFFFFFFFu, a[4] = { 0, 0, 0, 0 };
        UmNoteSyscallFrame(frame);                  /* NtGetContextThread while in here */
        UINT64 r;
        if ((INT32)CopyFromUser(a, (const void *)(uintptr_t)blk, sizeof(a)) < 0)
            r = 0xC0000005u;                        /* STATUS_ACCESS_VIOLATION */
        else
            r = KiSystemCallEntry(frame->rax & 0xFFFFFFFFu, a[0], a[1], a[2], a[3], blk);
        frame->rax = r & 0xFFFFFFFFu;
        frame->rdx = r >> 32;
        return;
    }
    if (vector == VECTOR_SYSCALL) {
        /* Dispatch via the NT syscall table.
         * Windows NT ABI: RAX=num, RCX=arg1, RDX=arg2, R8=arg3, R9=arg4 */
        frame->rax = (uint64_t)KiSystemCallDispatch(
            frame->rax,
            frame->rcx,
            frame->rdx,
            frame->r8,
            frame->r9,
            frame->rsp);
        return;
    }

    /* ---- All other IRQs ---- */
    if (vector >= IRQ_BASE) {
        /* (the device interrupts routed here are the ACPI SCI and the
         * touchpad's, through the GPIO controller or its own line: PS/2 is
         * read on the timer tick, and the other drivers mask theirs and
         * poll).  The handler quiets a level-triggered source before the EOI. */
        IrqDispatch((UINT8)vector);
        apic_eoi();
        return;
    }

    kprintf("[IRQ] Unhandled vector %lu\n", vector);
}

/* -----------------------------------------------------------------------
 * interrupt_dispatch — called from isr_common in isr_stubs.asm
 * ----------------------------------------------------------------------- */
/* isr_stubs.asm: the kernel ran with a program's GS (where 0: an
 * interrupt from ring 0 found it; 1: an interrupt was about to return to
 * ring 3 without the KPCR in GS).  The
 * SWAPGS pairing is broken: report it and stop, rather than run on with
 * some other structure as this CPU's KPCR. */
void __attribute__((noreturn)) gs_mismatch(InterruptFrame *f, int where)
{
    static const char *const what[] = {
        "interrupt from ring 0 with a program's GS",
        "return to ring 3 with GS not the KPCR",
    };
    kprintf("\n=== GS MISMATCH: %s ===\n", what[where]);
    if (f) {
        kprintf("  vector %lu, error code 0x%lx\n", f->vector, f->error_code);
        dump_frame(f);
        if (!(f->cs & 3)) KsymBacktrace(f->rip, f->rbp, f->rsp);
        /* the stack where it happened: an IRETQ's frame, for one */
        const uint64_t *sp = (const uint64_t *)(where ? (uint64_t)&f->rip : f->rsp);
        for (int i = 0; i < 8; i++) kprintf("  [%p] %016lx\n", (const void *)&sp[i], sp[i]);
    }
    PKPCR k = KiGetCurrentKpcr();
    Thread *prev = (Thread *)k->PrevThread;
    kprintf("  KernelRsp %lx, previous thread %s\n", (uint64_t)k->KernelRsp, prev ? prev->name : "-");
    cpu_halt_forever();
}

void interrupt_dispatch(InterruptFrame *frame)
{
    uint64_t vector = frame->vector;
    /* IPIs need nothing the kernel lock guards: handle them without it
     * (another CPU may hold it while it waits for us to flush). */
    if (vector == IPI_WAKE || vector == IPI_TLB) {
        smp_ipi(vector);
        return;
    }
    if (vector == EXC_NMI && SleepFreezing()) {   /* the machine is going to sleep */
        SleepFreezeCpu();
        return;
    }
    /* A fault inside a user-memory copy (the page went away under it):
     * the copy returns an error instead (probe.c) */
    if (vector == EXC_PAGE_FAULT && !(frame->cs & 3)) {
        /* the kernel touching a program's committed page for the first
         * time (a copy to or from user memory): back it and retry */
        uint64_t cr2 = read_cr2();
        if (!(frame->error_code & 1) && cr2 < UINT64_C(0x00007FFFFFFF0000) && sched_current()->um && UmDemandFault(cr2))
            return;
        if (UserCopyFixup(frame)) return;
    }
    /* A timer tick while this CPU is halted waiting for the kernel lock:
     * nothing to do (the clock follows the TSC, and the other CPUs keep
     * it), and switching the waiting thread out halfway would be wrong. */
    if (vector == IRQ_TIMER)
        ProfSample(frame->rip, frame->rbp, (frame->cs & 3) != 0, KiGetCurrentKpcr()->LockWait,
                   KiGetCurrentKpcr()->Idle || sched_current() == KiGetCurrentKpcr()->IdleThread);
    if (vector == IRQ_TIMER && KiGetCurrentKpcr()->LockWait) {
        apic_eoi();
        sched_timer_rearm();                            /* (the timer is one-shot) */
        return;
    }
    KiGetCurrentKpcr()->Idle = 0;
    /* The timer (the scheduler) and spurious interrupts run under their
     * own locks; everything else under the big kernel lock (nesting if the
     * interrupted thread holds it already).  A switch in between hands the
     * lock over and back (smp.h). */
    /* (a 32-bit program's system call takes the lock itself, like SYSCALL) */
    bool big = vector != IRQ_TIMER && vector != IRQ_SPURIOUS &&
               !(vector == VECTOR_SYSCALL && (frame->cs & ~3ULL) == GDT_USER_CODE32);
    if (big) { ProfInterrupt(vector); bkl_acquire(); }
    dispatch(frame);
    /* Returning to a user program that has been killed meanwhile? */
    if ((frame->cs & 3) && sched_current()->um) UmReturnToUserFrame(frame);
    if (big) bkl_release();
    if ((frame->cs & 3) && bkl_held()) {                /* never into user mode with it */
        kprintf("[SMP] Bug: returning to user mode holding the kernel lock (vector %lu)\n", vector);
        bkl_leave_kernel();
    }
    if (frame->cs & 3) sched_resched_pending();
}
