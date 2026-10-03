## Starting from a USB stick, with the boot log on the stick (Phase 21.2)

Phase 21 takes NovaOS from QEMU to a real PC.  Its second step is the
way in: `nova.iso` written to a USB stick, started on UEFI firmware with
Secure Boot off and the firmware's GOP framebuffer as the display.

- **The ISO is a USB stick image now.**  `scripts/create-iso.sh` passed
  `-isohybrid-gpt-basdat`, which on its own (it needs `-isohybrid-mbr`)
  writes no partition table at all: the ISO started as a disc but was
  just bytes to firmware when written to a stick.  The EFI System
  Partition image is now also a partition of a GPT (partition 2)
  behind a protective MBR (`-efi-boot-part --efi-boot-image`), the same sectors the El
  Torito boot entry points at, so the ISO starts from a stick and still
  from a disc.  (An appended partition, the other common layout, broke
  the disc: firmware sizes a disc's El Torito image from the ISO9660
  volume, which an appended partition lies outside.)
- **Live from the stick.**  The bootloader told the installation disc
  from an installed disk by the CD-ROM node in its boot device's path.
  The ISO's ESP now carries `\EFI\NOVA\bootlog.txt`, which the installer
  does not copy, so a boot volume holding it is the installation media
  wherever it is; a USB node in the path adds `BOOT_FLAG_LIVE_USB`.
  NovaOS then runs live as from the disc, opens Install NovaOS ("You
  are running NovaOS from the installation USB stick"), and the disk
  list marks the stick as the disk NovaOS started from.  Installing from
  the stick onto an NVMe disk and starting from that disk was checked in
  QEMU.
- **GPTs with more than 128 entries.**  xorriso writes 248; drive C:'s
  search and the drive letters skipped such disks, so the stick's ESP
  never mounted.  Both now read up to 1,024 entries.
- **The boot log on the stick** (`kernel/fs/bootlog.c`).  Most laptops
  have no serial port, so after a hang there was nothing to read.  The
  kernel now keeps its output from the start, whole up to about 1 MiB
  (`klog_boot_text`, beside the Terminal's 16 KB `dmesg` ring).  When
  the stick NovaOS started from appears, the log so far is written into
  `bootlog.txt` (1 MiB set aside on the ISO, its clusters one after
  another), then what follows at most once a second, before a restart or
  shutdown, and once more after a kernel fault (a page fault or another
  exception in kernel mode) if the stick is free.  Writing in place,
  sector by sector, changes no FAT metadata, so the stick's read-only
  drive letter (`NOVA_EFI`) stays right.  Read it on another computer:
  the stick's EFI partition, `EFI\NOVA\bootlog.txt`.
- **Tests** (devices suite): `usbboot` starts with nothing but the ISO
  written to a 2 GiB USB stick on xHCI and QEMU's `ramfb` (a display
  only the firmware's GOP drives, like a laptop's integrated graphics);
  it checks the live start, the GOP display, the log file being found
  and the stick's drive letter, then reads `bootlog.txt` off the stick
  image with mtools and finds the end of the boot and the Terminal's
  program in it, and, after `crash kernel`, the fault and its symbolized
  backtrace.  `cdboot` starts from the same ISO as a disc.

On the reference machine (the Lenovo ThinkPad T14 Gen 4, Intel), the
manual check is: write the ISO to a stick, turn Secure Boot off in the
firmware setup (F1), start from the stick with F12, reach the desktop
and Install NovaOS, then shut down and read `EFI\NOVA\bootlog.txt` on
another computer.  It has not been done on the machine yet.
