## Pen WM_POINTER messages

What [Pen tilt and rotation, and virtio pens](#pen-tilt-and-rotation-and-virtio-pens)
left open: a pen's pressure, tilt and rotation reached programs only
through Wintab, and to every other program a pen was a mouse.  Programs
written for Windows 8 and later (Qt 5.12+, Chromium, Firefox, WinUI) read
pens through the `WM_POINTER` messages instead.

- **The desktop tags a pen's pointer motion.**  A pen (USB, virtio or
  synthetic) now posts its packet first and then the pointer motion it
  causes, marked as the pen's (`InputEvent.from_pen`); the mouse messages
  that motion makes go to the program with the packet's number in `MSG`'s
  padding after `message` (`UmSetInputPen`; a 32-bit program's
  `ntdll` puts it after the `MSG`).  When the pen goes out of range the
  window under it gets one more, tagged, move.
- **user32 makes `WM_POINTER*` of them** (`userland/user32/pointer.c`), in
  a client area, as Windows 8 does: `WM_POINTERENTER` when the pen comes
  over a window, `WM_POINTERUPDATE` while it hovers, moves or presses the
  barrel button, `WM_POINTERDOWN` / `WM_POINTERUP` for the tip,
  `WM_POINTERLEAVE` when it moves to another window or out of range, and
  `WM_POINTERCAPTURECHANGED` when the input leaves the window it is
  touching.  The pen is pointer 12, primary; while it touches it stays
  with the window it touched down on.
- **`GetPointerType`** says `PT_PEN`; **`GetPointerPenInfo`** (and
  `GetPointerInfo`, `GetPointerFramePenInfo`, the `...History` forms) give
  the message's pressure (0-1024), `tiltX` / `tiltY` and `rotation` in
  degrees with `penMask` saying which the pen reports, `PEN_FLAG_BARREL`,
  `PEN_FLAG_INVERTED` and `PEN_FLAG_ERASER`, the button flags and
  `ButtonChangeType`, and HIMETRIC positions that
  **`GetPointerDeviceRects`** maps onto the screen (Qt computes its
  sub-pixel pen position that way).  **`GetPointerDevices`** /
  `GetPointerDevice` list the pen as an external one while a pen is
  present.  Each message's data is kept in a ring; what a thread took
  last with `GetMessage` / `PeekMessage` is what these answer with.
- **`DefWindowProc`** gives a pointer message it gets back as the mouse
  message it was made of (`WM_LBUTTONDOWN` for the tip, `WM_RBUTTONDOWN`
  for the barrel button, the moves), so programs that know only the mouse
  or Wintab work as before, and a program that handles `WM_POINTER*`
  itself gets no mouse messages for the pen.
- **`EnableMouseInPointer(TRUE)`** makes the mouse's own client-area
  messages `WM_POINTERDOWN` / `UPDATE` / `UP` for pointer 1 (`PT_MOUSE`,
  the buttons as the first, second and third button), promoted back the
  same way; `IsMouseInPointerEnabled` says so.
- **Test**: `pentest` (core suite) drives a synthetic pen over two
  windows and checks the message sequence, the pen data and the
  promotion; `wintabtest` still sees the pen's clicks (now through the
  promotion); `usbcheck`'s pen checks expect the packet before the motion.

Not yet: `GetMessageExtraInfo` does not carry the pen signature
(`0xFF515700`) on promoted mouse messages; non-client pen messages
(`WM_NCPOINTER*`) and `WM_POINTERACTIVATE` are not sent, a pen over a
title bar is a mouse; mouse-in-pointer mode has no `WM_POINTERENTER` /
`LEAVE` and is not covered by a self-test (no way to drive the mouse from
inside the guest).
