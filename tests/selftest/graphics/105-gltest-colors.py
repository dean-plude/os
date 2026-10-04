# gltest's colour-depth test on Mesa 3D's llvmpipe: four coloured bars in
# every colour depth Mesa offers a window (8:8:8, 5:6:5, 5:5:5:1, 4:4:4:4,
# 10:10:10:2), presented with SwapBuffers and read back with GetPixel.
# Mesa presents the 16-bit ones as DIBs with BI_BITFIELDS masks; SDL2 games
# (Chocolate Doom) get 4:4:4:4 and came out in wrong colours before gdi32
# honoured the masks
TESTS = [
    Test('gltest colors x86', r'C:\Tests\gltest32.exe colors llvmpipe', [r'gltest colors: \d+ passed, 0 failed'],
         timeout=600, shot=r'OpenGL 4:4:4:4'),
    Test('gltest colors x64', r'C:\Tests\gltest.exe colors llvmpipe', [r'gltest colors: \d+ passed, 0 failed'],
         timeout=600),
]
