## The graphics job's second hang: the whole guest stopped, and said nothing

After the Venus ring fix (HPET calibration, ring re-wake), the graphics job
still hung once on PR #205's merge of main (job 111414850206): `d3dtest x64`
printed its D3D11 frame count and then nothing for 15 minutes, and `d3dtest
x86`, `d3d9 through DXVK` x64 and x86 each ran into their limits too, until
the job's hour ended.  This was not the Venus wait: the host was idle (QEMU's
main loop in `ppoll`, the render server in `poll`, no fence pending), both
guest CPUs sat in HLT using about 5 % of a host core each (a 100 Hz timer),
the serial log did not grow by a byte after the last D3D11 line, and neither
the harness's Ctrl+Alt+F12 (answered by the desktop thread) nor its Ctrl+C
did anything; the desktop's clock stood still for the whole time.  The
desktop watchdog (3 s without a pass, logged from the timer tick) did not log
either, which only happens when no CPU runs a full timer tick: a CPU halted in
`raw_lock` waiting for the big kernel lock only acknowledges its ticks.  Both
CPUs waiting for a lock that no running thread holds fits every one of these
signs.  The log did not say which thread had taken it (the serial log was not
kept: the job was cancelled before the self-tests' final copy), so the leak
itself is not found yet.

- **The kernel** (`kernel/ke/smp.c`): the big lock records who took it and
  when.  A halted waiter's timer tick (and the desktop watchdog) now calls
  `bkl_stall_check`: a lock held for 3 s is logged once (`[WATCHDOG] the kernel
  lock has been held for N ms by CPU c, thread ...; waiters, contenders`, then
  each CPU's current thread and whether it waits for the lock).  When the
  holder is no CPU's current thread, a thread left or was switched out
  without handing the lock back (the scheduler does that for every switch),
  so the lock is let go (`[SMP] Bug: the kernel lock was never handed back`)
  and the machine goes on, with the log naming the bug, instead of ending in a
  timeout.  A lock held by a thread that does run (a real deadlock, or the
  holder waiting for its own lock) is only reported.
- **The host watcher** (`tools/ci/host_watch.py`, `tools/novarun.py`): QEMU now
  has a second, private monitor socket; on a hang the watcher asks it for each
  CPU's registers (RIP named from `build/kernel.elf`), the local APIC timer
  state and `g_bkl`, and copies the whole serial log to `serial-hang-N.log`.
- **The self-tests** (`tools/selftest.py`): the output of a failed test is
  printed at once (it used to wait in a buffer for the next test's line, which
  made one test's output look like another's), the serial log so far is kept
  after every failure, and a run whose machine answers neither Ctrl+Alt+F12
  nor Ctrl+C after a timeout stops there: the later tests are reported "not
  run (NovaOS stopped answering ...)" instead of each waiting out its own
  limit.  A test that does not finish is still a failure; no limit changed.

If the graphics job hangs again with a silent guest, read `host-hang-N.txt`
(the monitor part) and `serial-hang-N.log`: a `[WATCHDOG] the kernel lock`
line names the holder; otherwise the two RIPs and the APIC timer say where each
CPU is and whether timers still run.
