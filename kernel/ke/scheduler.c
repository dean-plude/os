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
#include "ksym.h"
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
 *
 * A queue runs from the highest (current, dynamic) priority down, and the
 * threads of one priority take turns: a thread queued to wait its turn
 * goes after the others of its priority, ahead of those of lower priority.
 * A thread's priority changes only while it runs or waits (a wake-up boost,
 * its decay) or by a relink (the balance-set boost), so the order holds.
 * ----------------------------------------------------------------------- */

/* Link @t in before @c (NULL: at the tail) */
static void rq_link(RunQueue *rq, Thread *t, Thread *c)
{
    if (t->next || t->prev) {                       /* queued already: it would run twice */
        kprintf("[SCHED] BUG: thread '%s' (TID %lu, state %d, CPU %u) queued twice, by CPU %u\n",
                t->name, t->tid, t->state, t->cpu, this_cpu());
        KsymBacktraceHere();
        for (;;) { cli(); hlt(); }
    }
    t->state = THREAD_READY;
    t->ready_tick = tick_count;
    if (!rq->head) {
        t->next = t;
        t->prev = t;
        rq->head = t;
    } else {
        Thread *at = c ? c : rq->head;              /* (before the head: at the tail) */
        t->next = at;
        t->prev = at->prev;
        at->prev->next = t;
        at->prev = t;
        if (c == rq->head) rq->head = t;
    }
    rq->count++;
}

static void rq_unlink(RunQueue *rq, Thread *t)
{
    if (t->next == t) {
        rq->head = NULL;                            /* only one element */
    } else {
        t->prev->next = t->next;
        t->next->prev = t->prev;
        if (rq->head == t) rq->head = t->next;
    }
    t->next = t->prev = NULL;
    rq->count--;
}

/* Where @t goes in the queue: the first thread it goes before (NULL: the
 * tail).  AT_TAIL: after the threads of its priority; AT_FRONT: ahead of
 * them; AFTER_WOKEN: after those of them queued first as woken. */
enum { AT_TAIL, AT_FRONT, AFTER_WOKEN };
static Thread *rq_place(RunQueue *rq, const Thread *t, int where)
{
    Thread *c = rq->head;
    for (size_t n = 0; n < rq->count; n++, c = c->next) {
        if (c->priority < t->priority) return c;
        if (c->priority == t->priority && (where == AT_FRONT || (where == AFTER_WOKEN && !c->woken))) return c;
    }
    return NULL;
}

static void rq_enqueue(RunQueue *rq, Thread *t)
{
    rq_link(rq, t, rq_place(rq, t, AT_TAIL));
}

/* A woken thread that should run now: first among its priority */
static void rq_enqueue_front(RunQueue *rq, Thread *t)
{
    rq_link(rq, t, rq_place(rq, t, AT_FRONT));
    t->woken = true;
}

/* A thread preempted for one woken by a timer (sched_unblock_timer) or of
 * higher priority: after the woken threads queued first, but ahead of the
 * threads of its priority waiting their turn, as on NT, not last (else a
 * waker its wakee preempts would wait out every other thread's slice) */
static void rq_enqueue_preempted(RunQueue *rq, Thread *t)
{
    t->preempted = true;
    rq_link(rq, t, rq_place(rq, t, AFTER_WOKEN));
}

/* A thread became runnable (not merely preempted or yielding): let a
 * halted CPU run it — its own, or any idle one, which will steal it */
static void ready_wake(RunQueue *rq, Thread *t)
{
    rq_enqueue(rq, t);
    smp_kick(t->cpu);
}

/* The same for a thread woken by a timer (its deadline, or the timer it
 * waits on) that does not preempt only because a thread of higher
 * priority runs on its CPU just then (a kernel thread's few microseconds,
 * usually): it still goes first in the queue, so it runs once that thread
 * is done.  Queued last, it waited out the slice of each same-priority
 * thread ahead of it (sleeptest's 1 ms timer queue timer: 9 ms late at
 * the 95th percentile with a busy thread on every processor). */
