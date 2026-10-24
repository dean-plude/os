## NTFS: a rename to a longer name makes room in the file's record

GIMP's and SuperTux's installs failed on the app corpus's NTFS drive C:
(Inno Setup exit code 5). Inno Setup writes each file as `is-XXXXX.tmp`,
then renames it to its real name. A small file's contents are kept in
its MFT record, and when they left no room for the longer name the rename
failed (`STATUS_OBJECT_NAME_INVALID`); this was in NovaOS's NTFS writer
from the start, and the corpus's FAT32 C: had hidden it.

- **`kernel/fs/ntfs.c`**: a rename or a new link (`NtfsRename`,
  `NtfsLink`) whose name does not fit moves the file's resident contents
  to clusters first, as Windows does (`make_room`); the clusters go back
  if the change fails before the record is written.
- **`tests/tools/test_ntfs_many.py`**: 600 files with 600-899 bytes are
  made as `is-XXXXX.tmp`, renamed to long names, every tenth given a
  second long name, then read back after a remount and checked with
  `scripts/ntfs-check.py`. It fails at the first rename without the fix.
