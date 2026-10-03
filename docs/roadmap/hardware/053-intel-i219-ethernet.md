- ~~Drive the reference machine's Intel I219 Ethernet controller~~ Done
  (Phase 21.3): the `e1000e` driver takes every I219-LM and I219-V
  (55 IDs, [ethernet.md](ethernet.md)) with the PHY bring-up from
  Intel's BSD-licensed code (ULP exit, LANPHYPC, the shared reset,
  per-chipset errata); tested in QEMU on the 82574L, which shares the
  rings and the PHY path.  Still to confirm on the T14 itself.
