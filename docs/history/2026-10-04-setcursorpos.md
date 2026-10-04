## SetCursorPos and ClipCursor

Games recentre the pointer: SDL's relative mouse mode, unless a game asks
for Raw Input, reads `WM_MOUSEMOVE` and puts the pointer back in the middle
of the window with `SetCursorPos` after every move, and many games keep the
pointer inside their window with `ClipCursor`.  NovaOS refused
`SetCursorPos` ("the pointer is the user's") and ignored `ClipCursor`, so
in Teeworlds, with its default settings, the menu cursor ran to the
screen's edge and stuck there.

- **Kernel** (`kernel/um/um_gui.c`, `kernel/wm/wm.c`): two new window
  system calls.  `SET_CURSOR_POS` moves the pointer (kept on the monitors)
  and gives the window under it the mouse move, as the mouse would, even
  when the pointer was already there (SDL takes that move as the end of
  its warp); it is not Raw Input, which stays the mouse's own motion.
  `CLIP_CURSOR` keeps the pointer in a rectangle: it moves inside at once,
  and the mouse, `SetCursorPos` and tablets stop at its edges.  Both are
  for the foreground process only (the one whose window is active, or a
  console program while its Terminal is): the pointer is still the
  user's, and a program in the background cannot take it.  The
  confinement ends when another process comes to the foreground or the
  display mode changes, as on Windows.
- **user32** (`misc.c`): `SetCursorPos`, `SetPhysicalCursorPos`,
  `ClipCursor` and `GetClipCursor` use them, converting coordinates for
  the thread's DPI awareness; a refusal sets `ERROR_ACCESS_DENIED`.
  `GetClipCursor` without a confinement returns the whole desktop (every
  monitor), not only the primary one.

The Teeworlds corpus test now starts the game with its own defaults
(without `inp_grab 1`) and opens Settings with the mouse.  New self-test
`warptest` (64- and 32-bit).
