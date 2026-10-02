# usptest: Arabic and Devanagari shaped through Uniscribe (usp10 on
# HarfBuzz in novatext.dll) and drawn by ExtTextOut, 64- and 32-bit
DOC = '`usptest` (Arabic and Devanagari shaped through Uniscribe and drawn by `ExtTextOut`, 64- and 32-bit)'
TESTS = [
    Test('usptest', 'usptest', [r'usptest: \d+ passed, 0 failed']),
    Test('usptest x86', r'C:\Programs\x86\usptest.exe', [r'usptest: \d+ passed, 0 failed']),
]
