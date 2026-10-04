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

    /* Scheduling priority (0 = lowest, 31 = highest in NT model): the
     * current (dynamic) one, which a wake-up raises above the base by the
     * waker's increment (sched_unblock_boost) and which decays back to the
     * base one level per quantum the thread runs (see scheduler.c) */
    uint8_t         priority;
    uint8_t         base_priority;
    uint8_t         boost_ticks;    /* ticks run while boosted since the last decay step */
    bool            balance_boost;  /* raised for starving (sched_tick): back to base after a quantum */
    bool            no_boost;       /* no wake-up boosts (SetThreadPriorityBoost) */
    uint64_t        ready_tick;     /* when it was last queued (the balance-set boost) */
    /* The Multimedia Class Scheduler (sched_set_mmcss): the real-time
     * priority it runs at (0: not registered), its own base for when it
     * has used up its share, the ticks it was found running in the
     * current period and when that began */
    uint8_t         mm_priority;
    uint8_t         mm_base;
    uint8_t         mm_ran;
    bool            mm_exhausted;
    uint64_t        mm_period;

    /* Page table root (CR3 physical address) for this thread's process.
     * 0 = kernel thread (no CR3 switch needed).
     * Set before the first user-mode entry in PsUserThreadEntry. */
    uint64_t        cr3;

    /* Future: pointer to owning KPROCESS */
    void           *process;

    /* The ETHREAD this thread runs as (kernel/ps): the ETHREAD it is
     * embedded in (PsCreateSystemThread), one Ps made for it on first use
     * (PsGetCurrentThread), or NULL before that.  Never cast a Thread to
     * an ETHREAD: most are not embedded in one. */
    void           *ethread;

    /* User-mode threads (kernel/um): owning process and the state the
     * scheduler swaps for them.  NULL/0 for kernel threads. */
    void           *um;             /* UmThread */
    void           *um_proc;        /* its UmProcess (sched_set_foreground compares it) */
    uint8_t        *fpu;            /* 512-byte FXSAVE area, 16-byte aligned */
    uint64_t        gs_base;        /* user GS (the TEB): MSR_KERNEL_GS_BASE while in the kernel */
    uint64_t        fs_base;        /* user FS: the 32-bit TEB of a 32-bit program's thread */
    uint64_t        user_rsp;       /* user RSP at the last syscall (stack args) */
    volatile bool   off_cpu;        /* DEAD and switched away: safe to free */
    struct Thread  *sleep_next;     /* sched_sleep_tick list */
    uint64_t        wake_tick;
    uint64_t        wake_tsc;       /* sched_sleep_until_tsc: the TSC deadline (0 none) */
    uint32_t        wait_rounds;    /* sched_wait calls since the thread last made progress */
    bool            idle;           /* a CPU's idle thread: never queued, runs only there */
    uint32_t        bkl_depth;      /* nested bkl_acquire calls (smp.h) */
    bool            in_sleepers;    /* on its CPU's timed-sleep list (run queue lock) */
    uint32_t        sleep_cpu;      /* whose timed-sleep list that is */
    volatile uint32_t cpu;          /* the CPU whose run queue it belongs to */
    volatile bool   on_cpu;         /* running, or not yet fully switched out */
    bool            preempted;      /* preempted for a woken thread: goes on with its slice */
    bool            woken;          /* queued first as woken from a wait (until it runs) */
} Thread;

/* Priorities: 1-15 are dynamic (boosted on wake, never above 15),
 * 16-31 real-time (never boosted, as on NT) */
#define PRIO_MAX_DYNAMIC   15
#define PRIO_LOW_REALTIME  16

/* The system threads programs must never hold up: above anything a
 * program can ask for without SeIncreaseBasePriorityPrivilege (15:
 * THREAD_PRIORITY_TIME_CRITICAL, or HIGH_PRIORITY_CLASS + HIGHEST) and
 * any boost, as Windows runs its system threads in the real-time range.
 * Among them, the shorter and more urgent the work, the higher:
 *   19  the device poll thread (keyboards, mice, touch) and the audio
 *       mixer: a few microseconds each tick, and a key press or a sound
 *       buffer must never wait;
 *   18  the network stack (receive, lwIP's timers) and the USB thread
 *       (ports, hubs, plugging devices in): device work programs wait on;
 *   17  the desktop (input, window management, drawing; 50-100 ms a
 *       redraw without KVM) and starting programs (under its lock);
 *   16  bulk work: saving drive C: (writeback), ACPI (batteries, thermal
 *       zones, buttons; AML can run long), Setup copying NovaOS to a disk.
 * Every one blocks or sleeps when it has nothing to do (they never spin),
 * and gives way to any thread when it waits for a device (sched_yield).
 * Kernel threads at 8 or below are only boot-time tests, and csrss's
 * stub, which never runs. */
