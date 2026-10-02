# acltest, 64- and 32-bit (tools/selftest.py's core suite)
DOC = '`acltest` (64- and 32-bit)'
TESTS = [
    Test('acltest', 'acltest', [r'acltest: \d+ passed, 0 failed']),
    Test('acltest x86', r'C:\Programs\x86\acltest.exe', [r'acltest: \d+ passed, 0 failed']),
]
