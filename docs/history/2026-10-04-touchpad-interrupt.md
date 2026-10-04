## The touchpad's interrupt: Intel GPIO controllers

Phase 21.4's touchpad driver polled the touchpad every 10 ms, because the
line a laptop touchpad pulls low when it has a report is a pin of the
chipset's GPIO controller and NovaOS had no driver for it.  It has one
now, and the touchpad is read when its pin fires.

- **The GPIO controller** (`kernel/hal/gpio.c`).  Intel's chipsets from
  Tiger Lake to Meteor Lake (`INT34C5`, `INT34C6`, `INTC1055`, `INTC1056`,
  `INTC1057`, `INTC1085`, `INTC1083`, `INTC105E`; the ThinkPad T14 Gen 4's
  Raptor Lake is `INTC1055`) describe their pin controller in ACPI as a
  few memory-mapped "communities" and one shared interrupt (IRQ 14).  The
  driver maps the communities, masks every pin's interrupt, routes the
  controller's interrupt through the I/O APIC and translates a GpioInt
  pin (ACPI numbers pins per 32-pin group) to its pad.  A connected pad is
  set up as a GPIO input interrupting on the level or edge the resource
  asks for (inverted for an active-low line), with its SCI, SMI, NMI and
  I/O APIC routes off; a pad the firmware gave a native function is
  refused.  A level-triggered pin stays masked from its interrupt until
  its driver has read the device, and the pads come back after sleep.
  The register layout and each chipset's pad groups are ported from
  OpenBSD's `pchgpio(4)` (ISC licence).
- **The touchpad** (`i2chid.c`).  The touchpad's GpioInt (or, on firmware
  that gives it one, its own Interrupt resource through the I/O APIC)
  wakes the `i2chid` thread, which reads reports while the line stays
  asserted and then unmasks the pin.  Once a second it also looks without
  an interrupt; a touchpad whose reports keep turning up that way (an
  interrupt that never arrives) is polled from then on, and one whose pin
  can't be had (an unknown controller, no interrupt routed) is polled as
  before, so the touchpad works either way.  The boot log's `[GPIO]` and
  `[I2C]` lines say which.
- **Tests.**  QEMU has no Intel GPIO controller, so the core suite's ACPI
  table (`tests/acpi/i2c-touchpad.asl`) now describes one as a Raptor Lake
  DSDT does, under `_HID NOVA1055`, which NovaOS serves with a model of
  the registers (the way the laptop table's embedded controller is
  modelled), and puts the touchpad's GpioInt on its pin 277 (GPP_C21).
  `hwcheck` connects its modelled touchpad to that pin: nothing interrupts
  while the touchpad is quiet, and a tap and a two-finger scroll are read
  on the interrupt, the controller's vector raised as its line would
  raise it, with no empty reads.  The core test `touchpad interrupt`
  checks this and the refusals.

None of this has run on a T14 yet; the hand check is step 5 in
[hardware.md](../hardware.md#checking-audio-and-the-touchpad-on-the-t14-step-214).
