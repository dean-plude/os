## Built-in windows open in the same place every time

The nightly app corpus failed one program on every run: File Explorer's
This PC screenshot differed from its reference in 3.4% of its pixels.
NovaOS drew This PC correctly; the window had opened one cascade step
(32 pixels right, 28 down) further than in the reference, because an
earlier program in the run had opened and closed one of NovaOS's own
windows.

- **Cascade steps are given back**: a built-in app's window (File
  Explorer, Notepad, Settings, the Terminal, ...) used to take the next of
  eight cascade steps from a counter that only ever went up, so where a
  window opened depended on every window opened before it since boot.  It
  now takes the first step whose top edge no open window has
  (`AppCreateWindow` in `kernel/apps/apps.c`), so a closed window's step
  is used again and the same windows open in the same places, as
  Windows' own default positions do once the windows before them are
  closed.  With all eight steps taken it goes round as before.
- **The corpus's This PC check** (`tests/appcorpus/800-novaos-screens.py`)
  now passes on the nightly's full run; its reference
  (`tests/reference/this-pc.png`) was right and is unchanged.
