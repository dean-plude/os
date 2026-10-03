# looptest: Winsock over 127.0.0.1 and ::1 (lwIP's loopback interface), as
# Firefox's parent and child processes use it, "localhost", and wsock32's ordinals
DOC = '`looptest` (a socket pair over 127.0.0.1 and ::1, data before `accept`, `localhost`, Winsock 1.1 ordinals)'
TESTS = [
    Test('looptest', 'looptest', [r'looptest: \d+ passed, 0 failed']),
]
