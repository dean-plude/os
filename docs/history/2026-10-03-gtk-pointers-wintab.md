## GTK pointers and Wintab pen tablets

Two gaps [GTK programs](#gtk-programs-inkscape) left open: GTK's own
pointers did not show (Inkscape's tools all had the arrow), and there was
no `wintab32.dll`, so GTK, Qt and Krita found no pen pressure.

- **DIB sections of 1, 4, 8 and 16 bits per pixel.**  GDK makes a cursor
  from a pixbuf as a 32-bit image with alpha (a `BITMAPV5HEADER` section)
  and a 1-bit mask (a `BITMAPV4HEADER` section with two colours), then
  `CreateIconIndirect`.  gdi32 took only 24- and 32-bit sections, so the
  mask failed and GDK fell back to the default pointer.  A section of
  fewer bits now keeps the program's own rows and colour table beside the
  32-bit pixels gdi32 draws on, synced at each use the way 24-bit sections
  already were: what the program writes is read as its colours, and what
  gdi32 draws is written back as the nearest index (only where it
  changed).  `GetObject` describes them, `GetDIBColorTable` and
  `SetDIBColorTable` work on them (recolouring keeps the indices), 16-bit
  ones are 5-5-5 or, with `BI_BITFIELDS`, 5-6-5, and a DIB's colour table
  is found after its header whatever the header's size.
- **Monochrome cursors.**  `CreateIconIndirect` with only a mask (twice the
  cursor's height, the AND half over the XOR half) and `CreateCursor`'s
  planes now give white, black and clear pixels (white was black before);
  an "invert the screen" pixel, which the desktop cannot draw, is black.
- **Pens** (`kernel/wm/tablet.c`).  USB digitizer pens (HID page 0x0D: tip
  pressure, in range, barrel buttons, eraser) report packets beside the
  pointer motion they make (the tip clicks, the barrel button
  right-clicks); `usbcheck` runs a pen's report descriptor through the
  parser.  A plain absolute pointer such as QEMU's `usb-tablet` stays a
  mouse, as on Windows.  Programs can make a pen too:
  `CreateSyntheticPointerDevice(PT_PEN)` and `InjectSyntheticPointerInput`
  (Windows 10's pointer injection) move the pointer, click with the tip
  and send the pen's pressure; `SM_DIGITIZER` reports `NID_EXTERNAL_PEN`
  while a pen is there.  The desktop keeps every pen's last 256 packets,
  numbered, for programs to read (`NtNovaGuiCtl` op 30).
- **`wintab32.dll`**, written from the Wintab 1.4 specification (Wine's is
  LGPL and was not used).  With no pen, `WTInfo(0, 0, NULL)` is 0, which
  GTK and Qt take as "no Wintab", and `WTOpen` fails.  With one, there is
  one device with a pen and an eraser cursor, X and Y 0-65535 and pressure
  0-1023.  A context gets the packets that come while it is enabled and
  one of its process's windows is in front: mapped to its output extents
  (a negative extent turns the axis round, as GTK asks for Y), laid out
  as its `lcPktData` asks, buttons and pressure absolute or relative
  (`lcPktMode`), in a queue of the size `WTQueueSizeSet` gives, with
  `WT_PACKET`, `WT_PROXIMITY` and `WT_CSRCHANGE` posted to its window.
  `WTPacket`, `WTPacketsGet`/`Peek`, `WTDataGet`/`Peek`,
  `WTQueuePacketsEx`, `WTEnable`, `WTOverlap`, `WTGet`/`WTSet` and the
  rest of the interface are there, by name and by Wintab's ordinals.
- **Tests**: `bmpcurtest` (the sections, GDK's two kinds of cursor, and the
  desktop showing a program's pointer set as the class cursor) and
  `wintabtest` (wintab32 loaded as GTK loads it, with no pen and with a
  synthetic one) in the core self-tests.

Not yet: tilt and rotation (no pen reports them yet), pens on virtio input
(QEMU has none with pressure), and pen `WM_POINTER` messages.
