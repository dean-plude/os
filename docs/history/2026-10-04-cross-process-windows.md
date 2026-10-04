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

The new `xpwin` selftest covers all of this with three processes.  With
it, `wv2host` gets its WebView2 controller, the page loads and a script
runs in it.  The browser then subscribed to the event log; wevtapi's
missing `EvtCreateBookmark` was a breakpoint through its delay-load hook,
so wevtapi now has the rest of the Event Log API, answering as a system
with no event channels (a subscription that never fires, a log with no
records, empty channel and publisher lists), and shell32 has
`SHCreateAssociationRegistration`.  The page is not drawn yet: Chromium's
software output in the GPU process asks for a DXGI factory, which
Windows gives even without a GPU and NovaOS does not.  For Chromium's
Direct3D path, a window that is a child of another process's window from
the start (the GPU process's own child window) and a DXGI swap chain
presenting to it remain.
