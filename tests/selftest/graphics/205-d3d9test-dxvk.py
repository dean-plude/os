# d3d9test dxvk: with the App Store's DXVK (its d3d9 installed as
# d3d9_dxvk.dll behind NovaOS's own d3d9.dll), Direct3D 9 calls reach DXVK,
# whose IDirect3D9 lists an adapter; 64- and 32-bit
TESTS = [
    Test('d3d9 through DXVK x64', 'd3d9test dxvk', [r'd3d9test: \d+ passed, 0 failed']),
    Test('d3d9 through DXVK x86', r'C:\Programs\x86\d3d9test.exe dxvk', [r'd3d9test: \d+ passed, 0 failed']),
]
