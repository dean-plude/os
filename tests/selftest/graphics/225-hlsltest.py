# tools/hlsltest: NovaOS's HLSL compiler (d3dcompiler_47.dll on Wine's
# vkd3d-shader) as Chromium's ANGLE and games use it: vs/ps 4_0 and 5_0 and
# ps_3_0 compiled, errors, #include and macros, D3DPreprocess, D3DReflect,
# D3DDisassemble and the container parts, then the shaders drawn with
# Direct3D 11 on DXVK (a textured, constant-buffer-tinted triangle read
# back), 64- and 32-bit (C:\Tests)
TESTS = [
    Test('hlsltest x64', r'C:\Tests\hlsltest.exe', [r'D3D11 pixels \(vs_4_0/ps_4_0\)', r'hlsltest: \d+ passed, 0 failed'], timeout=600),
    Test('hlsltest x86', r'C:\Tests\hlsltest32.exe', [r'D3D11 pixels \(vs_4_0/ps_4_0\)', r'hlsltest: \d+ passed, 0 failed'], timeout=600),
]
