# d3d9test none: NovaOS's own d3d9.dll before DXVK is installed (Qt
# WebEngine, and so GOG GALAXY's client, imports it): every export Windows'
# d3d9.dll has; Direct3DCreate9 returns NULL and Direct3DCreate9Ex
# D3DERR_NOTAVAILABLE, as on a PC with no Direct3D 9 driver; 64- and 32-bit
TESTS = [
    Test('d3d9 without DXVK x64', 'd3d9test none', [r'd3d9test: \d+ passed, 0 failed']),
    Test('d3d9 without DXVK x86', r'C:\Programs\x86\d3d9test.exe none', [r'd3d9test: \d+ passed, 0 failed']),
]
