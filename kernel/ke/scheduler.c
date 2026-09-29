/*
 * scheduler.c — round-robin preemptive kernel scheduler
 *
 * Context switch mechanism:
 *
 * The heart of the scheduler is the context switch: saving the current
 * thread's registers and loading the next thread's registers.
 *
 * We use an inline assembly function that:
 *   1. Saves callee-saved registers + RFLAGS to the current thread's context
 *   2. Updates the current thread pointer
 *   3. Loads the next thread's context
 *   4. Returns (in the context of the new thread)
 *
 * The first time a thread runs, it returns from what looks like a function
 * call into its own entry point (set up by sched_create_thread).
 *
 * Stack layout for a newly created thread:
 *   [stack_top - 8]   = thread_trampoline address (pushed as "return address")
 *
 * thread_trampoline calls thread->entry(thread->arg) and then calls
 * sched_exit() when the entry returns.
 */

#include "scheduler.h"
#include "printf.h"
#include "kpcr.h"
#include "smp.h"
#include "spinlock.h"
#include "../mm/vmm.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/paging.h"
#include "../arch/x86_64/apic.h"

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */

/* The running thread and the idle thread are per CPU (KPCR).  Everything
 * shared (the ready queue, thread states, the sleepers) is under sched_lock,
 * a spinlock: the scheduler runs on every CPU at once, without the big
 * kernel lock.
 *
 * A switch holds sched_lock from choosing the next thread until that
 * thread runs (finish_switch releases it), so no other CPU can pick up the
 * outgoing thread before its registers are saved; and a thread that blocks
 * or sleeps changes its state and switches out under the same hold, so a
 * wake-up can't slip in between. */
#define current_thread ((Thread *)KiGetCurrentKpcr()->CurrentThread)
static inline Thread *cpu_idle(void) { return KiGetCurrentKpcr()->IdleThread; }

/* Ready queue (circular doubly-linked list, sentineled by idle_thread) */
static Thread *ready_head;   /* Points to the thread to run next */
static size_t  ready_count;

/* The boot CPU's idle thread: the boot context (main.c's final loop) */
static Thread  idle_thread_obj;

/* Monotonically increasing thread ID */
static uint64_t next_tid = 1;

/* Global tick counter: 10 ms units of the TSC since boot, advanced by
 * whichever CPU's timer interrupt sees it has moved on */
static volatile uint64_t tick_count;
static uint64_t tsc_at_boot;

/* Ticks per time slice before preemption */
#define TICKS_PER_SLICE  2   /* 2 ticks @ 100Hz = 20ms quantum */

static KSpinLock sched_lock = KSPINLOCK_INIT;

/* -----------------------------------------------------------------------
 * Ready queue manipulation (assumes sched_lock held)
 * ----------------------------------------------------------------------- */

static void ready_enqueue(Thread *t)
{
    t->state = THREAD_READY;
    if (!ready_head) {
        t->next = t;
        t->prev = t;
        ready_head = t;
    } else {
        /* Insert before ready_head (at the tail of the circular list) */
        Thread *tail = ready_head->prev;
        tail->next     = t;
        t->prev        = tail;
        t->next        = ready_head;
        ready_head->prev = t;
    }
    ready_count++;
}

/* A thread became runnable (not merely preempted or yielding): let a
 * halted CPU pick it up */
static void ready_wake(Thread *t)
{
    ready_enqueue(t);
    smp_kick();
}

/* Threads above this priority are "foreground" (the desktop, programs, the
 * network); the idle threads and csrss run only when none of those is ready. */
#define BACKGROUND_PRIO 4

static Thread *ready_dequeue(void)
{
    if (!ready_head) return NULL;
    Thread *t = ready_head;
    for (Thread *c = ready_head;;) {                /* first foreground thread in turn */
        if (c->priority > BACKGROUND_PRIO) { t = c; break; }
        c = c->next;
        if (c == ready_head) break;
    }
    if (t->next == t) {
        /* Only one element */
        ready_head = NULL;
    } else {
        t->prev->next = t->next;                    /* unlink t */
        t->next->prev = t->prev;
        if (ready_head == t) ready_head = t->next;
    }
    t->next = t->prev = NULL;
    ready_count--;
    return t;
}

/* True when a foreground thread other than the caller is waiting to run */
bool sched_foreground_ready(void)
{
    IrqState irq = spin_lock_irqsave(&sched_lock);
    bool any = false;
    if (ready_head) {
        Thread *c = ready_head;
        do { if (c->priority > BACKGROUND_PRIO) { any = true; break; } c = c->next; } while (c != ready_head);
    }
    spin_unlock_irqrestore(&sched_lock, irq);
    return any;
}

