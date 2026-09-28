/*
 * um_exception.c — structured exception handling: the kernel half
 *
 * A CPU exception in a program (page fault, divide error, int3, ...) is
 * handed to ntdll as on Windows: the thread's registers go into a CONTEXT
 * and the cause into an EXCEPTION_RECORD, both pushed on the user stack,
 * and the thread resumes at ntdll!KiUserExceptionDispatcher(RCX = CONTEXT,
 * RDX = EXCEPTION_RECORD).  ntdll runs the vectored and frame-based
 * handlers; a handler that fixes things up resumes through NtContinue.
 * An exception nobody handles comes back as NtRaiseException(...,
 * FirstChance = FALSE) and ends the process with a crash report.  If the
 * exception can't even be delivered (no stack left), the process ends too.
 */

#include "um_internal.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../lib/string.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/idt.h"

#define CONTEXT_SIZE      0x4D0
#define RECORD_SIZE       0x98
#define FRAME_SIZE        0x570        /* CONTEXT + EXCEPTION_RECORD, 16-byte multiple */
#define CONTEXT_ALL       0x0010001Fu  /* AMD64 | CONTROL | INTEGER | SEGMENTS | FLOATING_POINT | DEBUG */
#define USER_TOP          UINT64_C(0x00007FFFFFFFFFFF)
#define USER_FLAGS        0x00250FD5ULL /* CF PF AF ZF SF TF DF OF, AC, ID (+ IF, bit 1 set below) */

/* The registers a thread returns to user mode with */
typedef struct {
    UINT64 rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    UINT64 r8, r9, r10, r11, r12, r13, r14, r15;
    UINT64 rip, rflags;
} Regs;

/* CONTEXT offsets */
enum {
    C_FLAGS = 0x30, C_MXCSR = 0x34, C_CS = 0x38, C_DS = 0x3A, C_ES = 0x3C, C_FS = 0x3E, C_GS = 0x40,
    C_SS = 0x42, C_EFLAGS = 0x44, C_RAX = 0x78, C_RIP = 0xF8, C_FLT = 0x100
};

static UINT8 g_fx[512] __attribute__((aligned(16)));    /* scratch (interrupts off) */

static void put16(UINT8 *b, UINT16 v) { memcpy(b, &v, 2); }
static void put32(UINT8 *b, UINT32 v) { memcpy(b, &v, 4); }
static void put64(UINT8 *b, UINT64 v) { memcpy(b, &v, 8); }
static UINT64 get64(const UINT8 *b)   { UINT64 v; memcpy(&v, b, 8); return v; }

/* CONTEXT from registers; the FPU/SSE state is the live one */
static void build_context(UINT8 *c, const Regs *r)
{
    memset(c, 0, CONTEXT_SIZE);
    put32(c + C_FLAGS, CONTEXT_ALL);
    put16(c + C_CS, GDT_USER_CODE | 3);
    put16(c + C_DS, GDT_USER_DATA | 3);
    put16(c + C_ES, GDT_USER_DATA | 3);
    put16(c + C_FS, GDT_USER_DATA | 3);
    put16(c + C_GS, GDT_USER_DATA | 3);
    put16(c + C_SS, GDT_USER_DATA | 3);
    put32(c + C_EFLAGS, (UINT32)r->rflags);
    const UINT64 gpr[16] = { r->rax, r->rcx, r->rdx, r->rbx, r->rsp, r->rbp, r->rsi, r->rdi,
                             r->r8, r->r9, r->r10, r->r11, r->r12, r->r13, r->r14, r->r15 };
    for (int i = 0; i < 16; i++) put64(c + C_RAX + 8 * i, gpr[i]);
    put64(c + C_RIP, r->rip);
    IrqState s = irq_save();
    __asm__ volatile ("fxsave64 %0" : "=m"(g_fx));
    memcpy(c + C_FLT, g_fx, 512);
    irq_restore(s);
    memcpy(c + C_MXCSR, g_fx + 24, 4);
}

