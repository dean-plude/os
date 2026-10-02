/*
 * sleep.c — S3 (suspend to RAM) for every CPU
 *
 * Going to sleep: the calling CPU sends the others an NMI, which they take
 * wherever they are (even spinning on a lock with interrupts off); they
 * save their state inside it and wait.  From then on the calling CPU, and
 * the boot CPU when it wakes, take no locks: a stopped CPU may hold one.
 * The calling CPU saves its own state, points the firmware waking vectors
 * at the start-up page (the trampoline that starts the other CPUs at
 * boot) and writes SLP_TYP(S3) | SLP_EN.  Memory stays powered; the CPUs
 * and devices don't.
 *
 * Waking: the firmware starts the boot CPU at a waking vector (real or
 * 32-bit protected mode), and the trampoline takes it to long mode and
 * resume_entry.  That puts back what the CPU lost (control registers, GDT
 * and TSS, IDT, MSRs, MTRRs, the local APIC, the TSC, FPU state) and the
 * PCI configuration of every function, and restarts the other CPUs
 * through the same page.  Each CPU then jumps back into the context it
 * saved: the others return from the NMI; the CPU that asked returns from
 * SleepEnter, which brings the devices back (disks, network, keyboard and
 * mouse, display) and moves the wall clock on by the time spent asleep.
 */

#include "sleep.h"
#include "kpcr.h"
#include "printf.h"
#include "scheduler.h"
#include "smp.h"
#include "spinlock.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"
#include "../arch/x86_64/gdt.h"
#include "../hal/acpi.h"
#include "../hal/aml.h"
#include "../hal/ioapic.h"
#include "../hal/pci.h"
#include "../hal/ps2.h"
#include "../hal/rtc.h"
#include "../hal/framebuffer.h"
#include "../drivers/ahci.h"
#include "../drivers/nvme.h"
#include "../net/net.h"
#include "../drivers/hda.h"
#include "../drivers/xhci.h"
#include "../lib/string.h"

void UmClockAdvance(UINT64 delta_100ns);

/* callee-saved registers, stack and return address (setjmp-style) */
typedef struct { UINT64 rbx, rbp, r12, r13, r14, r15, rsp, rip; } SleepCtx;

int  sleep_ctx_save(SleepCtx *c) __attribute__((returns_twice));
void sleep_ctx_restore(SleepCtx *c) __attribute__((noreturn));
__asm__(
    ".text\n"
    ".globl sleep_ctx_save\n"
    "sleep_ctx_save:\n\t"
    "movq %rbx, 0(%rdi)\n\t"
    "movq %rbp, 8(%rdi)\n\t"
    "movq %r12, 16(%rdi)\n\t"
    "movq %r13, 24(%rdi)\n\t"
    "movq %r14, 32(%rdi)\n\t"
    "movq %r15, 40(%rdi)\n\t"
    "leaq 8(%rsp), %rax\n\t"
    "movq %rax, 48(%rdi)\n\t"
    "movq (%rsp), %rax\n\t"
    "movq %rax, 56(%rdi)\n\t"
    "xorl %eax, %eax\n\t"
    "ret\n"
    ".globl sleep_ctx_restore\n"
    "sleep_ctx_restore:\n\t"
    "movq 0(%rdi), %rbx\n\t"
    "movq 8(%rdi), %rbp\n\t"
    "movq 16(%rdi), %r12\n\t"
    "movq 24(%rdi), %r13\n\t"
    "movq 32(%rdi), %r14\n\t"
    "movq 40(%rdi), %r15\n\t"
    "movq 48(%rdi), %rsp\n\t"
    "movl $1, %eax\n\t"
    "jmpq *56(%rdi)\n");

typedef struct __attribute__((packed)) { UINT16 limit; UINT64 base; } DescPtr;

static const UINT32 g_msr_ids[] = {
    MSR_IA32_EFER, MSR_IA32_STAR, MSR_IA32_LSTAR, MSR_IA32_CSTAR, MSR_IA32_FMASK,
    MSR_IA32_PAT, MSR_IA32_FSBASE, MSR_IA32_GSBASE, MSR_IA32_KERNEL_GSBASE,
};
#define N_MSRS (sizeof(g_msr_ids) / sizeof(g_msr_ids[0]))

