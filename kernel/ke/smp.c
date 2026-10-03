/*
 * smp.c — the other CPUs: finding them (ACPI MADT), starting them
 * (INIT + STARTUP IPIs through a real-mode trampoline), the big kernel
 * lock, idle halts, wake-up IPIs and TLB shootdowns.  See smp.h.
 */

#include "smp.h"
#include "kpcr.h"
#include "printf.h"
#include "scheduler.h"
#include "syscall.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/paging.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../hal/acpi.h"
#include "spinlock.h"

void UmCpuCountChanged(void);

/* -----------------------------------------------------------------------
 * Big kernel lock
 *
 * Held by threads, not CPUs: Thread.bkl_depth counts a thread's nested
 * bkl_acquire calls.  The scheduler lets go of the lock when it switches a
 * holder out and takes it back when the holder resumes, so a thread
 * waiting for anything never keeps the other CPUs out of code that needs
 * the lock.  Code that doesn't need it (the scheduler, the timer, the
 * services in um_syscall.c's list, the desktop's drawing) runs on every
 * CPU at once under its own locks.
 *
 * Underneath is a test-and-set lock.  A waiter spins a little (with
 * interrupts off, answering TLB flush requests: the holder may be waiting
 * for that), then halts with interrupts on until the holder, on release,
 * sends it IPI_WAKE — so waiting CPUs don't burn the time the holder needs
 * (under emulation, host CPUs).  Timer interrupts during that halt are
 * only acknowledged (KPCR.LockWait, see interrupt_dispatch), so a waiting
 * thread is never switched out halfway.
 * ----------------------------------------------------------------------- */
static struct {
    volatile uint32_t locked;
    volatile uint32_t waiters;               /* CPUs halted waiting for it */
    volatile uint32_t contenders;            /* CPUs in raw_lock */
    uint32_t          next_wake;
} g_bkl = { 0, 0, 0, 0 };

/* TLB shootdowns, one at a time (a sender and its targets never wait for
 * each other in a circle) */
static KSpinLock g_tlb_lock = KSPINLOCK_INIT;

static void tlb_flush_local(PKPCR k)
{
    uint32_t f = __atomic_load_n(&k->TlbFlush, __ATOMIC_ACQUIRE);
    if (!f) return;
    if (f == 2) {                            /* global pages too */
        uint64_t cr4 = read_cr4();
        write_cr4(cr4 & ~(uint64_t)CR4_PGE);
        write_cr4(cr4);
    } else {
        write_cr3(read_cr3());
    }
    __atomic_store_n(&k->TlbFlush, 0, __ATOMIC_RELEASE);
}

void smp_poll_tlb(void)
{
    PKPCR k = KiGetCurrentKpcr();
    if (k && k->TlbFlush) tlb_flush_local(k);
}

static bool raw_try(void)
{
    return !__atomic_load_n(&g_bkl.locked, __ATOMIC_RELAXED) &&
           !__atomic_exchange_n(&g_bkl.locked, 1, __ATOMIC_ACQUIRE);
}

/* Interrupts off.  @may_halt: false while a CPU starts (it may not take
 * interrupts yet). */
static void raw_lock(bool may_halt)
{
    PKPCR k = KiGetCurrentKpcr();
    uint32_t bit = 1u << k->CpuNumber;
    __atomic_fetch_add(&g_bkl.contenders, 1, __ATOMIC_SEQ_CST);
    for (;;) {
        for (int i = 0; i < 1024; i++) {
            if (raw_try()) {
                __atomic_fetch_sub(&g_bkl.contenders, 1, __ATOMIC_SEQ_CST);
                return;
            }
            tlb_flush_local(k);
            pause_cpu();
        }
        if (!may_halt || g_cpu_count < 2) continue;
        __atomic_fetch_or(&g_bkl.waiters, bit, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_bkl.locked, __ATOMIC_SEQ_CST)) {   /* else released meanwhile */
            k->LockWait = 1;
            __asm__ volatile ("sti; hlt; cli" ::: "memory");
            k->LockWait = 0;
        }
        __atomic_fetch_and(&g_bkl.waiters, ~bit, __ATOMIC_SEQ_CST);
    }
}

