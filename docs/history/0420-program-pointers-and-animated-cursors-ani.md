## Program pointers and animated cursors (.ani)

Until now `SetCursor` only remembered its argument: the desktop always
drew its own arrow.  The pointer is now the program's, and animated
cursors play.  No MIT, BSD or zlib licensed .ani reader was found (Wine's
is LGPL), so the parser is written here; the format is a small RIFF file.

- **The kernel draws a program's pointer** (`kernel/wm/wm.c`,
  `kernel/gdi/gdi.c`): `NtNovaGuiCtl` op 19 hands it a shape (up to
  64 x 64 logical pixels, its hot spot, up to 64 frames and 256 steps,
  each step's time in jiffies), or asks for the arrow or no pointer.  It
  shows over the client area of that process's windows, and anywhere while
  one of them has the mouse captured; the desktop, title bars, borders and
  window drags keep the arrow.  The desktop's tick steps animated shapes
  (100 Hz against the .ani's 60 Hz jiffies) and redraws the pointer when
  the window under it changes.  Op 20 reports what the pointer shows, for
  tests.
- **user32**: `SetCursor` sends the cursor to the kernel when it changes
  (the system `IDC_*` cursors are the arrow; `NULL` hides the pointer),
  `ShowCursor` below zero hides it, and `GetCursorInfo` says whether it
  shows.
- **Animated cursors** (`userland/user32/res.c`): RIFF `ACON` files with
  `anih`, `rate`, `seq ` and the `fram` list of .cur/.ico frames, from
  `LoadCursorFromFile`, `LoadImage(LR_LOADFROMFILE)`,
  `CreateIconFromResourceEx` and `ANICURSOR`/`ANIICON` resources
  (`LoadCursor`, `LoadImage`).  `DrawIconEx` draws the frame of the step it
  is given, and `GetCursorFrameInfo` reports each step's frame and rate.
  `LoadCursorFromFile` loads .cur files too, and returns `NULL` for a
  missing file as Windows does (it used to return the arrow).
- **`anitest.exe`** (in the core self-tests) loads a spinner
  (`userland/programs/anitest.ani`, made by `tools/mkani.py`: 8 frames
  played in a custom order with two rates) from its resource, from memory
  and from a file, checks the steps, rates, hot spot and each step's
  drawing, then makes it the pointer over a window and checks the desktop
  shows it and steps through at least 6 of the 8 frames in 1.5 s, that
  `SetCursor(NULL)` hides it and the arrow comes back.  `anitest show N`
  keeps the window up for N seconds.  It runs as a 32-bit program too
  (`C:\Programs\x86\anitest`).
- Not yet: the system cursors themselves (I-beam, resize arrows, the
  busy and "working in background" animations) are all the arrow;
  `SetSystemCursor` does nothing; `CopyIcon` of an animated cursor keeps
  only its first frame; at 200 % the pointer is scaled up by nearest
  neighbour.
