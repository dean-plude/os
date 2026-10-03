## Kernel service threads above programs, and the foreground boost

Since programs got real priorities, a busy `HIGH_PRIORITY_CLASS` program
(13) outranked the kernel's network, USB, ACPI and drive-saving threads,
which still ran at 8: on a machine with every CPU busy at HIGH, a byte
sent over a 127.0.0.1 connection took 3.6 s to arrive, the time the balance
set takes to lift a starved thread.  And Windows' boost for the process
the user is working with was missing.

- **Every kernel thread above programs** (`kernel/ke/scheduler.h`), as
  Windows runs its system threads in the real-time range, ordered by how
  short and urgent their work is: the device poll thread and the audio
  mixer at 19 (were 17), the network stack and the USB thread at 18 (were
  8), the desktop and the threads that start programs at 17 (were 16 and
  8), and bulk work at 16: saving drive C:, ACPI and Setup (were 8).  The
  only kernel threads left at 8 or below are boot-time tests and csrss's
  stub, which never runs.  None of them spins: each blocks or sleeps when
  idle and gives way to any thread while it polls a device.
- **The network thread no longer yields round and round** while a program
  waits for data: above programs, each yield handed the CPU to a busy
  program for its whole 20 ms slice.  It now polls the adapter every
  millisecond on a timed wake-up, which preempts a busy program, and keeps
  the CPU free otherwise (it used to spin a CPU for as long as any program
  waited on a socket).  While data is moving it goes round again at once,
  or, when programs are waiting for its CPU, after 0.2 ms
  (`sched_yield_goes_lower`).
- **The foreground boost**: the process whose window is active (an owned
  or modal dialog's too, not an IDLE-class one) is the foreground process.
  Its threads get NT's PsPrioritySeparation, 2 on client Windows, on top
  of every wake-up boost, still never above 15: a woken NORMAL thread of
  the foreground process runs at 11 where a background one runs at 9.
  The desktop follows the active window every tick (`UmUpdateForeground`),
  and `NtQueryInformationProcess(ProcessPriorityClass)` reports it in
  `Foreground`.  Windows' other foreground mechanism, longer time slices
  for the foreground process (quantum stretching), is not done.
- **Measured** in QEMU (TCG, 2 CPUs) with `prioritytest`: with a busy
  HIGH thread on every CPU, 40 loopback round trips took 8.9 ms at the median and 20 ms at worst
  (on main the first one took 3.6 s); with a
  background process's busy `THREAD_PRIORITY_HIGHEST` thread on every CPU,
  the foreground process's woken NORMAL thread ran after 0.09 ms at the
  95th percentile and the background process's after 3.1 s (the balance
  set).  `sleeptest timer` (0.37-0.56 ms) and `boosttest` (0.07-0.08 ms) are
  as on main; `smpstress` on 4 CPUs passes, its 8-thread critical section
  1740-2420 ms against 910-2180 ms on main on the same host (no change
  beyond the run-to-run spread); the network suite and the devices
  suite's USB boots all pass.
- **Not done**: quantum stretching; a console program in the Terminal is
  never the foreground process (the Terminal is a built-in app, not the
  program's window); Winsock's `setsockopt` is still a no-op, so
  `TCP_NODELAY` does nothing (the test bounces each byte back so Nagle's
  algorithm never waits for a delayed ACK).
