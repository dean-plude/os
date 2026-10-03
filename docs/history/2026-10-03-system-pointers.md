## System pointers (I-beam, busy, resize arrows, hand...) and SetSystemCursor

The system cursors were all the arrow, `SetSystemCursor` did nothing and a
program's pointer was scaled up by nearest neighbour at 200 %.  Now:

- **Every `IDC_*` pointer has its own shape** (`kernel/gdi/syscursor.c`):
  arrow, I-beam, busy (a turning ring), "working in background" (the arrow
  and a small ring), cross, up arrow, the four resize arrows, move, "no",
  hand and help.  No permissively licensed cursor set has the whole Windows
  set (X.org's MIT "whiteglass" lacks the diagonal resize arrows and the
  "no" sign; Breeze, Bibata, DMZ and Phinger are GPL or CC-BY-SA), so they
  are outlines drawn the way the desktop's arrow already was: unions of
  polygons, discs, rings and ring arcs with an anti-aliased outline and
  fill, rendered at the display's device resolution, so they are sharp at
  200 %.  The kernel renders a shape once per scale and busy-ring phase.
- **The desktop shows resize arrows** over a window's resize edges and
  corners and while one is being dragged.
- **user32**: `LoadCursor(NULL, IDC_*)` gives real 32 x 32 cursors (with a
  64 x 64 image) that `DrawIconEx` and `GetIconInfo` see; `SetCursor` of
  one asks the kernel to draw that system shape (`NtNovaGuiCtl` op 19,
  arg 3).  `DefWindowProc`'s `WM_SETCURSOR` sets the resize arrows for
  `HTLEFT`, `HTTOPRIGHT` and the other edge codes, for programs that draw
  their own frame, and the arrow for other non-client parts.
- **`SetSystemCursor`** replaces a system pointer for every program and
  destroys the cursor it is given, as on Windows (op 27);
  `SystemParametersInfo(SPI_SETCURSORS)` puts NovaOS's own back.  Op 28
  hands user32 the kernel's drawing of a system pointer.
- **Program cursors at 200 %**: a program's cursor is sent at the
  display's scale (op 19, arg 4: device pixels): the 64 x 64 image of a
  cursor that has one, otherwise its 32 x 32 image smoothed up rather than
  doubled pixel by pixel.
- **`cursortest.exe`** (in the core self-tests) checks each `IDC_*` image
  and hot spot, that the desktop draws each pointer over a window and turns
  the busy ring, `SetSystemCursor` and `SPI_SETCURSORS`, and the size a
  program's cursor reaches the desktop at.  `anitest` now expects its
  spinner at the display's scale.
- Not yet: `CopyIcon` of an animated cursor keeps only its first frame;
  `SetSystemCursor` replacements last until restart (they are not saved
  in the registry).
