## Services: Steam's service starts under the control manager

Steam's client started its service, `SteamService.exe /RunAsService`,
over and over, and every start failed with `ERROR_SERVICE_REQUEST_TIMEOUT`
(1053).  The service checks whether its copy in `Common Files\Steam` is
the one Steam shipped by comparing version, size and last-write time; if
they differ it copies the new one over, restarts itself through an updater
and exits before it connects to the control manager.  NovaOS's `CopyFile`
gave every copy a fresh last-write time, so the copy never matched and the
service updated itself forever.

- **`CopyFile`** (and `CopyFileEx`, `CopyFile2`) keeps the source's
  last-write time and its read-only, hidden and system attributes, as on
  Windows.  Steam's service now updates itself once and then runs as a
  service.
- **The service control manager** gives `NOVA_SERVICE` to the service
  process only, in its own environment block, instead of setting it in the
  caller's environment for a moment (where other threads' programs could
  inherit it), and starts the process suspended until its id is written
  down.  Only that process may connect as the service; a program it starts
  first (Steam's updater) gets `ERROR_FAILED_SERVICE_CONTROLLER_CONNECT`,
  as on Windows.
- **`ControlService`** refuses a pause, continue or parameter-change
  control the service has not said it accepts with
  `ERROR_INVALID_SERVICE_CONTROL`, before it reaches the service.
- **Test**: `svctest` installs itself as a service the way Steam's is
  installed (a name with spaces, `"path" /service`), starts it with
  arguments, follows the status handshake to `SERVICE_RUNNING`, sends it
  controls, stops and deletes it, in 64 and 32 bits.

Steam's service now stops on its next missing piece: `SteamService.dll`
calls `StopTraceA`, and NovaOS has no event-tracing controller functions
yet ([compatibility](../compatibility.md#steam)).
