## Display persistence (Phase 18.9)

- **The chosen resolution survives a restart.**  Choosing a mode in
  Settings > Display, or `ChangeDisplaySettings` with
  `CDS_UPDATEREGISTRY`, saves it where Windows keeps it,
  `HKLM\SYSTEM\CurrentControlSet\Control\Video\{NovaOS-Display}\0000`
  (`DefaultSettings.XResolution`, `YResolution`, `BitsPerPel`), which
  reaches the disk with the rest of drive C:.  At boot, once drive C: and
  the registry are loaded and before the desktop starts, the kernel
  switches to that mode if the adapter has it (`[DISPLAY] Restored the
  saved mode WxH`); on another adapter without it, NovaOS stays in the
  boot mode.
- **Windows grow back.**  A window a smaller mode shrank or pushed aside
  remembers the frame it had and gets it back when a later mode has room
  for it, unless it was moved or resized in between.  Maximized windows
  already followed the work area.
- `disptest` checks both (46 checks): a 1000x640 window shrinks to fit
  800x600 and comes back to its size and place, and `CDS_UPDATEREGISTRY`
  writes the registry values.  The core self-tests save 1024x768 with
  `disptest 1024 768`, and after the suite's restart (`shutdown /r`)
  `disptest saved 1024 768` passes only if NovaOS came up in that mode.
- Not yet: a per-monitor layout (NovaOS drives one display).  (Done since:
  "More than one monitor".)
