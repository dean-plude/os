## ACPI sleep (S3)

- **Sleep**: Start > Sleep (right-click Start, or the power button in the
  Start menu), or `SetSuspendState`, `NtSetSystemPowerState` and
  `NtInitiatePowerAction`, which return once the machine is awake.
  Drive C: is saved first.  A keypress wakes it (`system_wakeup` in the
  QEMU monitor); the power button that wakes it doesn't also shut down.
- **CPUs** (`kernel/ke/sleep.c`): the other CPUs are stopped with an NMI
  and save their state; each CPU's control registers, GDT/TSS, IDT, MSRs,
  MTRRs and FPU state are kept, and the FACS waking vectors (real mode and
  32-bit) point at the CPU start-up trampoline, found in `\_S3`.  On wake
  the boot CPU restores itself and the PCI configuration of every function,
  restarts the other CPUs through the trampoline, and each CPU jumps back
  into what it was doing.
- **Devices** set up again: AHCI disks, e1000 network, PS/2 keyboard and
  mouse (with scancode translation forced on), the Bochs/QEMU display
  mode, xHCI USB (the controller restarts and the keyboards and mice are
  enumerated again) and HD Audio (codec paths and the output stream).  The
  wall clock moves on by what the CMOS clock measured.
- `sleeptest.exe` sleeps and then checks the clock, threads and files.
  Tested in QEMU (OVMF, q35) with 1, 2 and 4 CPUs, three sleeps in a row,
  and with a USB keyboard and mouse and HD Audio attached.
- Not yet: wake devices such as USB keyboards.  (Display modes on other
  adapters came later: see "Display adapters: QXL, virtio, VMware, Cirrus,
  and their modes after sleep".)
