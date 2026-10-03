## Priority boosts on wake-up

A thread woken by an event, a semaphore, a condition variable or a
message, with a busy thread of the same priority on its processor, used
to wait for that thread's 20 ms time slice to end: about 19 ms at the
95th percentile.  Windows raises a woken thread's priority above its base
for a while so it runs at once, which is what makes a program's window
answer and an audio thread refill its buffer promptly while something
else computes.  NovaOS now does the same.

- **Two priorities per thread** (`kernel/ke/scheduler.c`): the base
  priority and the current one.  A wake-up raises the current priority to
  the base plus the waker's increment, NT's values: +1 for an event, a
  semaphore, a mutex, an alert (what SRW locks, condition variables and
  critical sections wake with) or a timed wait's deadline, +1 for finished
  file I/O, +2 for a named pipe, the network or a window message, +6 for
  keyboard and mouse input and console input.  Never above 15, never for
  a real-time thread (16 and up), and a boost never lowers a priority that
  is already higher.  A window message boosts only the thread it is for;
  the others the message queue wakes get nothing.
- **The boost wears off** one level for every quantum (two 10 ms ticks)
  the thread runs while boosted, its waits in between included, back to
  its base.  A woken thread that turns busy takes turns with the others
  again within a few quanta.
- **Run queues are ordered by priority**, threads of one priority taking
  turns as before.  A boosted thread preempts a busy thread of its base
  priority on its processor.  A thread whose slice ends goes on running
  while every queued foreground thread has a lower priority (csrss, below
  them, still gets its turn); a yield gives way to any thread.
- **The balance set**: once a second, a thread that has been ready for 3 s
  without running is raised to 15 for one quantum, then drops back to its
  base, so higher-priority threads can't starve it for good.
- **Kernel threads**: the device poll and audio mixer threads moved from
  12 to 16, above any boost, so a boosted program can't hold up input or
  sound.  The desktop moved from 8 to 9, so a +1 boost doesn't queue
  programs ahead of it, while a timed wait's wake (also +1) still preempts
  it.
- **`NtQueryInformationThread`** reports a thread's current and base
  priority instead of a fixed 8.
- **Measured** in QEMU (TCG) on 2 CPUs with a busy thread on each, with
  the new `boosttest` (64- and 32-bit): a woken thread ran after 19.0 ms
  (95th percentile) before, for each of event, semaphore, condition
  variable and thread message, and after 0.03 to 0.10 ms now.
  `sleeptest timer` still passes at 0.32 ms (0.37 ms before) and its
  "Event set" line fell from 19.0 ms to 0.07 ms.  `smpstress` on 4 CPUs
  (6 runs each): the 8-thread critical section took 940 to 1380 ms before
  and 800 to 1070 ms after, so waking a lock's waiter with a boost causes
  no convoy; event ping-pong 40 to 100 ms before, 80 to 150 ms after.
- **Not done**: `SetThreadPriority` and `SetPriorityClass` are still
  accepted and ignored, so every program thread has base 8; NT's extra
  foreground-process boost and quantum stretching are left out.
