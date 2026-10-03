- **Kernel** (`kernel/`): NT-style executive: object manager and handles,
  processes and threads, virtual memory with sections and guard pages
(the read-only pages of loaded DLLs are shared by every process with the
same bytes, as Windows shares image sections), I/O,
  registry, security tokens (restricted tokens, impersonation) and
  security descriptors checked when named objects and files on drive C:
  are opened.  SMP with per-core scheduling and fine-grained
  locks; wait queues; APCs; pipes; the NT system-call table at Windows 10
  1903 numbers.  Timers are the local APIC's, one-shot or TSC-deadline and
  calibrated against the HPET, so `Sleep(1)`, wait timeouts and waitable
  timers end within a fraction of a millisecond even with every CPU busy;
  a thread woken by a timer preempts the running one instead of waiting
  for its time slice to end.  NT's priority boosts: a thread woken by an
  event, a lock, I/O, a window message or input runs above its base
  priority (+1 to +6) and preempts busy threads of that priority, then
  decays back one level per quantum; a balance set lifts threads that
  have starved for 3 s.  Priority classes and thread priorities
  (`SetPriorityClass`, `SetThreadPriority`) set NT's base priorities;
  the desktop, input and audio threads stay above anything a program
  can ask for.
