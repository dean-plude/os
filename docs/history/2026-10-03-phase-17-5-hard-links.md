## Hard links (Phase 17.5)

A file can have several names now, as `CreateHardLink` makes them on
Windows; `ln` in the MSYS2 shell and Git's object store use it.

- **Drive C:** (`kernel/fs/ramfs.c`): a file's names are a ring of nodes
  that share its contents, size, attributes, times and security
  descriptor, so a write, `SetFileAttributes` or `SetSecurityInfo` by one
  name is seen by the others, open handles included.  Deleting a name
  leaves the file to the others; the last name takes it.  Link counts
  (`FileStandardInformation`, `GetFileInformationByHandle`'s
  `nNumberOfLinks`) and the file id (`FileInternalInformation`,
  `nFileIndex`) are the file's, the same by every name, and the volume
  reports `FILE_SUPPORTS_HARD_LINKS`.
- **Kept across restarts** (`kernel/fs/persist.c`): on an NTFS C: a
  linked file is saved as one record with a `$FILE_NAME` in each
  directory (`NtfsLink`), and loading joins the names of a record with
  several again.  FAT has no links, so each name is saved as a copy and
  `\NOVA\LINKS.TXT` lists the names of each linked file; at boot they are
  joined into one file again.
- **NTFS volumes** (`kernel/fs/ntfs.c`): `NtfsLink` adds a `$FILE_NAME`
  and an `$I30` entry and bumps the link count; deleting and renaming
  take the name concerned (a DOS alias goes with its long name), so the
  other names of a file stay; the record and its clusters are freed with
  the last name.  Mounted volumes (D:, ...) get the same through
  `RamfsSource` link calls, and a file whose record has several names is
  joined to its other loaded names when it is read.
- **Syscalls and kernel32**: `NtSetInformationFile(FileLinkInformation)`
  (refuses directories and other drives, replaces a file under the name
  when asked to, checks `FILE_ADD_FILE` on the folder); `CreateHardLinkA/W`
  call it.
- **Checks**: `linktest` in the core suite, and `linktest restarted` after
  the suite's restart checks a linked pair is still one file.  On a
  disk made by `scripts/make-ntfs-disk.sh`, `linktest D:\LinkTest` then
  `scripts/check-ntfs-disk.sh` (ntfsfix, ntfssecaudit and
  `ntfs-check.py`, which checks link counts against names) pass.
