# smftest: the C++17 special math functions (msvcp140_2.dll), 64- and 32-bit
DOC = '`smftest` (the C++17 special math functions in `msvcp140_2.dll`, 64- and 32-bit)'
TESTS = [
    Test('smftest x64', 'smftest', [r'smftest: cyl_bessel_j\(0, 1\) = 0\.765198', r'smftest: \d+ passed, 0 failed']),
    Test('smftest x86', r'C:\Programs\x86\smftest.exe',
         [r'smftest: cyl_bessel_j\(0, 1\) = 0\.765198', r'smftest: \d+ passed, 0 failed']),
]
