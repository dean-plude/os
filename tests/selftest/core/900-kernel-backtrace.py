# Keep this last (900): crash.exe asks the kernel to fault, which must print
# a backtrace with names, and NovaOS halts.
DOC = '`crash kernel`, a deliberate kernel fault whose serial log must show a backtrace with function names'
TESTS = [
    Test('kernel backtrace', 'crash kernel', [r'Backtrace:\r?\n  #0 [0-9a-f]{16}  KeCrashTestFault\+0x[0-9a-f]+\r?\n'
                                              r'  #1 [0-9a-f]{16}  KeCrashTest\+0x[0-9a-f]+\r?\n'
                                              r'  #2 [0-9a-f]{16}  sys_nova_bugcheck\+0x[0-9a-f]+\r?\n'],
         timeout=60, crash=True),
]