#define PRIO_SERVICE       16
#define PRIO_DESKTOP       17
#define PRIO_DEVICE_IO     18
#define PRIO_DEVICE        19

/* The Multimedia Class Scheduler (avrt.dll's AvSetMmThreadCharacteristics,
 * sched_set_mmcss): a program's audio thread runs in the real-time range
 * without SeIncreaseBasePriorityPrivilege, as MMCSS lifts it on Windows,
 * above every dynamic thread however boosted (a foreground program's
 * window threads reach 15), so a busy or freshly woken program never holds
 * up its sound.  Audio tasks ("Pro Audio", "Audio", "Capture", "Playback",
 * "Low Latency") run at 18: above the desktop (17), with the network and
 * USB threads (18), below device polling and the mixer (19), which feed
 * them.  Other tasks ("Games", "Distribution", "Window Manager"...) and
 * audio threads at AVRT_PRIORITY_LOW run at 16, below the desktop.  As on
 * Windows (SystemResponsiveness 20), such a thread may use at most 80% of
 * a processor: found running MMCSS_BUDGET_TICKS times in a period of
 * MMCSS_PERIOD_TICKS, it drops to its own base until the period ends, so
 * a program spinning in one cannot freeze the desktop. */
#define PRIO_MMCSS         16
#define PRIO_MMCSS_AUDIO   18
#define MMCSS_PERIOD_TICKS 10       /* 100 ms */
#define MMCSS_BUDGET_TICKS 8

/* NT's foreground boost (PsPrioritySeparation, 2 on client Windows): a
 * thread of the process owning the active window, woken from a wait, gets
 * this on top of the waker's increment (still never above 15; NT adds it
 * the same way, KiDeferredReadyThread in ReactOS) */
#define BOOST_FOREGROUND   2

/* Wake-up boosts: how far above its base a thread woken by each kind of
 * waker runs (NT's increments from wdm.h and Windows Internals) */
#define BOOST_NONE       0
#define BOOST_EVENT      1      /* EVENT_INCREMENT: events, alerts (SRW locks, condition variables, APCs) */
#define BOOST_TIMER      1      /* a timed wait's deadline (Sleep, a timeout) or a waitable timer: keeps
                                   such a wake ahead of a thread an event just boosted */
