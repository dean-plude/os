# crtthreads: fiber-local storage callbacks, the per-thread locale
# (_configthreadlocale) in msvcrt.dll and ucrtbase.dll, and getenv results
# that stay whole across threads, 64- and 32-bit
DOC = '`crtthreads` (FLS callbacks, per-thread locale and thread-safe `getenv`, 64- and 32-bit)'
TESTS = [
    Test('crtthreads x64', 'crtthreads', [r'crtthreads: \d+ passed, 0 failed']),
    Test('crtthreads x86', r'C:\Programs\x86\crtthreads.exe', [r'crtthreads: \d+ passed, 0 failed']),
]
