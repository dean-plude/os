## The I219 Ethernet controller of Intel PCs (Phase 21.3)

The reference machine's wired port is an Intel I219, the Ethernet MAC
built into Intel chipsets since 2015, with its PHY on a separate chip.
NovaOS's `e1000e` driver knew only the 82574L that QEMU emulates.

- **The driver** (`kernel/drivers/e1000.c`) now takes the 55 I219-LM and
  I219-V device IDs from Sunrise Point to Nova Lake chipsets (the
  ThinkPad T14 Gen 4's Raptor Lake-P among them), names each as Windows
  does ("Intel Ethernet Connection (16) I219-LM") and brings it up after
  Intel's BSD-licensed shared code in FreeBSD (`e1000_ich8lan.c`): out
  of D3 and Ultra Low Power mode (through the ME firmware on vPro
  machines, a LANPHYPC power cycle otherwise), the PHY found on MDIO
  (directly, in SMBus mode, or after a power cycle), the reset the MAC
  and the PHY share under the hardware semaphore, the per-generation
  errata, and the settings for the speed the link comes up at.  It
  repeats this after sleep, and logs each step, so a boot log shows how
  far a real machine got.  [docs/ethernet.md](ethernet.md) lists the IDs
  and the steps.
- **Shared with the 82574L**: the PHY over MDIO (its ID in the boot
  log), auto-negotiation at 10, 100 and 1000 Mb/s, link changes logged
  with speed and duplex, the BAR mapped wherever the firmware put it,
  and the PCI function woken from D3.
- **Test.**  The network suite boots a third time, with QEMU's e1000e
  instead of virtio-net (`tests/selftest/network-e1000e`): the PHY and
  link in the boot log, the link pulled and plugged back (`ipconfig`
  shows the media disconnected, then the address again), `ping`, sleep
  and wake with `ping` after it, and the IPv4 tests (Winsock, winhttp's
  HTTP/2, `looptest`, `prioritytest net`) on it.  The I219's own steps
  can only be checked on the machine itself.
