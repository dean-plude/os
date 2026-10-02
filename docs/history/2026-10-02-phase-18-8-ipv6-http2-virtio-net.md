## IPv6, HTTP/2 and virtio-net (Phase 18.8)

- **virtio-net** (`kernel/drivers/virtio_net.c`): virtio 1.0 network
  adapters (QEMU `virtio-net-pci`), through the modern PCI capabilities;
  one receive and one transmit queue of 64 buffers, polled like the e1000
  driver.  The network stack tries the e1000 first, then virtio-net, and
  sets either up again after sleep.
- **IPv6** in lwIP: a link-local address, SLAAC addresses from router
  advertisements, DNS servers from RDNSS (added beside DHCP's, not in their
  place: a small change to `nd6.c`), MLD, and AAAA lookups (IPv6 first
  when there is a global IPv6 address).  `ipconfig` shows the addresses,
  `ping -6`, `curl -6` and `wget -6` force IPv6 (`-4` forces IPv4), and
  `curl http://[addr]/` takes literals.
- **Winsock over IPv6**: `AF_INET6` sockets (dual-stack: IPv4 peers show
  as v4-mapped), `sockaddr_in6` in `connect`, `bind`, `accept`,
  `sendto`/`recvfrom` and the name calls; `getaddrinfo` returns IPv6 and
  IPv4 addresses (with `AI_V4MAPPED`, `AI_CANONNAME`, numeric hosts with
  `%zone`, service names), and `inet_pton`/`inet_ntop`,
  `WSAStringToAddress`/`WSAAddressToString` and `getnameinfo` handle IPv6.
  `netcat` resolves with `getaddrinfo` and takes `-4`/`-6`/`-p`.
- **winhttp.dll** (`userland/winhttp`) became a real HTTP client: sessions,
  connections and requests, request headers, request bodies
  (`WinHttpWriteData`), `WinHttpQueryHeaders` (by index, name, number or
  date), `WinHttpQueryDataAvailable`/`WinHttpReadData`, redirects, Basic
  credentials, `WinHttpCrackUrl`/`WinHttpCreateUrl`, options and time-outs,
  and asynchronous sessions that report through the status callback.
  HTTPS goes through Schannel (secur32); with
  `WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL` it offers `h2` by ALPN and speaks
  HTTP/2 through [nghttp2](https://nghttp2.org/) 1.64.0 (MIT, vendored in
  `third_party/nghttp2`) when the server picks it
  (`WINHTTP_OPTION_HTTP_PROTOCOL_USED` says which).  Each request opens its
  own connection; there is no proxy support.
- **Tests**: `tools/selftest.py --suite network` boots twice with a
  virtio-net card.  On QEMU's user network it checks `ipconfig`, `ping`,
  Winsock over IPv4 and `httptest suite` (`userland/programs/httptest.c`:
  HTTP/2 negotiated, a 300 KB body, POST, redirects, a refused untrusted
  certificate, chunked HTTP/1.1, the asynchronous API) against
  `tools/h2server.js` (Node).  On an IPv6-only network, `tools/v6peer.py`
  (a router, DNS and HTTP server reached through a QEMU datagram netdev,
  so the host needs no IPv6) checks SLAAC and RDNSS, `ping -6`, `curl -6`
  and Winsock over IPv6.
- Also fixed: the USB hot-plug thread could be enumerating when the
  machine went to sleep, so after the wake its command timed out and the
  USB keyboard did not come back (an intermittent `powertest` failure).
  Sleep now waits for it, and it stays idle until the controller is
  running again.
