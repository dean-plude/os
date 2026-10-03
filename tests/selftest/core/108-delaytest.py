# delaytest (tools/selftest.py's core suite)
DOC = '`delaytest` (the DLLs Firefox delay-loads)'
TESTS = [
    Test('delaytest', 'delaytest', [r'delaytest: \d+ passed, 0 failed']),
    Test('delaytest x86', r'C:\Programs\x86\delaytest.exe', [r'delaytest: \d+ passed, 0 failed']),
]
