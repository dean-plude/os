# d3dcompiler_47.dll's build hooks (tools/build_userland.py calls them with
# itself as b).  The HLSL compiler is vkd3d-shader (third_party/vkd3d-shader,
# LGPL-2.1, Wine's), compiled unchanged into this DLL with the config.h and
# header shim in vkd3d/; the SPIR-V headers its SPIR-V back end includes come
# from third_party/spirv-headers (MIT).  d3dcompiler_47's own sources (MIT)
# use only vkd3d-shader's public API (vkd3d_shader.h).
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
VKD3D = os.path.join(ROOT, 'third_party', 'vkd3d-shader')
SPIRV = os.path.join(ROOT, 'third_party', 'spirv-headers', 'include')
SHADER = ['checksum', 'd3d_asm', 'd3dbc', 'dxbc', 'dxil', 'fx', 'glsl', 'hlsl', 'hlsl.tab', 'hlsl.yy', 'hlsl_codegen',
          'hlsl_constant_ops', 'ir', 'msl', 'preproc.tab', 'preproc.yy', 'spirv', 'tpf', 'vkd3d_shader_main']
COMMON = ['debug', 'error', 'memory']
API = ['-I', os.path.join(VKD3D, 'include'), '-DLIBVKD3D_SHADER_SOURCE']


def cflags(b):
    """flags for d3dcompiler_47's own sources"""
    return API


def objs(b, odir):
    """vkd3d-shader for this architecture"""
    flags = b.cflags() + API + ['-I', os.path.join(HERE, 'vkd3d'), '-I', os.path.join(VKD3D, 'include', 'private'),
                                '-I', os.path.join(VKD3D, 'libs', 'vkd3d-shader'), '-I', SPIRV, '-DHAVE_CONFIG_H',
                                '-include', os.path.join(HERE, 'vkd3d', 'nova_vkd3d.h'), '-w']
    srcs = [os.path.join(VKD3D, 'libs', 'vkd3d-shader', s + '.c') for s in SHADER]
    srcs += [os.path.join(VKD3D, 'libs', 'vkd3d-common', s + '.c') for s in COMMON]
    headers = [os.path.join(HERE, 'vkd3d', h) for h in os.listdir(os.path.join(HERE, 'vkd3d'))]
    for d in (os.path.join(VKD3D, 'include'), os.path.join(VKD3D, 'include', 'private'),
              os.path.join(VKD3D, 'libs', 'vkd3d-shader')):
        headers += [os.path.join(d, h) for h in os.listdir(d) if h.endswith('.h')]
    return b.compile_many(srcs, odir, 'vkd3d_', flags, headers)
