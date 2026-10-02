# tlbtest: COM type libraries, 64- and 32-bit (tools/selftest.py's core suite)
DOC = '`tlbtest` (type libraries, 64- and 32-bit)'
TESTS = [
    Test('tlbtest', 'tlbtest', [r'tlbtest: \d+ passed, 0 failed']),
    Test('tlbtest x86', r'C:\Programs\x86\tlbtest.exe', [r'tlbtest: \d+ passed, 0 failed']),
]
