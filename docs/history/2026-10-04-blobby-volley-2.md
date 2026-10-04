## Blobby Volley 2: a game that switches the display mode

The fifth free game is the first that changes the screen resolution when
it goes full screen: Blobby Volley 2 (GPL, 32-bit, SDL2), from the App
Store (its official zip).  Its Fullscreen option makes an
`SDL_WINDOW_FULLSCREEN` window, not SDL's desktop-sized kind, so SDL
switches the display to 800x600 with `ChangeDisplaySettingsEx`
(`CDS_FULLSCREEN`) and draws with Direct3D 9 in exclusive full screen
(`Windowed = FALSE`), which NovaOS's `d3d9.dll` hands to DXVK on Mesa 3D's
Vulkan.  When the game ends, the display goes back to its own mode
([compatibility.md](../compatibility.md)).

Two NovaOS gaps stopped it:

- **SDL2 would not start**: MSVC's x86 compiler calls the C runtime's math
  through entry points of its own, `_libm_sse2_sin_precise` and the
  others (argument and result in SSE registers, for `/arch:SSE2` code)
  and the older `_CIfmod`, `_CIatan2` and the rest (on the x87 stack).
  The 32-bit `ucrtbase.dll` had none of them, so SDL2's start-up code
  (UPX-packed: its unpacker resolves the imports itself) gave up before
  relocating the DLL, and the game crashed in it.  `ucrtbase.dll` and
  `msvcrt.dll` now export all of them, thin wrappers over the C functions
  (`libmtest` checks each against them).
- **A window's DC kept from before the switch drew into freed memory**:
  SDL takes its window's DC once, when it makes the window, and blits
  every frame through it.  The window's bitmap is made again when the
  window is first shown at another display size (the switch comes in
  between) or changes scale, and the DC still pointed at the old one.
  The DCs a program keeps on a window now follow its bitmap whenever it
  is made again or resized (`disptest`'s full-screen child does what SDL
  does).

The corpus test (`tests/appcorpus/935-blobby-volley.py`) installs Mesa 3D,
DXVK and the game, goes Options, Graphic Options, Fullscreen Mode, OK
with the keyboard, checks the display is 800x600 and the main menu's
screenshot, ends the game with Alt+F4 and checks the display is back at
2560x1600.