static void ready_wake_timer(RunQueue *rq, Thread *t)
{
    rq_enqueue_front(rq, t);
    smp_kick(t->cpu);
}

/* A thread woken from a wait (sched_unblock) that should run before the
 * one running on its CPU: that CPU switches at its next interrupt, which
 * the waker sends it (IPI_WAKE, to itself too) — rather than at the end
 * of the running thread's 20 ms time slice.  Set under the CPU's queue
 * lock, cleared by the CPU's next switch. */
static volatile bool g_resched[MAX_CPUS];
/* The switch about to happen on a CPU is the one g_resched asked for, before
 * the running thread's slice ends (rq_enqueue_preempted) */
static bool g_preempting[MAX_CPUS];
/* The switch about to happen on a CPU ends the running thread's time slice
 * (sched_tick), not a yield or a wait */
static bool g_slice_end[MAX_CPUS];
/* (The preempted thread also keeps the ticks of its slice it has used,
 * Thread.preempted: given a new slice at each preemption, one preempted
 * often would never reach the end of one, and the threads queued behind
 * it would starve.  A thread that waited starts a new slice.) */

/* Threads above this priority are "foreground" (programs, the kernel's
 * service threads); the idle threads and csrss run only when none of those
 * is ready.  (Not the foreground process: sched_set_foreground.) */
#define BACKGROUND_PRIO 4

/* The next thread to run: the first in the queue (the highest priority) */
static Thread *rq_dequeue(RunQueue *rq)
{
    Thread *t = rq->head;
    if (t) rq_unlink(rq, t);
    return t;
}

/* -----------------------------------------------------------------------
 * Priority boosts (NT's, Windows Internals ch. 4 "Priority boosts")
 *
 * A thread woken from a wait runs at its base priority plus the waker's
 * increment (scheduler.h: +1 for an event, a semaphore, a mutex or an
 * alert, +2 for a window message, +6 for keyboard and mouse input...), up
 * to 15 and never for a real-time thread (16 and up); a boost never lowers
 * a priority already higher.  So a woken thread runs ahead of the busy
 * threads of its base priority, preempting the one on its CPU (wake_preempts),
 * where before it waited behind each of them for up to a 20 ms slice.  The
 * boost decays one level for each quantum (TICKS_PER_SLICE ticks) the
 * thread runs while boosted, waits in between included (sched_tick), back
 * to its base: a woken thread that turns busy soon takes turns with the
 * others again.
 *
 * The balance set (NT's balance set manager): once a second, a thread that
 * has been ready for STARVE_TICKS without running (higher-priority threads
 * kept the processor) is raised to 15 for one quantum, then drops back to
 * its base.  At most BALANCE_MAX threads a pass, as on NT.
 * ----------------------------------------------------------------------- */
#define STARVE_TICKS  300   /* 3 s */
#define BALANCE_MAX   16

/* The foreground process (sched_set_foreground) */
static void *volatile g_foreground;

void sched_set_foreground(void *um) { __atomic_store_n(&g_foreground, um, __ATOMIC_RELAXED); }
void *sched_foreground(void)        { return __atomic_load_n(&g_foreground, __ATOMIC_RELAXED); }

/* (@t waiting, its queue locked).  A thread of the foreground process gets
 * NT's foreground boost on top (PsPrioritySeparation; Windows Internals,
 * "Priority boosts for foreground threads after waits"). */
static void boost(Thread *t, int incr)
{
    if (incr <= 0 || t->idle || t->no_boost || t->base_priority >= PRIO_LOW_REALTIME) return;
    if (t->um_proc && t->um_proc == sched_foreground()) incr += BOOST_FOREGROUND;
    int p = t->base_priority + incr;
    if (p > PRIO_MAX_DYNAMIC) p = PRIO_MAX_DYNAMIC;
    if (p <= t->priority) return;
    t->priority = (uint8_t)p;
    t->boost_ticks = 0;
    t->balance_boost = false;
}