static void raw_unlock(void)
{
    __atomic_store_n(&g_bkl.locked, 0, __ATOMIC_SEQ_CST);
    uint32_t w = __atomic_load_n(&g_bkl.waiters, __ATOMIC_SEQ_CST);
    if (!w) return;
    /* wake one halted waiter, taking turns */
    for (uint32_t n = 0; n < MAX_CPUS; n++) {
        uint32_t c = (g_bkl.next_wake + n) % MAX_CPUS;
        if (!(w & (1u << c))) continue;
        __atomic_fetch_and(&g_bkl.waiters, ~(1u << c), __ATOMIC_SEQ_CST);
        g_bkl.next_wake = c + 1;
        apic_send_ipi(g_kpcr[c].ApicId, APIC_IPI_FIXED | IPI_WAKE);
        return;
    }
}

static Thread *me(void) { return KiGetCurrentKpcr()->CurrentThread; }

/* Take the lock back for @t, which held it @t->bkl_depth deep (interrupts
 * off).  While raw_lock halts, an interrupt may come in and want the lock
 * itself: the depth reads 0 meanwhile, so it takes the lock (and lets go)
 * rather than run as though this thread held it. */
static void relock(Thread *t)
{
    uint32_t depth = t->bkl_depth;
    t->bkl_depth = 0;
    raw_lock(true);
    t->bkl_depth = depth;
}

void bkl_acquire(void)
{
    IrqState s = irq_save();
    Thread *t = me();
    /* (the depth goes up once the lock is ours: an interrupt taken while
     * raw_lock halts must not think this thread holds it already) */
    if (t->bkl_depth == 0) raw_lock(true);
    t->bkl_depth++;
    irq_restore(s);
}

void bkl_release(void)
{
    IrqState s = irq_save();
    Thread *t = me();
    if (t->bkl_depth > 0 && --t->bkl_depth == 0) raw_unlock();
    irq_restore(s);
}

void bkl_acquire_boot(void)
{
    raw_lock(false);
    me()->bkl_depth = 1;
}

void bkl_leave_kernel(void)
{
    IrqState s = irq_save();
    Thread *t = me();
    if (t->bkl_depth) { t->bkl_depth = 0; raw_unlock(); }
    irq_restore(s);
}

uint32_t bkl_drop(void)
{
    IrqState s = irq_save();
    Thread *t = me();
    uint32_t depth = t->bkl_depth;
    if (depth) { t->bkl_depth = 0; raw_unlock(); }
    irq_restore(s);
    return depth;
}

void bkl_restore(uint32_t depth)
{
    if (!depth) return;
    IrqState s = irq_save();
    Thread *t = me();
    raw_lock(true);
    t->bkl_depth = depth;
    irq_restore(s);
}

bool bkl_held(void)
{
    Thread *t = me();
    return t && t->bkl_depth > 0;
}

/* The scheduler, with interrupts off: a holder leaving and coming back */
void bkl_switch_out(Thread *t) { if (t->bkl_depth) raw_unlock(); }
void bkl_switch_in(Thread *t)  { if (t->bkl_depth) relock(t); }

void bkl_relax(void)
{
    Thread *t = me();
    if (!t->bkl_depth || !__atomic_load_n(&g_bkl.contenders, __ATOMIC_SEQ_CST)) return;
    IrqState s = irq_save();
    raw_unlock();                             /* (wakes a halted waiter) */
    /* Let a contender have it: it is spinning, or waking up to try */
    for (int i = 0; i < 1000000 && !__atomic_load_n(&g_bkl.locked, __ATOMIC_ACQUIRE) &&
                    __atomic_load_n(&g_bkl.contenders, __ATOMIC_ACQUIRE); i++)
        pause_cpu();
    relock(t);
    irq_restore(s);
}

void cpu_idle_wait(void)
{
    IrqState s = irq_save();
    Thread *t = me();
    uint32_t depth = t->bkl_depth;
    t->bkl_depth = 0;                         /* an interrupt in the window takes it anew */
    if (depth) raw_unlock();
    KiGetCurrentKpcr()->Idle = 1;
    /* A thread queued between this CPU's last look and Idle going up was
     * not kicked for (smp_kick saw it busy): halting now would leave it
     * there until the next tick.  (The fences pair: either the waker sees
     * Idle, or this sees its thread.) */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!sched_work_waiting())
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
    /* An interrupt ended the halt (and may have moved this thread to
     * another CPU) */
    KiGetCurrentKpcr()->Idle = 0;
    if (depth) { raw_lock(true); t->bkl_depth = depth; }
    irq_restore(s);
}

