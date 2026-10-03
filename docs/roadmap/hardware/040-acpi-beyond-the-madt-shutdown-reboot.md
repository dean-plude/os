- ACPI beyond the MADT: ~~shutdown, reboot, sleep, batteries~~ Done:
  power-off (S5), sleep (S3), reset and the fixed power button from the
  FADT; the AML interpreter (uACPI) for batteries, AC adapters,
  control-method power buttons and `_PTS`/`_WAK`; the lid (closing it
  sleeps), thermal zones (passive cooling reported, sleep at `_HOT`,
  shutdown at `_CRT`), wake devices from `_PRW` (the lid, power buttons,
  USB controllers, with USB keyboards set for remote wakeup), the SCI as
  a real interrupt through the I/O APIC and PCI interrupt routing from
  `_PRT` (Phase 18.6).  Still to do: CPU throttling for passive cooling;
  GPE blocks other than `\_GPE`; routing behind PCI bridges; USB wake
  tested only up to what QEMU emulates (it has no USB-to-platform wake).
  (Display modes after S3 are set again on every adapter NovaOS drives:
  the VBE ones, QXL, virtio-vga, VMware SVGA and Cirrus.  Real GPUs have
  no driver yet.)
