# tools/d3dtest shared: a Direct3D 11 texture shared by NT handle between
# two DXVK devices and with a second process, as Chromium shares its frames
# with Qt WebEngine (GOG Galaxy); on Mesa's lavapipe, where NovaOS's Vulkan
# loader provides VK_KHR_external_memory_win32, 64- and 32-bit (C:\Tests)
TESTS = [
    Test('d3dtest shared x64', r'C:\Tests\d3dtest.exe shared', [r'd3dtest shared: \d+ passed, 0 failed'], timeout=600),
    Test('d3dtest shared x86', r'C:\Tests\d3dtest32.exe shared', [r'd3dtest shared: \d+ passed, 0 failed'], timeout=600),
]
