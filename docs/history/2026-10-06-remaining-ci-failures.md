## Installer capacity and app-corpus fixes

The disk installer sizes its EFI partition for the bundled kernel and boot
loader, a complete staged update, and 32 MiB of filesystem/configuration
space, rounded to 32 MiB with a 128 MiB minimum. It checks the layout before
detaching volumes or erasing the target disk. This matches the installation
image's capacity policy as bundled applications grow.

Windowed DirectDraw uses a 32-bit desktop primary surface and preserves
window coordinates without installing the fullscreen game hooks. VLC's
clipped video blits can then reach its child window. Fullscreen games retain
the existing renderer.

The app-corpus runner installs the MinGW compiler, CMake and Make used to
build PuTTY. Teeworlds' screenshot fixture keeps only the winter day map, so
its menu uses the same background regardless of the host's hour. Screenshot
tolerances, video-content assertions and the full app matrix remain intact.