typedef struct {
    SleepCtx ctx;
    DescPtr  gdtr, idtr;
    UINT16   tr;
    UINT64   cr0, cr3, cr4, xcr0;
    UINT64   msr[N_MSRS];
    UINT8    fpu[4096] __attribute__((aligned(64)));
} CpuState;

static CpuState g_cpu[MAX_CPUS];
static UINT8 g_resume_stack[MAX_CPUS][8192] __attribute__((aligned(16)));

/* The boot CPU's MTRRs: the firmware sets them up on the boot CPU when
 * the machine wakes, but the others come back with their reset values */
#define MSR_MTRRCAP      0xFE
#define MSR_MTRR_DEFTYPE 0x2FF
static const UINT32 g_fixed_mtrrs[] = { 0x250, 0x258, 0x259, 0x268, 0x269, 0x26A, 0x26B,
                                        0x26C, 0x26D, 0x26E, 0x26F };
static struct {
    bool   have;
    UINT64 deftype, fixed[11], var[2 * 16];
    int    nvar;
} g_mtrr;

static volatile UINT32 g_frozen;         /* CPUs parked in SleepFreezeCpu */
static volatile UINT32 g_thaw;           /* going to sleep failed: parked CPUs go on */
static volatile UINT32 g_freezing;       /* NMIs mean "park" */
static UINT32 g_cpus_lost;               /* didn't restart after waking */
static volatile UINT32 g_ap_back;        /* a restarted CPU reached its old context */
static volatile UINT64 g_tsc;            /* TSC to set: before sleeping / on the boot CPU */

static bool has_mtrr(void)
{
    UINT32 a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return (d >> 12) & 1;
}

static void save_mtrrs(void)
{
    g_mtrr.have = has_mtrr();
    if (!g_mtrr.have) return;
    UINT64 cap = rdmsr(MSR_MTRRCAP);
    g_mtrr.nvar = (int)(cap & 0xFF) > 16 ? 16 : (int)(cap & 0xFF);
    g_mtrr.deftype = rdmsr(MSR_MTRR_DEFTYPE);
    if (cap & (1u << 8))
        for (int i = 0; i < 11; i++) g_mtrr.fixed[i] = rdmsr(g_fixed_mtrrs[i]);
    for (int i = 0; i < 2 * g_mtrr.nvar; i++) g_mtrr.var[i] = rdmsr(0x200 + (UINT32)i);
}

/* The SDM's sequence: caches off and flushed, MTRRs off, write, back on */
static void restore_mtrrs(void)
{
    if (!g_mtrr.have) return;
    UINT64 cr0 = read_cr0();
    write_cr0((cr0 | (1u << 30)) & ~(UINT64)(1u << 29));      /* CD, not NW */
    wbinvd();
    write_cr3(read_cr3());
    wrmsr(MSR_MTRR_DEFTYPE, g_mtrr.deftype & ~(UINT64)0xC00);  /* E, FE off */
    if (rdmsr(MSR_MTRRCAP) & (1u << 8))
        for (int i = 0; i < 11; i++) wrmsr(g_fixed_mtrrs[i], g_mtrr.fixed[i]);
    for (int i = 0; i < 2 * g_mtrr.nvar; i++) wrmsr(0x200 + (UINT32)i, g_mtrr.var[i]);
    wrmsr(MSR_MTRR_DEFTYPE, g_mtrr.deftype);
    wbinvd();
    write_cr3(read_cr3());
    write_cr0(cr0);
}

static UINT64 xgetbv0(void)
{
    UINT32 lo, hi;
    __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((UINT64)hi << 32) | lo;
}

/* What a CPU loses in S3 (the general registers are saved by
 * sleep_ctx_save, right after) */
static void save_cpu(CpuState *s)
{
    __asm__ volatile ("sgdt %0" : "=m"(s->gdtr));
    __asm__ volatile ("sidt %0" : "=m"(s->idtr));
    __asm__ volatile ("str %0" : "=m"(s->tr));
    s->cr0 = read_cr0();
    s->cr3 = read_cr3();
    s->cr4 = read_cr4();
    s->xcr0 = (s->cr4 & (1u << 18)) ? xgetbv0() : 0;
    for (UINT32 i = 0; i < N_MSRS; i++) s->msr[i] = rdmsr(g_msr_ids[i]);
    if (s->xcr0)
        __asm__ volatile ("xsave64 %0" : "=m"(s->fpu) : "a"((UINT32)s->xcr0), "d"((UINT32)(s->xcr0 >> 32)) : "memory");
    else
        __asm__ volatile ("fxsave64 %0" : "=m"(s->fpu) : : "memory");
}

