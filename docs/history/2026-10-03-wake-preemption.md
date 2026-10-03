## Timer wake-ups preempt the running thread

A thread already waiting on a waitable timer when another thread set the
timer (a timer queue's worker, say) used to wait for the running thread's
20 ms time slice to end before it could look at the new due time, when
every CPU was busy.  Now it runs at once, as a thread woken by its own
deadline already did.

- **The scheduler** (`kernel/ke/scheduler.c`): a thread woken from a wait
  preempts the running thread when it has a higher priority, or the same
  priority and a timer woke it (`sched_unblock_timer`, which `um_ob_wake`
  uses for a timer object's waiters).  It goes first in its CPU's run
  queue; a halted CPU takes it if there is one, otherwise the waker sends
  the thread's CPU `IPI_WAKE` (itself too, taken once it re-enables
  interrupts) with a reschedule flag, and that CPU switches in the
  interrupt.  A CPU halted waiting for the kernel lock doesn't switch in
  the middle of that wait; its next timer tick does.
- **The preempted thread** goes back after the woken threads but ahead of
  the rest (as on NT), not last, so a waker its wakee preempts doesn't
  wait out every other thread's slice; and it keeps what it has used of
  its slice, so one preempted often still reaches the end of it and the
  threads behind it are not starved.
- **Other wakes are as they were.**  An event set or a lock released with
  no priority difference doesn't preempt, and the thread is queued last:
  preempting there made lock convoys (smpstress's critical section shared
  by 8 threads took about 3 times as long).  Wider versions of this change
  also starved the desktop thread for 3 s once, and let a waiter woken
  while a higher-priority thread ran wait behind the thread it had
  preempted, which CI's `sleeptest timer` caught.
- **Measured** with `sleeptest timer` on two CPUs in QEMU, a busy thread
  on each: the 1 ms timer queue timer was 9 to 19 ms late (95th
  percentile) and is now 0.21 to 0.44 ms late over twenty runs.
  `sleeptest timer` now judges the timer queue case, and reports (without
  judging) how soon a thread waiting on an event runs once another thread
  sets it (9 to 19 ms under load, as before).
- **Not changed**: under load in QEMU the timer interrupt itself sometimes
  fires 1 to 10 ms late (a debug count found as many late fires with the
  old scheduler), so a rare `sleeptest timer` run can still slip past
  1 ms on one case.
