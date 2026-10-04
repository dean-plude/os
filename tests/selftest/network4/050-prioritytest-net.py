# prioritytest net: the kernel's network thread runs above every program
# thread, so a loopback round trip completes in bounded time with a busy
# HIGH_PRIORITY_CLASS thread on every processor
DOC = '`prioritytest net` (with a busy HIGH_PRIORITY_CLASS thread on every processor, a byte sent over a 127.0.0.1 TCP connection and back takes at most 250 ms every time, 500 ms under QEMU TCG: the network thread runs above programs)'
TESTS = [
    Test('prioritytest net', 'prioritytest net', [r'prioritytest: 40 loopback round trips', r'prioritytest: PASS']),
]
