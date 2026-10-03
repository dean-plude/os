# looptest: Winsock over 127.0.0.1 and ::1 (lwIP's loopback interface), as
# Firefox's parent and child processes use it, "localhost", and wsock32's ordinals
DOC = '`looptest` (a socket pair over 127.0.0.1 and ::1, `getpeername` and `shutdown`, data before `accept`, `localhost`, Winsock 1.1 ordinals, socket options that read back, and `TCP_NODELAY`, `SO_RCVTIMEO`, `SO_SNDTIMEO`, `SO_LINGER` and `SO_REUSEADDR` changing what a socket does)'
TESTS = [
    Test('looptest', 'looptest', [r'looptest: \d+ passed, 0 failed']),
]
