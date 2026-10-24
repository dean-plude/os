- ~~NTFS read~~ ~~NTFS write~~ Done: NTFS volumes mount as drives D:, E:,
  ...; files on them are written, created, renamed and deleted (volumes
  Windows left hibernated or unclean stay read-only).  ~~NTFS as drive C:~~
  Done: the installer formats C: as NTFS (only, since
  2026-10-24), the installed system mounts it in place, and files there keep
  security descriptors that opening, deleting and renaming obey.  ~~Hard
  links and persistence~~ Done (Phase 17.5; `linktest` and `linktest
  restarted`; on a mounted NTFS volume across a restart, with
  `FindFirstFileNameW` and an offline volume check, in the devices
  suite's "ntfs" boot).  ~~Interrupted writes and a full volume~~ Done:
  a volume NovaOS is writing to is marked dirty until the changes are on
  the disk, and after a power cut NovaOS checks and repairs it as chkdsk
  would, leaving it clean (`ntfstest churn`, the machine reset, `ntfstest
  cut`; every cut point on the host, `tests/tools/test_ntfs_cut.py`); a write past the free space fails
  with `ERROR_DISK_FULL` (`ntfstest fill`), both on the "ntfs" boot's
  disposable disk.  ~~Many files~~ Done: the MFT grows by half its size
  (and its bitmap with it), so a volume takes as many files as fit
  (`tests/tools/test_ntfs_many.py`: 40,000), and a rename or link that
  needs room in a file's record moves its small contents to clusters.  ~~NVMe~~ Done: NVMe disks, installed to and booted from.
