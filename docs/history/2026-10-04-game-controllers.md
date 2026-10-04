## Game controllers: XInput and DirectInput 8

Windows games read a game pad through XInput (Xbox controllers) or
DirectInput 8 (any game controller); NovaOS had neither DLL and no
driver for a controller.

- **Drivers** (`kernel/drivers/gamepad.c`, `xpad.c`): wired Xbox 360
  controllers (vendor class 0xFF/0x5D/0x01) and Xbox One ones (the GIP
  protocol, 0xFF/0x47/0xD0, started with its power-on message) on any
  USB controller, with their motors and the Xbox 360 one's player light;
  and HID game pads and joysticks (Generic Desktop usage 4 or 5), whose
  report descriptor gives up to 32 buttons, eight axes and a hat (or a
  D-pad's four usages).  Each controller is a slot in a small registry
  that programs read with `NtNovaGuiCtl` op 33 (`CTL_GAMEPAD`); Xbox
  controllers get the lowest free XInput user (0 to 3), as on Windows,
  and keep it until they are unplugged.  HID game pads are probed before
  the HID mouse and keyboard driver, which is left as it was.
- **XInput** (`xinput1_4`, with `xinput1_3`, `1_2`, `1_1` and
  `xinput9_1_0` built from the same source): `XInputGetState`,
  `XInputSetState`, `XInputGetCapabilities`, `XInputGetKeystroke`,
  `XInputGetBatteryInformation` (wired), `XInputEnable`, the
  audio-device calls (none), and the ordinal-only `XInputGetStateEx`
  (100, with the guide button) and `XInputGetCapabilitiesEx` (108, with
  the vendor and product IDs) that SDL and others use.
- **DirectInput 8** (`dinput8.dll`, also as `CLSID_DirectInput8` for
  `CoCreateInstance`): `EnumDevices` lists the keyboard, the mouse and
  each controller (Xbox ones named as Windows names them, "Controller
  (XBOX 360 For Windows)"; product GUIDs in Windows' `PIDVID` form);
  devices take any data format (`c_dfDIJoystick`, `DIJOYSTATE2`, a
  game's own), with `EnumObjects`, `GetCapabilities`, ranges, dead zones
  and saturation per axis, buffered `GetDeviceData`, event notification,
  `DIPROP_GUIDANDPATH` (an `&IG_` path for Xbox controllers, which SDL
  uses to leave them to XInput), `VIDPID`, `PRODUCTNAME` and the other
  properties games read, and `DIERR_INPUTLOST` once a device is
  unplugged.  Force feedback effects are not supported.
- For Chocolate Doom's SDL2 (32-bit MinGW builds): `msvcrt`'s
  `__p__iob`, `_snwprintf_s`, `_vsnwprintf_s`, `swprintf_s`, `_wutime`
  and `_memccpy`; `setupapi`'s Configuration Manager calls and two more
  `SetupDi*` ones; `ImmGetIMEFileNameA`; `Get`/`SetDeviceGammaRamp`
  (no ramp); and `GetDIBits` describing a bitmap as Windows does on a
  32-bit display, `BI_BITFIELDS` with its colour masks when asked again
  (SDL reads the display's pixel format that way, and with none it made
  no window surface and crashed blitting the first frame).

New devices-suite boot "gamepad": three controllers on xHCI, each
`tools/padpeer.py` (a new usbredir peer that is a wired Xbox 360, an
Xbox One or a HID game pad, moved through a control port) behind a QEMU
`usb-redir` device.  `padtest` checks what XInput and DirectInput list
and, as the test presses buttons, pulls triggers and moves sticks and
the hat, what both APIs read; the motors it sets must reach the Xbox
controllers, and the Xbox 360 one is unplugged while it runs.  The
32-bit `padtest still` reads the two left.

Checked by hand with Chocolate Doom 3.1.0 (32-bit, SDL2) and Freedoom:
SDL's game controller API finds the Xbox 360 controller through XInput
("I_InitGamepad: Xbox 360 Controller"), and its left stick walks and
turns the player (on Mesa's OpenGL from the App Store; NovaOS has no
`d3d9.dll` of its own).
