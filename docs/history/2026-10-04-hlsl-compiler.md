## An HLSL compiler: d3dcompiler_47 on vkd3d-shader

NovaOS's `d3dcompiler_47.dll` had the blob functions and a `D3DCompile`
that answered "no HLSL compiler", because no permissively licensed one
exists.  Chromium's ANGLE compiles its Direct3D 11 shaders from HLSL at
run time, so GOG Galaxy's web view (Qt WebEngine) stopped at "Failed to
create D3D Shaders", and Steam's and WebView2's Chromium would stop there
too once they draw with Direct3D 11.

The compiler is now **vkd3d-shader 1.19**, Wine's HLSL compiler
(`third_party/vkd3d-shader`, LGPL-2.1, unchanged; its bison and flex
outputs are generated once and committed), compiled into
`d3dcompiler_47.dll` alone, 64- and 32-bit.  The DLL's own sources stay
MIT and use only vkd3d-shader's public API:

- `D3DCompile`, `D3DCompile2`, `D3DCompileFromFile`: vs/ps/gs/hs/ds/cs
  4_0 to 5_1 to DXBC, 1_x to 3_0 to Direct3D 9 bytecode, fx profiles to
  effects; macros, `#include` through the caller's `ID3DInclude` or
  `D3D_COMPILE_STANDARD_FILE_INCLUDE` (beside the including file), the
  matrix packing and backwards-compatibility flags, and the compiler's
  messages in the error blob.
- `D3DPreprocess` and `D3DDisassemble` (vkd3d-shader's preprocessor and
  assembly writer).
- `D3DReflect`: `ID3D11ShaderReflection` (every IID the SDKs have used,
  and `ID3D12ShaderReflection`) read from the shader's RDEF, signature,
  STAT, SHEX and SFI0 sections: constant buffers, variables and their
  types, bound resources, input/output/patch-constant parameters,
  instruction counts, thread group size, minimum feature level.
- The container: `D3DGetBlobPart` and the signature and debug-info
  shortcuts, `D3DStripShader`, `D3DSetBlobPart` (private data),
  `D3DReadFileToBlob`, `D3DWriteBlobToFile`.  The rest of the export
  table (`D3DAssemble`, the linker and function-linking graph, shader
  compression, trace disassembly) returns `E_NOTIMPL`, as vkd3d-shader has
  nothing to build them on.

The SPIR-V headers vkd3d-shader's SPIR-V back end includes are Khronos'
(`third_party/spirv-headers`, MIT).  The README's licence section names
the LGPL library and how to rebuild it.

- Tested: `tools/hlsltest` (graphics suite, `225-hlsltest`, 64- and
  32-bit, 36 checks): the profiles ANGLE uses, errors, `#include` and
  macros, the preprocessor, reflection, the disassembler and the
  container parts, then the vs/ps 4_0 and 5_0 shaders drawn with
  Direct3D 11 on DXVK (a textured, tinted triangle read back exactly).
  `delaytest` now checks that `D3DCompile` turns HLSL into DXBC.
