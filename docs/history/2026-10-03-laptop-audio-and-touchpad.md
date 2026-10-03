## Sound and the touchpad on the reference laptop (Phase 21.4)

Phase 21's fourth step is the ThinkPad T14 Gen 4's sound and touchpad.
Neither can be emulated by QEMU, so each code path has a QEMU-side check
against a modelled device, and the machine itself gets a hand check
([hardware.md](../hardware.md)).

- **HD Audio controllers with the DSP on.**  Intel's controllers since
  Skylake sit beside an audio DSP; with the DSP enabled in the firmware,
  as on the T14 (Raptor Lake-P, `8086:51ca`), they report PCI class 04.01
  instead of 04.03, with the same HD Audio registers.  The driver now
  takes those by device ID (the list from FreeBSD's `hdac`, BSD licence),
  and other Intel class 04.01 functions only if their version registers
  read HD Audio 1.0, so an AC'97 card (also 04.01, with I/O BARs) is left
  alone.  Around the controller reset it turns off the link's dynamic
  clock gating (`CGCTL.MISCBDCGE`), and it selects traffic class 0 and
  snooped DMA, as Linux does on these chips; codecs get up to 100 ms to
  announce themselves.  The registers are reached through `PciMapBar`, so
  a BAR above the 64 GiB physical map works too.  The digital
  microphones hang off the DSP and stay silent.
- **Speakers and headphones.**  Output pins are routed as before, and the
  driver now remembers each codec's speaker pins and the headphone jacks
  that can sense a plug.  Twice a second (from the mixer thread) it reads
  the jacks and turns the speaker pins off while headphones are in,
  logging `[HDA] Headphones plugged in: speakers off`.  Realtek's ALC256
  family (ALC256, ALC257 as in the T14, ALC236) gets the one vendor
  setting Linux makes for it at start: processing coefficient 0x36 =
  0x5757, which keeps pin 0x1A's PC-beep loopback out of the outputs.
  The boot log line for each codec now carries its subsystem ID, which a
  machine-specific fix needs.
- **I2C-HID touchpads** (`kernel/drivers/i2chid.c`, `i2c_dw.c`).  Laptop
  touchpads are HID devices on an I2C bus.  The ACPI thread now finds
  every PNP0C50 device whose `_STA` says present and reads its I2C
  address, speed and controller from `_CRS`, the HID descriptor register
  from `_DSM` and the controller's PCI function and timings from `_ADR`,
  `FMCN` and `SSCN`, and powers both up (`_PS0`).  The controller driver
  takes Intel's LPSS I2C functions (DesignWare cores, recognised by their
  signature, set up as FreeBSD's `ig4` does) and makes polled transfers.
  The I2C-HID driver reads the HID descriptor, powers the device on,
  resets it and waits for the acknowledge, and hands the report
  descriptor to the USB HID parser.  Touchpads start in mouse mode, so
  relative motion and the click come from their mouse collection; the
  parser now recognises a Touch Pad collection and leaves its finger
  reports alone (which also stops a USB precision touchpad being taken
  for a touch screen).  The touchpad's interrupt is a GPIO pin, and
  NovaOS has no GPIO driver yet, so the `i2chid` thread polls it, every
  10 ms while reports come and every 50 ms when idle, as FreeBSD's
  `iichid` does without an interrupt.  It stops polling around sleep and
  sets the controller and device up again after waking.
- **Tests.**  The Terminal's `hwcheck` (core suite) runs the controller
  matching on the T14's IDs, the codec setup and jack handling on a
  modelled ALC257 (speaker and headphone pins routed with EAPD, the
  headset microphone recorded, coefficient 0x36 written, speakers off
  with headphones in and on again after), and the I2C-HID protocol on a
  modelled touchpad (descriptor, power, reset acknowledge, finger reports
  ignored, mouse reports moving and clicking, an absent device not
  taken).  The core boot gets an ACPI table (`tests/acpi/i2c-touchpad.asl`)
  describing a touchpad as a ThinkPad's DSDT does, on an I2C controller
  QEMU doesn't have: the boot log must show it found and the controller
  reported missing, and a second model whose `_STA` says absent skipped.
  The devices suite's `usbheadset` boot gains an AC'97 card, which the HD
  Audio driver must not take.

On the T14 the hand check is: `devices` shows the audio controller with
**HD Audio** and an I2C controller with **I2C (touchpad)**;
`soundtest tone 440 1000` plays through the speakers and, with
headphones plugged in, through the headphones only; the touchpad moves
the pointer and clicks.  It has not been done on the machine yet.
