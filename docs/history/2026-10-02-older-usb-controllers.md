## Older USB controllers: EHCI, OHCI and UHCI

USB devices now work on every kind of PC USB controller, not only xHCI,
and on any number of controllers at once, so machines from before about
2012 (and virtual machines that emulate only USB 1.1 or 2.0) get their
keyboards, mice, hubs and sticks too.

- **USB core** (`kernel/drivers/usb.c`, `usb_hc.h`): what used to be part
  of the xHCI driver and is the same for every controller moved into a
  core of its own: enumeration (SET_ADDRESS, or xHCI's Address Device),
  descriptors, a pipe per endpoint, offering interfaces to the hub, HID and
  mass-storage drivers, taking devices away, and the `usb` thread that
  watches the root ports.  A controller driver supplies root-port reset,
  control, bulk and interrupt IN transfers, and its own per-device and
  per-endpoint state (`UsbHcOps`).  The class drivers (`usb.h`) are
  unchanged.
- **xHCI** (`xhci.c`): its state moved into a per-controller structure,
  so every xHCI controller is started, each a bus of its own (`usb1 port
  3` in the log).  On Intel 7- to 9-series chipsets it takes the shared
  USB 2 ports over from EHCI, as Windows does.
- **EHCI** (`ehci.c`, USB 2): a queue head per endpoint, control and bulk
  on the asynchronous ring and interrupt ones on a periodic list that
  every frame points at; transfers are chains of qTDs (20 KiB each) and a
  short packet ends a bulk or interrupt transfer through an inactive
  "stop" qTD.  Full- and low-speed devices on a root port are passed to
  the companion controller that shares the port (`PORT_OWNER`); behind a
  high-speed hub they go through the hub's transaction translator (split
  transactions).  The firmware's legacy support is turned off first.
- **OHCI** (`ohci.c`, USB 1.1): an endpoint descriptor per endpoint on the
  control, bulk or interrupt list, transfers queued behind a dummy TD
  (8 KiB per TD), the data toggle kept in the ED, and a short packet in
  the middle of a bulk transfer ending it.  The controller is taken from
  the firmware's SMM driver (`OwnershipChangeRequest`).
- **UHCI** (`uhci.c`, USB 1.1): skeleton queue heads for interrupt,
  control and bulk transfers behind every frame, a queue head per
  endpoint, a TD per packet with the data toggle kept by the driver, and
  short-packet detection that ends a transfer (or moves a control
  transfer on to its status stage).  Legacy keyboard emulation is turned
  off (`USBLEGSUP`).
- All of them are polled from the timer tick like the rest of NovaOS's
  drivers (their interrupts stay off); an interrupt endpoint is polled
  each frame.  Controllers start in the order that lets EHCI hand devices
  to its companions: xHCI, EHCI, then OHCI and UHCI.  Before sleep (S3)
  every xHCI controller arms its root ports to wake the machine (Phase
  18.6's keyboard wake, now a controller operation, `prepare_sleep`);
  after it each controller is reset and its devices enumerated again.
- **Keyboard LEDs**: Num Lock, Caps Lock and Scroll Lock follow the lock
  keys (`InputLockState`, which now tracks Num and Scroll Lock too).  The
  report descriptor's LED outputs are found (the boot-protocol descriptor
  gained them), and the `usb` thread sends an output report
  (`SET_REPORT`) to every keyboard when the state changes.
- Tested in QEMU 8.2 (`-machine q35,i8042=off`, so typing goes over USB)
  with a keyboard, a tablet and a FAT stick (`dir`, `type`) on each of
  `qemu-xhci`, `usb-ehci`, `pci-ohci` and `piix3-usb-uhci` (on OHCI and
  UHCI the stick sat behind the hub QEMU adds when the root ports run
  out); an ICH9 EHCI with three
  UHCI companions (a full-speed hub with keyboard and mouse passed to a
  companion, a high-speed stick and tablet on EHCI); EHCI with an OHCI
  companion and full-speed keyboards (`usb_version=1`) unplugged and
  plugged in on other ports; two xHCI controllers with the keyboard moved
  from one to the other; devices added and removed while running on
  each; `sleeptest` on xHCI (two controllers), OHCI and ICH9 EHCI + UHCI
  with every device back after waking.  Caps Lock reached the keyboards
  as `SET_REPORT` output reports (QEMU's USB packet capture).
- Not yet: isochronous transfers (webcams, USB audio), interrupt
  endpoints are polled every frame whatever their interval, and split
  transactions through a high-speed hub's transaction translator are
  untested (QEMU's EHCI has none).  Unplugging a hub from a port that EHCI
  passed to a UHCI companion aborts QEMU 8.2 (an assertion in its USB
  core when it hands the port back); unplugging other devices there, or
  a hub elsewhere, works.
