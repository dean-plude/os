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
#include "../mm/vmm.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/paging.h"

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */

/* The currently running thread (per-CPU; we have one CPU in Phase 1) */
static Thread *current_thread;

/* Ready queue (circular doubly-linked list, sentineled by idle_thread) */
static Thread *ready_head;   /* Points to the thread to run next */
static size_t  ready_count;

/* Idle thread — runs when nothing else is ready */
static Thread  idle_thread_obj;

/* Monotonically increasing thread ID */
static uint64_t next_tid = 1;

/* Global tick counter (incremented by sched_tick) */
static volatile uint64_t tick_count;

/* Ticks per time slice before preemption */
#define TICKS_PER_SLICE  2   /* 2 ticks @ 100Hz = 20ms quantum */

/* Spinlock protecting the scheduler data structures */
typedef struct { volatile uint32_t n, o; } SchedLock;
static SchedLock sched_lock;

static void sched_lock_acquire(void)
{
    uint32_t t = __atomic_fetch_add(&sched_lock.n, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&sched_lock.o, __ATOMIC_ACQUIRE) != t)
        pause_cpu();
}
static void sched_lock_release(void)
{
    __atomic_fetch_add(&sched_lock.o, 1, __ATOMIC_RELEASE);
}

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
    IrqState irq = irq_save();
    bool any = false;
    if (ready_head) {
        Thread *c = ready_head;
        do { if (c->priority > BACKGROUND_PRIO) { any = true; break; } c = c->next; } while (c != ready_head);
    }
    irq_restore(irq);
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

    entry(arg);

    /* Thread returned — mark it dead and yield forever */
    kprintf("[SCHED] Thread '%s' (TID %lu) exited\n",
            current_thread->name, current_thread->tid);
    current_thread->state = THREAD_DEAD;
    for (;;) sched_yield();
}

/* -----------------------------------------------------------------------
 * Idle thread
 * ----------------------------------------------------------------------- */
static void idle_thread_fn(void *arg)
{
    (void)arg;
    for (;;) {
        /* HLT saves power; interrupts wake us up for the next tick */
        sti();
        hlt();
        cli();
    }
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
    __builtin_memcpy(idle->name, "idle", 5);
    /* idle's stack is the current boot stack — we don't track it */

    current_thread = idle;
    ready_head     = NULL;
    ready_count    = 0;

    kprintf("[SCHED] Scheduler initialized (idle TID=%lu)\n", idle->tid);

    /* Create the idle thread as a proper schedulable thread so the
     * scheduler always has something to switch to */
    Thread *idle2 = sched_create_thread("idle", idle_thread_fn, NULL, 0);
    if (!idle2) {
        kprintf("[SCHED] WARNING: could not create idle thread\n");
    }
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
     *   context.rflags = IF (interrupts enabled)
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
    t->context.rflags = 0x202;   /* IF=1, reserved bit 1 */

    /* Fill in metadata */
    t->tid      = next_tid++;
    t->priority = priority;
    t->state    = THREAD_READY;

    size_t name_len = 0;
    while (name[name_len] && name_len < THREAD_NAME_MAX - 1) name_len++;
    __builtin_memcpy(t->name, name, name_len);
    t->name[name_len] = '\0';

    /* Add to ready queue */
    IrqState irq = irq_save();
    sched_lock_acquire();
    ready_enqueue(t);
    sched_lock_release();
    irq_restore(irq);

    kprintf("[SCHED] Created thread '%s' TID=%lu stack=%p\n",
            t->name, t->tid, t->kernel_stack);
    return t;
}

/* -----------------------------------------------------------------------
 * perform_switch — pick the next thread and switch to it
 * (called with interrupts disabled, sched_lock NOT held)
 * ----------------------------------------------------------------------- */
