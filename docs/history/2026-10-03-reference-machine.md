## A reference machine for real hardware (Phase 21.1)

Phase 21 takes NovaOS from QEMU to one real PC.  The first step picks
that PC and lists what NovaOS runs on it.

- **The machine.**  The Lenovo ThinkPad T14 Gen 4 (Intel) with
  integrated graphics: wired Intel Ethernet (I219), one NVMe SSD, xHCI
  USB, HD Audio with a Realtek ALC3287 codec, a lid, a battery, a
  TrackPoint and a touchpad, sold in large numbers and fully specified
  by Lenovo.  [docs/hardware.md](hardware.md) lists each of its devices
  with the Linux driver for it and NovaOS's status: supported (NVMe,
  xHCI, PS/2 keyboard and TrackPoint, ACPI, battery, lid, timers, UEFI
  boot), partial (the display is the firmware's GOP framebuffer in one
  mode; the touchpad works only as a PS/2 mouse) or missing (the I219
  Ethernet controller, HD Audio on a controller with an audio DSP, which
  reports class 04.01 instead of 04.03, a serial console, Wi-Fi,
  Thunderbolt, camera, fingerprint reader, TPM).  The rest of Phase 21
  fills in the missing rows the gate needs.
- **`devices`.**  A new Terminal command (also `lspci`) lists every PCI
  function NovaOS found at boot with its ID, vendor, class and the
  driver that took it, functions without one in red, and a count.  Each
  PCI driver now records what it claimed (`PciClaim` in
  `kernel/hal/pci.c`): the boot display (whichever driver draws through
  it, or the GOP framebuffer), AHCI, NVMe, the four USB host
  controllers, HD Audio, `e1000`/`e1000e`, virtio-net, virtio-gpu and
  virtio-input.  The PCI table holds 128 functions (was 64; a laptop has
  30 to 60) and says so when a machine has more.
- **Test.**  The core suite's `devices` checks that QEMU's AHCI
  controller, VGA card, HD Audio card and xHCI controller show their
  drivers and the host bridge shows as a bridge.
