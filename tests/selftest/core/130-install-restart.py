# an installer replacing a running program: done at the next boot
# (MoveFileEx DELAY_UNTIL_REBOOT).  Later tests run in the new boot.
DOC = ('an installer that replaces a running program and finishes after a restart (`filetest install`, '
       '`shutdown /r`, `filetest installed`)')
TESTS = [
    Test('install in use', 'filetest install', [r'filetest install: \d+ passed, 0 failed']),
    Test('restart', 'shutdown /r', [r'Pending file operations at boot: 2 done, 0 failed'], reboot=True),
    Test('installed', 'filetest installed', [r'filetest installed: \d+ passed, 0 failed']),
]
