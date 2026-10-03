# tlsslots: TlsAlloc's 1024 expansion slots past the TEB's 64 (per thread,
# zero in new threads and again after TlsFree in every thread), and FLS
# callbacks at process exit, 64- and 32-bit
DOC = '`tlsslots` (1088 TLS indexes and FLS callbacks at process exit, 64- and 32-bit)'
TESTS = [
    Test('tlsslots x64', 'tlsslots', [r'tlsslots: \d+ passed, 0 failed']),
    Test('tlsslots x86', r'C:\Programs\x86\tlsslots.exe', [r'tlsslots: \d+ passed, 0 failed']),
]
