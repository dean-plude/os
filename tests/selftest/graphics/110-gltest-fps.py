# gltest's frame-rate test: the same OpenGL scene on virgl (the host's GPU
# through the virtio-gpu) and on llvmpipe (NovaOS's CPU); virgl must draw
# more frames per second
TESTS = [
    Test('OpenGL frame rate x64', r'C:\Tests\gltest.exe fps 10', [r'gltest fps: 5 passed, 0 failed'], timeout=900,
         shot=r'fps: virgl for'),
]
