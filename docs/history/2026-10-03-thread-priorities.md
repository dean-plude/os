## Thread priorities and priority classes

`SetThreadPriority` and `SetPriorityClass` used to be accepted and
ignored, so every program thread ran at base priority 8: an audio thread
asking for `THREAD_PRIORITY_TIME_CRITICAL` waited behind a program's busy
worker like any other thread, and a background job at `IDLE_PRIORITY_CLASS`
competed with the foreground one.  They now set NT's base priorities.

- **NT's mapping** (`kernel/um/um_thread.c`): a process's class sets the
  base (IDLE 4, BELOW_NORMAL 6, NORMAL 8, ABOVE_NORMAL 10, HIGH 13,
  REALTIME 24) and `SetThreadPriority` adds -2 to 2, kept within 1-15;
  `THREAD_PRIORITY_IDLE` and `TIME_CRITICAL` saturate at 1 and 15 (16 and
  31 in a real-time process).  Changing a class moves every thread of the
  process.  `GetThreadPriority`, `GetPriorityClass`,
  `Set/GetThreadPriorityBoost` and `Set/GetProcessPriorityBoost` work, through
  `NtSetInformationThread` (`ThreadPriority`, `ThreadBasePriority`,
  `ThreadPriorityBoost`) and `NtSetInformationProcess` /
  `NtQueryInformationProcess` (`ProcessPriorityClass`,
  `ProcessPriorityBoost`), which 32-bit programs can now call too.
  `CreateProcess` honours the `*_PRIORITY_CLASS` flags, and a child of an
  IDLE or BELOW_NORMAL process inherits its class, as on Windows.
- **No real-time for programs**: REALTIME (and an absolute priority of 16
  or more) needs SeIncreaseBasePriorityPrivilege, which only an
  administrator's token holds; without it `SetPriorityClass` gives HIGH, as
  Windows does.  So a program never gets above 15.
- **The scheduler** (`sched_set_base_priority`): a new base takes effect
  at once.  A queued thread moves to its new place and preempts the thread
  running on its CPU if it now outranks it; a running thread lowered below
  a queued one gives way.  A priority change ends any boost, and boosts
  from then on start from and decay back to the thread's own base.
- **System threads above programs**: the desktop moved from 9 to 16, the
  device poll and audio mixer threads from 16 to 17, so no program thread,
  at `TIME_CRITICAL` or boosted, can hold up input, window management or
  sound, and a long redraw can't delay a sound buffer.
- **`THREAD_BASIC_INFORMATION.BasePriority`** is now the thread's
  increment over its class, as on NT (it was the absolute base);
  `ProcessBasicInformation` reports the class's base priority.
- **Measured** in QEMU (TCG) on 2 CPUs with the new `prioritytest` (64- and
  32-bit): with wake-up boosts off and a busy NORMAL thread on each CPU, a
  `THREAD_PRIORITY_HIGHEST` thread woken by an event ran after 0.05 ms at
  the 95th percentile, a NORMAL one after 18.8 ms (the busy thread's time
  slice).  `boosttest` 0.08 ms and `sleeptest timer` 0.25 ms still pass.
  `smpstress` on 4 CPUs passes; its 8-thread critical section took
  2060-2440 ms against 1790-2090 ms on main on the same host, the
  desktop now preempting the threads it shares a CPU with.
- **Not done**: NT's foreground-process boost and quantum stretching;
  kernel threads other than these (network, USB, ACPI, saving drive C:)
  stay at 8, so a busy HIGH-class program can delay them until the balance
  set lifts them after 3 s; `ProcessBasePriority` is accepted and ignored.
