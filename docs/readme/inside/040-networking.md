- **Networking**: lwIP (TCP/IP over IPv4 and IPv6: DHCP, SLAAC, DNS over
  either), an HTTP/1.1 client, and Mbed TLS with the Mozilla root store.
  The loopback interface carries 127.0.0.1 and ::1 (and traffic to the
  machine's own addresses), and `localhost` resolves to both without DNS.
  Winsock (`ws2_32`, and `wsock32` with Winsock 1.1's ordinals) speaks IPv6
  and dual-stack sockets with `getaddrinfo`, and its socket options reach
  the TCP/IP stack (`TCP_NODELAY`, `SO_RCVTIMEO`/`SO_SNDTIMEO`,
  `SO_LINGER`, `SO_REUSEADDR`, `SO_KEEPALIVE`, `SO_BROADCAST`, `IP_TTL`,
  `SO_RCVBUF`/`SO_SNDBUF`) and read back;
  overlapped requests that have to wait (an `AcceptEx`, a `ConnectEx`, a
  `WSARecv` with nothing to read yet) stay pending and complete on an I/O
  completion port, which proactor event loops such as Python's asyncio need;
  `winhttp` is a real HTTP client over Schannel TLS, with HTTP/2 by ALPN
  (nghttp2).
