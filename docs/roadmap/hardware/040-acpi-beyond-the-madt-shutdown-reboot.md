- ACPI beyond the MADT: ~~shutdown, reboot, sleep, batteries~~ Done:
  power-off (S5), sleep (S3), reset and the fixed power button from the
  FADT; the AML interpreter (uACPI) for batteries, AC adapters,
  control-method power buttons and `_PTS`/`_WAK`.  Still to do: the lid
  switch, thermal zones, wake devices (USB keyboards), a real SCI
  interrupt and PCI interrupt routing from `_PRT`; HPET or TSC-deadline
  timers.  (Display modes after S3 are set again on every adapter NovaOS
  drives: the VBE ones, QXL, virtio-vga, VMware SVGA and Cirrus.  Real
  GPUs have no driver yet.)
