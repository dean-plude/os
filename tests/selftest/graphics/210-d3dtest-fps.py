# d3dtest's frame-rate test: the same Direct3D 9 scene on Venus (the host's
# GPU through the virtio-gpu) and on lavapipe (NovaOS's CPU); Venus must
# draw more frames per second
TESTS = [
    Test('frame rate x64', r'C:\Tests\d3dtest.exe fps 10', [r'd3dtest fps: 5 passed, 0 failed'], timeout=900,
         shot=r'fps: Venus for'),
]
