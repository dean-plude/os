## Controls rescaled on a DPI change, coordinates across awareness contexts, comctl32 at 192 DPI

- **Controls follow a DPI change.**  User32's controls were measured
  when they were made, so a combo box, list box or edit control made at
  96 DPI kept its line and item heights when its window moved to a 192
  DPI monitor (or the monitor's DPI changed), and drew its text twice
  the size in a field half as tall.  Now, when a window's DPI changes,
  its built-in controls that use the default font measure it again at
  the new DPI (list box items, a combo box's field and height, an edit
  control's lines and margins).  In a per-monitor v2 dialog, as
  Windows' dialog manager does, the controls' places and sizes scale
  with the dialog and the template's font is made again at the new DPI
  (8 points: 11 pixels at 96, 21 at 192) for the dialog and the controls
  that use it, so dialog units follow.  The common controls get
  `WM_DPICHANGED_AFTERPARENT` and measure their default font and sizes
  again (tree view, list view, toolbar, status bar, rebar, ComboBoxEx).
- **Coordinates across awareness contexts.**  `GetWindowRect` and the
  other coordinate calls on a window of another DPI awareness returned
  that window's own coordinates.  Now, as on Windows, they are converted
  to the calling thread's: a per-monitor-aware thread sees an unaware
  window in its own 192 DPI pixels and an unaware one sees an aware
  window in logical pixels.  This covers `GetWindowRect`,
  `GetClientRect`, `ClientToScreen`, `ScreenToClient`,
  `MapWindowPoints`, `GetWindowInfo`, `GetWindowPlacement`,
  `SetWindowPos`/`MoveWindow` (whose coordinates are the caller's) and
  `WindowFromPoint`, and another program's windows, whose rectangles
  the desktop gives in logical pixels.
- **comctl32 at 192 DPI.**  The common controls used the UI font at
  their window's DPI but kept fixed 96 DPI sizes around it.  Now their
  margins, gaps and minimum sizes scale with the window's DPI: the
  header's minimum height, text margins and divider grab zone, the tree
  view's indent (19 pixels at 96 DPI, 38 at 192), item height, check
  boxes and buttons, the list view's rows, icon cells and check boxes,
  the toolbar's default padding, separators and drop-down arrows, the
  status bar's height, borders, icons and size grip, the tab control's
  padding and borders, the trackbar's channel and thumb, the up-down
  control's width and the rebar's gripper and band borders.  Sizes a
  program sets (`TVM_SETINDENT`, `TB_SETPADDING`, `TCM_SETPADDING`,
  `TVM_SETITEMHEIGHT`) are kept as given.
- `dpitest` checks it: comctl32's sizes in a 192 DPI window against a
  96 DPI one, a window and a dialog made at 96 DPI measured again at 192
  and back at 96, and the coordinate calls both ways between its
  per-monitor-aware thread and an unaware window.
- Not yet: a combo box's drop-down list is a window of its own and keeps
  its item height until it is made again; only 96 and 192 DPI exist
  (no 120 or 144).