/* -----------------------------------------------------------------------
 * Context switch (assembly implementation)
 *
 * void context_switch(ThreadContext *save, ThreadContext *load);
 *
 * Saves current CPU state to *save, then loads state from *load.
 * Both pointers are passed in RDI and RSI (System V ABI).
 * ----------------------------------------------------------------------- */
static void __attribute__((naked)) context_switch(
    ThreadContext *save __unused, ThreadContext *load __unused)
{
    __asm__ volatile (
        /* Save callee-saved registers to *save (RDI) */
        "pushfq\n\t"
        "pop  8*7(%%rdi)\n\t"   /* rflags */
        "mov  %%rbp, 8*1(%%rdi)\n\t"
        "mov  %%rbx, 8*2(%%rdi)\n\t"
        "mov  %%r12, 8*3(%%rdi)\n\t"
        "mov  %%r13, 8*4(%%rdi)\n\t"
        "mov  %%r14, 8*5(%%rdi)\n\t"
        "mov  %%r15, 8*6(%%rdi)\n\t"
        "mov  %%rsp, 8*0(%%rdi)\n\t"   /* save RSP last */

        /* Load callee-saved registers from *load (RSI) */
        "mov  8*0(%%rsi), %%rsp\n\t"   /* restore RSP first */
        "mov  8*1(%%rsi), %%rbp\n\t"
        "mov  8*2(%%rsi), %%rbx\n\t"
        "mov  8*3(%%rsi), %%r12\n\t"
        "mov  8*4(%%rsi), %%r13\n\t"
        "mov  8*5(%%rsi), %%r14\n\t"
        "mov  8*6(%%rsi), %%r15\n\t"
        "push 8*7(%%rsi)\n\t"   /* rflags */
        "popfq\n\t"

        "ret\n\t"
        :
        : /* no constraints — naked function */
        :
    );
}

/* -----------------------------------------------------------------------
 * Thread entry trampoline
 *
 * All new threads start here (this is the "return address" placed on
 * the initial stack).  We store the entry and argument in r12/r13
 * (callee-saved, so they survive context_switch).
 * ----------------------------------------------------------------------- */

static void finish_switch(void);

/* A new thread's first instructions: complete the switch that started it
 * (it begins with interrupts off and sched_lock held), then run. */
void sched_thread_start(void)
{
    finish_switch();
    sti();
}

/* These are stored as per-thread data in r12 (entry) and r13 (arg).
 * We arrange this by setting up the initial stack in sched_create_thread. */
static void __attribute__((noreturn)) thread_trampoline(void)
{
    /* r12 = entry function, r13 = arg (set up in new thread's initial context) */
    ThreadEntry entry;
    void *arg;
    __asm__ volatile (
        "mov %%r12, %0\n\t"
        "mov %%r13, %1\n\t"
        : "=r"(entry), "=r"(arg)
    );
    sched_thread_start();

    entry(arg);

    /* Thread returned */
    kprintf("[SCHED] Thread '%s' (TID %lu) exited\n",
            current_thread->name, current_thread->tid);
    sched_exit_current();
}

/* -----------------------------------------------------------------------
 * sched_init
 * ----------------------------------------------------------------------- */
void sched_init(void)
{
    /* Initialize the idle thread (it IS the current boot context initially) */
    Thread *idle = &idle_thread_obj;
    __builtin_memset(idle, 0, sizeof(*idle));
    idle->tid      = next_tid++;
    idle->state    = THREAD_RUNNING;
    idle->priority = 0;
    idle->idle     = true;
    __builtin_memcpy(idle->name, "idle", 5);
    /* (the boot context takes the big kernel lock next, in main.c) */
    /* idle's stack is the current boot stack — we don't track it */

    KiGetCurrentKpcr()->IdleThread    = idle;
    KiGetCurrentKpcr()->CurrentThread = idle;
    ready_head     = NULL;
    ready_count    = 0;
    tsc_at_boot    = rdtsc();

    kprintf("[SCHED] Scheduler initialized (idle TID=%lu)\n", idle->tid);
}

Thread *sched_new_idle_thread(uint32_t cpu, void *stack, size_t stack_size)
{
    Thread *idle = kzalloc(sizeof(Thread));
    if (!idle) return NULL;
    idle->tid          = __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED);
    idle->state        = THREAD_RUNNING;
    idle->idle         = true;
    idle->kernel_stack = stack;
    idle->stack_size   = stack_size;
    ksnprintf(idle->name, sizeof(idle->name), "idle%u", cpu);
    return idle;
}

