# looptest: Winsock over 127.0.0.1 and ::1 (lwIP's loopback interface), as
# Firefox's parent and child processes use it, "localhost", wsock32's ordinals,
# connected UDP sockets and WSADuplicateSocket (Chromium's browser hands its network process sockets)
DOC = '`looptest` (a socket pair over 127.0.0.1 and ::1, `getpeername` and `shutdown`, data before `accept`, `localhost`, Winsock 1.1 ordinals, socket options that read back, and `TCP_NODELAY`, `SO_RCVTIMEO`, `SO_SNDTIMEO`, `SO_LINGER` and `SO_REUSEADDR` changing what a socket does, `connect` on a UDP socket, and `WSADuplicateSocket` handing a socket to another process)'
TESTS = [
    Test('looptest', 'looptest', [r'looptest: \d+ passed, 0 failed']),
]
