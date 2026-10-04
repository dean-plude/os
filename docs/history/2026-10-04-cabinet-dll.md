## cabinet.dll: installers extract their cabinets

The Visual C++ Redistributable's installer (a WiX Burn bundle, which GOG
GALAXY needs for `mfc140u.dll`) read its manifest through MSXML and then
stopped: it loads `cabinet.dll` to unpack the payloads attached to its
`.exe`, and NovaOS had none.  It has one now.

- **The File Decompression Interface**: `FDICreate`, `FDIIsCabinet`,
  `FDICopy` and `FDIDestroy` (cdecl, at Windows' ordinals 20 to 23, in
  `System32` and `SysWOW64`), with the caller's own memory and file
  functions, so a cabinet inside another file (Burn's attached container)
  works through the caller's offset.  `FDICopy` sends `fdintCABINET_INFO`,
  `fdintCOPY_FILE` (the caller returns a handle, 0 to skip, -1 to stop),
  `fdintCLOSE_FILE_INFO` and `fdintPARTIAL_FILE`, follows a folder into
  the next cabinet of a set (`fdintNEXT_CABINET`, the caller may change the
  path) and extracts that cabinet's files too; errors come back in the
  caller's `ERF` (`FDIERROR_CABINET_NOT_FOUND`, `NOT_A_CABINET`,
  `CORRUPT_CABINET`, `BAD_COMPR_TYPE`, `USER_ABORT`...).  Header
  `userland/include/fdi.h`.  Not yet: the compression side (FCI), Quantum
  compression, `FDITruncateCabinet`.
- **The decompressors** are the Windows Installer's own (`userland/msi/cab.c`
  for stored and MSZIP blocks, `userland/msi/lzx.c` for LZX), compiled
  into `cabinet.dll` as well, so no new code had to be borrowed.  They now
  also join a data block split between two cabinets (the first part, with
  an uncompressed size of 0, ends one cabinet; makecab writes sets that
  way), which `msi.dll`'s multi-cabinet media gets too.
- **Self-test** `cabtest` (core 148, 64- and 32-bit): MSZIP and LZX
  cabinets, a skipped file, an attached container, a two-cabinet set and
  the failures.  Its cabinets come from `tools/make_cabtest_data.py`, with
  a small LZX encoder of its own; the output was checked with cabextract
  and 7-Zip.
- **More 32-bit DLL slots**: with `cabinet.dll` and `msxml6.dll` both in,
  the 41 automatic 16 MiB slots for 32-bit DLLs (0x97000000 to
  0xC0000000) ran out and the build stopped; they now go up to
  0xF0000000.
- **Where the Visual C++ Redistributable stops now** (built with MSXML 6):
  Burn extracts its manifest through `cabinet.dll` and then cannot find
  the manifest's `UX` element (0x80070490): it selects `UX` with no
  prefix in a document whose elements are in a default namespace, which
  MSXML 3's XSLPattern matches and XPath does not.
