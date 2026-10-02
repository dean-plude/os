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

/* Each CPU has its own run queue: the threads ready to run there and the
 * threads sleeping with a deadline there, under the queue's spinlock.  A
 * thread belongs to the queue of the CPU it last ran on (Thread.cpu), and
 * that queue's lock also guards the thread's state: whoever wakes it locks
 * its queue (lock_thread_rq).  A CPU with nothing to run takes a ready
 * thread from another CPU's queue (steal); a thread that becomes ready
 * kicks an idle CPU (smp_kick), which then comes looking.
 *
 * A switch holds the CPU's queue lock from choosing the next thread until
 * that thread runs (finish_switch releases it), so no other CPU can take
 * the outgoing thread before its registers are saved (Thread.on_cpu says
 * when they are); and a thread that blocks or sleeps changes its state and
 * switches out under the same hold, so a wake-up can't slip in between.
 * The scheduler runs on every CPU at once, without the big kernel lock. */
#define current_thread ((Thread *)KiGetCurrentKpcr()->CurrentThread)
static inline Thread *cpu_idle(void) { return KiGetCurrentKpcr()->IdleThread; }
static inline uint32_t this_cpu(void) { return KiGetCurrentKpcr()->CpuNumber; }

typedef struct {
    KSpinLock  lock;
    Thread    *head;        /* ready threads: circular list, next to run first */
    size_t     count;
    Thread    *sleepers;    /* sched_sleep_until with a deadline */
} RunQueue;

static RunQueue g_rq[MAX_CPUS];
static inline RunQueue *my_rq(void) { return &g_rq[this_cpu()]; }

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

/* -----------------------------------------------------------------------
 * Run queues (the queue's lock held)
 * ----------------------------------------------------------------------- */

static void rq_enqueue(RunQueue *rq, Thread *t)
{
    t->state = THREAD_READY;
    if (!rq->head) {
        t->next = t;
        t->prev = t;
        rq->head = t;
    } else {
        /* Insert before head (at the tail of the circular list) */
        Thread *tail = rq->head->prev;
        tail->next     = t;
        t->prev        = tail;
        t->next        = rq->head;
        rq->head->prev = t;
    }
    rq->count++;
}

/* A thread became runnable (not merely preempted or yielding): let a
 * halted CPU run it — its own, or any idle one, which will steal it */
static void ready_wake(RunQueue *rq, Thread *t)
{
    rq_enqueue(rq, t);
    smp_kick(t->cpu);
}

/* Threads above this priority are "foreground" (the desktop, programs, the
 * network); the idle threads and csrss run only when none of those is ready. */
#define BACKGROUND_PRIO 4

static Thread *rq_dequeue(RunQueue *rq)
{
    if (!rq->head) return NULL;
    Thread *t = rq->head;
    for (Thread *c = rq->head;;) {                  /* first foreground thread in turn */
        if (c->priority > BACKGROUND_PRIO) { t = c; break; }
        c = c->next;
        if (c == rq->head) break;
    }
    if (t->next == t) {
        rq->head = NULL;                            /* only one element */
    } else {
        t->prev->next = t->next;                    /* unlink t */
        t->next->prev = t->prev;
        if (rq->head == t) rq->head = t->next;
    }
    t->next = t->prev = NULL;
    rq->count--;
    return t;
}

static void rq_drop_sleeper(RunQueue *rq, Thread *t)
{
    for (Thread **pp = &rq->sleepers; *pp; pp = &(*pp)->sleep_next)
        if (*pp == t) { *pp = t->sleep_next; break; }
    t->sleep_next = NULL;
    t->in_sleepers = false;
}

/* Lock the queue @t belongs to (it may move meanwhile: check after) */
static RunQueue *lock_thread_rq(Thread *t, IrqState *s)
{
    for (;;) {
        uint32_t c = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        RunQueue *rq = &g_rq[c];
        *s = spin_lock_irqsave(&rq->lock);
        if (t->cpu == c) return rq;
        spin_unlock_irqrestore(&rq->lock, *s);
    }
}

/* Take a ready thread from another CPU's queue for this one (our queue's
 * lock held: the others only by trylock, so two CPUs never wait on each
 * other) */
