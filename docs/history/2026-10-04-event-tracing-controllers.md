## Event tracing: controllers and consumers with no sessions

Steam's service stopped with `0xC0000139` as soon as it ran: its
`SteamService.dll` imports the event-tracing (ETW) controller functions,
and NovaOS's `advapi32` had only the provider side.  The service stops
any old session of its own, tries to start a real-time one to watch
process starts, and when that fails watches processes another way.

- **Controllers** answer as Windows does when no logging session is
  running and none can be started.  `StartTrace` checks its properties
  block (`ERROR_BAD_LENGTH`, `ERROR_INVALID_PARAMETER`) and then finds the
  session limit reached (`ERROR_NO_SYSTEM_RESOURCES`); `StopTrace`,
  `ControlTrace`, `QueryTrace`, `FlushTrace` and `UpdateTrace` find no
  such session (`ERROR_WMI_INSTANCE_NOT_FOUND`); `EnableTrace`,
  `EnableTraceEx` and `EnableTraceEx2` need a running session;
  `QueryAllTraces` lists none.  A and W forms of each.
- **Consumers**: `OpenTrace` opens a real-time consumer, and
  `ProcessTrace` finds no session to read; a missing log file fails with
  `CreateFile`'s error, and NovaOS cannot read `.etl` files
  (`ERROR_NOT_SUPPORTED`).  `CloseTrace` closes a handle once.
- **Test**: `etwtest`, 64- and 32-bit, ending with the stop-then-start
  sequence Steam's service runs.

Steam's service now updates itself once, starts under the control
manager, installs its helper files and keeps running.  Steam itself still
stops where its browser does ([compatibility](../compatibility.md#steam)).