void sched_init_cpu(Thread *idle)
{
    PKPCR k = KiGetCurrentKpcr();
    uintptr_t top = (uintptr_t)idle->kernel_stack + idle->stack_size;
    k->IdleThread    = idle;
    k->CurrentThread = idle;
    k->KernelRsp     = top;
    gdt_set_rsp0(top);
}

/* -----------------------------------------------------------------------
 * sched_create_thread
 * ----------------------------------------------------------------------- */
Thread *sched_create_thread(const char *name, ThreadEntry entry,
                             void *arg, uint8_t priority)
{
    return sched_create_thread_ex(name, entry, arg, priority, THREAD_STACK_SIZE);
}

Thread *sched_create_thread_ex(const char *name, ThreadEntry entry,
                                void *arg, uint8_t priority, size_t stack_size)
{
    Thread *t = sched_new_thread(name, entry, arg, priority, stack_size);
    if (t) sched_start_thread(t);
    return t;
}

void sched_start_thread(Thread *t)
{
    IrqState irq = spin_lock_irqsave(&sched_lock);
    ready_wake(t);
    spin_unlock_irqrestore(&sched_lock, irq);
}

Thread *sched_new_thread(const char *name, ThreadEntry entry,
                         void *arg, uint8_t priority, size_t stack_size)
{
    /* Allocate thread descriptor */
    Thread *t = kzalloc(sizeof(Thread));
    if (!t) return NULL;

    /* Allocate kernel stack */
    stack_size = (stack_size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    t->kernel_stack = kernel_alloc_pages(stack_size / PAGE_SIZE);
    if (!t->kernel_stack) {
        kfree(t);
        return NULL;
    }
    t->stack_size = stack_size;

    /* Stack top (stack grows down) */
    uintptr_t stack_top = (uintptr_t)t->kernel_stack + stack_size;

    /* Set up the initial stack so context_switch "returns" to thread_trampoline,
     * and thread_trampoline will find entry in r12, arg in r13.
     *
     * Initial context layout:
     *   context.rsp → points to [stack_top - 8] = address of thread_trampoline
     *   context.r12 = entry
     *   context.r13 = arg
     *   context.rbp = 0 (no frame)
     *   context.rflags = 0: it starts with interrupts off, still inside
     *   the switch to it (thread_trampoline finishes that, then sti)
     */
    uintptr_t *sp = (uintptr_t *)stack_top;
    /* Dummy return slot so that after context_switch's `ret` pops the
     * trampoline address, RSP ≡ 8 (mod 16) — the SysV ABI state on entry
     * to a function (as if it had been reached via `call`). */
    *--sp = 0;
    *--sp = (uintptr_t)thread_trampoline;  /* "return address" for context_switch */

    t->context.rsp    = (uint64_t)(uintptr_t)sp;
    t->context.rbp    = 0;
    t->context.rbx    = 0;
    t->context.r12    = (uint64_t)(uintptr_t)entry;
    t->context.r13    = (uint64_t)(uintptr_t)arg;
    t->context.r14    = 0;
    t->context.r15    = 0;
    t->context.rflags = 0x002;   /* IF=0, reserved bit 1 */

    /* Fill in metadata */
    t->tid       = __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED);
    t->priority  = priority;
    t->state     = THREAD_READY;
    t->bkl_depth = 1;            /* kernel threads hold the big kernel lock unless they let go */

    size_t name_len = 0;
    while (name[name_len] && name_len < THREAD_NAME_MAX - 1) name_len++;
    __builtin_memcpy(t->name, name, name_len);
    t->name[name_len] = '\0';

    kprintf("[SCHED] Created thread '%s' TID=%lu stack=%p\n",
            t->name, t->tid, t->kernel_stack);
    return t;
}

/* -----------------------------------------------------------------------
 * switch_locked — pick the next thread and switch to it.  Called with
 * interrupts off and sched_lock held; returns (in the thread that called
 * it, once it runs again) with sched_lock released, interrupts still off.
 * ----------------------------------------------------------------------- */