/* -----------------------------------------------------------------------
 * IPIs
 * ----------------------------------------------------------------------- */
void smp_ipi(uint64_t vector)
{
    PKPCR k = KiGetCurrentKpcr();
    k->Idle = 0;
    if (vector == IPI_TLB) tlb_flush_local(k);
    apic_eoi();
    /* A thread woken for this CPU should run now (sched_unblock) — unless
     * this CPU halts waiting for the kernel lock (see interrupt_dispatch:
     * its next timer tick switches then) */
    if (vector == IPI_WAKE && !k->LockWait) sched_resched_ipi();
}

bool smp_kick(uint32_t prefer)
{
    if (g_cpu_count < 2) return false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* (the thread queued before Idle is read: cpu_idle_wait) */
    PKPCR self = KiGetCurrentKpcr();
    PKPCR p = prefer < MAX_CPUS ? &g_kpcr[prefer] : NULL;
    if (p && p != self && p->Online && p->Idle) {    /* the thread's own CPU, if it is idle */
        p->Idle = 0;
        apic_send_ipi(p->ApicId, APIC_IPI_FIXED | IPI_WAKE);
        return true;
    }
    for (uint32_t i = 0; i < MAX_CPUS; i++) {    /* a CPU halted in its idle thread: it will steal it */
        PKPCR k = &g_kpcr[i];
        if (k == self || !k->Online || !k->Idle || k->CurrentThread != k->IdleThread) continue;
        k->Idle = 0;                          /* one IPI per halt is enough */
        apic_send_ipi(k->ApicId, APIC_IPI_FIXED | IPI_WAKE);
        return true;
    }
    return false;
}

void smp_tlb_flush(uint64_t cr3)
{
    if (g_cpu_count < 2) return;
    IrqState s = spin_lock_irqsave(&g_tlb_lock);
    PKPCR self = KiGetCurrentKpcr();
    uint32_t wait = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        PKPCR k = &g_kpcr[i];
        if (k == self || !k->Online) continue;
        Thread *t = k->CurrentThread;
        if (cr3 && (!t || t->cr3 != cr3)) continue;   /* not in that address space */
        __atomic_store_n(&k->TlbFlush, cr3 ? 1 : 2, __ATOMIC_RELEASE);
        apic_send_ipi(k->ApicId, APIC_IPI_FIXED | IPI_TLB);
        wait |= 1u << i;
    }
    while (wait) {
        for (uint32_t i = 0; i < MAX_CPUS; i++)
            if ((wait & (1u << i)) && !__atomic_load_n(&g_kpcr[i].TlbFlush, __ATOMIC_ACQUIRE))
                wait &= ~(1u << i);
        pause_cpu();
    }
    spin_unlock_irqrestore(&g_tlb_lock, s);
}

/* -----------------------------------------------------------------------
 * Finding the CPUs: the ACPI MADT's processor-local-APIC entries
 * ----------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    char     sig[4];
    uint32_t len;
    uint8_t  rev, csum;
    char     oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} AcpiHeader;

static bool     g_low_ok;

#define TRAMP_PA  0x8000u                     /* trampoline; its page tables follow */
#define TRAMP_PML4 (TRAMP_PA + 0x1000)
#define TRAMP_PDPT (TRAMP_PA + 0x2000)
#define TRAMP_PD   (TRAMP_PA + 0x3000)

void smp_early(const BootInfo *info)
{
    const BootMemDescriptor *m = PHYS_TO_VIRT(info->mem_map);
    g_low_ok = true;
    for (uint64_t a = TRAMP_PA; a < TRAMP_PD + 0x1000; a += PAGE_SIZE) {
        bool free = false;
        for (uint32_t i = 0; i < info->mem_map_count; i++) {
            const BootMemDescriptor *d = &m[i];
            if (a >= d->physical_base && a < d->physical_base + d->num_pages * PAGE_SIZE)
                free = d->type == BOOT_MEM_CONVENTIONAL || d->type == BOOT_MEM_BOOT_SERVICES;
        }
        if (!free) g_low_ok = false;
    }
}