/* The tick's work (one CPU, interrupts off, no queue locked): boost the
 * threads that have waited too long in a queue */
static void balance_set(void)
{
    uint32_t done = 0;
    for (uint32_t c = 0; c < g_cpu_count && done < BALANCE_MAX; c++) {
        RunQueue *rq = &g_rq[c];
        if (!__atomic_load_n(&rq->head, __ATOMIC_RELAXED) || !spin_trylock(&rq->lock)) continue;
        Thread *starved[BALANCE_MAX];
        uint32_t n = 0;
        Thread *t = rq->head;
        for (size_t i = 0; i < rq->count && done + n < BALANCE_MAX; i++, t = t->next)
            if (t->priority < PRIO_MAX_DYNAMIC && tick_count - t->ready_tick >= STARVE_TICKS)
                starved[n++] = t;
        for (uint32_t i = 0; i < n; i++) {
            t = starved[i];
            rq_unlink(rq, t);
            t->priority = PRIO_MAX_DYNAMIC;
            t->boost_ticks = 0;
            t->balance_boost = true;
            rq_enqueue(rq, t);
        }
        done += n;
        spin_unlock(&rq->lock);
    }
}

/* A queued thread outranks @t (running here; the queue locked) */
static bool rq_outranks(RunQueue *rq, const Thread *t)
{
    return rq->head && rq->head->priority > t->priority;
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
    t->base_priority = priority;
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
    g_resched[kpcr->CpuNumber] = false;             /* (this switch is the one asked for) */
    bool preempted = g_preempting[kpcr->CpuNumber];
    g_preempting[kpcr->CpuNumber] = false;
    bool slice_end = g_slice_end[kpcr->CpuNumber];
    g_slice_end[kpcr->CpuNumber] = false;
    /* A thread whose slice is over gives way to the next thread of its
     * priority or a higher one, not to one of lower priority (boosted, it
     * runs on, as on NT) — but still to a background one (csrss gets its
     * turn, as it always did).  A yield gives way to any. */
    Thread *top = rq->head;
    bool keep = slice_end && prev->state == THREAD_RUNNING && !prev->idle && top &&
                top->priority > BACKGROUND_PRIO && top->priority < prev->priority;
    Thread *next = keep ? NULL : rq_dequeue(rq);
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
        if (!preempted) next->ticks_slice = 0;
        next->preempted   = false;
        next->woken       = false;
        spin_unlock(&rq->lock);
        return;
    }

    /* If current is still running, put it back on the ready queue (an
     * idle thread only ever runs as the fallback above) */
    if (prev->state == THREAD_RUNNING) {
        if (prev->idle) prev->state = THREAD_READY;
        else if (preempted) rq_enqueue_preempted(rq, prev);
        else rq_enqueue(rq, prev);
    }
    /* (a thread is queued only once switched out, under the lock of the
     * queue its CPU was switching from: this never waits in practice) */
    if (next->state != THREAD_READY && !next->idle) {   /* (only a queued thread is switched to) */
        kprintf("[SCHED] BUG: switching to thread '%s' (TID %lu) in state %d on CPU %u\n",
                next->name, next->tid, next->state, this_cpu());
        for (;;) { cli(); hlt(); }
    }
    while (__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE)) pause_cpu();
    next->on_cpu        = true;
    next->cpu           = this_cpu();
    next->state         = THREAD_RUNNING;
    if (!next->preempted) next->ticks_slice = 0;    /* (a preempted one goes on with its slice) */
    next->preempted     = false;
    next->woken         = false;
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

/* Whether a yield now would hand this CPU to a thread of lower priority
 * (only such threads are queued here): a kernel thread above programs that
 * wants to come back soon sleeps briefly instead, as a timed wake-up then
 * preempts that thread, where a yield leaves it its whole time slice */
