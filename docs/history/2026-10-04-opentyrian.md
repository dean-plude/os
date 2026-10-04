## Tyrian on Direct3D 9

The fourth free game is the first that draws with Direct3D 9: Tyrian 2.1,
Epic MegaGames' 1995 shoot 'em up (freeware since 2004), on OpenTyrian,
its free and open-source engine.  It installs from the App Store (the
official 64-bit zip, which carries the freeware game data) and draws
through SDL2's Direct3D 9 renderer, SDL's first choice on Windows, which
NovaOS's `d3d9.dll` hands to DXVK on Mesa 3D's Vulkan (llvmpipe)
([compatibility.md](../compatibility.md)).

It needed no NovaOS changes.  What the corpus test now checks, in one run:

- **Direct3D 9 through DXVK in a real game**: OpenTyrian's attract-mode
  demo plays its first level in a window, every frame a Direct3D 9
  texture drawn by DXVK and presented through Vulkan's Win32 surface.
- **Going to full screen and back**: Alt+Enter switches the game to full
  screen; SDL resets the Direct3D 9 device with a back buffer the size of
  the display, and DXVK makes a new swap chain for it.
- **Menus from the keyboard** into a new game (one player, episode 1,
  normal), to the game's own menu between levels.  OpenTyrian fades
  between its menus, slowly without KVM, and takes no keys while it
  fades, so the test presses Enter only once the screen stands still,
  until the game menu shows.  Started by hand from there, level 1 plays
  in full screen with the keyboard (the README's screenshot).
- **Its music** (AdLib music, synthesized by OpenTyrian's own OPL emulator) plays
  through the sound card for the whole run.

Found and left for later:

- **The display mode never changes**: OpenTyrian's full screen is SDL's
  desktop full screen (a window covering the display), as most SDL games
  use, so NovaOS's display-mode switch for a Direct3D 9 exclusive full
  screen (`Windowed = FALSE`) is still to be tried with a game that asks
  for one.
