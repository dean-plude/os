## Phase 11 — Multiprocessor (SMP)

- **Every CPU core runs threads** (`kernel/ke/smp.c`): the kernel finds the
  processors in the ACPI MADT, starts each one with INIT and STARTUP IPIs
  through a real-mode trampoline (`arch/x86_64/ap_trampoline.asm`), and gives
  it its own KPCR (reached through GS), GDT, TSS and exception stacks, LAPIC
  timer, and idle thread.  Up to 16 CPUs.
- **Programs run in parallel**, and much of the kernel runs on every core at
  once too (below).
- **Per-core ready queues** (`ke/scheduler.c`): each core has its own run
  queue, lock and timed-sleep list, so cores schedule without touching each
  other.  A thread stays on the core it last ran on (warm caches); a new
  thread starts on the core that created it, and a core with nothing to run
  steals a ready thread from a busy one (every tick while idle, or at once
  when it is woken).  Making a thread ready wakes its own core if that one is
  idle, else any idle core, so the work spreads out.
- TLB shootdowns (IPIs) keep the other cores' page-table caches right when a
  program frees or re-protects memory; halted cores are woken by IPI when a
  thread becomes ready.  The clock follows the TSC, so it keeps time whatever
  the cores are doing.
- Programs see the core count: `GetSystemInfo`, the PEB,
  `KUSER_SHARED_DATA` and `NUMBER_OF_PROCESSORS`.  `cpus.exe` runs the same
  work on 1 thread and then on one thread per core: with `-smp 4` it reports
  a speedup of up to about 3.7× (less when the host's own cores are busy).
- **Finer-grained kernel locking**: the big kernel lock now belongs to
  threads (it nests, and the scheduler drops it when a holder is switched
  out), and these run without it, on all cores at once, under locks of
  their own:
  - the scheduler (each core's run-queue lock, held across its switches),
    the timer and IPIs;
  - memory: the physical page allocator and the kernel heap (spinlocks), a
    program's address space (its process lock), with TLB shootdowns before
    pages are freed;
  - waits, events, mutexes, semaphores and handle closing (the handle table
    under the process lock, object state under one spinlock, destructors
    taking the big lock themselves);
  - sockets (`net_lock` around the network stack), and program windows:
    message queues, `PostMessage`, `InvalidateRect` and timers have a
    spinlock of their own, and the desktop draws without the big lock under
    `DesktopLock` (the built-in apps' painters take it back);
  - the list, and what each service relies on, is in `um_syscall.c`
    (`um_lock_free_init`); files, the registry, process creation, the
    console and the loader still use the big lock.
  - the program loader holds `DesktopLock` only to look a module's file
    up: the file is then pinned (`RamfsPin`: its contents stay put, and
    writes to it fail, as Windows refuses writes to a mapped image) and
    copied, relocated, bound and committed without it, and without the
    big lock (`bkl_drop`; the work touches only the heap, the page
    allocator and the new image's address space); `LoadLibrary`
    serializes on a per-process loader lock instead.  Programs started from the Terminal,
    Explorer, `start` or the desktop are loaded on a worker thread
    (`UmSpawnStart`), so starting `node.exe` (90 MB) or loading CoreCLR
    no longer freezes the desktop for seconds.
- Waits are woken, not polled: `SetEvent`, `ReleaseSemaphore`, a thread or
  process ending, a message arriving or network data coming in wakes the
  waiting threads at once (wait queues, `ke/waitq.c`), and `select()` waits
  in the kernel.  Copies to and from program memory survive the memory
  being freed by another thread meanwhile (they fail with an access
  violation instead of crashing the kernel).
- `smpstress.exe` works all of this from many threads (critical sections,
  event ping-pong, semaphores, memory, handles, and freeing memory while the
  kernel copies into it) and checks the results.
- Boot QEMU with `-smp 4` (or any count) to use it.
- Also: window shadows skip the part their window covers (the desktop draws
  about twice as fast), and the network thread and ntdll's lock waits sleep
  instead of spinning.
