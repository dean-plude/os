## Phase 4 — VFS & InitRD
- VFS abstraction layer (`VfsMount`, `VfsOpen`, `VfsRead`, `VfsReadDir`, `VfsStat`)
- NT path alias resolution (`\??\C:\`, `\SystemRoot`, `\DosDevices\C:` → first mount)
- CPIO newc InitRD driver (`InitrdMount`): parses in-memory CPIO archive, mounts as `\Device\InitRD`
- Boot protocol v2: bootloader passes `initrd_base`/`initrd_size` in `BootInfo`
- File I/O via IRP dispatch: `NtReadFile`, `NtWriteFile` fully wired through VFS to InitRD
