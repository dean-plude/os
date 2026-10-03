## HPET and one-shot/TSC-deadline timers (Phase 18.7)

- **HPET** (`kernel/hal/hpet.c`): found through the ACPI `HPET` table
  (read straight from the RSDP, before the rest of ACPI starts), its main
  counter started; it replaces the PIT as the reference that calibrates
  the TSC and the local APIC timer (the PIT stays the fallback).
- **The APIC timer is one-shot**, in TSC-deadline mode where the CPU has
  it.  Each timer interrupt re-arms it for the CPU's next 10 ms tick or
  the earliest TSC-deadline sleeper on that CPU, whichever is sooner, so
  the tick work stays at 100 Hz while sleeps end when they are due.
- **Sleeps and timed waits** (`NtDelayExecution`, `NtWaitFor*Object(s)`
  timeouts) sleep until a TSC deadline (`sched_sleep_until_tsc`) instead
  of whole 10 ms ticks.  A thread woken by its deadline goes to the front
  of its run queue and preempts a running thread of no higher priority.
- `sleeptest timer` measures how late `Sleep(1)`, `Sleep(5)` and a 1 ms
  wait timeout end, idle and with a busy thread on every CPU; it is in the
  core self-tests.  In QEMU (TCG, 2 CPUs) the 95th percentile under load
  was 0.26 ms late (it was 10 to 20 ms with the 100 Hz tick).
- **Fix (2026-10-03):** `sleeptest timer` failed in CI at 1.4 to 1.6 ms
  (95th percentile under load).  Two causes.  The keyboard and mouse
  polls (PS/2 and USB) ran in the timer interrupt, and their port and MMIO
  reads block for up to a few milliseconds under QEMU (it serializes
  device access), with interrupts off, so every sleep due on that CPU
  meanwhile ended late; they now run in a kernel thread (`devpoll`) woken
  by its TSC deadline at each tick, interrupts enabled.  And a timer
  interrupt taken while a CPU halted waiting for the kernel lock re-armed
  the timer for the 10 ms grid only, forgetting that CPU's TSC-deadline
  sleepers; it now re-arms for the soonest sleeper too.
- QEMU emulates the TSC-deadline timer only with KVM, so the self-tests
  exercise the one-shot mode; TSC-deadline mode is untested.
- Not yet: waitable timers (`SetWaitableTimer`) still fire on the 10 ms
  tick.
