# wintabtest: pen tablets through wintab32.dll, with a synthetic pen (tools/selftest.py's core suite)
DOC = '`wintabtest` (wintab32 with no pen and with a synthetic one: contexts, packets, pressure)'
TESTS = [
    Test('wintabtest', 'wintabtest', [r'wintabtest: \d+ passed, 0 failed']),
]
