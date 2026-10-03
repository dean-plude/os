## Media keys, side buttons and the horizontal wheel

Keyboards' media, volume, browser and launch keys, mice's back and forward
buttons and tilting (horizontal) wheels now work, on USB and on PS/2.

- **Media keys** (`usbhid.c`): USB keyboards send them as Consumer Control
  usages (page 0x0C), usually in a report or on an interface of their own,
  either as an array of usages or as one bit per key; the Power, Sleep and
  Wake keys come as System Control (Generic Desktop 0x81-0x83).  Both are
  found in the report descriptor, a device that has only them is taken
  too ("media keys" in the log), and each key becomes the E0-prefixed
  scancode a PS/2 keyboard sends for it (Microsoft's keyboard scan code
  specification), so everything above the drivers sees one kind of key.
  Keys held are tracked per report, and held volume keys repeat.  The
  keyboard page's own Mute, Volume Up and Volume Down usages (which QEMU's
  USB keyboard sends) map to the same codes.
- **Mouse buttons 4 and 5 and AC Pan**: report-protocol mice report up to
  five buttons, and the horizontal wheel (Consumer "AC Pan", + to the
  right) goes in a new `InputEvent.dw`.  PS/2 mice are switched to
  IntelliMouse Explorer mode (sample rates 200, 200, 80 after the wheel
  mouse's 200, 100, 80), which adds buttons 4 and 5 and a horizontal wheel
  in the fourth byte.
- **Programs** (`um_gui.c`, `user32`): side buttons arrive as
  `WM_XBUTTONDOWN`/`UP`/`DBLCLK` (or the `WM_NCXBUTTON*` forms) with
  `XBUTTON1`/`XBUTTON2` in the high word, `MK_XBUTTON1`/`2` and
  `VK_XBUTTON1`/`2` for `GetKeyState`; the tilt wheel as `WM_MOUSEHWHEEL`.
  `DefWindowProc` turns a side button's release into `WM_APPCOMMAND`
  (`APPCOMMAND_BROWSER_BACKWARD`/`FORWARD`, `FAPPCOMMAND_MOUSE`) and a
  browser, volume, media or launch key (`VK_BROWSER_BACK` to
  `VK_LAUNCH_APP2`) into the matching `APPCOMMAND_*` with
  `FAPPCOMMAND_KEY`; unhandled, it goes up to the parent window, as on
  Windows.
- **The shell**: Volume Up and Down change the playback volume by 2% a
  press, Mute toggles it (the focused program still gets the key), Sleep
  sleeps (S3) and Power shuts down like the power button.
- **Tests**: `inputtest` (core self-tests) plugs a USB mouse in for the
  test and presses its buttons 4 and 5, then unplugs it and tilts the PS/2
  mouse's wheel both ways and presses its button 4, then presses the
  volume keys on the USB keyboard, and checks the messages a full-screen
  window gets.  QEMU has no USB device with Consumer Control or a
  horizontal wheel, so the Terminal's `usbcheck` runs those report
  descriptors (a consumer array, consumer bits, system control, a
  five-button mouse with AC Pan, a keyboard sending the volume usages)
  through the same parser and report handling and compares the events
  they make.