bool sched_yield_goes_lower(void)
{
    IrqState irq = irq_save();
    Thread *h = __atomic_load_n(&my_rq()->head, __ATOMIC_RELAXED);
    bool lower = h && h->priority < current_thread->priority;
    irq_restore(irq);
    return lower;
}

/* -----------------------------------------------------------------------
 * sched_tick — called from timer interrupt handler (interrupts disabled)
 * ----------------------------------------------------------------------- */
static KSpinLock tick_lock = KSPINLOCK_INIT;
static bool wake_sleepers(RunQueue *rq, uint64_t *soonest);
void DesktopWatchdog(uint64_t now);
void UmTimerTick(uint64_t ticks);


/* Each CPU's timer is one-shot (apic.c): armed for its next tick on the
 * global 10 ms grid, or for its earliest TSC-deadline sleeper if that
 * comes sooner.  g_armed is what it was last armed for. */
static uint64_t g_next_tick[MAX_CPUS], g_armed[MAX_CPUS];

static void timer_arm(uint32_t cpu, uint64_t tsc)
{
    g_armed[cpu] = tsc;
    apic_timer_arm(tsc);
}

uint64_t sched_tick_tsc(uint64_t tick)
{
    return tsc_at_boot + tick * g_tsc_per_tick;
}

uint64_t sched_tsc_after(uint64_t t100ns)
{
    if (t100ns > UINT64_C(100000000000)) return UINT64_MAX;      /* (beyond about 3 hours) */
    return rdtsc() + t100ns * g_tsc_per_tick / 100000;
}

/* The next tick on the grid after @tsc */
static uint64_t next_grid_tick(uint64_t tsc)
{
    if (!g_tsc_per_tick || tsc < tsc_at_boot) return tsc + g_tsc_per_tick;
    return sched_tick_tsc((tsc - tsc_at_boot) / g_tsc_per_tick + 1);
}

static bool wake_preempts(const Thread *t, bool timer);

/* This CPU waits for the kernel lock (sched_timer_rearm, @rq locked): a
 * TSC-deadline sleeper that is due and does not hold the lock runs on
 * another CPU instead, as a timer wake (sched_unblock_timer) — else it
 * waits as long as the lock's holder keeps it, which writing drive C:
 * once did for seconds (fs/persist.c now writes without it), and the
 * device poll thread with it: keystrokes the PS/2 controller could not
 * hand over meanwhile were lost. */
static void hand_off_due(RunQueue *rq, uint32_t cpu, uint64_t tsc)
{
    for (Thread **pp = &rq->sleepers; *pp;) {
        Thread *t = *pp;
        if (!t->wake_tsc || t->wake_tsc > tsc || t->state != THREAD_WAITING || t->bkl_depth) {
            pp = &t->sleep_next;
            continue;
        }
        RunQueue *to = NULL;
        uint32_t c = cpu;
        for (uint32_t n = 1; n < g_cpu_count && !to; n++) {
            c = (cpu + n) % g_cpu_count;
            if (!g_kpcr[c].Online || g_kpcr[c].LockWait) continue;
            if (spin_trylock(&g_rq[c].lock)) to = &g_rq[c];   /* (trylock: no waiting on each other) */
        }
        if (!to) return;
        *pp = t->sleep_next;
        t->sleep_next = NULL;
        t->in_sleepers = false;
        __atomic_store_n(&t->cpu, c, __ATOMIC_RELEASE);
        boost(t, BOOST_TIMER);
        if (wake_preempts(t, true)) {
            rq_enqueue_front(to, t);
            if (!smp_kick(c)) {
                g_resched[c] = true;
                apic_send_ipi(g_kpcr[c].ApicId, APIC_IPI_FIXED | IPI_WAKE);
            }
        } else {
            ready_wake_timer(to, t);
        }
        spin_unlock(&to->lock);
    }
}

