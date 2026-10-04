## Drive C: files take the memory their contents need

App corpus round four.  In the first corpus run on a tree with round
three's fixes (nightly run 55, on PR #166's branch; that runner had no
KVM, so it ran under TCG), 19 of 21 programs passed: Audacity records
without dropouts and Krita starts and builds its resource database (round
three's floating-point fix), but Krita then stopped with
`0xc0000139` after `[PMM] out of memory: 16385 page(s) wanted, 61 MiB
free`, and the Firefox install stopped with 7-Zip's "Cannot set length
for output file" (`ERROR_DISK_FULL`).  Both came right after the newly
added Roblox install, the first corpus program to install hundreds of
megabytes onto drive C:, and drive C: is kept in memory.

A file's buffer on drive C: grew by doubling as the file was written, so
appending stayed cheap, and it kept the doubled size for as long as the
file existed: a file written in pieces took up to twice its size.  Setting
a file's length (`SetEndOfFile`, which 7-Zip does before unpacking each
file) also rounded up to the next power of two, so Firefox's 84 MB
`omni.ja` needed one free piece of 128 MiB.

- **Files give the rest back.**  When nothing holds a file any more, the
  pages of its buffer past its contents go back to the machine.
- **Files grow where they are.**  A file whose buffer can't hold a write
  first takes the free pages right after it (new `kresize`, with
  `pmm_claim_pages`), and only when those are in use moves to a new,
  doubled buffer.  Setting the length takes exactly that length.
- **A DLL loaded later no longer copies the DLLs it imports from.**  To
  bind a newly loaded DLL's imports from a module the process already
  had, the loader copied that module's whole image into one piece of
  kernel memory (7 MB for Qt5Core, 164 MB for Firefox's `xul.dll`).  It
  now reads only the module's export directory.  When even that fails
  for lack of memory, the load fails with "Out of memory"; before, every
  import from that module was bound to NovaOS's stub for a missing
  function, which is how Krita ended with `0xc0000139` ("unimplemented
  `QString::fromAscii_helper` in Qt5Core.dll", a function Qt5Core.dll
  has) when its next plugin loaded.
- **Seeing it.**  The Terminal's `mem` also prints how many files drive C:
  holds and the memory they take; the app corpus prints both lines after
  each program.

- **Test.**  Core self-test `ramdisktest`: 33 MiB appended in 64 KiB
  writes takes about 33 MiB once closed (64 MiB before), `SetEndOfFile` to
  72 MiB takes 72 MiB (128 MiB before), contents survive files growing in
  turn, shrinking and growing again and appending to a closed file, and
  deleting gives the memory back.

In the corpus with Roblox, Krita and Firefox alone (TCG), the three left
1,924 MB taken before and 1,263 MB after; drive C: then held 2,021 MB of
files in 2,029 MB of memory.
