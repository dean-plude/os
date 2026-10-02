## Installing NovaOS on a disk

The ISO is also the installation disc.  Booted from it, NovaOS runs
live (nothing is kept after a restart) and opens **Install NovaOS**
(`kernel/apps/setup.c`, the engine in `kernel/fs/setup.c`); an icon on
the desktop brings it back.  On an installed system it is in the Start
menu and `start setup` in the Terminal opens it, to copy NovaOS to
another disk.

- **Welcome, choose a disk, confirm, install, finish.**  Setup lists the
  SATA disks with their size and what is on them, marks the one NovaOS
  started from and the one drive C: is kept on, and asks before erasing
  anything.  Disks under 256 MB are shown but cannot be picked.
- **What it writes**: a GPT (protective MBR, primary and backup headers
  and entry arrays with their CRCs) with two partitions: an EFI System
  Partition (FAT32 `NOVA_EFI`, 128 MiB) holding `\EFI\BOOT\BOOTX64.EFI`
  and `\EFI\NOVA\kernel.elf`, and a basic data partition (FAT32
  `NOVADATA`, the rest of the disk) where drive C: is saved.  The copied
  kernel is read back and compared.  UEFI firmware finds
  `\EFI\BOOT\BOOTX64.EFI` by itself, so no boot entry is written.
- **Where the files come from**: booted from the disc, the bootloader
  sees a CD-ROM node in its device path and hands the kernel the two
  boot files in memory (boot protocol v3).  On an installed system Setup
  reads them from the disk NovaOS started from.
- **Your session comes along**: from the disc, drive C: lives on a blank
  disk that NovaOS formatted at boot, often the very disk being
  installed on.  Setup stops saving there before erasing it, then
  writes everything on C: to the new data partition and keeps saving
  there.  A C: kept on another disk stays where it is.
- **Restart now** reboots into the installed system (remove the disc
  first, or pick the disk in the firmware's boot menu).
- Tested in QEMU/OVMF: installing from the ISO onto a blank 1 GB disk,
  booting that disk alone with a file made in the live session still in
  Documents, and installing from the installed system onto a second
  disk.  Both new FAT volumes pass `fsck.fat`, and the GPT's checksums
  verify.
- To try it:

```bash
truncate -s 1G disk.img
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -drive file=disk.img,format=raw -cdrom nova.iso
# after installing: the same command without -cdrom starts from disk.img
```
