## GOG GALAXY: its client service starts and serves the client

`GalaxyClientService.exe` ended at start-up with "abnormal program
termination", so starting it gave error 1053 and the client then faulted.
Its socket server is Poco's `SocketReactor`, whose `PollSet` on Windows is
wepoll: an epoll built on the `\Device\Afd` driver and keyed events.
NovaOS had neither, so the poll set's constructor threw and nothing caught
it.  Four gaps stood between the service and the client; each is now what
Windows does, and nothing in GOG GALAXY is skipped or changed.

- **AFD polling** (`kernel/um/um_afd.c`): `NtCreateFile` opens
  `\Device\Afd\...` helper handles, and `IOCTL_AFD_POLL` through
  `NtDeviceIoControlFile` polls sockets for accept, receive, send, the peer
  closing or resetting, a failed connect and the socket closed here.  A
  poll that is not ready yet stays pending until a socket is or its
  time-out (relative, absolute, none or zero) passes, then writes the
  ready handles back, finishes its status block and sets its event or
  posts its `ApcContext` to the helper's completion port (kernel32 pumps a
  bound helper's results onto the port).  `NtCancelIoFileEx` ends one or
  all, and closing the helper cancels the rest.  ws2_32's `WSAIoctl` gives
  the socket itself for `SIO_BASE_HANDLE` and the other `SIO_BSP_HANDLE*`
  codes; x86 programs reach the same through WoW64.
- **Keyed events**: `NtCreateKeyedEvent`, `NtOpenKeyedEvent`,
  `NtWaitForKeyedEvent` and `NtReleaseKeyedEvent`.  A release waits for a
  waiter on the same key (and the other way round), each with an optional
  time-out; an odd key is refused and a NULL handle is the process's own.
- **The TCP connection tables** (iphlpapi): `GetExtendedTcpTable` (the
  basic, owner-PID and owner-module classes, listeners, connections or
  both, IPv4 and IPv6, sorted on request), `GetTcpTable` and
  `GetTcp6Table` list the kernel's TCP sockets with their states and the
  process that made or accepted each.  They were empty, so the service
  could not tell which process had connected and dropped every connection.
- **A UDP socket bound to 127.0.0.1 or ::1 sending**: lwIP refused a
  source address that is not the interface's own, so Poco's
  `PollSet::wakeUp`, which sends a byte to its own loopback socket, threw
  every time a handler was added and when the service stopped.
- **Unhandled C++ exceptions** are named in the log when they end a
  program (`unhandled C++ exception TYPE: what`).
- **Tests**: `afdtest` (34 checks: polls finishing on a completion port or
  an event, time-outs, cancelling, closing, a loopback UDP socket waking
  its own poll, keyed events) and `tcptabletest` (19), each 64- and 32-bit.
  The service also checks that the caller is `GalaxyClient.exe` in its
  own folder with `QueryFullProcessImageNameW`, which #217 made work for
  other processes.
- **Where GOG GALAXY stops now**: the service starts, recognises the
  client, deletes its stale lock files, clears its compatibility flags and
  stops itself after 30 idle seconds, as on Windows.  The client shows its
  NTFS warning; past it, it finishes initialising and then faults inside
  `RtlVirtualUnwind` while a C++ exception is dispatched
  ([compatibility.md](../compatibility.md)).
