# crash reports (Phase 22.3): crashtest starts crash.exe, which writes
# through a NULL pointer, and reads the report NovaOS leaves in
# C:\NovaOS\Crashes (64- and 32-bit); then crash.exe from the Terminal,
# which names the report under the crash line, and "crashes last".
DOC = ('`crashtest` (crash.exe\'s report in `C:\\NovaOS\\Crashes`: exception, module+offset, stack, modules; '
       '64- and 32-bit), and the Terminal\'s `Crash report:` line and `crashes last`')
TESTS = [
    Test('crash report x64', 'crashtest', [r'crashtest: \d+ passed, 0 failed']),
    Test('crash report x86', r'C:\Programs\x86\crashtest.exe', [r'crashtest: \d+ passed, 0 failed']),
    Test('crash report named', 'crash', [r'Crash report: C:\\NovaOS\\Crashes\\crash-\d{8}-\d{6}-\d+\.txt'],
         builtin=True),
    Test('crashes last', 'crashes last', [r'NovaOS crash report', r'access violation \(0xC0000005\)',
                                          r'At:\s+crash\.exe\+0x[0-9a-f]+'], builtin=True),
]
