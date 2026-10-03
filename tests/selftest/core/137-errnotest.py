# errnotest: the C runtime's per-thread data (errno, _doserrno, rand's
# seed, strtok, gmtime's buffer) in msvcrt.dll and ucrtbase.dll, 64- and
# 32-bit
DOC = '`errnotest` (errno and the other C runtime per-thread state belong to each thread, 64- and 32-bit)'
TESTS = [
    Test('errnotest x64', 'errnotest', [r'errnotest: \d+ passed, 0 failed']),
    Test('errnotest x86', r'C:\Programs\x86\errnotest.exe', [r'errnotest: \d+ passed, 0 failed']),
]
