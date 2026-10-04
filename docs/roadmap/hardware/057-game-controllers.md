- ~~Game controllers for Windows games~~ Done: wired Xbox 360 and Xbox
  One controllers (their motors and the Xbox 360 one's player light
  included) and HID game pads on USB, hot-plugged, for XInput
  (`xinput1_4`, `xinput1_3` and older, `xinput9_1_0`), DirectInput 8
  (game controllers, keyboards and mice, immediate and buffered), Raw
  Input (`WM_INPUT` with each controller's HID reports) and `hid.dll`
  (`HidD_*` on a controller's HID path, `HidP_*` on its report
  descriptor; an Xbox controller has the one Windows' Xbox driver
  gives).  Still to do: raw keyboard and mouse input, HID output and
  feature reports, `IOCTL_HID_*` through `DeviceIoControl`, the Xbox
  360 wireless receiver, Bluetooth controllers, force feedback through
  DirectInput, and virtio game pads (QEMU has none).
