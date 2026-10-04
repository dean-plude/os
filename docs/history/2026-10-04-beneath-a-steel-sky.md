## Beneath a Steel Sky on ScummVM, and OpenGL 1.1 that draws

The second free game from GOG plays: Beneath a Steel Sky, which GOG gives
away with ScummVM.  GOG's own copy needs an account to download, so the
corpus takes ScummVM's Windows installer (Inno Setup: a 32-bit setup
program that installs the 64-bit ScummVM) and the freeware floppy release
Revolution and ScummVM publish.  ScummVM installs silently, starts the
game, Esc skips the intro and a click walks Robert Foster along the
gantry of the first scene ([compatibility.md](../compatibility.md)).

- **What ScummVM imports**: `wldap32.dll` (new, `userland/wldap32`: LDAP
  sessions made as Windows makes them; binds and searches answer
  `LDAP_SERVER_DOWN`, as with no server, since NovaOS has no LDAP client),
  `CertFreeCTLContext`, `GetDeviceGammaRamp`/`SetDeviceGammaRamp` (no
  gamma ramp), `ImmGetIMEFileNameA`, `CM_Get_Parent` and
  `CM_Get_Device_ID{A,W}` from `setupapi` (cfgmgr32's), Winsock 1's
  blocking hooks (`WSACancelBlockingCall` and the rest, answering as
  Winsock 2 does), and for the 32-bit build `__p__iob` and
  `__lc_codepage`.  The gamma, IME, `__p__iob` and `GetDIBits` changes
  are the same lines as the game-controller pull request's, so either
  merges cleanly after the other.
- **OpenGL 1.1 that draws**: with no OpenGL driver installed, ScummVM's
  OpenGL renderer showed a black window, because NovaOS's own OpenGL 1.1
  ("GDI Generic") only cleared.  It now draws 2D as Windows' does
  (`userland/opengl32/draw.c`): textures (RGBA, RGB, BGRA, luminance and
  alpha bytes, nearest or linear, clamped or repeated), the three matrix
  stacks with `glOrtho`/`glFrustum`/`glTranslatef`/`glScalef`, vertex,
  colour and texture-coordinate arrays, `glDrawArrays`, `glDrawElements`
  and `glBegin`/`glEnd` filling triangles, strips, fans, quads and
  polygons, modulated or replaced by the texture, alpha-tested,
  scissored and blended, each pixel of an edge two triangles share drawn
  once.  No depth buffer, lighting, fog, lines or points; Mesa 3D from
  the App Store is still the OpenGL for anything more.
- **App Store**: ScummVM, under Media.
- **Tests**: app corpus `920-gog-beneath-a-steel-sky` (installs ScummVM,
  starts the game, skips the intro, checks a click walks Foster, and
  compares the screen with `tests/reference/beneath a steel sky.png`);
  `glgeneric` draws a textured quad, a blended quad and a scissored one
  and reads them back, 64- and 32-bit.
- **Seen, not fixed**: Inno Setup's log says it found an earlier 32-bit
  install of the program it has just registered, because NovaOS has no
  separate `WOW6432Node` registry view yet; the game's sound is not
  checked (the corpus runs without a sound card).