/* APIC IDs of the usable CPUs (the boot CPU included); returns the count */
static uint32_t madt_cpus(uint8_t *ids, uint32_t max)
{
    const AcpiHeader *madt = AcpiFindTable("APIC");
    if (!madt) return 0;
    const uint8_t *p = (const uint8_t *)madt + sizeof(AcpiHeader) + 8;   /* LAPIC address, flags */
    const uint8_t *end = (const uint8_t *)madt + madt->len;
    uint32_t n = 0;
    while (p + 2 <= end && p[1] >= 2) {
        if (p[0] == 0 && p[1] >= 8) {                     /* processor local APIC */
            uint32_t flags = *(const uint32_t *)(p + 4);
            if ((flags & 3) && n < max) ids[n++] = p[3];  /* enabled or online-capable */
        }
        p += p[1];
    }
    return n;
}

/* -----------------------------------------------------------------------
 * Starting the other CPUs
 * ----------------------------------------------------------------------- */
extern const uint8_t ap_trampoline_start[], ap_trampoline_end[], ap_trampoline_data[],
                     ap_trampoline_wake32[];

#define AP_STACK_SIZE (16 * 1024)

typedef struct {
    uint8_t *stack;
    CpuGdt  *gdt;
    uint8_t *ist;                              /* 4 × EXCEPTION_STACK_SIZE */
    Thread  *idle;
} ApSetup;

static ApSetup g_ap[MAX_CPUS];
static volatile uint32_t g_ap_started;
static uint64_t g_bsp_cr0, g_bsp_cr4, g_bsp_efer, g_bsp_xcr0;

static void __attribute__((noreturn)) ap_entry(uint64_t cpu)
{
    /* Leave the trampoline's page table for the kernel's */
    write_cr3(paging_get_kernel_cr3());
    write_cr0(g_bsp_cr0);
    write_cr4(g_bsp_cr4);
    wrmsr(MSR_IA32_EFER, rdmsr(MSR_IA32_EFER) | (g_bsp_efer & (EFER_SCE | EFER_NXE)));
    if (g_bsp_cr4 & (1u << 18))                        /* OSXSAVE: same XCR0 as CPU 0 */
        __asm__ volatile ("xsetbv" : : "c"(0), "a"((uint32_t)g_bsp_xcr0),
                          "d"((uint32_t)(g_bsp_xcr0 >> 32)));

    ApSetup *a = &g_ap[cpu];
    uint8_t *ist = a->ist;
    gdt_init_cpu(a->gdt, ist, ist + EXCEPTION_STACK_SIZE, ist + 2 * EXCEPTION_STACK_SIZE,
                 ist + 3 * EXCEPTION_STACK_SIZE);
    PKPCR k = &g_kpcr[cpu];
    KiInitializeKpcr(k, (UINT32)cpu);                 /* after the GDT: loading GS clears its base */
    k->Gdt = a->gdt;
    k->Tss = &a->gdt->tss;
    idt_load();
    SyscallInitCpu();
    __asm__ volatile ("fninit");
    apic_init_ap();
    k->ApicId = apic_id();
    sched_init_cpu(a->idle);
    __atomic_store_n(&g_ap_started, 1, __ATOMIC_RELEASE);

    /* Join the scheduler once CPU 0 lets go of the kernel lock */
    bkl_acquire_boot();
    k->Online = 1;
    g_cpu_count++;
    kprintf("[SMP] CPU %u online (APIC ID %u)\n", (unsigned)cpu, (unsigned)k->ApicId);
    UmCpuCountChanged();
    bkl_release();                            /* the idle loop needs no lock */
    for (;;) {
        sched_yield();
        cpu_idle_wait();
    }
}

/* The start-up page: the trampoline, and a page table for it (the kernel's
 * upper half plus the low 2 MiB mapped 1:1, where the trampoline runs as
 * paging turns on).  Set up once; S3 resume uses it as the waking vector. */
static bool g_tramp_ready;

