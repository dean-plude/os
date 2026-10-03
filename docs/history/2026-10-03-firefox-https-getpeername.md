## Firefox over HTTPS: getpeername and shutdown (Phase 16.4)

With `http://` pages loading, every `https://` page stayed blank.  The
server saw a TCP connection close without a TLS ClientHello.  Firefox's
network log showed the first write failing with
`PR_ADDRESS_NOT_SUPPORTED_ERROR`: before it starts a handshake, NSS asks
the socket for its peer (`getpeername`) and accepts only an IPv4 or IPv6
address.  NovaOS filled in a socket's peer only when a *blocking*
`connect` returned, so after Firefox's non-blocking connect the peer came
back empty.

Once handshakes ran, the kernel crashed in lwIP's pool allocator a few
minutes in.  Firefox ends its TLS connections with
`shutdown(SD_BOTH)`, which NovaOS passed to lwIP as "close": lwIP then
frees the connection's control block on its own (at once when unread data
forces a reset, or when the connection ends, with no callback once the
receive side is shut), while the socket still pointed at it, and the
later `closesocket` freed it a second time.

- The kernel's TCP layer (`kernel/net/sock.c`) records the peer when the
  connection is made, so `getpeername` answers after a blocking or a
  non-blocking `connect` alike.
- `shutdown` shuts the receive side in NovaOS's own socket (unread and
  later data is dropped, `recv` returns 0) and only sends lwIP the FIN;
  lwIP reports the connection's end through the error callback
  (`ERR_CLSD`, now an orderly close rather than a reset), which clears the
  socket's pointer.
- `looptest` (network suite) checks `getpeername` after a non-blocking
  connect over 127.0.0.1 and ::1, and three rounds of
  `shutdown(SD_BOTH)` with unread data followed by the peer closing.
- With these, Floorp completes TLS handshakes (a self-signed test server
  gets its certificate warning page), and typing into a form field and
  scrolling a long page work.
