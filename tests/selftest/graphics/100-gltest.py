# tools/gltest, 64- and 32-bit (C:\Tests): on Mesa's virgl (the host's GPU
# through the 3D virtio-gpu; NovaOS's opengl32.dll picks it once Venus is
# installed) and on Mesa 3D's llvmpipe (GALLIUM_DRIVER=llvmpipe)
TESTS = [
    Test('gltest x64 virgl', r'C:\Tests\gltest.exe 6 virgl', [r'gltest: 15 passed, 0 failed'], timeout=600, shot=r'GLSL '),
    Test('gltest x86 virgl', r'C:\Tests\gltest32.exe 6 virgl', [r'gltest: 15 passed, 0 failed'], timeout=600),
    Test('gltest x64 llvmpipe', r'C:\Tests\gltest.exe 6 llvmpipe', [r'gltest: 15 passed, 0 failed'], timeout=600),
    Test('gltest x86 llvmpipe', r'C:\Tests\gltest32.exe 6 llvmpipe', [r'gltest: 15 passed, 0 failed'], timeout=600),
]
