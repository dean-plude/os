## Cave Story: DirectDraw, from cnc-ddraw

The seventh free game is the first on DirectDraw, which NovaOS did not
have: Cave Story (Studio Pixel's 2004 freeware, in Aeon Genesis' English
translation, 32-bit), from the App Store.  NovaOS's `ddraw.dll` is
cnc-ddraw (MIT, vendored in `third_party/cnc-ddraw`), the DirectDraw that
players of old Windows games already use on Windows 10 and 11: every
surface lives in memory, and a render thread draws the primary surface
with Direct3D 9 (DXVK, when installed), OpenGL (Mesa) or GDI.  It is
32-bit code, so it is built for SysWOW64 alone (`x86_only` in its
`dll.json`), with its `ddraw.ini` beside it: cnc-ddraw's own defaults,
with `hook=0` (it leaves the program's import tables alone) and nothing
saved back.  The game's DirectInput 7 joystick code needed `dinput.dll`,
now built from `dinput8`'s source with the DirectInput 3 to 7 interfaces.

NovaOS gaps the game found:

- **Missing functions**: kernel32's `SetHandleCount`, msvcrt's `_strcmpi`
  and `_makepath`, and ntdll's `RtlVerifyVersionInfo`, which cnc-ddraw
  asks for by name.  `VerifyVersionInfoW` now compares with the version
  NovaOS reports instead of always answering yes.
- **A black screen**: gdi32 mapped a 16-bit DIB section's file mapping at
  the offset the program gave, which must be a multiple of the allocation
  granularity for a view, but need only be a multiple of 4 for a DIB
  section (cnc-ddraw puts guard rows before each surface's pixels).  The
  view now starts at the mapping's beginning.
- **No keys**: activating a window gave it the keyboard focus only through
  `DefWindowProc`'s `WM_ACTIVATE`; Cave Story answers `WM_ACTIVATE`
  itself, so its keys came as `WM_SYSKEYDOWN` to a window without focus.
  As on Windows, the focus now moves into a window that becomes active
  unless its procedure put it there or set it to NULL.
- **No text**: the game writes its text with GDI on surface DCs, which
  gdi32 draws on 32-bit pixels and copies to a 16-bit section's own bits
  only at its sync points, none of which cnc-ddraw reached.  `RestoreDC`
  (which cnc-ddraw's `ReleaseDC` calls) now syncs, as a non-batched call
  flushes Windows' GDI batch.
- **The pointer over the game**: with `hook=0`, cnc-ddraw's mouse lock
  still balanced `ShowCursor` against a count only its hooks keep, and
  showed the pointer the game had hidden; it now stays off when nothing
  is hooked (one change in the vendored code, noted in its
  `NOVA-VENDOR.txt`).

The corpus test (`tests/appcorpus/945-cave-story.py`) installs the game,
checks the title screen at 640x480, starts a new game with Z, checks its
opening line's screenshot, ends the game with Alt+F4 and checks the
display is back at 2560x1600.
