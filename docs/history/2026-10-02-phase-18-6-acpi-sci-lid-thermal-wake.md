## ACPI: SCI interrupt, lid, thermal zones, wake devices, _PRT (Phase 18.6)

- **The SCI is an interrupt** (`kernel/hal/ioapic.c`): the I/O APICs come
  from the MADT, every input is masked at boot, and the SCI (FADT
  `SCI_INT`, with the MADT's interrupt source override) is routed to vector
  0x32 on the boot CPU.  Its handler runs uACPI's, which queues GPE methods
  and `Notify` handlers and wakes the `acpi` thread to run them.  The
  thread still calls the handler once a second in case an edge was missed,
  and polls every 100 ms as before when there is no I/O APIC.  The routes
  are put back after S3.  It is the only device interrupt; the drivers
  still poll.
- **PCI interrupt routing**: with an I/O APIC, `\_PIC(1)` selects APIC
  mode and the root bridge's `_PRT` is read, link devices (`PNP0C0F`)
  resolved through their `_CRS`; `AmlPciIrq` answers which GSI a pin is
  wired to, and each function on bus 0 gets it in its Interrupt Line
  register.  (QEMU's q35: 128 entries, all through links.)
- **The lid** (`PNP0C0D`): `_LID` is read at load, on `Notify 0x80` and
  after waking.  Closing it puts the machine to sleep, as Windows does by
  default.
- **Thermal zones**: `_TMP`, `_PSV`, `_HOT`, `_CRT` and `_TZP`, read every
  `_TZP` (at least every 10 s) and on `Notify 0x80`/`0x81`.  Crossing the
  passive trip point is logged (NovaOS can't throttle the CPUs yet);
  `_HOT` puts the machine to sleep and `_CRT` shuts it down, drive C: saved
  first.
- **Wake devices** (`_PRW`): the lid, power buttons and USB host
  controllers have their wake GPEs set up, and before S3 `_DSW` (or
  `_PSW`) tells them to arm.  USB keyboards that can are set for remote
  wakeup (`SET_FEATURE(DEVICE_REMOTE_WAKEUP)`), and before S3 the xHCI root
  ports enable wake on connect, disconnect and over-current, suspend
  (U3) the ports with a device and turn on PME#.  After waking the kernel
  logs which wake GPE (or the power button, or the RTC alarm) woke it.
- **For programs**: `NtPowerInformation` answers `SystemPowerCapabilities`
  (`LidPresent`, `SystemS3`, `ThermalControl`, batteries),
  `ThermalInformation` (the first zone; converted to the 32-bit layout in
  ntdll) and `LastSleepTime`/`LastWakeTime`; powrprof's
  `GetPwrCapabilities` and `CallNtPowerInformation` use them.
- **Self-test**: `tests/acpi/lid-thermal.asl` describes a lid, a thermal
  zone (40 C; passive 60, hot 90, critical 95 C) and the xHCI controller as
  a wake device, with QEMU's `pc-testdev` (ports 0xE8 and 0xE9, written
  from the QEMU monitor) as the embedded controller.  `powertest` checks
  the capabilities and readings, then asks the test to close the lid:
  NovaOS sleeps, the test opens the lid, presses a key on the USB keyboard
  and wakes the machine, and `powertest` sees `LastSleepTime` and
  `LastWakeTime` move on; then the zone is heated to 70 C (passive cooling
  on) and cooled to 45 C.  It runs in the CI boot, which now also has a
  USB keyboard on an xHCI controller (the self-tests are typed through
  it).  Also tested by hand: 91 C sleeps and 96 C shuts down.
- **Not done**: in QEMU a USB key can't wake the machine itself.  QEMU
  8.2 delivers the key to the suspended port (`xhci_wakeup`) but has no
  path from there to the platform, so the test wakes it with
  `system_wakeup` (which QEMU reports as the power button).  USB wake
  needs checking on real hardware, as do GPE block devices other than
  `\_GPE` and routing behind PCI bridges.