static bool tramp_setup(void)
{
    if (g_tramp_ready) return true;
    if (!g_low_ok) return false;
    memcpy(PHYS_TO_VIRT(TRAMP_PA), ap_trampoline_start, (size_t)(ap_trampoline_end - ap_trampoline_start));
    uint64_t *pml4 = PHYS_TO_VIRT(TRAMP_PML4), *pdpt = PHYS_TO_VIRT(TRAMP_PDPT), *pd = PHYS_TO_VIRT(TRAMP_PD);
    const uint64_t *kpml4 = PHYS_TO_VIRT(paging_get_kernel_cr3());
    memset(pml4, 0, PAGE_SIZE); memset(pdpt, 0, PAGE_SIZE); memset(pd, 0, PAGE_SIZE);
    for (int i = 256; i < 512; i++) pml4[i] = kpml4[i];
    pml4[0] = TRAMP_PDPT | PTE_PRESENT | PTE_WRITE;
    pdpt[0] = TRAMP_PD | PTE_PRESENT | PTE_WRITE;
    pd[0]   = 0 | PTE_PRESENT | PTE_WRITE | PTE_HUGE;

    g_bsp_cr0 = read_cr0();
    g_bsp_cr4 = read_cr4();
    g_bsp_efer = rdmsr(MSR_IA32_EFER);
    if (g_bsp_cr4 & (1u << 18)) {
        uint32_t lo, hi;
        __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        g_bsp_xcr0 = ((uint64_t)hi << 32) | lo;
    }
    g_tramp_ready = true;
    return true;
}

uint32_t smp_trampoline(void (*entry)(uint64_t), void *stack_top, uint64_t arg)
{
    if (!tramp_setup()) return 0;
    uint64_t *data = PHYS_TO_VIRT(TRAMP_PA + (uint64_t)(ap_trampoline_data - ap_trampoline_start));
    data[0] = TRAMP_PML4;
    data[1] = (uint64_t)(uintptr_t)entry;
    data[2] = (uint64_t)(uintptr_t)stack_top;
    data[3] = arg;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return TRAMP_PA;
}

uint32_t smp_trampoline_wake32(void)
{
    return TRAMP_PA + (uint32_t)(ap_trampoline_wake32 - ap_trampoline_start);
}

bool smp_start_cpu(uint8_t apic, volatile uint32_t *started)
{
    apic_send_ipi(apic, APIC_IPI_INIT);
    udelay(10000);
    for (int attempt = 0; attempt < 2; attempt++) {
        apic_send_ipi(apic, APIC_IPI_SIPI | (TRAMP_PA >> 12));
        for (int i = 0; i < (attempt ? 20000 : 100); i++) {      /* 1 ms, then 200 ms */
            if (__atomic_load_n(started, __ATOMIC_ACQUIRE)) return true;
            udelay(10);
        }
    }
    return false;
}

static bool start_ap(uint32_t cpu, uint8_t apic)
{
    ApSetup *a = &g_ap[cpu];
    a->stack = kernel_alloc_pages(AP_STACK_SIZE / PAGE_SIZE);
    a->gdt = kernel_alloc_pages(1);
    a->ist = kernel_alloc_pages(4 * EXCEPTION_STACK_SIZE / PAGE_SIZE);
    a->idle = a->stack ? sched_new_idle_thread(cpu, a->stack, AP_STACK_SIZE) : NULL;
    if (!a->stack || !a->gdt || !a->ist || !a->idle) return false;
    memset(a->gdt, 0, PAGE_SIZE);

    smp_trampoline((void (*)(uint64_t))ap_entry, a->stack + AP_STACK_SIZE, cpu);
    __atomic_store_n(&g_ap_started, 0, __ATOMIC_SEQ_CST);
    return smp_start_cpu(apic, &g_ap_started);
}

uint32_t smp_start(void)
{
    g_kpcr[0].ApicId = apic_id();
    uint8_t ids[64];
    uint32_t n = madt_cpus(ids, 64);
    if (n <= 1) {
        kprintf("[SMP] One CPU%s\n", n ? "" : " (no ACPI MADT)");
        return 1;
    }
    if (!g_low_ok) {
        kprintf("[SMP] %u CPUs, but the start-up page 0x%x is in use: using one\n", n, TRAMP_PA);
        return 1;
    }

    tramp_setup();

    uint32_t cpu = 1, started = 0;
    for (uint32_t i = 0; i < n && cpu < MAX_CPUS; i++) {
        if (ids[i] == g_kpcr[0].ApicId) continue;
        if (start_ap(cpu, ids[i])) { cpu++; started++; }
        else kprintf("[SMP] CPU with APIC ID %u did not start\n", ids[i]);
    }
    kprintf("[SMP] %u CPUs found, %u started%s\n", n, started + 1,
            n > MAX_CPUS ? " (the rest exceed MAX_CPUS)" : "");
    return started + 1;
}
