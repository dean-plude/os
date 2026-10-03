# tools/dwtest: DirectWrite text layout with font fallback: Latin, Arabic and
# Devanagari in one line, each script in a font that has it, shaped and
# drawn; 64- and 32-bit (C:\Tests)
TESTS = [
    Test('dwtest x64', r'C:\Tests\dwtest.exe 4', [r'dwtest: \d+ passed, 0 failed'], shot=r'DirectWrite fallback on screen'),
    Test('dwtest x86', r'C:\Tests\dwtest32.exe 4', [r'dwtest: \d+ passed, 0 failed'], shot=r'DirectWrite fallback on screen'),
]
