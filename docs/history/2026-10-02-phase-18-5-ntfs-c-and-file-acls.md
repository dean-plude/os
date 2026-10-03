## NTFS as drive C: and file ACLs (Phase 18.5)

Drive C: can now be kept on NTFS, with each file's security descriptor,
and the kernel enforces those DACLs.

- **`$Secure` writing** (`kernel/fs/ntfs.c`): `NtfsAddSecurity` stores a
  descriptor once (found again by hash and bytes), appending it to `$SDS`
  and its mirror 256 KiB on, never across a 256 KiB block, and adds it to
  the `$SII` and `$SDH` view indexes; `NtfsSetSecurityId` points a file's
  standard information at it.  The index code takes any named index now,
  view-index roots are kept small so record 9 doesn't fill, `$SDS` grows
  64 KiB at a time, allocations extend a stream's last run when they can,
  and attributes of one type are kept in name order (ntfs-3g looks them up
  that way).  `NtfsSetInfo` saves times and attribute bits and
  `NtfsLookup` finds a name in a folder.  New volumes give the root
  `CREATOR OWNER` full control, inherited, as Windows' C:\ has.
- **File security** (`kernel/fs/fsec.c`): a RamNode may carry a
  self-relative descriptor; one without inherits from the nearest folder
  above that has one (object- and container-inherit ACEs, no-propagate,
  `CREATOR OWNER`).  `FsecAccess` checks a request against the user's
  token SIDs with the file generic mapping; the owner can always read and
  change the DACL.  With no descriptor anywhere above, as on FAT, anyone
  may do anything.
- **Syscalls** (`kernel/um/um_syscall.c`): opening, creating, overwriting,
  deleting, renaming and setting basic information check the DACL (delete
  falls back to the parent's `FILE_DELETE_CHILD`; `MAXIMUM_ALLOWED` gets
  what is granted).  A descriptor given in `OBJECT_ATTRIBUTES` (from
  `CreateFile`'s or `CreateDirectory`'s `SECURITY_ATTRIBUTES`) is applied
  to the new file.  `NtQuerySecurityObject` (0x155) and
  `NtSetSecurityObject` (0x1A1, their Windows 10 1903 numbers) moved
  from ntdll into the kernel; other
  handles still get the default descriptor.  advapi32's
  `Get/SetNamedSecurityInfo`, `Get/SetSecurityInfo` and
  `Get/SetFileSecurity` are real now.
- **Persistence** (`kernel/fs/persist.c`): an NTFS volume labelled
  `NOVADATA` can hold C:.  C: is its root (on FAT it stays `\NOVA\C`),
  NovaOS's own bookkeeping goes in a hidden `$NovaOS` folder, and each
  file is saved with its times, attributes and security id (one that
  inherits gets the root's id, read back as "inherit").
- **Installer**: the confirm page asks how to keep drive C:, NTFS
  (recommended) or FAT32; NTFS data partitions have no 1 TiB cap.
- **Checks**: `acltest` gained file tests on C:\AclTest (inheritance,
  deny write and delete, rename, `MAXIMUM_ALLOWED`, the owner restoring
  access, a DACL from `SECURITY_ATTRIBUTES`) and leaves `kept.txt`, whose
  DACL a second run after a restart checks.  `scripts/ntfs-check.py`
  checks `$SII`, `$SDH` and `$SDS` against each other and attribute order;
  `check-ntfs-disk.sh` also runs `ntfssecaudit -a`.  In QEMU, NovaOS was
  installed with C: on NTFS to a blank NVMe disk and booted from it;
  `acltest` passed 33 of 33 on both boots and `kept.txt` kept its DACL.
  ntfs-check, `ntfsfix -n` and `ntfssecaudit` found no errors on the
  partition, nor on a host test volume with 5,100 descriptors.
- **Not done**: hard links (`CreateHardLink`) are still to come.
