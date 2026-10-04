## Raw Input for the mouse and the keyboard

Games read the mouse through Raw Input: SDL's relative mouse mode (menus
and aiming in SDL games such as Teeworlds) takes `WM_INPUT` and ignores
`WM_MOUSEMOVE`, so until now the pointer stood still in them.  Raw Input
had game pads only (PR #204); the mouse and the keyboard are raw input
devices now too.

- **Kernel** (`kernel/wm/desktop.c`, `kernel/drivers/gamepad.c`): as the
  desktop takes each mouse and key event from the input queue (PS/2,
  USB, virtio and I2C touchpads alike), it also puts it in the ring Raw
  Input already reads for game pads, as two more slots: a mouse event as
  Windows' `RAWMOUSE` (relative motion, or a position with
  `MOUSE_MOVE_ABSOLUTE` from a tablet; the buttons that went down or up
  as `RI_MOUSE_*` flags; a wheel's turn, 120 a notch, the horizontal
  wheel as a block of its own) and a key as `RAWKEYBOARD` (scan code,
  `RI_KEY_BREAK`, `RI_KEY_E0`, the virtual key and the message the key
  makes, `WM_SYSKEYDOWN` with Alt).  The ring grew to 256 entries.
- **user32** (`rawinput.c`): `GetRawInputDeviceList` lists a mouse and
  a keyboard (`RIM_TYPEMOUSE`, `RIM_TYPEKEYBOARD`) before the game pads,
  named like a PS/2 mouse and keyboard on Windows (the mouse and
  keyboard device interface classes), with their `RID_DEVICE_INFO`.
  Registering for Generic Desktop usage 2 or 6 brings `WM_INPUT` for
  them, read with `GetRawInputData` or `GetRawInputBuffer`;
  `RIDEV_DEVNOTIFY` announces them at once.  `RIDEV_NOLEGACY` stops
  the process's legacy mouse or key messages; its value (0x30) shares
  bits with `RIDEV_EXCLUDE` and `RIDEV_PAGEONLY`, which are now read as
  the one mode they are, so a mouse registered with `RIDEV_NOLEGACY` no
  longer takes every Generic Desktop device as if it were `PAGEONLY`.

New core self-test `rawtest` (64- and 32-bit) moves, clicks and turns
the wheel of the PS/2 mouse and presses keys, and checks what
`WM_INPUT` and `GetRawInputBuffer` carry.  `rawpadtest` counts only the
HID devices in the list now.

Teeworlds' menus now follow the mouse: with its `inp_grab 1` setting SDL
2.0.8 reads Raw Input in relative mouse mode, and the app corpus's
Teeworlds run opens Settings with a click
([compatibility.md](../compatibility.md)).  Its default, `inp_grab 0`,
has SDL recentre the pointer with `SetCursorPos` after each move instead,
which NovaOS does not do yet, so there the menu cursor still sticks at
the screen's edge.
