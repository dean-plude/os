- **Networking**: lwIP (TCP/IP over IPv4 and IPv6: DHCP, SLAAC, DNS over
  either), an HTTP/1.1 client, and Mbed TLS with the Mozilla root store.
  The loopback interface carries 127.0.0.1 and ::1 (and traffic to the
  machine's own addresses), and `localhost` resolves to both without DNS.
  Winsock (`ws2_32`) speaks IPv6 and dual-stack sockets with `getaddrinfo`;
  `winhttp` is a real HTTP client over Schannel TLS, with HTTP/2 by ALPN
  (nghttp2).
