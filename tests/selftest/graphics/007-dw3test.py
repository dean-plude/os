# tools/dw3test: DirectWrite's Windows 10 font model as Chromium and Skia use
# it: IDWriteFactory2/3, the system font set and face references, matching
# by full and PostScript name, set builders, collections from sets,
# IDWriteFont3/IDWriteFontFace3, system and built font fallback, grayscale
# glyph run analysis and rendering params 3; 64- and 32-bit (C:\Tests)
TESTS = [
    Test('dw3test x64', r'C:\Tests\dw3test.exe', [r'dw3test: \d+ passed, 0 failed']),
    Test('dw3test x86', r'C:\Tests\dw3test32.exe', [r'dw3test: \d+ passed, 0 failed']),
]
