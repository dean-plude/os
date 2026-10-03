## USB mass storage (Phase 18.2)

- **Bulk-Only Transport + SCSI** (`kernel/drivers/usbmsc.c`): INQUIRY,
  TEST UNIT READY, REQUEST SENSE, READ CAPACITY (10 and 16), READ/WRITE
  (10 and 16) and SYNCHRONIZE CACHE, with stall handling and reset
  recovery.  Each stick becomes a removable block device (`usb0`, `usb1`,
  ...); `BlockUnregister` takes it off the list when it is pulled.
- **Drives** (`kernel/fs/drives.c`): a removable disk's FAT12/16/32 and
  NTFS volumes (whole disk, MBR or GPT) are mounted as the next free drive
  letter when it arrives, and unmounted when it goes
  (`RamfsUnmountDrive`: nodes still held stay valid but read nothing).
  Fixed disks still only mount NTFS, since their FAT volumes are NovaOS's
  own.  Mounts are read-only for now.
- **File Explorer** lists the other drives (D: to Z:) under the places in
  its sidebar, names them by label in the title and breadcrumb, and goes
  back to This PC when the drive it shows is unplugged.
- Tested in QEMU with a FAT32 (MBR) `usb-storage` stick present at boot
  (`dir`, `type`, Explorer, Notepad), unplugged and plugged back in with
  `device_del` / `device_add`, and an NTFS stick added while running.