/* Push CONTEXT + EXCEPTION_RECORD below @r->rsp and point the registers
 * at KiUserExceptionDispatcher.  False if they don't fit on the stack. */
static bool push_exception(UmProcess *p, Regs *r, const UINT8 *ctx, const UINT8 *rec)
{
    if (!p->exc_dispatcher) return false;
    UINT64 sp = ((r->rsp - 0x80) & ~0xFULL) - FRAME_SIZE;       /* keep clear of the old frame */
    if (r->rsp > USER_TOP || sp < 0x10000) return false;
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)sp, ctx, CONTEXT_SIZE)) ||
        !NT_SUCCESS(CopyToUser((void *)(uintptr_t)(sp + CONTEXT_SIZE), rec, RECORD_SIZE)))
        return false;
    r->rsp = sp;
    r->rcx = sp;
    r->rdx = sp + CONTEXT_SIZE;
    r->rip = p->exc_dispatcher;
    r->rflags &= ~0x100ULL;                                      /* TF: no single-stepping */
    return true;
}

/* Exception information for a CPU exception */
static UINT32 cpu_status(UINT64 vector, UINT64 *nparams, UINT64 *info, UINT64 err, UINT64 cr2)
{
    *nparams = 0;
    switch (vector) {
    case 0:  return 0xC0000094u;                                 /* INTEGER_DIVIDE_BY_ZERO */
    case 1:  return 0x80000004u;                                 /* SINGLE_STEP */
    case 3:  return 0x80000003u;                                 /* BREAKPOINT */
    case 4:  return 0xC0000095u;                                 /* INTEGER_OVERFLOW */
    case 5:  return 0xC000008Cu;                                 /* ARRAY_BOUNDS_EXCEEDED */
    case 6:  return 0xC000001Du;                                 /* ILLEGAL_INSTRUCTION */
    case 16: return 0xC0000090u;                                 /* FLOAT_INVALID_OPERATION */
    case 17: return 0x80000002u;                                 /* DATATYPE_MISALIGNMENT */
    case 19: {                                                   /* SIMD: which exception? */
        UINT32 mx;
        __asm__ volatile ("stmxcsr %0" : "=m"(mx));
        UINT32 hit = mx & ~(mx >> 7) & 0x3F;                     /* flagged and unmasked */
        return hit & 1  ? 0xC0000090u :                          /* invalid */
               hit & 4  ? 0xC000008Eu :                          /* divide by zero */
               hit & 8  ? 0xC0000091u :                          /* overflow */
               hit & 16 ? 0xC0000093u :                          /* underflow */
               hit & 2  ? 0xC000008Du :                          /* denormal */
               hit & 32 ? 0xC000008Fu : 0xC00002B5u;             /* inexact; multiple traps */
    }
    case 14:
        *nparams = 2;
        info[0] = (err & 16) ? 8 : (err & 2) ? 1 : 0;            /* execute (DEP), write, read */
        info[1] = cr2;
        return UM_STATUS_ACCESS_VIOLATION;
    default:                                                     /* #GP and the rest */
        *nparams = 2;
        info[0] = 0;
        info[1] = UINT64_C(0xFFFFFFFFFFFFFFFF);
        return UM_STATUS_ACCESS_VIOLATION;
    }
}

/* A CPU exception taken in user mode by a program thread (interrupts off).
 * Rewrites @f so the IRET lands in ntdll's dispatcher, or ends the process. */
