## NTFS write (Phase 18.4)

- **Writer** (`kernel/fs/ntfs.c`, NovaOS's own code: ntfs-3g and the
  Linux ntfs3 driver are GPL): rewrites a file's data (resident when it
  fits in its MFT record, else in newly allocated clusters, the old ones
  freed once the record points at the new ones), creates files and
  directories (a `$STANDARD_INFORMATION` carrying the parent's security
  id, a Win32 `$FILE_NAME`, an empty `$DATA` or `$I30` index), renames
  and moves them, and deletes files and empty directories (a file with
  other hard links only loses the name).  Directory indexes are rebuilt
  whole on each change, as a B+ tree of INDX blocks packed bottom up
  (`$UpCase` collation), in the index root alone while it fits; the
  `$INDEX_ALLOCATION` grows, and is given back when the index fits in the
  root again.  The MFT grows 64 records at a time.  The cluster and MFT
  bitmaps are kept in memory and written through; records 0-3 are
  mirrored to `$MFTMirr`.  There is no journal: `$LogFile` is emptied when
  writing starts (as `ntfsfix` does) and each call leaves the volume
  consistent.  Files in an attribute list, and compressed, sparse or
  encrypted files, are not rewritten: the first time something asks
  whether such a file is writable its record is read, and it shows as
  read-only from then on (the directory's copy of its attributes can't be
  trusted for this; ntfs-3g leaves it stale).
- **When it is writable**: a volume Windows left hibernated (a
  `hiberfil.sys` starting `hibr`, which Fast Startup leaves too), marked
  dirty, or with unfinished transactions in `$LogFile` stays read-only.
- **Drives** (`kernel/fs/drives.c`, `ramfs.c`): `RamfsSource` gained
  create, remove, rename, write and free-space calls.  Creating, deleting
  and renaming on a writable drive go to the disk at once; a file's new
  contents are written when nothing holds it any more, after a quiet
  second (`DrivesPoll`), and at shutdown (`DrivesSync` from `UmSaveAll`).
  Volume information reports the real free space and drops
  `FILE_READ_ONLY_VOLUME`.  Renames within drives D:, E:, ... are real
  renames now (they were refused as "another device", so `MoveFile`
  copied and deleted).
- **Checks**: `drivetest` now writes (small, appended and 700 KB files,
  overwrites, renames, moves, a case change, a folder of 300 files, then
  deletes) and leaves two files for the host; `scripts/check-ntfs-disk.sh`
  runs `ntfsfix -n` and `scripts/ntfs-check.py`, a chkdsk-style check of
  the cluster and MFT bitmaps against what the records own, the records'
  headers, attributes and link counts, every `$I30` index (order, leaf
  depth, VCNs, index bitmap, entries against the `$FILE_NAME`s) and
  `$LogFile`.  It reports no errors on volumes mkntfs and ntfs-3g made,
  and none after drivetest and Terminal tests (MFT growth, a 90-file
  folder split across INDX blocks and shrunk back, a USB stick written to
  and pulled out without a sync) wrote to them.
  Windows `chkdsk`, run on a disk NovaOS had written to (3 October),
  reported no errors either.
