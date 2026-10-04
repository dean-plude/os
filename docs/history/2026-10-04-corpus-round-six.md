## Drive C: lets go of saved files when memory runs short

App corpus round six.  Drive C: lives in memory and is saved to the data
disk, and until now every file on it stayed in memory for good: the 2.3 GB
of programs the app corpus copies to `C:\Apps` were all read in at boot,
and every program installed afterwards added its files on top.  In the
newest full corpus run on a 6 GB machine (nightly run 71, under KVM),
drive C: held 3.7 GB by the time Steam had updated itself, 91 MB were
left, and the twelve programs after it failed: 7-Zip could not set the
length of Firefox's files, Krita and Notepad++ could not be loaded, and
PuTTY was out of memory.  Round five had raised the corpus machine to
10 GB to make room.

- **Saved contents are let go of.**  When free memory falls below an
  eighth of the machine's, the "persist" thread lets go of the contents of
  saved files of C: (64 KiB or larger) that no handle, window or mapped
  view holds, those unused longest first, until a fifth of memory is free
  again, as Windows drops a file's cached pages.  The file keeps its name,
  size, times and security descriptor; only its bytes leave memory.  It
  happens only while no save is writing and when the last save wrote
  everything it took, so the data disk holds exactly what memory held.
  Each file's place on the disk (its first FAT cluster or NTFS record) is
  looked up then and checked against its size.
- **Read back when wanted.**  Opening the file to read, write, map or run
  it, a hard link to it, and the program loader read the contents back
  from that place (`RamfsLoad`, as for files on mounted drives).  Opening
  it only for its details (`FILE_READ_ATTRIBUTES`) reads nothing, and
  emptying it (`TRUNCATE_EXISTING`) needs nothing read.  A file renamed,
  moved or given new details while only on the disk is read back before
  the save that writes it under its new name.
- **Restored without reading.**  At boot, files of 64 KiB or more are
  restored to C: with their contents left on the data disk until wanted
  (`[PERSIST] Restored N file(s) to drive C:; M MB of them are read when
  wanted`).
- **Changing the save disk.**  Before Setup moves C: to another disk, or
  saving stops, every file's contents are read back first.
- **Asking for it.**  `NtSetSystemInformation(SystemMemoryListInformation)`,
  as RAMMap and EmptyStandbyList call it, with
  `SeProfileSingleProcessPrivilege`: `MemoryFlushModifiedList` saves C:,
  `MemoryPurgeStandbyList` lets go of every saved file nothing holds.
- **Seeing it.**  The Terminal's `mem` prints how much of C: is in memory
  and how many files have been let go of so far.
- **Commit limit.**  A program could commit up to 7/8 of the physical
  page range, holes below the highest address included (6144 pages' worth
  more than RAM on a 4 GB machine), so a page promised past the RAM became
  an access violation when first touched (Krita's in run 60).  The limit is
  now 7/8 of the RAM, and a commit past it fails up front, as on Windows.
- **The corpus machine** is back to 6 GB (data disk 12 GB).

- **Test.**  New core self-test `cachetest` (runs as administrator for the
  privilege): files saved and let go of give their memory back and read
  back as they were through `ReadFile` and a mapped view, opening one for
  its details reads nothing back, files renamed, hard-linked and emptied
  while only on the disk read back right once saved and let go of again,
  and after `shutdown /r` C: is restored without reading them and
  `cachetest after` reads them back.
