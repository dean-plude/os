- Display: GPU-backed or at least faster blits.  ~~Mode changes~~ Done:
  run-time resolutions (Phase 12 onwards); the chosen one is kept across
  restarts, and windows a smaller mode shrank grow back when it is undone
  (Phase 18.9).  ~~More than one
  monitor~~ Done: one desktop across several display adapters, arranged in
  Settings and kept across restarts, with the Win32 monitor calls
  reporting it.  ~~Several monitors on one card, plugged in and out while
  running~~ Done: each output of a virtio GPU is a monitor, and the
  desktop and programs (`WM_DISPLAYCHANGE`) follow monitors connected or
  disconnected.  ~~Per-monitor DPI that programs see~~ Done: a monitor at
  scale 2 can show DPI-aware programs 192 DPI (Settings > Display); they
  get `GetDpiForMonitor`, per-monitor-aware contexts, `WM_DPICHANGED` and
  the screen's own pixels, while unaware ones keep 96 DPI; user32's
  controls, menus, dialogs and fonts follow each window's DPI, and threads
  can have their own awareness context.
