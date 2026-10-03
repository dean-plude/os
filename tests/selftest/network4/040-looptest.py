# looptest: Winsock over 127.0.0.1 and ::1 (lwIP's loopback interface), as
# Firefox's parent and child processes use it, and "localhost"
DOC = '`looptest` (a socket pair over 127.0.0.1 and ::1, data before `accept`, `localhost`)'
TESTS = [
    Test('looptest', 'looptest', [r'looptest: \d+ passed, 0 failed']),
]
