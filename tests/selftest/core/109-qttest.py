# qttest: what Qt programs need (glyph outlines, C++/WinRT HSTRINGs, Windows Hello, the process DACL)
DOC = '`qttest` (what Qt programs need: `GetGlyphOutline`, C++/WinRT `HSTRING`s, Windows Hello, the process DACL)'
TESTS = [Test('qttest', 'qttest', [r'qttest: \d+ passed, 0 failed'])]
