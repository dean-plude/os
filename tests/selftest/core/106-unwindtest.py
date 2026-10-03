# unwindtest: RtlUnwindEx as MinGW's C++ runtimes (libunwind, libgcc) use it
# (tools/selftest.py's core suite)
DOC = '`unwindtest`'
TESTS = [
    Test('unwindtest', 'unwindtest', [r'unwindtest: \d+ passed, 0 failed']),
]
