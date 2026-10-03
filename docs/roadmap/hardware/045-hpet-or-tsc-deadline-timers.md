- ~~HPET or TSC-deadline timers~~ Done (Phase 18.7): the HPET calibrates
  the TSC and the APIC timer, which is one-shot (TSC-deadline where the
  CPU has it), armed for the next tick or the earliest `Sleep` or wait
  timeout; `sleeptest timer` checks 1 ms resolution under load.
  Waitable timers, their completion routines, timer queues, threadpool
  timers and `timeSetEvent` followed (2026-10-03): they end on the TSC
  too.
