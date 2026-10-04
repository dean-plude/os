## Steam: connected UDP sockets, WSADuplicateSocket and read-only file handles

Steam's browser (`steamwebhelper.exe`, Chromium 126) restarted every few
minutes, and its network process logged Winsock error 10038
(`WSAENOTSOCK`).  Three NovaOS gaps were behind it:

- **Connected UDP sockets.**  Chromium's DNS client and its IPv6 probe
  `connect` a UDP socket to a peer and then use `send`, `recv` and
  `getsockname` on it.  NovaOS's kernel only connected TCP sockets and
  answered "not a socket" for UDP, which `ws2_32` reported as 10038.  A
  UDP socket can now be connected: it sends to its peer, receives only
  from it, and `getsockname` reports the source address the route picks.
- **`WSADuplicateSocket`** was missing.  It now duplicates the socket's
  handle into the target process and fills in `WSAPROTOCOL_INFO` the way
  Windows does (the handle in `dwProviderReserved`), and
  `WSASocket(FROM_PROTOCOL_INFO)` in the child takes it.
- **Handle access rights.**  Chromium's browser checks, with
  `NtQueryObject(ObjectBasicInformation)`, that a file handle it hands to
  a child holds no write, delete or ownership rights, and stops with a
  breakpoint otherwise.  NovaOS reported every handle as holding all
  rights.  File handles now remember the access they were opened with
  (generic rights mapped as Windows maps them), handles duplicated with
  narrower access remember it too, and `NtQueryObject` reports it.

With the three fixed, the browser no longer stops, and about half an hour
in Steam opens its first window of its own (an "Unexpected Transport
Error" dialog whose text is not drawn yet); the browser's network process
still restarts every minute or two
([compatibility.md](../compatibility.md#steam)).

The "Thread creation failed" messages seen once in `steam.exe`'s log did
not come back in about three hours of runs, and NovaOS never logged a
refused thread in them.

- **Tests**: `looptest` connects a UDP socket (its local name, its peer,
  dropping a stranger's datagram) and hands a socket to a child process
  with `WSADuplicateSocket`; `chrometest` checks the access
  `NtQueryObject` reports for read-only and read-write sections and files.
