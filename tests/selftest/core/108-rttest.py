# rttest: the UCRT's C99 complex functions, conio's _cprintf, and the DLL
# search directories (AddDllDirectory, SetDllDirectory), 64- and 32-bit
DOC = '`rttest` (complex math, conio, DLL directories, 64- and 32-bit)'
TESTS = [
    Test('rttest x64', 'rttest', [r'rttest: cprintf 42', r'rttest: \d+ passed, 0 failed']),
    Test('rttest x86', r'C:\Programs\x86\rttest.exe', [r'rttest: cprintf 42', r'rttest: \d+ passed, 0 failed']),
]
