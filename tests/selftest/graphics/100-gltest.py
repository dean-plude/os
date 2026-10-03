# tools/gltest on Mesa's opengl32.dll, 64- and 32-bit (C:\Tests)
TESTS = [
    Test('gltest x64', r'C:\Tests\gltest.exe 6', [r'gltest: 14 passed, 0 failed'], timeout=600, shot=r'GLSL '),
    Test('gltest x86', r'C:\Tests\gltest32.exe 6', [r'gltest: 14 passed, 0 failed'], timeout=600, shot=r'GLSL '),
]