static void switch_locked(void)
{
    PKPCR kpcr = KiGetCurrentKpcr();
    Thread *prev = current_thread;
    Thread *next = ready_dequeue();
    if (!next) {
        /* No other ready thread.  Keep running the current one if it can,
         * else this CPU's idle thread. */
        next = (prev->state == THREAD_RUNNING) ? prev : cpu_idle();
    }

    if (prev == next) {
        next->state       = THREAD_RUNNING;
        next->ticks_slice = 0;
        spin_unlock(&sched_lock);
        return;
    }

    /* If current is still running, put it back on the ready queue (an
     * idle thread only ever runs as the fallback above) */
    if (prev->state == THREAD_RUNNING) {
        if (prev->idle) prev->state = THREAD_READY;
        else ready_enqueue(prev);
    }
    next->state         = THREAD_RUNNING;
    next->ticks_slice   = 0;
    kpcr->CurrentThread = next;
    kpcr->Idle          = 0;
    kpcr->PrevThread    = prev;

    /* Update TSS RSP0 to new thread's kernel stack top
     * (used when this thread returns to ring 3 in the future) */
    if (next->kernel_stack) {   /* boot context has no tracked stack */
        uintptr_t kstack_top = (uintptr_t)next->kernel_stack + next->stack_size;
        gdt_set_rsp0(kstack_top);

        /* Phase 5: Update KPCR.KernelRsp so syscall_entry.asm picks up the
         * correct kernel stack when this thread makes a system call. */
        kpcr->KernelRsp = (UINT64)kstack_top;
    }

    /* Address space: a user process's page table, or the kernel's for
     * kernel threads (never keep running on a table that may be freed
     * once its process exits). */
    uint64_t cr3 = next->cr3 ? next->cr3 : paging_get_kernel_cr3();
    if (read_cr3() != cr3) paging_load_cr3((uintptr_t)cr3);

    /* User threads: SSE state and the user GS base (the TEB, kept in
     * MSR_KERNEL_GS_BASE while in the kernel: GS itself is this CPU's
     * KPCR) are per thread.  Kernel code never uses SSE, so kernel
     * threads need neither saved. */
    if (prev->um) {
        __asm__ volatile ("fxsave64 (%0)" : : "r"(prev->fpu) : "memory");
        prev->gs_base = rdmsr(MSR_IA32_KERNEL_GSBASE);
    }
    if (next->um) {
        __asm__ volatile ("fxrstor64 (%0)" : : "r"(next->fpu) : "memory");
        wrmsr(MSR_IA32_KERNEL_GSBASE, next->gs_base);
    }

    bkl_switch_out(prev);               /* its big kernel lock waits for it */

    /* Perform the actual register swap */
    context_switch(&prev->context, &next->context);
    /* Running as the thread that called us, switched back in */
    finish_switch();
}

/* The second half of a switch, in the thread switched to */
static void finish_switch(void)
{
    PKPCR kpcr = KiGetCurrentKpcr();
    Thread *prev = kpcr->PrevThread;
    kpcr->PrevThread = NULL;
    if (prev && prev->state == THREAD_DEAD) prev->off_cpu = true;   /* its stack is free now */
    spin_unlock(&sched_lock);
    bkl_switch_in(current_thread);
}

static void perform_switch(void)
{
    spin_lock(&sched_lock);
    switch_locked();
}

/* -----------------------------------------------------------------------
 * sched_yield
 * ----------------------------------------------------------------------- */
void sched_yield(void)
{
    /* Other CPUs waiting to enter the kernel go first — unless this thread
     * is on its way to blocking or exiting (sched_block, sched_exit_current):
     * then it must be off this CPU before anyone could wake it. */
    if (current_thread->state == THREAD_RUNNING) bkl_relax();
    IrqState irq = irq_save();
    perform_switch();
    irq_restore(irq);
}

/* -----------------------------------------------------------------------
 * sched_tick — called from timer interrupt handler (interrupts disabled)
 * ----------------------------------------------------------------------- */
static Thread *g_sleepers;               /* sched_sleep_tick (sched_lock) */
static KSpinLock tick_lock = KSPINLOCK_INIT;
static void wake_sleepers(void);
void DesktopWatchdog(uint64_t now);
void UmTimerTick(uint64_t ticks);

void ps2_poll(void);

void sched_tick(void)
{
    /* Each CPU's timer runs at 100 Hz; the global tick follows the TSC, so
     * it neither runs N times too fast nor loses time while CPU 0 waits for
     * the kernel lock with interrupts off. */
    uint64_t now = g_tsc_per_tick ? (rdtsc() - tsc_at_boot) / g_tsc_per_tick : tick_count + 1;
    if (now > tick_count && spin_trylock(&tick_lock)) {   /* one CPU does the tick's work */
        if (now > tick_count) {
            tick_count = now;
            ps2_poll();                     /* keyboard/mouse, collected at 100 Hz */
            DesktopWatchdog(tick_count);
            UmTimerTick(tick_count);
            if (g_sleepers) wake_sleepers();
        }
        spin_unlock(&tick_lock);
    }
    if (!current_thread) return;

    current_thread->ticks_total++;
    current_thread->ticks_slice++;

    if (current_thread->ticks_slice >= TICKS_PER_SLICE) {
        /* Time slice expired — preempt */
        perform_switch();
    }
}