/* A timer interrupt taken while this CPU halts waiting for the kernel
 * lock (KPCR.LockWait): the waiting thread cannot be switched out, so no
 * sleeper is woken, but the one-shot timer is re-armed for the soonest
 * thing due, a TSC-deadline sleeper of this CPU included.  One already
 * due gets a short retry, so that the first interrupt after the wait ends
 * wakes it, not the next 10 ms grid tick. */
void sched_timer_rearm(void)
{
    uint32_t cpu = this_cpu();
    uint64_t tsc = rdtsc();
    if (g_next_tick[cpu] <= tsc) g_next_tick[cpu] = next_grid_tick(tsc);
    uint64_t at = g_next_tick[cpu];
    RunQueue *rq = my_rq();
    if (rq->sleepers) {
        uint64_t soonest = UINT64_MAX;
        if (spin_trylock(&rq->lock)) {
            hand_off_due(rq, cpu, tsc);
            for (Thread *t = rq->sleepers; t; t = t->sleep_next)
                if (t->wake_tsc && t->state == THREAD_WAITING && t->wake_tsc < soonest)
                    soonest = t->wake_tsc;
            spin_unlock(&rq->lock);
        } else {
            soonest = tsc;                          /* (the list is changing: look again soon) */
        }
        uint64_t retry = tsc + (g_tsc_per_tick ? g_tsc_per_tick / 200 : 1);   /* ~50 us */
        if (soonest <= tsc) soonest = retry;
        if (soonest < at) at = soonest;
    }
    timer_arm(cpu, at);
}