void UmUserException(void *frame, UINT64 cr2)
{
    InterruptFrame *f = frame;
    UmProcess *p = UmCurrent();
    UINT64 info[15] = { 0 }, nparams;
    UINT32 code = cpu_status(f->vector, &nparams, info, f->error_code, cr2);
    UINT64 addr = f->vector == 3 ? f->rip - 1 : f->rip;          /* int3: report the instruction */

    /* Touching just below a thread's stack: a stack overflow */
    UmThread *t = UmCurrentThread();
    if (code == UM_STATUS_ACCESS_VIOLATION && nparams == 2 && cr2 < t->stack_lo && cr2 + 0x10000 >= t->stack_lo &&
        f->rsp < t->stack_lo + 0x1000)
        UmFault(0xC00000FDu, addr, cr2);

    Regs r = { f->rax, f->rcx, f->rdx, f->rbx, f->rsp, f->rbp, f->rsi, f->rdi,
               f->r8, f->r9, f->r10, f->r11, f->r12, f->r13, f->r14, f->r15, addr, f->rflags };
    static UINT8 ctx[CONTEXT_SIZE], rec[RECORD_SIZE];            /* interrupts are off */
    build_context(ctx, &r);
    memset(rec, 0, sizeof(rec));
    put32(rec, code);
    put64(rec + 16, addr);                                       /* ExceptionAddress */
    put32(rec + 24, (UINT32)nparams);
    for (UINT64 i = 0; i < nparams; i++) put64(rec + 32 + 8 * i, info[i]);

    if (!push_exception(p, &r, ctx, rec))
        UmFault(code, addr, nparams == 2 ? info[1] : 0);
    f->rip = r.rip; f->rsp = r.rsp; f->rcx = r.rcx; f->rdx = r.rdx; f->rflags = r.rflags;
}

/* -----------------------------------------------------------------------
 * Resuming at a CONTEXT (NtContinue, NtRaiseException)
 * ----------------------------------------------------------------------- */
/* Load @r and IRET to user mode.  Called from a system call, so GS holds
 * the KPCR: swap back to the TEB first. */
static void __attribute__((naked, noreturn)) iret_to(const Regs *r __attribute__((unused)))
{
    __asm__ volatile (
        "cli\n\t"
        "pushq %[ss]\n\t"
        "pushq 32(%%rdi)\n\t"           /* rsp */
        "pushq 136(%%rdi)\n\t"          /* rflags */
        "pushq %[cs]\n\t"
        "pushq 128(%%rdi)\n\t"          /* rip */
        "mov 0(%%rdi), %%rax\n\t"
        "mov 8(%%rdi), %%rcx\n\t"
        "mov 16(%%rdi), %%rdx\n\t"
        "mov 24(%%rdi), %%rbx\n\t"
        "mov 40(%%rdi), %%rbp\n\t"
        "mov 48(%%rdi), %%rsi\n\t"
        "mov 64(%%rdi), %%r8\n\t"
        "mov 72(%%rdi), %%r9\n\t"
        "mov 80(%%rdi), %%r10\n\t"
        "mov 88(%%rdi), %%r11\n\t"
        "mov 96(%%rdi), %%r12\n\t"
        "mov 104(%%rdi), %%r13\n\t"
        "mov 112(%%rdi), %%r14\n\t"
        "mov 120(%%rdi), %%r15\n\t"
        "mov 56(%%rdi), %%rdi\n\t"
        "swapgs\n\t"
        "iretq\n\t"
        : : [ss] "i"(GDT_USER_DATA | 3), [cs] "i"(GDT_USER_CODE | 3));
}

/* Registers (and FPU state) from a user CONTEXT, made safe to IRET to */
static bool load_context(const UINT8 *c, Regs *r)
{
    UINT64 *g = &r->rax;
    for (int i = 0; i < 16; i++) g[i] = get64(c + C_RAX + 8 * i);
    r->rip = get64(c + C_RIP);
    UINT32 fl;
    memcpy(&fl, c + C_EFLAGS, 4);
    r->rflags = (fl & USER_FLAGS) | 0x202;
    if (r->rip > USER_TOP || r->rsp > USER_TOP) return false;    /* IRET would fault in the kernel */

    UINT32 cflags;
    memcpy(&cflags, c + C_FLAGS, 4);
    if (cflags & 0x8) {                                          /* CONTEXT_FLOATING_POINT */
        IrqState s = irq_save();
        __asm__ volatile ("fxsave64 %0" : "=m"(g_fx));
        UINT32 mask;
        memcpy(&mask, g_fx + 28, 4);
        if (!mask) mask = 0xFFBF;
        memcpy(g_fx, c + C_FLT, 512);
        UINT32 mx;
        memcpy(&mx, g_fx + 24, 4);
        mx &= mask;                                              /* reserved bits would #GP */
        memcpy(g_fx + 24, &mx, 4);
        memcpy(g_fx + 28, &mask, 4);
        __asm__ volatile ("fxrstor64 %0" : : "m"(g_fx));
        irq_restore(s);
    }
    return true;
}

