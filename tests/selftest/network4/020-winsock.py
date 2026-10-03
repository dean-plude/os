# Winsock over IPv4 to tools/h2server.js's HTTP/1.1 port
TESTS = [
    Test('winsock IPv4', 'netcat -p 18080 10.0.2.2 /hello', [r'10\.0\.2\.2 -> 10\.0\.2\.2 \(IPv4\)',
                                                          r'HTTP/1\.1 200 OK', r'Hello over HTTP/1\.[01]']),
]
