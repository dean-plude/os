## Install and power on a laptop (Phase 21.5)

The reference ThinkPad T14 Gen 4 keeps its lid and battery behind an
embedded controller, has no S3 sleep, may hide its HPET, and has its disk
on NVMe.  NovaOS now handles each of these; [install-and-power.md](install-and-power.md)
has the details and the checks to run on the machine.

- **Embedded controller** (`kernel/hal/ec.c`): the ACPI embedded
  controller from the ECDT or the PNP0C09 device, as uACPI's address
  space handler for EmbeddedControl regions (with the global lock when
  `_GLK` asks), and its events (`_Qxx` after QR_EC) from its GPE, polled
  as well.  Without it a laptop's `_LID`, `_BST` and `_PSR` read nothing.
- **Sleep without S3** (`kernel/ke/sleep.c`, `kernel/hal/aml.c`): on
  firmware with no `\_S3` but the FADT's low-power S0 flag or an LPS0
  device, Sleep and closing the lid blank the screen, call the LPS0
  device's `_DSM` (Intel's and Microsoft's functions, in Linux's order)
  and idle until the lid opens, the power button is pressed or, with the
  lid open, a key or the mouse is used.  `GetPwrCapabilities` reports
  AoAc instead of S3 there.
- **Timers without an HPET** (`kernel/arch/x86_64/apic.c`): the TSC and
  the APIC timer from CPUID leaf 0x15 (0x16's base frequency when the
  crystal isn't given); the PIT, the last resort, can no longer hang the
  boot when the chipset gates its clock.
- **Installing** (`kernel/apps/terminal.c`, `bootloader/src/main.c`,
  `kernel/drivers/nvme.c`): the Terminal's `install [disk] [/fat]` does
  what the Setup app does; the first start from an installed disk adds a
  "NovaOS" firmware boot entry and puts it first in BootOrder; an Intel
  VMD controller hiding the NVMe disks is named in the log and by
  `install`, with the firmware setting that turns it off.
- **Test.**  The devices suite boots a "laptop" (`tests/acpi/laptop.asl`,
  QEMU without S3): the battery through the embedded controller (a model
  of one in `ec.c`, as QEMU has none), the lid sleeping it in S0 idle and
  waking it, `install` from the USB stick onto an NVMe disk, and the first
  start from that disk adding its boot entry.