/* Timer ticks since boot (100 Hz) */
uint64_t sched_ticks(void)
{
    return tick_count;
}

/* -----------------------------------------------------------------------
 * sched_current
 * ----------------------------------------------------------------------- */
Thread *sched_current(void)
{
    return current_thread;
}

/* -----------------------------------------------------------------------
 * sched_block / sched_unblock
 * ----------------------------------------------------------------------- */
void sched_sleep_until(volatile uint32_t *flag, uint64_t deadline)
{
    IrqState irq = spin_lock_irqsave(&sched_lock);
    if ((flag && *flag) || tick_count >= deadline) {  /* woken already, or due */
        spin_unlock_irqrestore(&sched_lock, irq);
        return;
    }
    Thread *t = current_thread;
    t->state = THREAD_WAITING;
    t->wake_tick = deadline;
    if (!t->in_sleepers) {                          /* (still there from a wake-up by sched_unblock) */
        t->sleep_next = g_sleepers;
        g_sleepers = t;
        t->in_sleepers = true;
    }
    switch_locked();                                /* woken by sched_unblock or sched_tick */
    irq_restore(irq);
}

void sched_sleep_tick(void)
{
    sched_sleep_until(NULL, tick_count + 1);
}

void sched_wait(void)
{
    if (current_thread->wait_rounds++ < 4) sched_yield();
    else sched_sleep_tick();
}

/* Timer tick (interrupts off): sleepers whose tick has come are ready again */
static void wake_sleepers(void)
{
    spin_lock(&sched_lock);
    for (Thread **pp = &g_sleepers; *pp;) {
        Thread *t = *pp;
        bool due = t->wake_tick <= tick_count;
        if (due || t->state != THREAD_WAITING) {    /* due, or woken some other way */
            *pp = t->sleep_next;
            t->sleep_next = NULL;
            t->in_sleepers = false;
            if (due && t->state == THREAD_WAITING) ready_wake(t);
        } else pp = &t->sleep_next;
    }
    spin_unlock(&sched_lock);
}

void sched_block(void)
{
    IrqState irq = spin_lock_irqsave(&sched_lock);
    current_thread->state = THREAD_WAITING;
    switch_locked();
    irq_restore(irq);
}

void sched_unblock(Thread *t)
{
    IrqState irq = spin_lock_irqsave(&sched_lock);
    if (t->state == THREAD_WAITING) {
        ready_wake(t);
    }
    spin_unlock_irqrestore(&sched_lock, irq);
}

/* -----------------------------------------------------------------------
 * Thread exit and reclamation
 * ----------------------------------------------------------------------- */
void sched_exit_current(void)
{
    cli();
    spin_lock(&sched_lock);
    current_thread->state = THREAD_DEAD;
    switch_locked();                    /* never comes back */
    for (;;) hlt();
}

bool sched_thread_gone(const Thread *t)
{
    return t->state == THREAD_DEAD && t->off_cpu;
}

void sched_free_thread(Thread *t)
{
    if (!sched_thread_gone(t) || t == current_thread) return;
    if (t->kernel_stack) kernel_free_pages(t->kernel_stack, t->stack_size / PAGE_SIZE);
    kfree(t);
}

/* -----------------------------------------------------------------------
 * sched_enqueue_thread — public API for PS to enqueue pre-built threads
 * ----------------------------------------------------------------------- */
void sched_enqueue_thread(Thread *t)
{
    IrqState irq = spin_lock_irqsave(&sched_lock);
    ready_wake(t);
    spin_unlock_irqrestore(&sched_lock, irq);
}

/* -----------------------------------------------------------------------
 * sched_dump
 * ----------------------------------------------------------------------- */
void sched_dump(void)
{
    kprintf("[SCHED] Current: '%s' TID=%lu  ticks=%lu  ready_count=%zu\n",
            current_thread->name, current_thread->tid,
            tick_count, ready_count);

    Thread *t = ready_head;
    if (t) {
        kprintf("[SCHED] Ready queue:\n");
        do {
            kprintf("  TID=%lu '%s' state=%d prio=%u\n",
                    t->tid, t->name, t->state, t->priority);
            t = t->next;
        } while (t != ready_head);
    }
}