void sched_tick(void)
{
    /* The global tick follows the TSC, so it neither runs N times too fast
     * with N CPUs nor loses time while CPU 0 waits for the kernel lock
     * with interrupts off. */
    uint32_t cpu = this_cpu();
    uint64_t tsc = rdtsc();
    /* This CPU's own tick: due, or the TSC moved back under it */
    bool tick = tsc >= g_next_tick[cpu] || g_next_tick[cpu] > tsc + 2 * g_tsc_per_tick;
    if (tick) g_next_tick[cpu] = next_grid_tick(tsc);
    uint64_t now = g_tsc_per_tick ? (tsc - tsc_at_boot) / g_tsc_per_tick : tick_count + 1;
    if (now > tick_count && spin_trylock(&tick_lock)) {   /* one CPU does the tick's work */
        if (now > tick_count) {
            tick_count = now;
            /* (The keyboard and mouse polls, ps2_poll and UsbPoll, run in
             * the device poll thread, kernel/ke/main.c: their port and
             * MMIO reads take QEMU's device lock and ran up to a few ms
             * here with interrupts off, which held up every Sleep and
             * wait timeout due on this CPU meanwhile.) */
            DesktopWatchdog(tick_count);
            UmTimerTick(tick_count);
            static uint64_t last_balance;
            if (tick_count - last_balance >= 100) {     /* once a second */
                last_balance = tick_count;
                balance_set();
            }
        }
        spin_unlock(&tick_lock);
    }
    RunQueue *rq = my_rq();                 /* each CPU wakes its own sleepers */
    uint64_t soonest = UINT64_MAX;
    bool preempt = rq->sleepers ? wake_sleepers(rq, &soonest) : false;
    timer_arm(cpu, soonest < g_next_tick[cpu] ? soonest : g_next_tick[cpu]);
    if (!current_thread) return;

    Thread *cur = current_thread;
    bool decayed = false;
    if (tick) {
        cur->ticks_total++;
        cur->ticks_slice++;
        /* A boost decays a level per quantum run (see boost) */
        if (cur->priority > cur->base_priority && !cur->idle && ++cur->boost_ticks >= TICKS_PER_SLICE) {
            cur->boost_ticks = 0;
            cur->priority = cur->balance_boost ? cur->base_priority : cur->priority - 1;
            cur->balance_boost = false;
            decayed = true;
        }
    }

    /* An idle CPU looks for work waiting on the others at every tick */
    bool steal_now = false;
    if (tick && current_thread->idle)
        for (uint32_t c = 0; c < g_cpu_count && !steal_now; c++)
            steal_now = __atomic_load_n(&g_rq[c].head, __ATOMIC_RELAXED) != NULL;
    /* A sleeper that is due runs now, or a thread woken for this CPU
     * (sched_unblock); or the time slice expired.  The thread preempted
     * for either keeps its place ahead of those waiting their turn
     * (rq_enqueue_preempted): sent to the back, it waited out their
     * slices, in the middle of starting a 1 ms wait of its own. */
    bool slice_over = cur->ticks_slice >= TICKS_PER_SLICE;
    /* Its boost decayed below a queued thread's priority: that one runs */
    if (decayed && !slice_over) {
        spin_lock(&rq->lock);
        preempt |= rq_outranks(rq, cur);
        spin_unlock(&rq->lock);
    }
    if ((g_resched[cpu] || preempt) && !slice_over) g_preempting[cpu] = true;
    g_slice_end[cpu] = slice_over;
    if (preempt || steal_now || g_resched[cpu] || slice_over)
        perform_switch();
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
/* Take @t (the current thread, interrupts off, no run queue lock held)
 * off the timed-sleep list it is still on after an early wake-up */
static void leave_sleepers(Thread *t)
{
    if (!__atomic_load_n(&t->in_sleepers, __ATOMIC_ACQUIRE)) return;   /* (only t itself sets it) */
    RunQueue *o = &g_rq[t->sleep_cpu];
    spin_lock(&o->lock);
    if (t->in_sleepers) rq_drop_sleeper(o, t);
    spin_unlock(&o->lock);
}

void sched_sleep_until(volatile uint32_t *flag, uint64_t deadline)
{
    IrqState irq = irq_save();
    Thread *t = current_thread;
    /* Still on another CPU's sleep list (woken early there, then moved
     * here): leave it.  Left there, that CPU's next tick would drop it
     * while this one thought it already queued, and the sleep would never
     * end. */
    if (t->sleep_cpu != this_cpu()) leave_sleepers(t);
    RunQueue *rq = my_rq();
    spin_lock(&rq->lock);
    if ((flag && *flag) || tick_count >= deadline) {  /* woken already, or due */
        spin_unlock_irqrestore(&rq->lock, irq);
        return;
    }
    t->state = THREAD_WAITING;
    t->wake_tick = deadline;
    t->wake_tsc = 0;
    if (!t->in_sleepers) {                          /* (still there from a wake-up by sched_unblock) */
        t->sleep_next = rq->sleepers;
        rq->sleepers = t;
        t->in_sleepers = true;
        t->sleep_cpu = this_cpu();
    }
    switch_locked(rq);                              /* woken by sched_unblock or sched_tick */
    irq_restore(irq);
}

void sched_sleep_until_tsc(volatile uint32_t *flag, uint64_t tsc)
{
    IrqState irq = irq_save();
    if (current_thread->sleep_cpu != this_cpu()) leave_sleepers(current_thread);   /* (see sched_sleep_until) */
    RunQueue *rq = my_rq();
    spin_lock(&rq->lock);
    uint64_t now = rdtsc();
    if ((flag && *flag) || now >= tsc) {            /* woken already, or due */
        spin_unlock_irqrestore(&rq->lock, irq);
        return;
    }
    Thread *t = current_thread;
    t->state = THREAD_WAITING;
    t->wake_tick = UINT64_MAX;
    t->wake_tsc = tsc;
    if (!t->in_sleepers) {
        t->sleep_next = rq->sleepers;
        rq->sleepers = t;
        t->in_sleepers = true;
        t->sleep_cpu = this_cpu();
    }
    /* Sooner than this CPU's timer is armed for: arm it for this.  Never
     * later: a deadline that has gone by may not have fired yet (the
     * one-shot count runs a little off the TSC, and a virtual timer fires
     * late), and arming over it would leave its sleeper until the next
     * 10 ms tick.  Armed for it again, it fires at once (as it does when
     * it was armed before a restart and the timer has stopped since), and
     * sched_tick arms the timer for what is due next, this sleep too. */
    uint32_t cpu = this_cpu();
    if (tsc < g_armed[cpu]) timer_arm(cpu, tsc);
    else if (g_armed[cpu] <= now) timer_arm(cpu, g_armed[cpu]);
    switch_locked(rq);
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

/* Timer interrupt (interrupts off): sleepers whose deadline has come are
 * ready again.  One woken by its TSC deadline goes to the front of the
 * queue, and the result says to switch to it now, when it has at least the
 * current thread's priority.  *soonest: the earliest TSC deadline left. */
static bool wake_sleepers(RunQueue *rq, uint64_t *soonest)
{
    bool preempt = false;
    Thread *cur = current_thread;
    uint64_t tsc = rdtsc();
    spin_lock(&rq->lock);
    for (Thread **pp = &rq->sleepers; *pp;) {
        Thread *t = *pp;
        bool due = t->wake_tick <= tick_count || (t->wake_tsc && t->wake_tsc <= tsc);
        if (due || t->state != THREAD_WAITING) {    /* due, or woken some other way */
            *pp = t->sleep_next;
            t->sleep_next = NULL;
            t->in_sleepers = false;
            if (!due || t->state != THREAD_WAITING) continue;
            if (t->wake_tsc) boost(t, BOOST_TIMER);
            if (t->wake_tsc && cur && t->priority >= cur->priority) {
                rq_enqueue_front(rq, t);
                preempt = true;
            } else if (t->wake_tsc) {
                ready_wake_timer(rq, t);
            } else {
                ready_wake(rq, t);
            }
        } else {
            if (t->wake_tsc && t->wake_tsc < *soonest) *soonest = t->wake_tsc;
            pp = &t->sleep_next;
        }
    }
    spin_unlock(&rq->lock);
    return preempt;
}

void sched_block(void)
{
    IrqState irq = irq_save();
    /* Still on a sleep list from an earlier timed sleep (woken early):
     * leave it, or that CPU's tick would find this thread waiting and
     * queue it there while a sched_unblock queued it here as well */
    leave_sleepers(current_thread);
    RunQueue *rq = my_rq();
    spin_lock(&rq->lock);
    current_thread->state = THREAD_WAITING;
    switch_locked(rq);
    irq_restore(irq);
}

/* Whether @t, woken in its CPU's queue (locked), should preempt the thread
 * running there: one of higher priority does (a boosted one, usually: an
 * event set, a message posted), and so does one of the same priority woken
 * by a timer (@timer: the timer it waits on was set or went off), as when
 * its own deadline wakes it (wake_sleepers).  Other wakes of the same
 * priority (no boost: a kernel wait queue) don't, and are queued after the
 * threads of their priority.  An idle CPU needs no preempting (smp_kick). */
static bool wake_preempts(const Thread *t, bool timer)
{
    PKPCR k = &g_kpcr[t->cpu];
    Thread *cur = (Thread *)__atomic_load_n(&k->CurrentThread, __ATOMIC_RELAXED);
    if (!k->Online || !cur || cur == t || cur->idle) return false;
    return t->priority > cur->priority || (timer && t->priority >= cur->priority);
}

static void unblock(Thread *t, bool timer, int incr)
{
    IrqState irq;
    RunQueue *rq = lock_thread_rq(t, &irq);
    if (t->state == THREAD_WAITING) {
        boost(t, incr);
        if (wake_preempts(t, timer)) {
            /* First in its CPU's queue: a halted CPU takes it if there is
             * one, else its own CPU switches to it at the IPI */
            rq_enqueue_front(rq, t);
            if (!smp_kick(t->cpu)) {
                g_resched[t->cpu] = true;
                apic_send_ipi(g_kpcr[t->cpu].ApicId, APIC_IPI_FIXED | IPI_WAKE);
            }
        } else if (timer) {
            ready_wake_timer(rq, t);
        } else {
            ready_wake(rq, t);
        }
    }
    spin_unlock_irqrestore(&rq->lock, irq);
}

void sched_unblock(Thread *t)                 { unblock(t, false, BOOST_NONE); }
void sched_unblock_timer(Thread *t)           { unblock(t, true, BOOST_TIMER); }
void sched_unblock_boost(Thread *t, int boost) { unblock(t, false, boost); }

/* A new base priority for @t (SetThreadPriority, SetPriorityClass): it runs
 * at the new base from now on, as on NT, where a priority change ends a
 * boost.  Queued, it moves to its new place; running, it gives way at once
 * to a queued thread that now outranks it, and a queued thread raised above
 * the one running on its CPU preempts it. */
void sched_set_base_priority(Thread *t, uint8_t base)
{
    if (base < 1) base = 1;
    if (base > 31) base = 31;
    IrqState irq;
    RunQueue *rq = lock_thread_rq(t, &irq);
    if (t->base_priority != base && t->state != THREAD_DEAD && !t->idle) {
        bool queued = t->next != NULL;
        uint64_t since = t->ready_tick;
        if (queued) rq_unlink(rq, t);
        t->base_priority = base;
        t->priority = base;
        t->boost_ticks = 0;
        t->balance_boost = false;
        PKPCR k = &g_kpcr[t->cpu];
        Thread *cur = (Thread *)__atomic_load_n(&k->CurrentThread, __ATOMIC_RELAXED);
        bool resched = false;
        if (queued) {
            rq_link(rq, t, rq_place(rq, t, AT_TAIL));
            t->ready_tick = since;                  /* (the balance set's clock goes on) */
            resched = k->Online && cur && cur != t && !cur->idle && t->priority > cur->priority;
        } else if (cur == t && t->state == THREAD_RUNNING) {
            resched = rq_outranks(rq, t);
        }
        if (resched) {
            g_resched[t->cpu] = true;
            apic_send_ipi(k->ApicId, APIC_IPI_FIXED | IPI_WAKE);
        }
    }
    spin_unlock_irqrestore(&rq->lock, irq);
}

void sched_resched_ipi(void)
{
    uint32_t cpu = this_cpu();
    if (!g_resched[cpu] || !current_thread) return;
    g_preempting[cpu] = current_thread->ticks_slice < TICKS_PER_SLICE;
    perform_switch();
}

void sched_resched_pending(void)
{
    if (!g_resched[this_cpu()]) return;             /* (a hint: looked at again below) */
    IrqState irq = irq_save();
    sched_resched_ipi();
    irq_restore(irq);
}

bool sched_work_waiting(void)
{
    if (g_resched[this_cpu()]) return true;
    for (uint32_t c = 0; c < g_cpu_count; c++)      /* (an idle CPU steals: switch_locked) */
        if (__atomic_load_n(&g_rq[c].head, __ATOMIC_RELAXED)) return true;
    return false;
}

/* -----------------------------------------------------------------------
 * Thread exit and reclamation
 * ----------------------------------------------------------------------- */
void sched_exit_current(void)
{
    cli();
    /* A thread woken early is still on a sleep list: it must not be once
     * freed (the list would run through freed memory and lose the
     * sleepers after it) */
    leave_sleepers(current_thread);
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

void (*sched_thread_free_hook)(Thread *t);

void sched_free_thread(Thread *t)
{
    if (!sched_thread_gone(t) || t == current_thread) return;
    if (sched_thread_free_hook) sched_thread_free_hook(t);
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
                kprintf("  TID=%lu '%s' state=%d prio=%u base=%u\n",
                        t->tid, t->name, t->state, t->priority, t->base_priority);
                t = t->next;
            } while (t != rq->head);
        }
        spin_unlock_irqrestore(&rq->lock, irq);
    }
}
