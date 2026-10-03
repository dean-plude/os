# overlaptest: overlapped sockets on an I/O completion port over 127.0.0.1,
# as Python's asyncio uses them (tools/selftest.py's network suite)
DOC = '`overlaptest` (AcceptEx, ConnectEx and WSARecv pending on a completion port, CancelIoEx, closing a socket with a receive pending), 64- and 32-bit'
TESTS = [
    Test('overlaptest', 'overlaptest', [r'overlaptest: \d+ passed, 0 failed']),
    Test('overlaptest x86', r'C:\Programs\x86\overlaptest.exe', [r'overlaptest: \d+ passed, 0 failed']),
]
