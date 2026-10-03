## Timer queue timers on time under load

`sleeptest timer` failed on main now and then (CI under TCG, and under
KVM): its 1 ms timer queue timer fired about 9 ms late at the 95th
percentile with a busy thread on every processor.  A trace of each CPU's
timer interrupts, arms, wake-ups and switches around every late firing
found three scheduler causes, none in the timer queue code itself.

- **A deadline armed over before it fired** (`sched_sleep_until_tsc`).  A
  new sleep re-armed its CPU's one-shot timer for itself whenever the
  deadline the timer was armed for had gone by, on the idea that it had
  fired already.  It may not have: the one-shot count runs a little off the
  TSC and a virtual CPU's timer fires late, so the interrupt for a deadline
  just passed was often still to come.  Re-armed for the new, later sleep,
  it never came, and the first sleeper waited for the next 10 ms tick (the
  "timed wait ends at the next tick" the KVM work traced).  A sleep now
  only arms the timer sooner; a deadline gone by is armed again and fires
  at once.
- **A timer wake queued last.**  A thread woken by a timer (its deadline,
  or the waitable timer it waits on being set, which is how a timer queue's
  worker learns its due time) preempts the running thread of the same
  priority.  When a kernel thread of higher priority was running just then
  (for a few microseconds, usually), it went to the back of the queue
  instead, and the busy thread queued ahead of it ran a whole 20 ms slice
  first.  It now goes first in the queue either way, and runs as soon as
  the higher-priority thread is done.
- **A thread preempted at a tick sent to the back.**  The thread preempted
  for a deadline wake at a timer tick lost its place and its slice (the
  preemption by `IPI_WAKE` already kept both), so a thread starting a 1 ms
  wait of its own waited out the other threads' slices first.  Both
  preemptions now keep it ahead of the threads waiting their turn.

Measured with `sleeptest timer` in QEMU (TCG, two CPUs, a busy thread on
each), on a 4-core host with and without busy host threads; the 1 ms
timer queue timer under load, before and after:

| Host | Runs | Before: median, 95th percentile, max | After |
|---|---|---|---|
| idle | 5 + 5 | 0.14-0.19 ms, 0.20-0.27 ms, up to 9.1 ms | 0.13-0.17 ms, 0.21-0.26 ms, up to 3.3 ms |
| one busy thread | 5 + 5 | 0.14-0.20 ms, 0.24-9.1 ms, up to 19.4 ms | 0.16-0.19 ms, 0.26-3.3 ms, up to 4.4 ms |

The 9 to 19 ms outliers (a missed deadline or a lost turn) are gone.  What
is left comes from a busy host: QEMU itself then delivers a CPU's timer
interrupt 1 to 5 ms late, often on both CPUs at once (the trace shows the
interrupt arriving late, with nothing in the guest holding it up), and
`sleeptest timer` can still fail there (1 run in 5 with one busy host
thread, every run with two).  The test's 1 ms limit is unchanged.
