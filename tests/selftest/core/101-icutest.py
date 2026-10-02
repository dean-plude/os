# icutest: ICU (icu.dll) as .NET loads it, and kernel32's locales, 64- and
# 32-bit (tools/selftest.py's core suite)
DOC = '`icutest` (ICU and locales, 64- and 32-bit)'
TESTS = [
    Test('icutest', 'icutest', [r'icutest: \d+ passed, 0 failed']),
    Test('icutest x86', r'C:\Programs\x86\icutest.exe', [r'icutest: \d+ passed, 0 failed']),
]
