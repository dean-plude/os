## Saving drive C: without holding the locks

Drive C: lives in memory and is written to its disk once it has been quiet
for a second.  Until now the desktop thread did that write itself, holding
the desktop lock, the file-system lock and the big kernel lock for the
whole disk write: in QEMU a 32 MiB file kept every window, every file call
and most system calls waiting for 1.1 s, and a 64 MiB one for 300-600 ms
(the [timer wake-up](#timer-wake-ups-preempt-the-running-thread) work found it).

- **Snapshot, then write unlocked.**  `kernel/fs/persist.c` now takes the
  file-system lock only long enough to copy the changed part of the tree
  (names, attributes, times, security descriptors and the list of what
  each changed folder should still hold).  File contents are not copied:
  `RamfsLend` lends the file's buffer to the save, and a program that
  writes to a lent file gets its own copy first (copy on write, in
  `kernel/fs/ramfs.c`).  The disk write then runs on its own kernel
  thread, `persist`, holding only a save lock (after desktop and
  file-system in the lock order) and no kernel lock.  Anything that could
  not be saved is remembered and tried again on the next save.
- **Drivers lock per request.**  AHCI now keeps a busy flag per disk, as
  NVMe already did, and USB mass storage takes the kernel lock per 64 KiB
  chunk, so a save does not stall other disks or USB input.
- **A power cut leaves the old file or the new one.**  The FAT driver
  writes the new cluster chain and the FAT before the directory entry
  that points at it, and frees the old chain only after the entries are
  written and the disk has flushed.  In 38 trials that killed QEMU before,
  during and after a 48 MiB save, the file always came back whole, old or
  new.  A kill in the middle of the write can leave the clusters it had
  already filled marked as used but belonging to no file (`fsck.fat`
  reclaims them; 5 of the 38 trials); reclaiming them at mount is a
  separate step.
- **Measured** with the new `savetest` self-test in QEMU (TCG, 2
  processors): with a 32 MiB file the save held the file-system lock for
  0.2-0.8 ms (was 1127 ms), the longest wait for the desktop or
  file-system lock fell from 1127 ms to under 75 ms, and for the kernel
  lock from 10 ms to 2-4 ms.  The save itself still takes 360-380 ms, but
  nobody waits for it.