static Thread *steal(void)
{
    uint32_t me = this_cpu();
    for (uint32_t n = 1; n < g_cpu_count; n++) {
        uint32_t c = (me + n) % g_cpu_count;
        RunQueue *v = &g_rq[c];
        if (!__atomic_load_n(&v->head, __ATOMIC_RELAXED)) continue;
        /* Its owner holds the lock only briefly (it may be queueing the
         * very thread it kicked us for, and still sending the IPI): try
         * for a while, not forever (two CPUs stealing from each other
         * would otherwise wait on each other) */
        bool locked = false;
        for (int i = 0; i < 20000 && !(locked = spin_trylock(&v->lock)); i++) {
            smp_poll_tlb();
            pause_cpu();
        }
        if (!locked) continue;
        Thread *t = rq_dequeue(v);
        if (t) {
            if (t->in_sleepers) rq_drop_sleeper(v, t);
            __atomic_store_n(&t->cpu, me, __ATOMIC_RELEASE);   /* ours now */
        }
        spin_unlock(&v->lock);
        if (t) return t;
    }
    return NULL;
}

/* True when a foreground thread other than the caller is waiting to run
 * on this CPU */
bool sched_foreground_ready(void)
{
    RunQueue *rq = my_rq();
    IrqState irq = spin_lock_irqsave(&rq->lock);
    bool any = false;
    if (rq->head) {
        Thread *c = rq->head;
        do { if (c->priority > BACKGROUND_PRIO) { any = true; break; } c = c->next; } while (c != rq->head);
    }
    spin_unlock_irqrestore(&rq->lock, irq);
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
 * (it begins with interrupts off and its CPU's queue locked), then run. */
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

    idle->cpu      = 0;
    idle->on_cpu   = true;
    KiGetCurrentKpcr()->IdleThread    = idle;
    KiGetCurrentKpcr()->CurrentThread = idle;
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
    idle->cpu        = k->CpuNumber;
    idle->on_cpu     = true;
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
    IrqState irq = irq_save();
    t->cpu = this_cpu();                      /* starts in the creator's queue */
    RunQueue *rq = &g_rq[t->cpu];
    spin_lock(&rq->lock);
    ready_wake(rq, t);
    spin_unlock_irqrestore(&rq->lock, irq);
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
 * interrupts off and this CPU's queue locked; returns (in the thread that
 * called it, once it runs again) with the lock released, interrupts off.
 * ----------------------------------------------------------------------- */
static void switch_locked(RunQueue *rq)
{
    PKPCR kpcr = KiGetCurrentKpcr();
    Thread *prev = current_thread;
    Thread *next = rq_dequeue(rq);
    /* Nothing queued here, and this CPU would go idle: take work waiting
     * on another CPU (a busy CPU doesn't: threads would bounce between
     * CPUs) */
    if (!next && (prev->state != THREAD_RUNNING || prev->idle)) next = steal();
    if (!next) {
        /* No other ready thread.  Keep running the current one if it can,
         * else this CPU's idle thread. */
        next = (prev->state == THREAD_RUNNING) ? prev : cpu_idle();
    }

    if (prev == next) {
        next->state       = THREAD_RUNNING;
        next->ticks_slice = 0;
        spin_unlock(&rq->lock);
        return;
    }

    /* If current is still running, put it back on the ready queue (an
     * idle thread only ever runs as the fallback above) */
    if (prev->state == THREAD_RUNNING) {
        if (prev->idle) prev->state = THREAD_READY;
        else rq_enqueue(rq, prev);
    }
    /* (a thread is queued only once switched out, under the lock of the
     * queue its CPU was switching from: this never waits in practice) */
    while (__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE)) pause_cpu();
    next->on_cpu        = true;
    next->cpu           = this_cpu();
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
        wrmsr(MSR_IA32_FSBASE, next->fs_base);       /* 32-bit programs: fs:0 is the TEB */
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
    if (prev) {
        if (prev->state == THREAD_DEAD) prev->off_cpu = true;   /* its stack is free now */
        __atomic_store_n(&prev->on_cpu, false, __ATOMIC_RELEASE);   /* registers saved */
    }
    spin_unlock(&g_rq[kpcr->CpuNumber].lock);
    bkl_switch_in(current_thread);
}

