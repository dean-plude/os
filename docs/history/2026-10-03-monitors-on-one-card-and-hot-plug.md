## Monitors on one card, plugged in and out

- **A virtio GPU's outputs are monitors.**  A new driver,
  `kernel/drivers/virtio_gpu.c`, drives QEMU's `virtio-vga` and
  `virtio-gpu-pci` in 2D: each output with a monitor on it (up to 16 on
  one card, `max_outputs=N`) shows a picture in memory that the GDI draws
  on like on video memory, and the card copies what changed to the monitor
  (`TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`, after each frame and each
  pointer move).  Each output gets the usual list of modes plus the size
  its monitor asks for, which it starts in.  So one card is now enough
  for several monitors, where before each needed its own adapter
  (`[DISPLAY] Head 1: QEMU virtio-vga output 2, 1024x768, 18 mode(s)`).
- **The boot display moves over.**  A `virtio-vga` shows its VGA
  framebuffer on its first output only until a picture is set on any
  output, so when the boot display is a `virtio-vga` with more than one
  output, the primary monitor moves onto a picture in memory too, keeping
  what the screen shows (`[DISPLAY] QEMU virtio-vga has 3 outputs: the
  primary monitor is its output 1`); resolutions still change at run time
  and after S3 every output gets its picture back.  With one output it
  stays on the VBE driver, as before.
- **Monitors come and go.**  When a monitor is connected to or
  disconnected from an output the card says so, and the desktop lays
  itself out again: a new monitor goes to the right of the others, the
  windows (and the pointer) on one that went move to the nearest one left,
  maximized ones fill their new monitor's work area, and programs get
  `WM_DISPLAYCHANGE`; `EnumDisplayMonitors`, `GetMonitorInfo`,
  `SM_CMONITORS` and the virtual screen follow (`[SHELL] Monitors: 3 (were
  2)`).  In QEMU an output gets a monitor from a display window, or from a
  VNC client on that output (`-vnc ...,display=gpu,head=N`) asking for a
  desktop size; 0 x 0 takes it away.
- **The bootloader skips a GOP it can't draw on.**  OVMF's GOP for a
  `virtio-gpu-pci` has no framebuffer (Blt only); when that is the first
  one the firmware lists, the bootloader takes the next adapter's.
- `montest hotplug` (devices self-tests, a new "monitors" boot: one
  `virtio-vga` with three outputs and a monitor on the first) has the test
  plug monitors into the second and third outputs, put a window on the
  third, then unplug both; it checks the monitors, `WM_DISPLAYCHANGE`, and
  that the window and the pointer end up on a monitor that is left.  The
  screenshot is one PNG per output.  `tools/novarun.py` takes screenshots
  of every output of a `virtio-vga`/`virtio-gpu-pci` given an id and
  `max_outputs`.
- Not yet: per-monitor DPI that programs see (`GetDpiForMonitor`,
  per-monitor-aware DPI contexts and `WM_DPICHANGED` when a window crosses
  to a monitor with another scale): programs still get 96 DPI logical
  pixels on every monitor.  Hot-plugging a whole display adapter (PCI
  hot-plug) isn't handled either; monitors come and go on a virtio GPU's
  outputs.
