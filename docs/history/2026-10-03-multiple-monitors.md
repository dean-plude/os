## More than one monitor

- **Each further display adapter is another monitor.**  Besides the boot
  display, the display driver drives every other Bochs/QEMU DISPI adapter
  it finds (QEMU's `-device secondary-vga` or `bochs-display`; up to four
  monitors), each with its own list of modes, set at run time through its
  BAR2 registers (`[DISPLAY] Head 1: QEMU secondary-vga ...`).  After S3
  each one gets its mode back.
- **One desktop across them.**  The GDI's back buffer covers the virtual
  desktop: the primary monitor at (0, 0), the others beside, above or
  below it (negative coordinates included).  Each monitor has its own
  scale: the desktop is drawn at the largest, and a 1280x800 monitor next
  to a 2560x1600 one shows each 2x2 block averaged, so both show the same
  logical size.  The other monitors show the wallpaper; the dock, Start
  menu and desktop icons stay on the primary.
- **Windows and the pointer.**  The pointer moves across the edges where
  monitors touch and stops at the outer ones, and is drawn on the screen
  it is on, at that screen's scale.  A dragged window goes with the
  pointer onto the other monitor; maximizing and snapping (by dragging to
  an edge, or Win+Left/Right) fill that monitor's work area, which is all
  of it on monitors without the dock.  After a change of mode or layout
  each window stays on its monitor.
- **The Win32 calls report the real layout.**  `EnumDisplayMonitors`,
  `GetMonitorInfo` (`\\.\DISPLAY1`, `\\.\DISPLAY2`, ..., the work area,
  `MONITORINFOF_PRIMARY`), `MonitorFromWindow`/`Point`/`Rect` (with the
  `MONITOR_DEFAULTTO*` fallbacks), `EnumDisplayDevices` (an adapter per
  monitor, and the monitor on it), `EnumDisplaySettings` and
  `ChangeDisplaySettingsEx` for a named display (its modes, and its place
  in `dmPosition` / `DM_POSITION`), and `GetSystemMetrics`'
  `SM_CMONITORS` and `SM_*VIRTUALSCREEN`.
- **Settings > Display arranges them.**  With more than one monitor the
  page shows them to scale: drag one to move it (it snaps against the
  others' edges and lines up with them), click one to choose it, and the
  resolution buttons below apply to the chosen one.  The layout and each
  display's mode are kept where Windows keeps them,
  `HKLM\SYSTEM\CurrentControlSet\Control\Video\{NovaOS-Display}\000N`
  (`DefaultSettings.*`, and `Attach.RelativeX/Y` for the place), and come
  back at the next boot (`[DISPLAY] Display 2 goes at (0, 800)`).
- `montest` (graphics self-tests, which now boot with a QEMU secondary-vga
  as the second monitor) checks all of that, moves the pointer across, and
  leaves a window on the second monitor for the screenshot; the test saves
  one PNG per monitor (`montest.png`, `montest-2.png`).
  `tools/novarun.py --monitors 2` boots with two monitors the same way.
- Not yet: more than one output of one adapter (QXL or virtio-gpu heads),
  a monitor plugged in or out while running, and per-monitor DPI that
  programs see: they all get 96 DPI logical pixels, as before.
