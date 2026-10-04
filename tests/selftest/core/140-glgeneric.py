# glgeneric: OpenGL with no driver installed (no App Store Mesa 3D):
# opengl32.dll's own OpenGL 1.1 gives a pixel format and a context, clears,
# reads back and swaps, and draws in 2D (textures, vertex arrays, blending,
# scissor, glBegin/glEnd), 64- and 32-bit
DOC = '`glgeneric` (OpenGL 1.1 with no OpenGL driver installed, drawing in 2D, 64- and 32-bit)'
TESTS = [
    Test('glgeneric x64', 'glgeneric', [r'glgeneric: \d+ passed, 0 failed']),
    Test('glgeneric x86', r'C:\Programs\x86\glgeneric.exe', [r'glgeneric: \d+ passed, 0 failed']),
]