/* Back from the trampoline (long mode, its page table, a stack of our own,
 * no GS base yet: nothing here may use the KPCR) */
static void restore_cpu(CpuState *s)
{
    write_cr4(s->cr4);
    write_cr3(s->cr3);
    write_cr0(s->cr0);
    if (s->xcr0)
        __asm__ volatile ("xsetbv" : : "c"(0), "a"((UINT32)s->xcr0), "d"((UINT32)(s->xcr0 >> 32)));
    wrmsr(MSR_IA32_EFER, s->msr[0]);
    __asm__ volatile ("lgdt %0" : : "m"(s->gdtr));
    ((UINT8 *)s->gdtr.base)[s->tr + 5] &= (UINT8)~0x02;      /* the TSS isn't busy any more */
    __asm__ volatile ("ltr %0" : : "r"(s->tr));
    gdt_reload_segments();
    __asm__ volatile ("lidt %0" : : "m"(s->idtr));
    for (UINT32 i = 1; i < N_MSRS; i++) wrmsr(g_msr_ids[i], s->msr[i]);   /* bases after the selectors */
    restore_mtrrs();
    apic_init_ap();
    __asm__ volatile ("fninit");
    if (s->xcr0)
        __asm__ volatile ("xrstor64 %0" : : "m"(s->fpu), "a"((UINT32)s->xcr0), "d"((UINT32)(s->xcr0 >> 32)) : "memory");
    else
        __asm__ volatile ("fxrstor64 %0" : : "m"(s->fpu) : "memory");
}

static void resume_entry(UINT64 cpu)
{
    CpuState *s = &g_cpu[cpu];
    if (cpu == 0) {
        wrmsr(MSR_IA32_TSC, g_tsc);                       /* the clock goes on where it stopped */
        restore_cpu(s);
        apic_resume();                                    /* the legacy PIC is back: mask it */
        PciRestoreAll();                                  /* (lock-free: we're alone) */
        g_cpus_lost = 0;
        /* The other CPUs, one at a time through the start-up page */
        for (UINT32 c = 1; c < MAX_CPUS; c++) {
            if (!g_kpcr[c].Online) continue;
            smp_trampoline(resume_entry, g_resume_stack[c] + sizeof(g_resume_stack[c]), c);
            __atomic_store_n(&g_ap_back, 0, __ATOMIC_SEQ_CST);
            g_tsc = rdtsc();
            if (!smp_start_cpu((UINT8)g_kpcr[c].ApicId, &g_ap_back)) g_cpus_lost++;
        }
    } else {
        wrmsr(MSR_IA32_TSC, g_tsc);
        restore_cpu(s);
        __atomic_store_n(&g_ap_back, 1, __ATOMIC_RELEASE);
    }
    sleep_ctx_restore(&s->ctx);
}

bool SleepFreezing(void) { return __atomic_load_n(&g_freezing, __ATOMIC_ACQUIRE) != 0; }

/* The NMI: park this CPU with its state saved */
void SleepFreezeCpu(void)
{
    CpuState *s = &g_cpu[KiGetCurrentKpcr()->CpuNumber];
    save_cpu(s);
    if (sleep_ctx_save(&s->ctx) == 0) {
        wbinvd();
        __atomic_fetch_add(&g_frozen, 1, __ATOMIC_SEQ_CST);
        while (!__atomic_load_n(&g_thaw, __ATOMIC_ACQUIRE)) {
            smp_poll_tlb();              /* a CPU still on its way here may wait for us */
            pause_cpu();
        }
        __atomic_fetch_sub(&g_frozen, 1, __ATOMIC_SEQ_CST);
    }
    /* thawed, or woken up: back to what was interrupted */
}

bool SleepSupported(void)
{
    return AcpiSleepSupported() && smp_trampoline(resume_entry, g_resume_stack[0] + sizeof(g_resume_stack[0]), 0);
}

