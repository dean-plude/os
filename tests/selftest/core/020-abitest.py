# abitest: Windows 10 1903 x64 layouts and system-call numbers (tools/selftest.py's core suite)
DOC = ('`abitest` (the PEB, TEB, `KUSER_SHARED_DATA`, `CONTEXT` and loader layouts, ntdll\'s stubs and the '
       'system-call numbers, against Windows 10 1903 x64)')
TESTS = [
    Test('abitest', 'abitest', [r'abitest: \d+ passed, 0 failed']),
]
