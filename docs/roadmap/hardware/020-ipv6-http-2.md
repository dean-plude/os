- ~~IPv6, HTTP/2~~ Done (Phase 18.8): lwIP's IPv6 (SLAAC, RDNSS, MLD),
  dual-stack Winsock with `getaddrinfo`, `ping -6` and `curl -6`; a
  virtio-net driver; `winhttp` on Schannel with HTTP/2 by ALPN (nghttp2).
  The `network` self-test suite checks both.  Stateless DHCPv6 supplies DNS
  configuration; stateful DHCPv6 address assignment remains unsupported by
  the vendored lwIP client.  WinHTTP reuses HTTP/1.1 connections and supports
  WebSockets.  HTTP/3 remains to do; it needs a QUIC transport.
