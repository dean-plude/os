## Space a power cut left marked as used comes back at boot

A power cut in the middle of saving drive C: never loses a file (the old
copy or the new one comes back), but it could leave the clusters the save
had already filled marked as used in the FAT with no file pointing at
them, and nothing gave them back: a machine that lost power during saves
slowly filled up.

- **The FAT says when to look.**  `kernel/fs/fat.c` now keeps the
  volume's clean-shutdown bit in FAT[1], as Windows does: it is cleared
  on the disk (and flushed) before the first FAT or directory sector of a
  change is written, and set again once a save has flushed everything.
  A volume mounted with it clear was cut off mid-save; one mounted with
  it set is not scanned, so an ordinary boot costs nothing.
- **Reclaimed at mount.**  When drive C:'s volume was not closed
  cleanly, `FatReclaim` reads the whole FAT once, walks every directory
  from the root marking each file's and directory's chain, and frees
  every cluster marked as used that nothing reached (bad clusters and
  reserved values stay).  A chain that runs into a cluster already reached
  (two files sharing clusters, or a loop) stops there and is left alone,
  so a reachable cluster is never freed; a read error or a lack of memory
  frees nothing.  The boot log says what happened:
  `[PERSIST] Drive C: was not closed cleanly: reclaimed N cluster(s) ...`.
  Other FAT volumes (USB sticks, the boot partition) are only mounted,
  never repaired, and keep their bit as they found it.
- **Tested** by the core self-test `power cut`: it stops QEMU, adds three
  chains no file reaches (66 clusters) to the data disk with the bit
  clear, as a cut-off save leaves them, and resets the machine; NovaOS
  must free exactly those and end up with the free-cluster count
  `fsck.fat` finds on a copy of the same disk.
