# linktest (tools/selftest.py's core suite): hard links on drive C:
DOC = '`linktest`'
TESTS = [
    Test('linktest', 'linktest', [r'linktest: \d+ passed, 0 failed'], settle=2),
]
