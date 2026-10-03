# stltest: the C++ standard library (msvcp140.dll and its satellites), 64- and 32-bit
DOC = '`stltest` (the C++ standard library, 64- and 32-bit)'
TESTS = [
    Test('stltest x64', 'stltest', [r'stltest: cout works', r'stltest: \d+ passed, 0 failed']),
    Test('stltest x86', r'C:\Programs\x86\stltest.exe', [r'stltest: cout works', r'stltest: \d+ passed, 0 failed']),
]
