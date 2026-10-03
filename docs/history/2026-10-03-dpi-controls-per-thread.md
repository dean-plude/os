## user32's own parts at 192 DPI, and per-thread DPI awareness

- **Controls, menus and dialogs follow the window's DPI.**  Since
  per-monitor DPI, a DPI-aware program on a 192 DPI monitor got twice the
  pixels, but user32 still drew its own parts at their 96 DPI sizes in
  them, so they came out half size.  Now, in a window at 192 DPI, the
  built-in controls (buttons, check boxes and radio buttons, group boxes,
  statics, edits, list boxes, combo boxes and their drop-down lists,
  scroll bars), the non-client area (borders, client edges, the menu bar,
  window scroll bars and the caption hit area) and popup menus are drawn
  twice the size, with the UI font (Segoe UI 9 point) at 192 DPI.  List
  box items, combo box fields and menu bars grow with the font.  Dialog
  templates are laid out with their font made at the dialog's DPI, so
  dialog units (`MapDialogRect`, the controls' places and the dialog's
  size) scale with it, and `MessageBox` is sized at the DPI it opens at.
  The common controls (list view, tree view, header, toolbar, rebar,
  status bar, tabs, links, ComboBoxEx) use the UI font at their window's
  DPI.  Windows at 96 DPI, including every window of a DPI-unaware
  program, look as before.
- **Metrics and stock fonts at the system DPI.**  `GetSystemMetrics`
  returns its sizes (scroll bars, caption, menu, borders, icons, ...) at
  the system DPI the calling thread sees, as on Windows: twice the 96 DPI
  values in a system-aware program started on a 192 DPI monitor, and the
  96 DPI ones for unaware threads.  So do `SystemParametersInfo`'s
  `SPI_GETNONCLIENTMETRICS` (whose ANSI form now fills in the fonts
  rather than returning zeros), `SPI_GETICONTITLELOGFONT` and
  `SPI_GETICONMETRICS`, `GetDialogBaseUnits`, and gdi32's stock fonts
  (`DEFAULT_GUI_FONT`, `SYSTEM_FONT` and the others).
  `GetSystemMetricsForDpi` and `SystemParametersInfoForDpi` give them at
  any DPI.
- **Awareness per thread.**  `SetThreadDpiAwarenessContext` used to be
  remembered and reported while everything followed the process.  Now a
  thread's context decides what it sees (`GetSystemMetrics`, monitor and
  cursor positions, `GetDpiForSystem`, stock fonts), and a window keeps
  the context of the thread that created it: its coordinates, bitmap and
  DPI are that awareness's, so an unaware thread of an aware program
  makes windows the desktop scales up and an aware thread of an unaware
  program makes sharp ones that get `WM_DPICHANGED`.  While a window
  procedure runs, its thread has the window's context, as on Windows.
  `SetProcessDpiAwarenessContext` sets the process default, which threads
  that never set their own follow.  `SetThreadDpiHostingBehavior` and
  `GetThreadDpiHostingBehavior` are accepted.
- `dpitest` checks it: at 192 DPI it measures a window of its own
  per-monitor-aware thread against one made by a thread switched to the
  unaware context (list box items, combo box field, scroll bar, menu bar,
  dialog units and dialog size twice the size, `GetDpiForWindow`,
  `GetWindowDpiAwarenessContext`, the window procedure's context, the
  unaware window's logical rectangle), and the system metrics, stock font
  and non-client metrics at 96; its unaware child does the same with a
  per-monitor-aware thread and its system-aware child with an unaware
  thread, where the metrics and fonts are at 192.  It also checks the
  `DPI_AWARENESS_CONTEXT_*` pseudo-handles.
- Not yet: a window's own parts are sized when it is created and drawn,
  so an edit or combo box made at 96 DPI keeps its measured line height
  when its window moves to 192 DPI (Windows rescales per-monitor v2
  dialogs on a DPI change; NovaOS does not); `GetWindowRect` and the
  other coordinate calls on a window of another awareness return that
  window's own coordinates rather than converting them; fixed pixel
  sizes inside the common controls (header minimum height, indents,
  margins) are still 96 DPI ones.
