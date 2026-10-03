- **Kernel** (`kernel/`): NT-style executive: object manager and handles,
  processes and threads, virtual memory with sections and guard pages, I/O,
  registry, security tokens (restricted tokens, impersonation) and
  security descriptors checked when named objects and files on drive C:
  are opened.  SMP with per-core scheduling and fine-grained
  locks; wait queues; APCs; pipes; the NT system-call table at Windows 10
  1903 numbers.  Timers are the local APIC's, one-shot or TSC-deadline and
  calibrated against the HPET, so `Sleep(1)`, wait timeouts and waitable
  timers end within a fraction of a millisecond even with every CPU busy.