#define BOOST_SEMAPHORE  1      /* SEMAPHORE_INCREMENT */
#define BOOST_MUTANT     1      /* a released mutex */
#define BOOST_DISK       1      /* IO_DISK: file I/O completed */
#define BOOST_GUI        2      /* a window or thread message, keyboard and mouse input to a window
                                   included (win32k's windowing boost: the +6 below is what a driver
                                   gives the thread reading the device, NovaOS's device poll thread) */
#define BOOST_NAMED_PIPE 2      /* IO_NAMED_PIPE */
#define BOOST_NETWORK    2      /* IO_NETWORK */
#define BOOST_KEYBOARD   6      /* IO_KEYBOARD: keyboard input */
#define BOOST_MOUSE      6      /* IO_MOUSE: mouse input */
#define BOOST_SOUND      8      /* IO_SOUND */

/* Default kernel stack size for new threads */
#define THREAD_STACK_SIZE (16 * 1024)   /* 16 KiB */

/*
 * Initialize the scheduler.
 * Creates the idle thread from the current execution context.
 * Must be called after PMM and VMM are initialized.
 */
void sched_init(void);
/* Another CPU's idle thread, which will run on @stack (made by CPU 0) */
Thread *sched_new_idle_thread(uint32_t cpu, void *stack, size_t stack_size);
/* Make @idle, the calling CPU's current context, its idle thread. */
void sched_init_cpu(Thread *idle);
/* First call of a thread started other than through sched_create_thread
 * (it begins inside the switch to it; see scheduler.c). */
void sched_thread_start(void);

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
/* The same in two steps, for a thread that needs more set up before it can
 * run (on another CPU, the moment it is queued): create it, then start it. */
Thread *sched_new_thread(const char *name, ThreadEntry entry,
                         void *arg, uint8_t priority, size_t stack_size);
void sched_start_thread(Thread *t);

/* End the current thread (never returns).  Its stack and Thread are
 * reclaimed later by sched_free_thread() once sched_thread_gone(). */
void sched_exit_current(void) __attribute__((noreturn));
bool sched_thread_gone(const Thread *t);
void sched_free_thread(Thread *t);
/* Called by sched_free_thread before @t goes (Ps drops its ETHREAD) */
extern void (*sched_thread_free_hook)(Thread *t);

/*
 * Yield the current thread's remaining time slice voluntarily.
 * This is equivalent to NT's NtYieldExecution().
 */
void sched_yield(void);
/* Only threads of lower priority than the caller are queued on its CPU (a
 * yield would let one run a whole time slice) */
bool sched_yield_goes_lower(void);
/* True when a foreground thread (priority above 4) is waiting to run. */
bool sched_foreground_ready(void);
/* Sleep until the next timer tick (10 ms): for threads waiting on something. */
void sched_sleep_tick(void);
/* Sleep until *flag is set (by a waker that then calls sched_unblock) or
 * the tick count reaches @deadline, whichever comes first. */
void sched_sleep_until(volatile uint32_t *flag, uint64_t deadline);
/* The same with a TSC deadline: the timer fires when it is due, not at
 * the next tick, and the woken thread preempts one of no higher priority
 * (Sleep, timed waits: sub-millisecond resolution under load) */
void sched_sleep_until_tsc(volatile uint32_t *flag, uint64_t tsc);
/* The TSC @t100ns (100 ns units) from now (UINT64_MAX if beyond days),
 * and the TSC at which the tick count reaches @tick */
uint64_t sched_tsc_after(uint64_t t100ns);
uint64_t sched_tick_tsc(uint64_t tick);
/* A timer interrupt this CPU can't act on (halted waiting for the kernel
 * lock): arm the next one */
void sched_timer_rearm(void);
/* For wait loops: yield the first few rounds, then sleep a tick per round
 * (the count restarts when the thread returns to user mode). */
void sched_wait(void);

/*
 * Called from the APIC timer interrupt (IRQ_TIMER), which fires at each
 * 10 ms tick and when a timed sleeper is due.  Advances the tick counter,
 * wakes due sleepers, re-arms the timer and preempts the current thread
 * if its time slice expired or a woken sleeper should run now.
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
 * Unblock a thread (move from WAITING to READY state).  It preempts the
 * thread running on its CPU at once (through IPI_WAKE) if it has a higher
 * priority — or the same, when a timer woke it (sched_unblock_timer: the
 * timer it waits on was set or went off); otherwise it is queued after the
 * threads of its priority.
 */
void sched_unblock(Thread *t);
void sched_unblock_timer(Thread *t);
/* The same with NT's wake-up boost: @t's priority rises to its base plus
 * @boost (BOOST_*; never above 15, never for a real-time thread, never
 * lower than it is), so it runs ahead of busy threads of its base
 * priority, preempting one if that is what runs on its CPU.  It decays
 * one level per quantum (20 ms) the thread runs, back to its base. */
void sched_unblock_boost(Thread *t, int boost);
/* Give @t a new base priority (1-31): it runs at it from now on, boost
 * ended, preempting or giving way at once as its new rank says.  Program
 * threads get theirs from SetThreadPriority/SetPriorityClass (kernel/um). */
void sched_set_base_priority(Thread *t, uint8_t base);
/* Register @t with the Multimedia Class Scheduler at @mm (PRIO_MMCSS or
 * PRIO_MMCSS_AUDIO; 0: unregister), @base its own base priority (what it
 * runs at when it has used up its share, or unregistered) */
void sched_set_mmcss(Thread *t, uint8_t mm, uint8_t base);
/* The foreground process (Thread.um_proc of its threads; NULL: none), whose
 * woken threads get BOOST_FOREGROUND and whose time slices are three times
 * as long (60 ms).  Set by the desktop as the active window changes
 * (UmUpdateForeground). */
void sched_set_foreground(void *um);
void *sched_foreground(void);
/* IPI_WAKE (interrupt context): switch if sched_unblock asked this CPU to */
void sched_resched_ipi(void);
/* On the way back to a program: a switch asked for while this CPU halted
 * waiting for the kernel lock (when IPI_WAKE leaves it) happens now */
void sched_resched_pending(void);
/* For an idle CPU about to halt (interrupts off): a thread is queued on
 * some CPU, or a switch was asked of this one */
bool sched_work_waiting(void);

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
