## Teeworlds in full screen, with its music

The third free game plays, and this time its sound is checked: Teeworlds
0.7.5, the free and open-source 2D shooter, installs from the App Store
(its official 64-bit zip), starts in full screen on Mesa 3D's OpenGL and
plays its menu music through NovaOS's sound card while it goes through its
first-start questions to its start menu
([compatibility.md](../compatibility.md)).

- **The dock steps aside for full-screen programs**: Teeworlds starts in
  full screen, as most games do, and NovaOS's dock and tray were drawn
  over it.  Windows' taskbar goes behind a window that covers the whole
  display; NovaOS's dock now does the same while such a window is active
  (`dock_hidden` in `kernel/wm/desktop.c`), and comes back when the
  program goes to a window, another window is activated, or Start opens
  (the Windows key).  Clicks where the dock would be then reach the game.
- **Sound in the app corpus**: `App(sound=(hz, ms))` wanted a pure tone,
  which only a test file plays.  `App(sound=(None, ms))` now asks for any
  sound, such as a game's music, for that long in all, counted only
  between the program's first test starting and its last one ending, so
  another program's sound in the same run does not count
  (`wavcheck.sounding_ms`; `tools/wavcheck.py FILE --sound MS`).
  Teeworlds plays about two minutes of music in its run.
- **`taskkill /IM name.exe`** in the Terminal stops every running copy of
  a program by its image name, as Windows' `taskkill` does.

Teeworlds needs OpenGL 1.2 (it makes a 3D texture at start-up), more
than NovaOS's own OpenGL 1.1, so like Krita and Chocolate Doom it runs on
the App Store's Mesa 3D.

Found and left for later:

- **The mouse does not move in it**: Teeworlds' menus use SDL's relative
  mouse mode, which reads Raw Input (`WM_INPUT`); NovaOS does not send
  Raw Input yet (being added in its own change), so the pointer stays put
  and the test answers the questions with Enter.
- **Joining a game on its own server** (`teeworlds_srv.exe` on the same
  machine, then `connect 127.0.0.1:8303`) times out under TCG: the
  handshake works, but the client's render thread stays busy in Mesa for
  about ten seconds (llvmpipe compiling shaders, as far as the threads'
  states show) while the main thread waits for it without reading the
  network, and both ends give up after their fixed ten-second timeout.
  It should pass with KVM; a corpus round under KVM will tell.
- **An IPv6 socket bound to `[::]` after an IPv4 one on the same port
  fails with `WSAEADDRINUSE`**, even with `IPV6_V6ONLY` set, because
  NovaOS's IPv6 sockets always take both families.  On Windows,
  `IPV6_V6ONLY` is on by default and the two binds coexist.  The
  Teeworlds server carries on over IPv4 only.
