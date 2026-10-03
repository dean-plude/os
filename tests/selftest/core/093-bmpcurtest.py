# bmpcurtest: pointers made from a program's bitmaps, as GTK makes them (tools/selftest.py's core suite)
DOC = '`bmpcurtest` (1-, 4-, 8- and 16-bit DIB sections, cursors from bitmaps with alpha or monochrome masks)'
TESTS = [
    Test('bmpcurtest', 'bmpcurtest', [r'bmpcurtest: \d+ passed, 0 failed']),
]
