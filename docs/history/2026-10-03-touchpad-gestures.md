## Touchpad gestures: tap to click and two-finger scrolling

Phase 21.4 left the ThinkPad T14 Gen 4's touchpad in its mouse mode: the
pointer moved and the pad clicked, nothing more.  It now works the way a
Windows precision touchpad does on Windows.

- **Touchpad mode.**  After the reset, the I2C-HID driver sends the
  touchpad a SET_REPORT of its Input Mode feature (3: touchpad, with the
  surface and button switches on), which the report-descriptor parser now
  finds in the configuration collection.  The touchpad then reports its
  fingers instead of mouse motion.  One that refuses keeps working in
  mouse mode, and the switch is repeated after sleep, since a reset puts
  the device back in mouse mode.  USB precision touchpads get the same
  SET_REPORT over their control pipe.
- **Gestures.**  A small state machine in `usbhid.c`, written for NovaOS,
  works on each finished frame (hybrid-mode frames that span several
  reports included), with finger positions converted to micrometres
  through the pad's physical size.  One finger moves the pointer (a pixel
  for every 80 um).  A touch shorter than 180 ms that moves less than 2 mm
  is a left click when it lifts, and with two fingers a right click.
  Pressing the pad clicks, or right-clicks with two fingers on it.  Two
  fingers moving scroll in whole mouse-wheel notches (one for every 3 mm),
  vertically or horizontally, whichever way they went first, so
  every program's `WM_MOUSEWHEEL` and `WM_MOUSEHWHEEL` handling works
  unchanged; the content follows the fingers, Windows' default direction.
  Contacts the touchpad marks as not confident (a palm) are ignored.
- **Tests.**  QEMU has no touchpad, so `hwcheck`'s modelled touchpad now
  takes (or, in a first round, refuses) touchpad mode and is fed finger
  reports for each gesture; the core test `touchpad gestures` checks the
  clicks and wheel notches that come out.  The hand check on the T14 is
  in [hardware.md](../hardware.md).

Not yet: tap-and-drag, scrolling that coasts on after the fingers lift,
three- and four-finger gestures, and wheel deltas finer than a notch.
