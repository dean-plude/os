# Installing and power on a laptop (Phase 21.5)

The [reference machine](hardware.md), a Lenovo ThinkPad T14 Gen 4 (Intel),
differs from QEMU in four ways that matter for installing NovaOS and for
its power handling:

| On the T14 | What NovaOS does |
|---|---|
| The disk is NVMe, possibly behind Intel VMD ("RST") | The NVMe driver installs to it and starts from it. VMD hides the disk; NovaOS names the VMD controller in its log and in `install`, and the firmware setting turns it off |
| The firmware starts what is in its boot entries | The installer writes `\EFI\BOOT\BOOTX64.EFI`, which firmware starts from a disk it has no entry for; the first start from the disk adds a "NovaOS" boot entry and puts it first |
| The lid, battery and charger sit behind the embedded controller | The ACPI embedded controller (`kernel/hal/ec.c`) gives uACPI the controller's registers and runs its events |
| No S3 sleep (Modern Standby firmware), and maybe no HPET | Sleep is low-power S0 idle with the LPS0 device's calls; the timers come from CPUID leaf 0x15 when there is no HPET |

All of it is checked in QEMU (the devices suite's `laptop` boot, below);
the T14 itself can only be checked by hand, with the steps at the end.

## Installing

The Setup app (Install NovaOS on the live desktop) and the Terminal's
`install` command do the same thing.  `install` alone lists the disks:

```
C:\Documents> install
  nvme0n1  488386 MiB  SAMSUNG MZVL2512HCJQ-00BL7  Volumes: SYSTEM, Windows
  usb0  14800 MiB  SanDisk Ultra  Volumes: NOVA_EFI  (NovaOS is running from it)
Usage: install <disk> [/fat]   (drive C: on NTFS, or on FAT32 with /fat)
```

`install nvme0n1` erases that disk, makes a GPT with an EFI System
Partition and a partition for drive C: (NTFS, or FAT32 with `/fat`), and
copies the boot loader and the kernel from the USB stick.

**Intel VMD.**  With VMD on (the RAID or "Intel RST" storage setting of
many laptops' firmware), the NVMe disk sits behind the VMD controller and
is not on the PCI bus; NovaOS has no VMD driver.  The boot log then says
`[NVME] Intel VMD at 00:0e.0 (8086:a77f) hides the NVMe disks behind it`,
`install` says the same, and turning VMD off in the firmware setup shows
the disk.  (Windows installed with VMD on may need its storage driver
changed before it starts with VMD off; NovaOS replaces it anyway.)

**The firmware's boot entry.**  The installer only writes the
removable-media path `\EFI\BOOT\BOOTX64.EFI`.  Firmware starts that from
a disk it has no entry for, and its boot menu (F12 on a ThinkPad) lists
the disk.  The first time NovaOS starts from the disk, its boot loader
adds a `Boot####` variable named "NovaOS" for itself and puts it first in
`BootOrder`, so the machine keeps starting NovaOS once the stick is out
(the log says `[SETUP] Added the firmware boot entry "NovaOS" for this
disk`, and on later starts that the firmware has one).  An old "Windows
Boot Manager" entry stays in the list, pointing at a partition that is
gone; firmware skips it.

## The embedded controller

A laptop's firmware describes its lid (`_LID`), battery (`_BIF`, `_BST`)
and charger (`_PSR`) in AML that reads fields of an `EmbeddedControl`
operation region: registers of the embedded controller, a small
microcontroller on two I/O ports (0x62 and 0x66 on most PCs).  uACPI
interprets the AML but has no controller of its own; `kernel/hal/ec.c` is
its address space handler for that region.  It finds the controller in
the ECDT table or as the PNP0C09 device (its `_CRS` gives the ports),
reads and writes each byte with the RD_EC and WR_EC commands, takes the
ACPI global lock when `_GLK` asks, and lets the controller's AML know it
can use the region (`_REG`) before any `_INI` method runs.

The controller raises an event (its GPE, from `_GPE` or the ECDT) when
the lid moves, the charger is plugged in or the battery changes; NovaOS
asks it which one (QR_EC) and runs the firmware's `_Qxx` method for it,
which usually notifies the lid or the battery.  The events are polled
once a second as well, in case an edge goes missing.  The log shows the
controller (`[EC] \_SB_.PCI0.LPCB.EC__: ports 0x62/0x66 (from the ECDT),
GPE 0x...`) and each kind of event the first time it comes
(`[EC] Event 0x2a: _Q2A`).

## Sleep without S3

Laptops sold for Windows' Modern Standby, the T14 Gen 4 among them, often
have no S3 (`\_S3`) at all.  On those, when the FADT has the low-power S0
idle flag or the firmware has an LPS0 device (`INT33A1` or `PNP0D80`),
Sleep (on the Start menu, or closing the lid) is low-power S0 idle:

1. The screen goes black.
2. The LPS0 device's `_DSM` is called the way Linux calls it: Intel's
   functions 3 (screen off) and 5 (entry), and Microsoft's 3 and 7 where
   the firmware has them, so the platform can lower its idle power.
3. The desktop waits while the CPUs halt between interrupts.  Opening the
   lid or pressing the power button wakes it; so does a key or the mouse
   if the lid was open.
4. The LPS0 exit calls (6, then 8 and 4), and the desktop is drawn again.

Devices stay powered and programs keep running, so this saves less than
S3 would: what it turns off is what the platform does with the LPS0
calls and the screen's picture, not the backlight (that needs a graphics
driver).  `GetPwrCapabilities` reports `AoAc` (Modern Standby) there
instead of S3.  On firmware that has S3, sleep is S3 as before.

## Timers without an HPET

NovaOS calibrates the TSC and the local APIC timer against the HPET.
Some laptop firmware hides the HPET, and recent Intel chipsets may gate
the 8254 PIT's clock as well, so the old fallback, the PIT, could wait
forever.  Without an HPET, NovaOS now takes both clocks from CPUID leaf
0x15 (the core crystal's frequency and the TSC's ratio to it, or leaf
0x16's base frequency where the crystal isn't given, as Linux does), and
only then measures against the PIT, giving up after a few seconds.  The
log says which: `[APIC] Timer: ... from CPUID 0x15 (no HPET)`.

## The QEMU test

The devices suite's `laptop` boot (`tests/selftest/devices/laptop`)
starts QEMU without `\_S3` or an HPET, with `tests/acpi/laptop.asl`: an
embedded controller holding the lid, a battery and the charger, an LPS0
device, and a thermal zone that passes the lid position the test sets to
the controller.  QEMU emulates no embedded controller, so NovaOS serves
the one with `_HID` `NOVA0EC1` with a model of one in `ec.c` (the real
command and status protocol, with an event the AML can raise).  The boot
has the ISO on a USB stick and an empty NVMe disk:

- `battery` reads 60% and 4 hours through the controller;
- the lid closes, NovaOS sleeps in S0 idle (with the LPS0 calls), and
  opening it wakes the machine;
- `install nvme0n1` installs NovaOS on the NVMe disk, and after a
  restart the machine starts from that disk and adds its boot entry.

```bash
python3 tools/selftest.py --suite devices --only 'ec battery,lid sleep (S0 idle),install on nvme,start from nvme'
```

## Checks on the T14

Start from the USB stick (written as [building.md](building.md) says,
Secure Boot off, F12 for the boot menu).  Running live from the stick,
the log goes to `\EFI\NOVA\bootlog.txt` on it, which another computer
can read; on the installed system the Terminal's `dmesg` shows it.

1. **The disk.**  In the Terminal, `devices` lists an NVMe controller
   with the driver `NVMe`, and `install` lists the disk (`nvme0n1`).  If
   it says Intel VMD hides the disks, turn VMD (Intel RST) off in the
   firmware setup (F1) and start again.
2. **The embedded controller.**  `dmesg` has an `[EC]` line with the
   ports (0x62/0x66) and a GPE, and `[ACPI] Battery`, `[ACPI] AC adapter`
   and `[ACPI] Lid` lines.  `battery` prints the charge and "Power
   source: AC"; unplug the charger and run it again within a few
   seconds: "Power source: battery" and a time left.
3. **Timers.**  `dmesg` has the `[APIC] Timer:` line (the HPET, or CPUID
   0x15), and `sleeptest timer` passes.
4. **Sleep.**  Close the lid for ten seconds, then open it: the screen
   was dark and the desktop comes back.  `dmesg` shows `[SHELL] Lid
   closed: sleeping`, `[SLEEP] No S3 on this machine: sleeping in
   low-power S0 idle`, the LPS0 lines and `[SLEEP] Woke up after N s
   (the lid opened)`.  (Firmware with S3 shows `[SLEEP] Woke up after N
   s` from S3 instead.)
5. **Install.**  `install nvme0n1` (this erases the disk, Windows with
   it), then shut down, take the stick out and switch on: NovaOS starts
   from the disk, and `dmesg` shows `[SETUP] Added the firmware boot
   entry "NovaOS" for this disk`.  If the firmware starts something else,
   choose the disk in the F12 menu once.
6. **Sleep again, installed.**  Repeat check 4 on the installed system.

If a check fails, the `[EC]`, `[ACPI]`, `[SLEEP]`, `[APIC]` and `[SETUP]`
lines of the log (or `bootlog.txt` from the stick) say how far it got.
