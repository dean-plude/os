- ~~Install to the internal NVMe disk, and S3, batteries and the lid on
  the reference machine's tables~~ Done (Phase 21.5): the ACPI embedded
  controller that laptops keep their lid, battery and AC adapter behind;
  sleep as low-power S0 idle on firmware without S3 (the T14 Gen 4 has
  none), with the LPS0 device's calls; timers from CPUID leaf 0x15 where
  the firmware hides the HPET; the Terminal's `install`, a firmware boot
  entry added at the first start from the disk, and a hint when Intel
  VMD hides the NVMe disk ([install-and-power.md](install-and-power.md)).
  Tested in QEMU (`laptop` in the devices suite); on the T14 itself the
  checks are by hand.
