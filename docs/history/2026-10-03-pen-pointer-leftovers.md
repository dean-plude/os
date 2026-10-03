## Pen and mouse pointers: title bars, activation, the pen signature, touch screens listed

What [Pen WM_POINTER messages](#pen-wm_pointer-messages) left open:

- **The pen signature.**  The mouse messages `DefWindowProc` makes of a
  pen's pointer messages (and any other mouse message a pen causes) carry
  Windows' pen marker: `GetMessageExtraInfo` masked with `0xFFFFFF00` is
  `0xFF515700`; a touch's carry `0xFF515780` (bit 7: touch).  Programs that
  ignore pen-made mouse messages because they read the pen themselves
  (Qt, Chromium, drawing programs) can now tell them apart.  user32's queue
  keeps each message's extra information, so `GetMessageExtraInfo` answers
  for the message taken last; `SetMessageExtraInfo` sets it.
- **Non-client pen messages.**  Over the parts of a window user32 hit-tests
  outside the client area (a program's own title bar that answers
  `WM_NCHITTEST` with `HTCAPTION`, as Chromium, Firefox and Qt's frameless
  windows do; scroll bars; the menu bar), a pen gives `WM_NCPOINTERUPDATE`,
  `WM_NCPOINTERDOWN` and `WM_NCPOINTERUP` with the hit-test code in
  `wParam`'s high word; `DefWindowProc` makes them `WM_NCLBUTTONDOWN` / `UP`
  and the moves, so scroll bars and menus work by pen as before.  Enter and
  leave cover the whole window.  The desktop's own title bar and frame stay
  the desktop's: a pen moves and sizes windows there as the mouse does, and
  programs get no messages for it (as for the mouse).
- **`WM_POINTERACTIVATE`.**  A pen tip (or, with `EnableMouseInPointer`, a
  mouse button) going down on a window that is not active sends it
  `WM_POINTERACTIVATE` (pointer id, hit-test code, the top-level window)
  before `WM_POINTERDOWN`; `DefWindowProc` asks the parent of a child window
  and otherwise answers `PA_ACTIVATE`.  The desktop has already activated
  the window by then, so `PA_NOACTIVATE` gives the activation back to the
  window of the program's that had it.
- **The mouse entering and leaving.**  With `EnableMouseInPointer(TRUE)` the
  mouse now gets `WM_POINTERENTER` when it comes over a window and
  `WM_POINTERLEAVE` when it moves to another or off the program's windows,
  and its non-client messages become `WM_NCPOINTER*` like the pen's.
- **Touch screens in `GetPointerDevices`.**  A multi-touch screen is listed
  as `POINTER_DEVICE_TYPE_TOUCH` (its contact count, cursor ids from 2)
  beside the pen; `GetPointerDevice` and `GetPointerDeviceRects` answer for
  it, and touch pointer messages name it as their `sourceDevice`.
- **A PS/2 mouse fix.**  The PS/2 driver left the rest of its input event
  uninitialised, so after a pen had been used its motion could arrive
  tagged as the pen's (and become pen pointer messages).
- **Test**: `pentest` (core suite, 64- and 32-bit) checks the pen
  signature, `WM_POINTERACTIVATE` with `PA_ACTIVATE` and `PA_NOACTIVATE`,
  the non-client messages over a window's own title bar and their
  promotion, the touch-screen listing, and the mouse entering and leaving
  two windows as the test harness moves it (`Nova.move_to`);
  `touchtest` (devices suite) checks the touch signature and the
  touch screen in `GetPointerDevices`.

Not yet: `WM_POINTERACTIVATE` comes after the desktop has activated the
window, so `PA_NOACTIVATE` briefly activates it; the desktop's own title bar
sends programs nothing for a pen (or the mouse).