static UINT64 rtc_seconds(void)
{
    RtcTime t;
    rtc_read(&t);
    /* days since 1970-01-01 (proleptic Gregorian) */
    int y = t.year, m = t.month;
    if (m <= 2) { y--; m += 12; }
    INT64 days = 365LL * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + t.day - 719469;
    return (UINT64)days * 86400 + t.hour * 3600u + t.minute * 60u + t.second;
}

static UINT64 g_last_sleep, g_last_wake;
UINT64 SleepLastSleepTime(void) { return g_last_sleep; }
UINT64 SleepLastWakeTime(void) { return g_last_wake; }

bool SleepEnter(void)
{
    if (!SleepSupported()) return false;
    g_last_sleep = sched_ticks() * 100000ULL;

    AmlPrepareSleep();                   /* \_PTS; only wake GPEs stay on */
    /* Devices: what only the driver knows */
    XhciPrepareSleep();                  /* USB keyboards may wake it */
    FbSuspend();
    PciSaveAll();
    save_mtrrs();
    UINT64 rtc_before = rtc_seconds(), tsc_before = rdtsc();

    UINT32 bkl = bkl_drop();             /* the other CPUs may need it to reach the freeze */
    IrqState irq = irq_save();
    UINT32 self = KiGetCurrentKpcr()->CpuNumber;  /* (the thread stays on this CPU from here) */
    __atomic_store_n(&g_thaw, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_frozen, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_freezing, 1, __ATOMIC_SEQ_CST);
    UINT32 others = 0;
    for (UINT32 c = 0; c < MAX_CPUS; c++) {
        if (c == self || !g_kpcr[c].Online) continue;
        others++;
        apic_send_ipi(g_kpcr[c].ApicId, APIC_IPI_NMI);
    }
    for (int i = 0; i < 100000 && __atomic_load_n(&g_frozen, __ATOMIC_ACQUIRE) < others; i++) udelay(10);
    bool slept = false;
    UINT32 frozen = __atomic_load_n(&g_frozen, __ATOMIC_ACQUIRE);
    if (frozen == others) {
        CpuState *s = &g_cpu[self];
        save_cpu(s);
        if (sleep_ctx_save(&s->ctx) == 0) {
            UINT32 vec = smp_trampoline(resume_entry, g_resume_stack[0] + sizeof(g_resume_stack[0]), 0);
            g_tsc = rdtsc();
            AcpiEnterS3(vec, smp_trampoline_wake32());   /* returns only if the machine stayed up */
        } else {
            slept = true;                /* woken: every CPU is back */
        }
    }
    __atomic_store_n(&g_freezing, 0, __ATOMIC_SEQ_CST);
    if (!slept) {
        __atomic_store_n(&g_thaw, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&g_frozen, __ATOMIC_ACQUIRE)) pause_cpu();
        irq_restore(irq);
        bkl_restore(bkl);
        if (frozen < others) kprintf("[SLEEP] %u of %u CPUs stopped: not sleeping\n", frozen, others);
        else kprintf("[SLEEP] The machine didn't enter S3\n");
        XhciResume();                    /* (its ports were suspended) */
        AmlWake();
        return false;
    }
    if (g_cpus_lost) kprintf("[SLEEP] %u CPU(s) didn't come back\n", g_cpus_lost);

    /* Devices the platform powered off */
    AcpiResume();
    IoApicResume();                      /* the SCI */
    AhciResume();
    NvmeResume();
    NetResume();                         /* the network adapter */
    XhciResume();
    HdaResume();
    ps2_resume();
    FbResume();
    /* The wall clock follows the tick count; add what the ticks missed
     * (all of the sleep where the TSC stopped, nothing where it ran on) */
    UINT64 rtc_after = rtc_seconds();
    UINT64 asleep = rtc_after > rtc_before ? (rtc_after - rtc_before) * 10000000ULL : 0;
    UINT64 ticked = g_tsc_per_tick ? (rdtsc() - tsc_before) / g_tsc_per_tick * 100000ULL : 0;
    if (asleep > ticked) UmClockAdvance(asleep - ticked);
    kprintf("[SLEEP] Woke up after %llu s\n", (unsigned long long)(asleep / 10000000ULL));
    g_last_wake = sched_ticks() * 100000ULL > g_last_sleep ? sched_ticks() * 100000ULL : g_last_sleep + 1;
    irq_restore(irq);
    bkl_restore(bkl);
    AmlWake();                           /* \_WAK; the runtime GPEs again */
    return true;
}
