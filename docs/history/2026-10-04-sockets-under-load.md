## Networking: sockets under a browser's load

Steam's own downloads failed with "http error 0" and Chromium's sockets
inside it with `WSAENOBUFS` once enough connections were open.  Every
layer under Winsock had a fixed table sized for a small device, and the
first one to run out refused the next socket:

- **lwIP's pools** held 16 TCP and 8 UDP PCBs for the whole machine.  A
  TCP PCB stays taken while its connection is in TIME-WAIT or closing,
  and each DNS lookup in flight takes a UDP PCB of its own (a random
  source port), so 8 open connections or a handful of lookups used them
  up.  The pools are now 512 TCP, 64 listening and 128 UDP PCBs, with
  1,024 TCP segments, 512 receive buffers and a 1 MB heap for what is
  being sent; DNS asks up to 32 names at once on at most 8 source ports.
  lwIP still reclaims the oldest TIME-WAIT PCB when the pool is full,
  never a live connection.
- **The kernel's socket table** had 64 slots for every program together;
  it has 1,024 (each takes its 32 KB receive ring only while open).
- **`WSAEventSelect`** (and `WSAAsyncSelect`) kept 64 registrations and
  `closesocket` never ended one, so a program that registered its sockets
  (Chromium does, for every socket) got `WSAENOBUFS` from its 65th socket
  ever, and a reused handle value could get a closed socket's events.  The
  table grows as needed and `closesocket` ends the socket's registration,
  as on Windows.
- **`select`** copied the caller's sets into 64-entry ones on its stack,
  so a program that defines `FD_SETSIZE` larger (libcurl does) overran
  it; it now works on the caller's sets in place.  `WSAPoll` took at most
  64 sockets and now takes any number.  `winsock2.h` lets a program define
  `FD_SETSIZE` first, as Windows' does.
- **Errors say what happened**: `socket()` returns `WSAEMFILE` when no
  socket or handle is left and `WSAENOBUFS` only when memory is, and a
  refused `connect` reports `WSAEHOSTUNREACH`, `WSAEADDRINUSE` or
  `WSAEINVAL` where lwIP said so instead of `WSAENOBUFS` for all of them.
  `WSAStartup` reports no fixed socket limit (`iMaxSockets` 0), as Windows
  does for Winsock 2.
- **Test**: `loadtest` (network suite) opens 401 sockets at once, waits on
  all of them with `select`, `WSAPoll` and `WSAEventSelect`, closes and
  reopens them, opens and closes 400 connections in turn, opens 100 UDP
  sockets and runs 24 parallel downloads.  Before the change it failed
  with `WSAENOBUFS` after 8 connections and 7 UDP sockets.
