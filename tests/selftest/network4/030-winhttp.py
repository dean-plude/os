# winhttp against tools/h2server.js: HTTP/2 by ALPN, bodies, POST, a redirect,
# an untrusted certificate, chunked HTTP/1.1, asynchronous calls, keep-alive
# connection reuse, and a WebSocket echo
TESTS = [
    Test('winhttp HTTP/2', 'httptest suite https://10.0.2.2:18443 http://10.0.2.2:18080',
         [r'httptest: 11 passed, 0 failed']),
]
