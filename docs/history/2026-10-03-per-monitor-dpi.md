## Per-monitor DPI

- **A monitor can show DPI-aware programs its real density.**  The desktop
  works in logical pixels (96 DPI) and draws them at each monitor's scale,
  so a 2560x1600 screen shows a 1280x800 desktop twice as sharp.  Until
  now every program saw 96 DPI and drew at that size.  Now a monitor at
  scale 2 can be set to 192 DPI for DPI-aware programs: in Settings >
  Display ("DPI for DPI-aware apps"), kept in the registry as `LogPixels`
  under `...\Video\{NovaOS-Display}\000N`, or with `NtNovaGuiCtl`
  `CTL_SET_DPI` (op 30).  96, the default, changes nothing for anyone.
- **Programs get the awareness they ask for.**  user32 reads the
  manifest's `dpiAwareness` (`PerMonitorV2`, `PerMonitor`, `System`,
  `Unaware`, first known value wins) and `dpiAware` (`true`, `true/pm`,
  `per monitor`), the compatibility layer (`__COMPAT_LAYER` with
  `DpiUnaware`, `GdiDpiScaling` or `HighDpiAware`, as Windows' "override
  high DPI scaling" setting writes it), and `SetProcessDpiAwarenessContext`,
  `SetProcessDPIAware` and shcore's `SetProcessDpiAwareness` (which fail
  with access denied once the manifest or an earlier call decided, or a
  window exists).  Programs without either are unaware, as on Windows.
  `GetThreadDpiAwarenessContext`/`SetThreadDpiAwarenessContext`,
  `GetWindowDpiAwarenessContext`, `GetAwarenessFromDpiAwarenessContext`,
  `AreDpiAwarenessContextsEqual` and `GetDpiFromDpiAwarenessContext`
  report it.  `shcore.dll`, which programs that link `shcore.lib` import
  by name, now loads (as shlwapi, where its functions are).
- **What each kind sees.**  Unaware programs keep 96 DPI and logical
  pixels everywhere and are scaled up as before.  System-aware ones see
  the primary monitor's DPI as it was when they started, everywhere.
  Per-monitor-aware ones see each monitor's own: `GetDpiForMonitor`
  (shcore), `GetDpiForWindow`, window and client rectangles, mouse
  positions, `GetCursorPos`, monitor rectangles and work areas,
  `SM_CXSCREEN` and the virtual screen are in the screen's own pixels.
  `GetDpiForSystem`, `LOGPIXELSX/Y`, `AdjustWindowRectEx` (and
  `...ForDpi`), `GetSystemMetricsForDpi`, `SystemParametersInfoForDpi`
  and dialog fonts follow the DPI too.  The desktop gives an aware
  window's bitmap two pixels per logical pixel and shows them one to one
  on the screen, so its text and lines are as sharp as the desktop's own.
- **`WM_DPICHANGED`.**  When a per-monitor-aware window goes to a monitor
  of another DPI (dragged there, or moved with `SetWindowPos`), or its
  monitor's DPI changes, it gets `WM_DPICHANGED` with the new DPI and the
  suggested rectangle (the same place and size on the screen); a program
  that leaves it to `DefWindowProc` is put there anyway.  Per-monitor v2
  programs also get `WM_GETDPISCALEDSIZE` first and
  `WM_DPICHANGED_BEFOREPARENT`/`AFTERPARENT` on their child windows.
- `dpitest` (core self-tests) is per-monitor aware v2 by its manifest: it
  sets the boot's monitor to 192 DPI and back and checks
  `WM_DPICHANGED`, the suggested rectangle, its window's and the monitor's
  rectangles, and that an unaware and a system-aware child (started with
  `__COMPAT_LAYER`) see 96 and 192 DPI.  With a second monitor it moves
  its window there and back.
- Not yet: user32's own controls, menus and scroll bars drawn at 192 DPI
  for an aware program (they keep their 96 DPI sizes in its pixels), the
  stock fonts at the system DPI, threads of one process in different
  awareness contexts (coordinates follow the process), and DPI scaling of
  another process's window coordinates (`GetWindowRect` on a window of a
  program with another awareness).
