## USB hubs and report-protocol HID (Phase 18.1)

- **USB core** (`kernel/drivers/xhci.c`, `usb.h`): devices are enumerated
  on root ports and behind hubs (the slot context carries the route
  string, the root port and, for low and full speed devices behind a high
  speed hub, the transaction translator).  Every endpoint of the
  configuration gets a ring in one Configure Endpoint, then
  SET_CONFIGURATION, and each interface is offered to the class drivers,
  which open pipes: interrupt IN with a completion callback, and bulk
  transfers that wait.  A device that leaves takes everything behind it
  with it; its drivers' `gone` callbacks run on the `usb` thread.
- **Hubs** (`usbhub.c`): USB 2 and USB 3 hubs.  Ports are powered, the
  status-change endpoint says which port changed, and the `usb` thread
  reads its status, debounces, resets it and enumerates the device.
- **HID** (`usbhid.c`): report protocol.  The report descriptor is parsed
  into fields (report IDs, usage pages, arrays and bitmaps, push/pop);
  keyboards report keys held (array or bitmap), mice relative X/Y, wheel
  and buttons, tablets and touch screens absolute X/Y (scaled to the
  screen) with a button or the first contact's tip switch.  Boot-class
  devices whose report descriptor can't be used fall back to boot
  protocol.  Absolute pointers move the cursor to a position
  (`InputEvent.absolute`, `WmCursorMoveAbs`).
- Tested in QEMU (`-machine q35,i8042=off -device qemu-xhci`) with a
  `usb-hub` on port 1 holding a `usb-kbd` and a `usb-mouse`, and a
  `usb-tablet` on port 2: typing in Terminal, relative motion and absolute
  positions all work; the keyboard was unplugged and a new one added on
  another hub port, and the tablet unplugged and added on a third root
  port, with `device_del` / `device_add`.
- Not yet: keyboard LEDs, multi-touch, more than one xHCI controller, and
  the older UHCI/OHCI/EHCI controllers.
