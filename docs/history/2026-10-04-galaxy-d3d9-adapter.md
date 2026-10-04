## GOG GALAXY: Qt finds the system's OpenGL (a Direct3D 9 adapter for the display card)

GOG GALAXY's client opened its sign-in window and started its first
Chromium renderer, then Qt WebEngine ended it with "Could not get handle
for shared context", even with the App Store's Mesa 3D installed.  Qt 6
decides between the system's OpenGL and its own software one
(`opengl32sw.dll`, which GOG GALAXY does not ship) by reading the display
card's IDs from Direct3D 9 and looking them up in its GPU blocklist.
NovaOS's `d3d9.dll` without DXVK returned NULL from `Direct3DCreate9`, so
Qt saw vendor 0, device 0, which its blocklist calls "Standard VGA" and
turns OpenGL off for.  With no `opengl32sw.dll` Qt had no OpenGL at all,
and Qt WebEngine's shared context had no WGL context behind it.

- **`d3d9.dll`** (`userland/d3d9/adapter.c`, x64 and x86): without DXVK,
  `Direct3DCreate9` and `Direct3DCreate9Ex` now give NovaOS's own
  `IDirect3D9`/`IDirect3D9Ex`, as Windows' basic display adapter does: an
  adapter per monitor, named by its display card's PCI vendor, device,
  subsystem and revision (QEMU's standard VGA is 1234:1111), with
  `\\.\DISPLAYn`, the monitor, the current mode, the 32-bit modes and an
  adapter LUID.  There is still no Direct3D 9 driver behind it:
  `CheckDeviceType`, `GetDeviceCaps` and `CreateDevice` answer
  `D3DERR_NOTAVAILABLE`, so programs take their software paths as before.
  With DXVK installed every call still goes to DXVK.
- **Kernel** (`kernel/um/um_gui.c`): `NtNovaGuiCtl` `CTL_ADAPTER` gives
  the n-th PCI display controller's IDs.
- **Tests**: `d3d9test none` (graphics suite, 64- and 32-bit) checks the
  object: one adapter per monitor, a nonzero vendor and device, the
  display's mode and modes, the monitor, the LUID, and no device.
- **Where GOG GALAXY stops now**: with Mesa 3D installed, Qt uses Mesa's
  OpenGL (llvmpipe) and the shared-context abort is gone.  Chromium's GPU
  thread then needs Direct3D 11 for ANGLE: without DXVK it finds no EGL
  display and Qt WebEngine faults on a null object; with DXVK it draws on
  DXVK, but its compositor shares D3D11 textures between devices, which
  DXVK on lavapipe cannot (`VK_KHR_external_memory_win32`), so its
  context is lost and Chromium stops on a check
  ([compatibility.md](../compatibility.md)).  Without Mesa 3D, Qt still
  finds only OpenGL 1.1, as on a PC with no OpenGL driver.
