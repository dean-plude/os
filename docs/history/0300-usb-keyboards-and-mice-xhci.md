## USB keyboards and mice (xHCI)

- **xHCI host controller driver** (`kernel/drivers/xhci.c`): takes the
  controller from the firmware, resets it, and runs one command ring and
  one event ring.  Like the disk and network drivers it is polled: the
  timer tick drains the event ring, so no interrupt routing is needed.
- **Enumeration**: each connected root port is reset, given a device slot
  and an address, and its device and configuration descriptors are read.
  The first HID boot-protocol keyboard or mouse interface is configured
  (Configure Endpoint, `SET_CONFIGURATION`, `SET_PROTOCOL(boot)`) and a
  transfer is kept queued on its interrupt endpoint.
- **HID** (`kernel/drivers/usbhid.c`): keyboard reports become set-1
  scancodes, the codes a PS/2 keyboard sends, so the window manager and
  `user32` see one kind of keyboard; held keys repeat (500 ms, then every
  30 ms) as PS/2 keyboards do on their own.  Mouse reports (buttons, motion,
  wheel) go to the same input queue as PS/2 mouse packets.
- **Hot-plug**: a small `usb` kernel thread enumerates devices plugged in
  after boot and releases the keys of a keyboard that is pulled out.
- `PciMapBar` maps BARs that lie above the 64 GiB physmap (OVMF puts 64-bit
  BARs at 512 GiB and up); the PS/2 driver now notices when there is no
  8042 controller instead of reading phantom bytes.
- Tested in QEMU with `-machine q35,i8042=off -device qemu-xhci -device
  usb-kbd -device usb-mouse`: Terminal commands typed and the pointer moved
  over USB alone, and a keyboard added and removed with `device_add` /
  `device_del` while running.
- Not yet: USB hubs (devices must sit on a root port), absolute pointers
  (`usb-tablet`, touch screens), HID report protocol (a keyboard's media
  keys, a mouse's extra buttons), keyboard LEDs, more than one xHCI
  controller, USB mass storage, and the older UHCI/OHCI/EHCI controllers.
