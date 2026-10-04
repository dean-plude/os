## Big kernel blocks need no long run of free memory; programs see the real memory

App corpus round five.  The first corpus runs on KVM after round three
(run 57) and on round four's own branch (run 60, KVM) confirm the earlier
fixes: Audacity records and saves (the floating-point fix holds under
KVM), Roblox installs, and OpenTTD now installs and reaches its menu (its
installer had stopped with exit code 2 in every run before round four,
with memory short).  Run 60 passed 21 of 23 programs.  The two left both
came down to memory:

- **Firefox could not be unpacked with 735 MB free.**  7-Zip stopped on
  `xul.dll` (164 MB) with `ERROR_DISK_FULL`, and Firefox then said
  "Couldn't load XPCOM".  A file on drive C: is one block of kernel
  memory, and a large block was one physically contiguous run of pages:
  after hours of programs coming and going, the free memory was in pieces
  and no 164 MB run was left.
- **Krita used up the last of the memory** (`[PMM] out of memory: 1
  page(s) wanted, 0 MiB free`) and crashed in `gdi32` when a page it had
  been promised could not be given.  The corpus machine has 4 GB and
  drive C:, which NovaOS keeps in memory, held 2.9 GB of programs and
  installs by then.

What changed:

- **Mapped blocks.**  A kernel block of 4 MB or more (drive C:'s files,
  a DLL's image while it loads), or any large block when no run of pages
  is free, is now single pages from anywhere, mapped one after another
  into a window of kernel addresses (physmap + 384 GiB, 128 GiB in 2 MiB
  slots).  `kresize` grows such a block in place into the slots after it
  and gives pages back from its end; freed slots are reused after one TLB
  shootdown for all of them.  Big blocks also stop using up the long runs
  that other large blocks still need.
- **Programs are told the machine's real memory.**  `GlobalMemoryStatusEx`
  said 512 MB total and 256 MB free whatever the machine had, and
  `NtQuerySystemInformation` 2 GB of pages.  The kernel now publishes the
  machine's RAM in KUSER_SHARED_DATA's `NumberOfPhysicalPages` and its
  free pages in a NovaOS field (0x7FFE0F08, kept current on every timer
  tick); `GlobalMemoryStatusEx`, `GlobalMemoryStatus` (capped at 4 GB - 1
  for 32-bit programs, as on Windows), `K32GetPerformanceInfo` and
  `SystemBasicInformation`/`SystemPerformanceInformation` read them.
  `GlobalMemoryStatusEx` fails with `ERROR_INVALID_PARAMETER` on a wrong
  `dwLength`, as Windows does.
- **The App Store reports a failed unpack.**  It said "Installed" when the
  program's `.exe` was there even if 7-Zip stopped with an error (exit
  code 2 and up), so the corpus failed later, on a cut-off `xul.dll`.
- **The Terminal's `mem`** counts the machine's RAM as its total (it
  counted the holes below the highest address too: 6144 MB on a 4 GB
  machine).
- **The corpus machine** has 6 GB of memory and a 6 GB data disk (was 4
  GB and 3 GB): the corpus copies 2.3 GB of programs onto drive C: before
  the first one starts, which left about 1 GB for the programs, and the
  data disk could no longer hold drive C: ("Could not save ... (disk
  full?)").

- **Test.**  Core self-test `ramdisktest` also checks what programs are
  told: `GlobalMemoryStatusEx`'s total is `SystemBasicInformation`'s
  pages and more than 1 GB, its free memory is drive C:'s free space and
  falls by the 105 MiB its two files take, its memory load matches, and a
  wrong `dwLength` fails.  Its 72 MiB and 33 MiB files are mapped blocks.
