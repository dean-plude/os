# schanneltest against tools/h2server.js as Qt's Schannel TLS backend (GOG
# Galaxy's) uses SSPI: manual certificate checks, the flags it asked for
# returned, the cipher and connection attributes, the server's certificate
# with its chain's store, CertGetCertificateChain on it (self-signed: an
# untrusted root), and an HTTP/1.1 request; wininet reports the network online
TESTS = [
    Test('schanneltest', 'schanneltest 10.0.2.2 18443', [r'schanneltest: \d+ passed, 0 failed']),
    Test('schanneltest online', 'schanneltest stores', [r'network: online', r'schanneltest: \d+ passed, 0 failed']),
]
