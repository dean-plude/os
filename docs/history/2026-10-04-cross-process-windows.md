## Windows of one process inside another's

A WebView2 host shows the page by moving the browser process's window
into its own: `SetParent` on a window another process made, then
`SetWindowPos` and `MoveWindow` to keep it in place, as every Chromium
embedder does.  NovaOS's window handles worked only in the process that
made them (beyond asking where another process's window was), so
`SetParent` failed with `ERROR_INVALID_WINDOW_HANDLE` and
`CreateCoreWebView2Controller` with `0x80070578`.  Window handles now
work across processes:

- **Messages**: `SendMessage`, `SendMessageTimeout`, `SendNotifyMessage`
  and `PostMessage` to another process's window go through the kernel to
  the thread that owns it, which runs the window procedure and answers;
  the sender keeps answering messages sent to it while it waits, as on
  Windows, so two processes calling each other do not deadlock.
  `WM_SETTEXT`, `WM_GETTEXT` and `WM_COPYDATA` carry their data across;
  other messages that carry pointers are refused, as Windows refuses
  them between processes.
- **Calls on the window**: `SetWindowPos`, `MoveWindow`, `ShowWindow`,
  `Get`/`SetWindowLong(Ptr)`, `EnableWindow`, `SetFocus`,
  `GetWindowText`, `GetClassName`, `GetWindowThreadProcessId` and
  `IsWindowEnabled` run in the owning thread.
- **`SetParent`**: a top-level window of another process goes inside a
  window of this one, child window or top-level.  It keeps its numeric
  position, now in the parent's client area, loses its frame, moves and
  hides with its parent and is clipped to it; clicking it gives it the
  keyboard while its parent's window stays the active one.
  `GetParent`, `GetAncestor`, `IsChild` and `MapWindowPoints` answer
  for it in both processes, and `SetParent(NULL)` (or the parent going
  away) makes it a top-level window again.
- **Drawing from a third process**: `GetDC` on another process's window
  gives a device context whose drawing lands in that window, as
  Chromium's GPU process draws into the browser's window in software.

The new `xpwin` selftest covers all of this with three processes.  What
remains for Chromium's Direct3D path is a window that is a child of
another process's window from the start (the GPU process's own child
window) and a DXGI swap chain presenting to it.
