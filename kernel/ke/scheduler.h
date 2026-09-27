/*
 * scheduler.h — Phase 1 kernel scheduler (round-robin)
 *
 * This is the minimal scheduler needed for Phase 1: a simple round-robin
 * preemptive scheduler driven by the APIC timer (100 Hz).
 *
 * Design:
 *   - Threads are the schedulable unit (matching NT's design where the
 *     scheduler operates on KTHREADs, not processes)
 *   - Each thread has a kernel stack, saved registers, and a state
 *   - The ready queue is a circular doubly-linked list (O(1) add/remove)
 *   - Time quantum: 10ms (one APIC timer tick)
 *   - No priorities in Phase 1 (Phase 2 adds NT-style priority levels)
 *
 * NT-compatibility notes:
 *   The NT scheduler uses a 32-level priority system (0–31) with
 *   real-time priorities (16–31) and dynamic priorities (0–15).
 *   We'll add this in Phase 2.  The data structures below are designed
 *   to evolve into full KTHREAD/KPROCESS structs.
 */

#pragma once

#include "../include/types.h"

/* Thread states */
typedef enum {
    THREAD_RUNNING   = 0,   /* Currently executing */
    THREAD_READY     = 1,   /* In ready queue, waiting for CPU */
    THREAD_WAITING   = 2,   /* Blocked on an event/wait */
    THREAD_SUSPENDED = 3,   /* Suspended (not eligible for scheduling) */
    THREAD_DEAD      = 4,   /* Terminated, waiting for cleanup */
} ThreadState;

/* Maximum thread name length */
#define THREAD_NAME_MAX 32

/* Saved register context for context switches.
 * We only save callee-saved registers (the System V AMD64 ABI specifies
 * that rbx, rbp, r12-r15 are callee-saved; the caller saves the rest).
 * The switch point is the call to scheduler_switch(), so callee-saved
 * registers must be preserved across it.
 *
 * This struct must match the assembly in scheduler_switch_asm.
 */
typedef struct ThreadContext {
    uint64_t rsp;   /* Stack pointer (saved/restored last/first) */
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rflags;
} ThreadContext;

/* Kernel Thread (KTHREAD in NT parlance) */
typedef struct Thread {
    /* Scheduler linkage (must be first for list manipulation) */
    struct Thread  *next;
    struct Thread  *prev;

    /* Identity */
    uint64_t        tid;            /* Thread ID (unique, monotonically increasing) */
    char            name[THREAD_NAME_MAX];

    /* Execution state */
    ThreadState     state;
    ThreadContext   context;        /* Saved register context */

    /* Stack */
    void           *kernel_stack;  /* Allocated kernel stack (bottom) */
    size_t          stack_size;

    /* Timing */
    uint64_t        ticks_total;   /* Total ticks consumed */
    uint64_t        ticks_slice;   /* Ticks used in current time slice */

    /* Scheduling priority (0 = lowest, 31 = highest in NT model) */
    uint8_t         priority;

    /* Page table root (CR3 physical address) for this thread's process.
     * 0 = kernel thread (no CR3 switch needed).
     * Set before the first user-mode entry in PsUserThreadEntry. */
    uint64_t        cr3;

    /* Future: pointer to owning KPROCESS */
    void           *process;
} Thread;

/* Default kernel stack size for new threads */
#define THREAD_STACK_SIZE (16 * 1024)   /* 16 KiB */

/*
 * Initialize the scheduler.
 * Creates the idle thread from the current execution context.
 * Must be called after PMM and VMM are initialized.
 */
void sched_init(void);

/*
 * Create a new kernel thread.
 *
 * @name:     thread name (for debugging)
 * @entry:    thread entry function
 * @arg:      argument passed to entry function
 * @priority: scheduling priority (0–31; use 8 for normal kernel threads)
 *
 * Returns a pointer to the new Thread, or NULL on OOM.
 * The thread is placed in the READY state immediately.
 */
typedef void (*ThreadEntry)(void *arg);
Thread *sched_create_thread(const char *name, ThreadEntry entry,
                            void *arg, uint8_t priority);
/* Same, with a kernel stack of @stack_size bytes (rounded up to pages). */
Thread *sched_create_thread_ex(const char *name, ThreadEntry entry,
                               void *arg, uint8_t priority, size_t stack_size);

/*
 * Yield the current thread's remaining time slice voluntarily.
 * This is equivalent to NT's NtYieldExecution().
 */
void sched_yield(void);

/*
 * Called from the APIC timer interrupt (IRQ_TIMER).
 * Increments tick counter; performs preemptive switch if time slice expired.
 * Must be called with interrupts DISABLED (we're in an IRQ handler).
 */
void sched_tick(void);

/* Timer ticks since boot (100 Hz, i.e. 10 ms each) */
uint64_t sched_ticks(void);

/*
 * Get the currently executing thread.
 */
Thread *sched_current(void);

/*
 * Block the current thread (move to WAITING state).
 * Used by synchronization primitives.
 */
void sched_block(void);

/*
 * Unblock a thread (move from WAITING to READY state).
 */
void sched_unblock(Thread *t);

/*
 * Print scheduler state to the debug console (for diagnostics).
 */
void sched_dump(void);

/*
 * Add a pre-initialized Thread to the scheduler's ready queue.
 * Used by PsCreateSystemThread to enqueue ETHREAD-embedded threads
 * that were set up directly (bypassing sched_create_thread).
 * The Thread must be fully initialized (tid, stack, context, etc.).
 */
void sched_enqueue_thread(Thread *t);
