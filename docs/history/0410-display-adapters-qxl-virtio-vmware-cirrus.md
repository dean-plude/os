## Display adapters: QXL, virtio, VMware, Cirrus, and their modes after sleep

- **More adapters with the DISPI registers** (`kernel/hal/display.c`): the
  VBE driver now also drives QEMU's QXL (`-vga qxl`), virtio-vga
  (`-vga virtio`) and VMware SVGA II (`-vga vmware`, whose VGA core has
  them, with the framebuffer in BAR1) besides the standard VGA,
  bochs-display and VirtualBox's VBoxVGA.  The adapter table follows
  OVMF's QemuVideoDxe (BSD-2-Clause-Patent).  All of them get run-time
  resolutions and Settings > Display names the adapter.
- **Cirrus Logic GD5446** (`-vga cirrus`): 800x600 and 640x480 at 32 bpp,
  set with QemuVideoDxe's VGA and Cirrus register tables.  The bootloader
  now only picks 32-bit GOP modes, so Cirrus boots in 800x600 instead of
  its 24-bit 1024x768 mode, which drew garbled.
- **Modes after S3** (`DisplayResume`): the driver sets the current mode
  again on wake from what it knows, not from registers saved through I/O
  ports, so bochs-display (MMIO only) and Cirrus come back too; the page
  that was on screen stays on screen, and the desktop is redrawn in case
  video memory was lost.
- `tools/novarun.py --display NAME` boots on another adapter (`cirrus`,
  `vmware`, `qxl`, `virtio`, or a `-device` such as `bochs-display`).
- Tested in QEMU on all six adapters: switch to a non-boot mode with
  `disptest W H`, `sleeptest`, `system_wakeup`, and the desktop is back in
  that mode; also with page flipping (`-global VGA.vgamem_mb=64`).
- Not yet: real GPUs (Intel, AMD, NVIDIA) and virtio-gpu without VGA have
  no driver, so they stay on the UEFI framebuffer, and after sleep they
  show whatever the firmware's wake path sets up, which is often nothing.
