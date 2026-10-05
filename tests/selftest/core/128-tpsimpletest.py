DOC = '`tpsimpletest` (5120 one-shot thread-pool callbacks: callback context and calling convention, no leaked handles; 64- and 32-bit)'
TESTS = [
    Test('simple callbacks x64', 'tpsimpletest', [r'tpsimpletest: 5120 callbacks, correct context, no leaked handles'], timeout=180),
    Test('simple callbacks x86', r'C:\Programs\x86\tpsimpletest.exe',
         [r'tpsimpletest: 5120 callbacks, correct context, no leaked handles'], timeout=180),
]
