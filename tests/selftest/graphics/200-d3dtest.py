# tools/d3dtest on DXVK on Venus (Vulkan on the host's GPU through the
# virtio-gpu), 64- and 32-bit (C:\Tests)
TESTS = [
    Test('d3dtest x64', r'C:\Tests\d3dtest.exe 6', [r'D3D9 adapter  Virtio-GPU Venus', r'd3dtest: 17 passed, 0 failed'], timeout=900, shot=r'D3D9 pixels'),
    Test('d3dtest x86', r'C:\Tests\d3dtest32.exe 6', [r'D3D9 adapter  Virtio-GPU Venus', r'd3dtest: 17 passed, 0 failed'], timeout=900, shot=r'D3D9 pixels'),
]
