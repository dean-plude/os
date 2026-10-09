# tools/d3dtest angle: Direct3D 11 brought up the way ANGLE's D3D11 back
# end does it in Chromium's GPU process (the screen's DC as the EGL
# display, adapter choice, device, DXGI 1.2, the queries, a swap chain and
# a WARP device), on DXVK on Venus, 64- and 32-bit (C:\Tests)
TESTS = [
    Test('d3dtest angle x64', r'C:\Tests\d3dtest.exe angle', [r'ANGLE adapter  Virtio-GPU Venus', r'ANGLE draw pixel  ff0000',
                                                               r'd3dtest: \d+ passed, 0 failed'], timeout=600),
    Test('d3dtest angle x86', r'C:\Tests\d3dtest32.exe angle', [r'ANGLE adapter  Virtio-GPU Venus', r'ANGLE draw pixel  ff0000',
                                                               r'd3dtest: \d+ passed, 0 failed'], timeout=600),
]
