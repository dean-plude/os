- ~~The desktop's redraws hold the file-system lock for the whole redraw
  (50-180 ms in QEMU without KVM), so file calls wait them out~~ Done
  (`savetest`: the file-system call waits under 100 ms during a save).
