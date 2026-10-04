## WebView2: the browser keeps its host's connection

With the runtime's browser process running, the host still got no
controller: the browser connected to the host's Mojo pipe and closed it
at once, the host started a second browser, and
`CreateCoreWebView2Controller` failed with `RPC_E_DISCONNECTED`.  The
GPU process's missing Direct3D 11 adapter, blamed before, was not the
cause (it fails later and the browser falls back to software).

- **Who is at the other end of a pipe**: Chromium checks that the
  server of the pipe it is given is the process named in the pipe's
  name (the host), with `GetNamedPipeServerProcessId`.  NovaOS answered
  0 for it and for `GetNamedPipeClientProcessId`, so the browser dropped
  the host.  Pipes now keep the process that created the server end and
  the one that connected, and answer `FSCTL_PIPE_GET_PIPE_ATTRIBUTE`
  (`ServerProcessId`, `ClientProcessId` and the session ids) as Windows'
  pipe file system does; kernel32 asks it, and gained
  `GetNamedPipeClientSessionId` and `GetNamedPipeServerSessionId`.
- **shlwapi by ordinal**: the browser's delay-load hook turned the
  missing ordinal 14 (`GetAcceptLanguagesA`) into a breakpoint; shlwapi
  now exports it, `GetAcceptLanguagesW` (15), `ConnectToConnectionPoint`
  (168) and `IUnknown_GetWindow` (172).
- **Tests**: `pipetest` connects a child process to a pipe and checks
  each side's process and session ids.
- **Where it stops now**: the browser accepts the host and makes the
  WebView's window, and the host then moves it into its own window with
  `SetWindowPos` and `SetParent` on the browser's window, a window of
  another process.  NovaOS's windows belong to one process each (user32
  keeps the window tree in the process), so both fail with
  `ERROR_INVALID_WINDOW_HANDLE` and the controller with `0x80070578`
  ([compatibility.md](../compatibility.md#webview2)).
