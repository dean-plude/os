## File Explorer scroll bars

File Explorer's file list used to show only the rows that fit: the rest
could be reached only with the arrow keys, nothing said there was more,
and the wheel did nothing.  It now has scroll bars, as Windows Explorer
does.

- **The bars.**  The list has a vertical scroll bar whenever its rows don't
  fit and a horizontal one when the window is narrower than its columns
  need (the Name column keeps at least 200 pixels; below that the columns
  scroll sideways, the header with them).  Each bar takes room from the
  other way, so the two can appear together, with the corner between
  them filled.  The sidebar (the places and the drives) gets a vertical
  bar of its own when it doesn't fit.  The bars come and go as the window
  is resized and as the folder changes, and the row only partly in view
  at the bottom is drawn too.
- **Using them.**  The wheel scrolls three rows a notch (the sidebar when
  the pointer is over it, sideways when the list only scrolls that way),
  the horizontal wheel scrolls sideways, the arrows scroll a row, a click
  in the trough scrolls a page toward the pointer, and the thumb drags;
  a held arrow or trough repeats after 350 ms, every 50 ms, until the
  thumb reaches the pointer.  Page Down moves the selection to the last
  row in view and then a page on, Page Up the same way up, Home and End
  go to the first and last rows, Left and Right scroll sideways, and the
  selection is kept in view, including a new folder or file and a click
  on the part-shown row.
- **One scroll bar for the built-in apps.**  The built-in apps draw with
  the kernel's GDI, not user32, so the bar is `UiScroll` in
  `kernel/apps/apps.c`: user32's parts, sizes and behaviour
  (`userland/user32/scroll.c`: 17-pixel arrows, a thumb sized to the
  page, `SetScrollInfo`'s range, page and position) in the dark theme's
  colours.  Other built-in apps can use it the same way.
- **The test.**  The graphics suite's `explorer scroll bars` opens File
  Explorer on `C:\Windows\System32` and checks the vertical bar is there,
  then that the wheel, Page Down, End, Home, a click on the down arrow and
  one in the trough each move the view by what they should.  Explorer
  logs a `[EXPLORER]` line each time its view changes (the folder, the
  rows shown, the bars and where the list is on the screen), which the
  test reads.
