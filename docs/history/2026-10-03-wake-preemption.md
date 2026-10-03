## Woken threads preempt the running one

A thread already waiting on an event, a timer or another object used to
wait for the running thread's 20 ms time slice to end once another thread
signalled it, when every CPU was busy.  It now runs at once.

- **The scheduler** (`kernel/ke/scheduler.c`): `sched_unblock` puts a
  woken thread first in its CPU's run queue and preempts the thread
  running there when the woken one has a higher priority, or the same
  priority and a TSC-deadline wait (every wait and `Sleep` of a program),
  which is what NT's wait boost gives an event's or a timer's waiter.  A
  halted CPU takes the thread if there is one; otherwise the waker sends
  the thread's CPU an `IPI_WAKE` (itself too, taken once it re-enables
  interrupts) and that CPU switches in the interrupt.  This is the same
  rule a thread woken by its own deadline already had.
- **A CPU halted waiting for the kernel lock** does not switch in the
  middle of that wait (as with the timer); its next timer tick does.
- **Measured** with `sleeptest timer` on two CPUs in QEMU, a busy thread
  on each: a thread waiting on an event ran 8.96 ms (95th percentile,
  median 8.8 ms) after another thread set it; now 0.06 ms.  `sleeptest
  timer` gains that case ("Event set (waiter)") and holds it to 1 ms like
  the others.