static void perform_switch(void)
{
    RunQueue *rq = my_rq();
    spin_lock(&rq->lock);
    switch_locked(rq);
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
static KSpinLock tick_lock = KSPINLOCK_INIT;
static void wake_sleepers(RunQueue *rq);
void DesktopWatchdog(uint64_t now);
void UmTimerTick(uint64_t ticks);

void ps2_poll(void);
void UsbPoll(void);

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
            UsbPoll();                      /* (USB ones too) */
            DesktopWatchdog(tick_count);
            UmTimerTick(tick_count);
        }
        spin_unlock(&tick_lock);
    }
    RunQueue *rq = my_rq();                 /* each CPU wakes its own sleepers */
    if (rq->sleepers) wake_sleepers(rq);
    if (!current_thread) return;

    current_thread->ticks_total++;
    current_thread->ticks_slice++;

    /* An idle CPU looks for work waiting on the others at every tick */
    bool steal_now = false;
    if (current_thread->idle)
        for (uint32_t c = 0; c < g_cpu_count && !steal_now; c++)
            steal_now = __atomic_load_n(&g_rq[c].head, __ATOMIC_RELAXED) != NULL;
    if (steal_now || current_thread->ticks_slice >= TICKS_PER_SLICE) {
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
    IrqState irq = irq_save();
    RunQueue *rq = my_rq();
    spin_lock(&rq->lock);
    if ((flag && *flag) || tick_count >= deadline) {  /* woken already, or due */
        spin_unlock_irqrestore(&rq->lock, irq);
        return;
    }
    Thread *t = current_thread;
    t->state = THREAD_WAITING;
    t->wake_tick = deadline;
    if (!t->in_sleepers) {                          /* (still there from a wake-up by sched_unblock) */
        t->sleep_next = rq->sleepers;
        rq->sleepers = t;
        t->in_sleepers = true;
    }
    switch_locked(rq);                              /* woken by sched_unblock or sched_tick */
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
static void wake_sleepers(RunQueue *rq)
{
    spin_lock(&rq->lock);
    for (Thread **pp = &rq->sleepers; *pp;) {
        Thread *t = *pp;
        bool due = t->wake_tick <= tick_count;
        if (due || t->state != THREAD_WAITING) {    /* due, or woken some other way */
            *pp = t->sleep_next;
            t->sleep_next = NULL;
            t->in_sleepers = false;
            if (due && t->state == THREAD_WAITING) ready_wake(rq, t);
        } else pp = &t->sleep_next;
    }
    spin_unlock(&rq->lock);
}

void sched_block(void)
{
    IrqState irq = irq_save();
    RunQueue *rq = my_rq();
    spin_lock(&rq->lock);
    current_thread->state = THREAD_WAITING;
    switch_locked(rq);
    irq_restore(irq);
}

void sched_unblock(Thread *t)
{
    IrqState irq;
    RunQueue *rq = lock_thread_rq(t, &irq);
    if (t->state == THREAD_WAITING) ready_wake(rq, t);
    spin_unlock_irqrestore(&rq->lock, irq);
}

/* -----------------------------------------------------------------------
 * Thread exit and reclamation
 * ----------------------------------------------------------------------- */
void sched_exit_current(void)
{
    cli();
    RunQueue *rq = my_rq();
    spin_lock(&rq->lock);
    current_thread->state = THREAD_DEAD;
    switch_locked(rq);                  /* never comes back */
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
    sched_start_thread(t);
}

/* -----------------------------------------------------------------------
 * sched_dump
 * ----------------------------------------------------------------------- */
void sched_dump(void)
{
    kprintf("[SCHED] Current: '%s' TID=%lu  ticks=%lu\n",
            current_thread->name, current_thread->tid, tick_count);
    for (uint32_t c = 0; c < g_cpu_count; c++) {
        RunQueue *rq = &g_rq[c];
        IrqState irq = spin_lock_irqsave(&rq->lock);
        Thread *t = rq->head;
        if (t) {
            kprintf("[SCHED] CPU %u ready queue (%zu):\n", c, rq->count);
            do {
                kprintf("  TID=%lu '%s' state=%d prio=%u\n",
                        t->tid, t->name, t->state, t->priority);
                t = t->next;
            } while (t != rq->head);
        }
        spin_unlock_irqrestore(&rq->lock, irq);
    }
}
