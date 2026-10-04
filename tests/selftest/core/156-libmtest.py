# libmtest (tools/selftest.py's core suite)
DOC = '`libmtest` (the 32-bit C runtime\'s `_libm_sse2_*` and `_CI*` math, which MSVC-built SDL2 imports)'
TESTS = [
    Test('libmtest x86', r'C:\Programs\x86\libmtest.exe', [r'libmtest: \d+ passed, 0 failed']),
]
