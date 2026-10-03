## App Store scroll bar

The App Store's list of programs had only a thin grey line at its right
edge to show where it was scrolled: it could not be clicked or dragged, so
the list moved only with the wheel and the keys.  It now has the same
scroll bar as File Explorer (`UiScroll` in `kernel/apps/apps.c`, from
[File Explorer scroll bars](#file-explorer-scroll-bars)), so the two
built-in apps scroll the same way.

- **The bar.**  The list of programs (every category, Installed included;
  the list is the Store's only view that scrolls) has a vertical scroll bar
  whenever its rows don't fit, 17 pixels wide with user32's arrows, trough
  and a thumb sized to the page, in place of the thin line.  The rows and
  their buttons make room for it, and it comes and goes as the window is
  resized and the category changes.
- **Using it.**  The arrows scroll a row (84 pixels), a click in the trough
  scrolls a page toward the pointer, the thumb drags, and a held arrow or
  trough repeats as in File Explorer.  The wheel still scrolls a row a
  notch, Up and Down a row, Page Up and Page Down a page, Home and End to
  the top and the bottom, and choosing a category goes back to the top.
- **`store open`.**  The Terminal's `store` command opens the App Store
  (or brings it forward) with `store open`, beside `store install` and
  `store close`.
- **The test.**  The graphics suite's `store scroll bar` runs `store open`
  and checks that All apps has a vertical bar, then that the wheel, Home,
  Page Down, End, a click on the down arrow, one in the trough and a drag
  of the thumb each move the list by what they should.  The Store logs a
  `[STORE] view:` line each time its view changes (the category, how far
  the list is scrolled, the bar and where the list is on the screen),
  which the test reads; Esc then closes the Store.