/* NtContinue(PCONTEXT, BOOLEAN TestAlert) */
static UINT64 sys_continue(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UINT8 buf[CONTEXT_SIZE];
    if (!NT_SUCCESS(CopyFromUser(buf, (const void *)(uintptr_t)a1, CONTEXT_SIZE))) return UM_STATUS_ACCESS_VIOLATION;
    Regs r;
    if (!load_context(buf, &r)) return 0xC000000Du;              /* INVALID_PARAMETER */
    cli();
    UmReturnToUser();                                            /* killed meanwhile? */
    iret_to(&r);
}

/* NtRaiseException(PEXCEPTION_RECORD, PCONTEXT, BOOLEAN FirstChance) */
static UINT64 sys_raise_exception(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UINT8 rec[RECORD_SIZE], ctx[CONTEXT_SIZE];
    if (!NT_SUCCESS(CopyFromUser(rec, (const void *)(uintptr_t)a1, RECORD_SIZE)) ||
        !NT_SUCCESS(CopyFromUser(ctx, (const void *)(uintptr_t)a2, CONTEXT_SIZE)))
        return UM_STATUS_ACCESS_VIOLATION;
    UINT32 code, np;
    memcpy(&code, rec, 4);
    memcpy(&np, rec + 24, 4);
    UINT64 addr = get64(rec + 16);
    if (!(a3 & 0xFF))                                            /* unhandled: the end */
        UmFault(code, addr, np >= 2 ? get64(rec + 40) : 0);
    Regs r;
    if (!load_context(ctx, &r)) return 0xC000000Du;
    if (!push_exception(UmCurrent(), &r, ctx, rec)) UmFault(code, addr, 0);
    cli();
    UmReturnToUser();
    iret_to(&r);
}

/* -----------------------------------------------------------------------
 * Another thread's registers (GetThreadContext / SetThreadContext)
 * ----------------------------------------------------------------------- */
#define CONTEXT_CIS 0x00100007u              /* AMD64 | CONTROL | INTEGER | SEGMENTS */

/* The target, stopped: suspended and parked in the kernel.  NtSuspendThread
 * takes effect on the thread's way back to user mode, so let it get there. */
static UmThread *stopped_thread(UINT64 h, UmObject **ref)
{
    UmObject *o = um_handle_object(UmCurrent(), h, UO_THREAD);
    if (!o) return NULL;
    UmThread *t = (UmThread *)o;
    if (t == UmCurrentThread() || t->suspend <= 0) { um_ob_unref(o); return NULL; }
    for (int i = 0; i < 2000 && !t->park && !t->exited; i++) sched_yield();
    if (!t->park || t->exited) { um_ob_unref(o); return NULL; }
    *ref = o;
    return t;
}

