## Firefox loads pages: wsock32.dll (Phase 16.4)

Floorp showed a blank page for every URL and no request ever left the
machine, while the same page from a `file:` URL rendered.  The parent's
socket thread opened a TCP socket and closed it again at once, never
calling `connect`.  The caller was NSPR's `_PR_MD_SOCKET` in `nss3.dll`,
which imports Winsock 1.1 (`wsock32.dll`) by ordinal and calls
`ioctlsocket` (`FIONBIO`) right after `socket`.  NovaOS answered
`wsock32.dll` with `ws2_32.dll` on the assumption that the two share
their ordinals; they do not: in `wsock32` `inet_addr` is 10, `inet_ntoa`
11 and `ioctlsocket` 12, where `ws2_32` has `ioctlsocket` at 10.  So
NSPR's `ioctlsocket` landed in `inet_ntoa`, whose non-zero return read as
failure, and every socket was closed before use.

- **wsock32.dll** (`userland/wsock32`) is now its own DLL, with Winsock
  1.1's ordinals, calling `ws2_32`'s functions, plus the old blocking-hook
  calls (`WSAIsBlocking` and friends, which never block).  The alias in
  the kernel loader, `ntdll` and `tools/pe_imports.py` is gone.
- A socket `accept` gives is non-blocking when its listener is, as on
  Windows.  NSPR's socket pair (the socket thread's wake-up) counts on
  that: with a blocking accepted end, the socket thread stalled in `recv`
  holding a lock the main thread then waited for.
- `looptest` (network suite) checks `wsock32`'s ordinals, takes a socket
  non-blocking the way NSPR does, and checks the inherited mode.
- With it, Floorp fetches and renders `http://` pages served to QEMU's
  guest network.
