# loadtest: Winsock under a browser's or a game client's load (many sockets,
# select/WSAPoll/WSAEventSelect over all of them, TIME-WAIT churn, parallel
# downloads from tools/h2server.js's HTTP/1.1 port), as Steam's client puts on it
DOC = '`loadtest` (401 TCP sockets open at once, `select` and `WSAPoll` over 200, a `WSAEventSelect` registration on each that `closesocket` ends, 100 UDP sockets, 400 connections opened and closed in turn, 24 parallel downloads)'
TESTS = [
    Test('loadtest', 'loadtest 10.0.2.2 18080', [r'loadtest: \d+ passed, 0 failed']),
]