/* NtGetContextThread(HANDLE Thread, PCONTEXT) */
static UINT64 sys_get_context_thread(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmObject *o;
    UmThread *t = stopped_thread(a1, &o);
    if (!t) return 0xC0000001u;                                  /* STATUS_UNSUCCESSFUL */
    Regs r;
    memset(&r, 0, sizeof(r));
    if (t->park == 2) {
        const InterruptFrame *f = t->uframe;
        r.rax = f->rax; r.rcx = f->rcx; r.rdx = f->rdx; r.rbx = f->rbx; r.rsp = f->rsp; r.rbp = f->rbp;
        r.rsi = f->rsi; r.rdi = f->rdi; r.r8 = f->r8; r.r9 = f->r9; r.r10 = f->r10; r.r11 = f->r11;
        r.r12 = f->r12; r.r13 = f->r13; r.r14 = f->r14; r.r15 = f->r15; r.rip = f->rip; r.rflags = f->rflags;
    } else {
        /* in a system call: as if the ntdll stub had just returned to its caller */
        UINT64 sp = t->kt ? t->kt->user_rsp : 0, ret = 0;
        CopyFromUser(&ret, (const void *)(uintptr_t)sp, 8);
        r.rip = ret; r.rsp = sp + 8; r.rflags = 0x202;
    }
    um_ob_unref(o);
    static UINT8 c[CONTEXT_SIZE];                                /* build_context: interrupts off */
    IrqState s = irq_save();
    build_context(c, &r);
    put32(c + C_FLAGS, CONTEXT_CIS);                             /* not this thread's FPU state */
    memset(c + C_FLT, 0, 512);
    put32(c + C_MXCSR, 0x1F80);
    UINT8 out[CONTEXT_SIZE];
    memcpy(out, c, CONTEXT_SIZE);
    irq_restore(s);
    /* keep the caller's P1Home..P6Home (the first 0x30 bytes) */
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)(a2 + 0x30), out + 0x30, CONTEXT_SIZE - 0x30))
           ? 0 : UM_STATUS_ACCESS_VIOLATION;
}

/* NtSetContextThread(HANDLE Thread, PCONTEXT): integer and control registers
 * of a thread stopped at an interrupt */
static UINT64 sys_set_context_thread(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UINT8 c[CONTEXT_SIZE];
    if (!NT_SUCCESS(CopyFromUser(c, (const void *)(uintptr_t)a2, CONTEXT_SIZE))) return UM_STATUS_ACCESS_VIOLATION;
    UmObject *o;
    UmThread *t = stopped_thread(a1, &o);
    if (!t) return 0xC0000001u;
    if (t->park != 2) { um_ob_unref(o); return 0xC0000001u; }
    UINT32 cflags;
    memcpy(&cflags, c + C_FLAGS, 4);
    Regs r;
    UINT64 *g = &r.rax;
    for (int i = 0; i < 16; i++) g[i] = get64(c + C_RAX + 8 * i);
    r.rip = get64(c + C_RIP);
    UINT32 fl;
    memcpy(&fl, c + C_EFLAGS, 4);
    InterruptFrame *f = t->uframe;
    if ((cflags & 0x1) && (r.rip > USER_TOP || r.rsp > USER_TOP)) { um_ob_unref(o); return 0xC000000Du; }
    IrqState s = irq_save();
    if (cflags & 0x1) {                                          /* CONTEXT_CONTROL */
        f->rip = r.rip; f->rsp = r.rsp;
        f->rflags = (fl & USER_FLAGS) | 0x202;
    }
    if (cflags & 0x2) {                                          /* CONTEXT_INTEGER */
        f->rax = r.rax; f->rcx = r.rcx; f->rdx = r.rdx; f->rbx = r.rbx; f->rbp = r.rbp;
        f->rsi = r.rsi; f->rdi = r.rdi; f->r8 = r.r8; f->r9 = r.r9; f->r10 = r.r10; f->r11 = r.r11;
        f->r12 = r.r12; f->r13 = r.r13; f->r14 = r.r14; f->r15 = r.r15;
    }
    irq_restore(s);
    um_ob_unref(o);
    return 0;
}

void um_exception_syscalls_init(void)
{
    um_install(SYSCALL_NtGetContextThread, sys_get_context_thread);
    um_install(SYSCALL_NtSetContextThread, sys_set_context_thread);
    um_install(SYSCALL_NtContinue,       sys_continue);
    um_install(SYSCALL_NtRaiseException, sys_raise_exception);
}
