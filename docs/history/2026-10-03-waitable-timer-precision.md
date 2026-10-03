## Waitable timers on the TSC

Phase 18.7 made `Sleep` and wait timeouts end when they are due; waitable
timers still fired on the 10 ms tick.  They now end on the TSC too.

- **Kernel timer objects keep TSC deadlines** (`kernel/um/um_thread.c`):
  `NtSetTimer` turns the due time and period into TSC values, a wait on a
  timer sleeps until exactly that deadline (`sched_sleep_until_tsc`), and
  a periodic timer stays on its own grid, so it doesn't drift.  Setting a
  timer wakes the threads already waiting on it, which then sleep until
  its new due time (before, such a wait looked again only every 100 ms).
  `NtQueryTimer` reports the time left in 100 ns units.
- **`CreateWaitableTimer` makes a kernel timer** instead of an event that
  a kernel32 thread set when `GetTickCount64` (a 10 ms clock) said it was
  due.  Named timers are shared between processes, and
  `OpenWaitableTimer` opens them.
- **Completion routines run on time.**  `SetWaitableTimer`'s routine runs
  on the thread that set the timer when it waits alertably, as on
  Windows; an alertable wait (`SleepEx`, `WaitFor*ObjectEx`) now ends its
  slice when one of its thread's timer routines is due, and measures time
  on the performance counter instead of the 10 ms tick count.
- **Timer queues** (`CreateTimerQueue`, `CreateTimerQueueTimer`,
  `ChangeTimerQueueTimer`, `DeleteTimerQueueTimer`, `DeleteTimerQueueEx`)
  are new; they, threadpool timers (`SetThreadpoolTimer`) and winmm's
  `timeSetEvent` run their callbacks from worker threads that wait on a
  kernel waitable timer.  A freed timer's worker is kept for the next one.
- `sleeptest timer` measures a 1 ms waitable timer, a 5 ms periodic one
  (each firing against its place on the grid), a 1 ms timer's completion
  routine in `SleepEx` and a 1 ms timer queue timer, idle and under load.
  In QEMU (TCG, 2 CPUs) the 95th percentile under load was 0.26 to
  0.59 ms late over four runs (the 10 ms tick made it up to 10 ms).
- Not yet: a timer queue timer (or any timer) set while another thread
  already waits on it can be late by up to a 20 ms time slice when every
  CPU is busy: a thread woken that way waits for the running thread's
  slice (the scheduler gives woken threads no boost).  `sleeptest timer`
  reports the timer queue case without judging it.  `NtSetTimer`'s own APC
  routine (native callers) is still ignored, and `NtSetTimerEx` is not
  implemented.
