## Game controllers: Raw Input and hid.dll

After XInput and DirectInput 8, the other two ways Windows programs read
a game pad: Raw Input, which SDL uses for Xbox controllers, and `hid.dll`,
which reads a controller's HID reports (Chromium's `libcef` delay-loads
its `HidP_*` calls for WebHID and the Gamepad API).

- **Reports** (`kernel/drivers/gamepad.c`): every controller now keeps
  its HID report descriptor and the input reports it sends, in a ring
  programs read through `CTL_GAMEPAD` (ops 5 to 10).  A HID game pad's
  are its own; an Xbox controller gets the descriptor Windows' Xbox
  driver gives its HID side (X, Y, Rx, Ry and the triggers as one Z
  axis, 16 bits each, ten buttons and a hat) and a report built from
  each state it sends.
- **HID paths** (`kernel/um/um_hid.c`): `CreateFile` on a controller's
  path (`\\?\HID#VID_045E&PID_02EA&IG_00#8&...#{4d1e55b2-...}`, `IG_`
  for Xbox controllers as on Windows) opens a message pipe the kernel
  writes each report into, so `ReadFile` returns one report at a time,
  overlapped or not, and fails once the controller is unplugged.
  setupapi lists these paths for `GUID_DEVINTERFACE_HID` (with the
  device's class, hardware IDs and instance ID), and so does
  `CM_Get_Device_Interface_List`; DirectInput's `DIPROP_GUIDANDPATH`
  gives a path of the same form.
- **Raw Input** (`user32/rawinput.c`): `RegisterRawInputDevices` (with
  `RIDEV_INPUTSINK`, `DEVNOTIFY`, `REMOVE`), `GetRawInputDeviceList`,
  `GetRawInputDeviceInfo` (`RIDI_DEVICENAME`, `DEVICEINFO`,
  `PREPARSEDDATA`), `WM_INPUT` with `GetRawInputData`,
  `GetRawInputBuffer`, `WM_INPUT_DEVICE_CHANGE` and
  `GetRegisteredRawInputDevices`.  The keyboard and mouse are not raw
  input devices yet.
- **hid.dll**: the `HidD_*` calls on a HID path (attributes, preparsed
  data, product string, input reports, the queue) and the `HidP_*`
  calls on preparsed data: caps, button and value caps, link
  collections, usages, values (raw, scaled and arrays), `GetData` and
  `SetData`, building reports, usage list differences, with Windows'
  `HIDP_STATUS_*` results.  The descriptor parser
  (`userland/include/novahidp.h`) is shared with user32.

New test `rawpadtest` in the devices suite's "gamepad" boot drives an
Xbox One controller and a HID game pad through all of it, unplugging
the Xbox One controller at the end; the 32-bit `rawpadtest still` reads
the HID game pad left.

Checked by hand with SDL 2.30.9 (64-bit): SDL finds the Xbox One
controller through its RAWINPUT driver (its joystick GUID ends in `r`),
and its game controller API reads A, B and Start, both sticks and the
right trigger as the test moves them.
