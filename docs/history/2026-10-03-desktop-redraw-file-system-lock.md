## The desktop's redraws no longer hold the file-system lock

The [save without locks](#saving-drive-c-without-holding-the-locks) work
left one long hold of the file-system lock: the desktop thread took it for
the whole of every redraw, because a few things it draws come from files
(program and file icons, the files on the desktop, the Start menu, most
built-in apps' windows).  A redraw takes 50-180 ms in QEMU without KVM, so
any file call (`GetFileAttributes`, `CreateFile`, a directory listing)
could wait that long, and on a busy CI runner `savetest` saw 275 ms
against its 250 ms limit.

- **Lock only what reads files.**  `kernel/wm/desktop.c` no longer takes
  the file-system lock around `WmComposite`.  What reads files takes it
  itself, around just that: program icons (`AppDrawProgramIcon`), the
  desktop's files (looking each one up and drawing its icon, not the
  labels' blurred shadows), the Start menu while it is open, and built-in
  apps' painters.  A built-in window whose painter reads no files sets the
  new `WND.paint_fs_free` (the Terminal: its prompt's path takes the lock
  for the moment it is read).  Program windows already drew without it.
- **Cheaper window edges.**  `GdiRoundBorderAlpha`, the hairline edge of
  every window and of the dock, worked out a distance for every pixel
  inside the box only to skip it; it now skips the inside of each row
  straight away (the same pixels are drawn).  A redraw of the desktop with
  the Terminal open went from 76-107 ms to 53-68 ms in QEMU (TCG), which
  is also how long calls behind the desktop lock can wait for one.
- **Measured** with `savetest` in QEMU (TCG, 2 processors), the longest
  wait for the file-system lock during the 32 MiB save fell from 177 ms to
  1.3-12 ms; no hold of the file-system lock by the desktop went over
  15 ms.  `savetest` now fails when the file-system call waits 100 ms or
  more (the kernel and desktop calls keep the 250 ms limit).
