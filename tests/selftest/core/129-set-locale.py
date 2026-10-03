# the user locale chosen with intl.exe: the boot after the restart (130)
# must still have it (131)
DOC = '`nlstest set ja-JP` (the user locale the restart must keep)'
TESTS = [
    Test('set user locale', 'nlstest set ja-JP', [r'nlstest set: \d+ passed, 0 failed'], settle=3),
]
