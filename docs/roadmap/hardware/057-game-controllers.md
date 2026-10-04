- ~~Game controllers for Windows games~~ Done: wired Xbox 360 and Xbox
  One controllers (their motors and the Xbox 360 one's player light
  included) and HID game pads on USB, hot-plugged, for XInput
  (`xinput1_4`, `xinput1_3` and older, `xinput9_1_0`) and DirectInput 8
  (game controllers, keyboards and mice, immediate and buffered).  Still
  to do: Raw Input and `hid.dll` for game pads (`WM_INPUT` with HID
  reports, `HidP_*` on their report descriptors), the Xbox 360 wireless
  receiver, Bluetooth controllers, force feedback through DirectInput,
  and virtio game pads (QEMU has none).
