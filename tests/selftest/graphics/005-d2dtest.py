# tools/d2dtest: Direct2D geometry answers, and a scene that must match the
# image tools/d2dtest/reference.py draws with Skia; 64- and 32-bit (C:\Tests)
TESTS = [
    Test('d2dtest x64', r'C:\Tests\d2dtest.exe 4', [r'd2dtest: \d+ passed, 0 failed', r'ok   scene matches the Skia reference'],
         shot=r'Direct2D scene on screen'),
    Test('d2dtest x86', r'C:\Tests\d2dtest32.exe 4', [r'd2dtest: \d+ passed, 0 failed', r'ok   scene matches the Skia reference'],
         shot=r'Direct2D scene on screen'),
]