static void perform_switch(void)
{
    sched_lock_acquire();

    Thread *prev = current_thread;
    Thread *next = ready_dequeue();
    if (!next) {
        /* No other ready thread.  Keep running the current one if it can;
         * only fall back to the boot context when the current thread has
         * blocked or exited (the real idle thread is normally queued). */
        next = (prev->state == THREAD_RUNNING) ? prev : &idle_thread_obj;
    }

    /* If current is still running, put it back on the ready queue */
    if (prev != next) {
        if (prev->state == THREAD_RUNNING) {
            ready_enqueue(prev);
        }
        next->state         = THREAD_RUNNING;
        next->ticks_slice   = 0;
        current_thread      = next;

        /* Update TSS RSP0 to new thread's kernel stack top
         * (used when this thread returns to ring 3 in the future) */
        if (next->kernel_stack) {   /* boot context has no tracked stack */
            uintptr_t kstack_top = (uintptr_t)next->kernel_stack + next->stack_size;
            gdt_set_rsp0(kstack_top);

            /* Phase 5: Update KPCR.KernelRsp so syscall_entry.asm picks up the
             * correct kernel stack when this thread makes a system call. */
            PKPCR kpcr = KiGetCurrentKpcr();
            if (kpcr) kpcr->KernelRsp = (UINT64)kstack_top;
        }

        /* Address space: a user process's page table, or the kernel's for
         * kernel threads (never keep running on a table that may be freed
         * once its process exits). */
        uint64_t cr3 = next->cr3 ? next->cr3 : paging_get_kernel_cr3();
        if (read_cr3() != cr3) paging_load_cr3((uintptr_t)cr3);

        /* User threads: SSE state and the two GS bases (TEB / KPCR) are
         * per thread.  Kernel code never uses SSE or GS, so kernel threads
         * need neither saved. */
        if (prev->um) {
            __asm__ volatile ("fxsave64 (%0)" : : "r"(prev->fpu) : "memory");
            prev->gs_base  = rdmsr(MSR_IA32_GSBASE);
            prev->kgs_base = rdmsr(MSR_IA32_KERNEL_GSBASE);
        }
        if (next->um) {
            __asm__ volatile ("fxrstor64 (%0)" : : "r"(next->fpu) : "memory");
            wrmsr(MSR_IA32_GSBASE, next->gs_base);
            wrmsr(MSR_IA32_KERNEL_GSBASE, next->kgs_base);
        }
        if (prev->state == THREAD_DEAD) prev->off_cpu = true;   /* never runs again */

        sched_lock_release();

        /* Perform the actual register swap */
        context_switch(&prev->context, &next->context);
        /* After return, we're running as 'next' */
    } else {
        /* Re-selected ourselves (possibly after being made READY by
         * sched_unblock while still on-CPU): we are running again. */
        next->state       = THREAD_RUNNING;
        next->ticks_slice = 0;
        sched_lock_release();
    }
}

/* -----------------------------------------------------------------------
 * sched_yield
 * ----------------------------------------------------------------------- */
void sched_yield(void)
{
    IrqState irq = irq_save();
    perform_switch();
    irq_restore(irq);
}

/* -----------------------------------------------------------------------
 * sched_tick — called from timer interrupt handler (interrupts disabled)
 * ----------------------------------------------------------------------- */
void DesktopWatchdog(uint64_t now);
void UmTimerTick(uint64_t ticks);

void sched_tick(void)
{
    tick_count++;
    DesktopWatchdog(tick_count);
    UmTimerTick(tick_count);
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
void sched_block(void)
{
    IrqState irq = irq_save();
    sched_lock_acquire();
    current_thread->state = THREAD_WAITING;
    sched_lock_release();
    irq_restore(irq);
    sched_yield();
}

void sched_unblock(Thread *t)
{
    IrqState irq = irq_save();
    sched_lock_acquire();
    if (t->state == THREAD_WAITING) {
        ready_enqueue(t);
    }
    sched_lock_release();
    irq_restore(irq);
}

/* -----------------------------------------------------------------------
 * Thread exit and reclamation
 * ----------------------------------------------------------------------- */
void sched_exit_current(void)
{
    cli();
    current_thread->state = THREAD_DEAD;
    for (;;) sched_yield();
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
    IrqState irq = irq_save();
    sched_lock_acquire();
    ready_enqueue(t);
    sched_lock_release();
    irq_restore(irq);
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
