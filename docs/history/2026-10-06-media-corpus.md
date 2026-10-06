## Windowed DirectDraw video and stable recording captures

VLC selected DirectDraw but its primary surface remained black. A windowed
player attaches an HWND clipper and does not call SetDisplayMode, so cnc-ddraw's
game render thread never starts. NovaOS now presents a primary Blt to that
clipper's client DC in this case, converting screen coordinates and clipping
the source bounds. Fullscreen/game render-thread paths retain their behavior.
The adaptation is compiled only for NovaOS.

Audacity's recording viewport depends on how far recording auto-scrolled.
After saving, the corpus moves to the start and fits the project width before
comparing the existing reference. Recording, file checks and screenshot
thresholds remain required. GUI test reporting preserves an interaction failure
instead of replacing it with the later screenshot difference, and still saves
the capture for diagnosis.
