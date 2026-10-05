DOC = '`waitmigrationtest` (yielding workers repeatedly wait-any and wait-all on unsignaled events, exercising current-thread lookup and timeout registration on SMP; x64 and x86)'
TESTS = [
    Test('wait migration x64', 'waitmigrationtest', [r'waitmigrationtest: \d+ workers, \d+ waits, 0 failed'], timeout=180),
    Test('wait migration x86', r'C:\Programs\x86\waitmigrationtest.exe',
         [r'waitmigrationtest: \d+ workers, \d+ waits, 0 failed'], timeout=180),
]
