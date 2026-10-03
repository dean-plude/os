## Pen tilt and rotation, and virtio pens

What [GTK pointers and Wintab pen tablets](#gtk-pointers-and-wintab-pen-tablets)
left open: pens reported only where they were and how hard they pressed,
and pens and tablets on virtio input were not used.

- **Tilt and barrel rotation** travel with every pen packet, from the
  device through the desktop (`kernel/wm/tablet.c`) to `wintab32.dll`, in
  tenths of a degree: X tilt (+ the pen's top leaning right), Y tilt (+
  toward the user) and the barrel's twist, clockwise.  A pen says what it
  can report when it comes; a value it can't report stays 0.
- **USB pens**: the HID parser reads X Tilt, Y Tilt and Twist (digitizer
  usages 0x3D, 0x3E, 0x41) through their physical range and unit
  exponent (it now keeps both), so Microsoft's -9000..9000 at exponent -2
  and a plain -127..127 over -90..90 degrees both come out right.
- **Synthetic pens**: `InjectSyntheticPointerInput` passes `tiltX`,
  `tiltY` and `rotation` when `penMask` has `PEN_MASK_TILT_X`/`_Y` or
  `PEN_MASK_ROTATION` (`NtNovaGuiCtl` op 30 arg 4 takes them; arg 5 says
  what the pens present can report).
- **`wintab32.dll`**: with a pen that reports tilt, `DVC_ORIENTATION` has
  azimuth (0-3599, clockwise from the tablet's top) and altitude
  (-900..900) axes, and with barrel rotation a twist axis (0-3599); each
  packet's `ORIENTATION` carries them, the tilt turned into azimuth and
  altitude the way Qt turns them back (the eraser end has a negative
  altitude, as Wacom's driver gives).  `PK_CHANGED` includes
  `PK_ORIENTATION`.  A pen without tilt stays upright (altitude 900).
- **Virtio input pens and tablets** (`kernel/drivers/virtio_input.c`): a
  device with absolute X/Y and `BTN_TOOL_PEN` or `ABS_PRESSURE` (a Linux
  pen passed through with `virtio-input-host-pci`) is a pen: tip, barrel
  buttons, eraser (`BTN_TOOL_RUBBER`), pressure, tilt (`ABS_TILT_X/Y`, by
  their resolution in units a radian) and an Art Pen's rotation (`ABS_Z`),
  moving the pointer and clicking with the tip as a USB pen does.  Any
  other device with absolute X/Y, such as QEMU's `virtio-tablet-pci`, is
  an absolute pointer with its buttons and wheels (it was ignored before).
- **Tests**: `usbcheck` runs a pen descriptor with tilt and twist through
  the HID parser and canned evdev events through the virtio pen and tablet
  decoding; `wintabtest` injects tilted and turned pen reports and checks
  the azimuth, altitude and twist each packet carries and the orientation
  axes; the devices suite's touch boot has a `virtio-tablet-pci` too and
  checks it comes up as an absolute pointer.

Not yet: pen `WM_POINTER` messages (`GetPointerPenInfo`), so tilt reaches
programs through Wintab only.  QEMU has no virtio or USB pen of its own,
so the pen decoding is tested on canned events.
