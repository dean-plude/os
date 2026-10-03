## Kernel under KVM

The kernel now runs correctly under KVM on AMD hosts such as GitHub's
runners.  The failures the earlier trial found ("Test VMs under KVM in CI")
had one cause: SYSRET's stack selector.

- **SYSRET and RPL 3.**  STAR[63:48] held 0x10.  Intel CPUs, and QEMU's
  emulation, force RPL 3 on the selectors SYSRET loads, but AMD CPUs load SS
  as STAR[63:48] + 8 unchanged, so programs ran with SS = 0x18 (RPL 0).
  That works in 64-bit mode until an interrupt taken in the program returns:
  IRETQ checks the saved SS and raises #GP after the SWAPGS, and the kernel
  went on with the program's TEB as its per-CPU block.  This was the boot
  #GP in the ACPI thread (`uacpi_gas_read_mapped`), the network tests that
  never finished, and the silent hangs at boot.  The STAR base is now
  0x13, so SS = 0x1B and CS = 0x23 on every CPU.
- **GS checks.**  An interrupt from the kernel that finds the user GS base
  loaded, or a return to a program with a kernel GS base, now stops the
  kernel with a report (vector, frame, backtrace, the IRETQ frame) instead
  of running on with the wrong per-CPU block.
- **Big kernel lock.**  `bkl_acquire`, `bkl_switch_in` and `bkl_relax`
  raised the thread's lock depth before `raw_lock` had the lock, so an
  interrupt taken while it halted (the ACPI SCI) ran its handler as though
  it held the lock.  The depth now goes up once the lock is taken.
- **Scheduler.**  A thread still on a timed-sleep list when it blocks
  leaves the list first (it could otherwise be queued twice), and the run
  queue stops the kernel with a backtrace if a thread is ever queued twice
  or switched to when not ready.  An idle CPU looks at the run queues again
  after marking itself idle, so a thread queued in that window is not left
  until the next tick.  A switch asked for while a CPU waited for the
  kernel lock now happens on the way back to the program.  The ACPI thread
  records its own thread pointer at start, closing a race with
  `AmlInitialize`.
- **Serial output.**  The Terminal's copy of program output to the serial
  port takes the kprintf lock, so a kernel message from another CPU no
  longer lands in the middle of a program's line (stltest failed on that).

Results under KVM on GitHub's runners (five CI runs and three-job soak
runs): boot in 10-11 s; network suite 9 of 9 (about 55 s, against 63 s on
TCG); devices suite 6 of 6; graphics job green (test step 111 s, against
193 s on TCG); core suite 56 of 57.  The one left is `sleeptest timer`: a
timed wait on an idle or busy CPU now and then ends at the next 10 ms tick
instead of its deadline (traced: the CPU's timer did not fire for the
sleeper's deadline), which puts the 1 ms timer queue timer's 95th
percentile at about 9 ms under load.  So CI stays on TCG
(`NOVARUN_ACCEL=tcg`); switching it to KVM is the udev step and dropping
that line once `sleeptest timer` passes there.  No test was loosened.
