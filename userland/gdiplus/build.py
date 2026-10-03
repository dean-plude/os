# gdiplus.dll's build hooks (tools/build_userland.py calls them with itself
# as b).  GDI+ draws with plutovg (third_party/plutovg, MIT), compiled into
# the DLL.
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PLUTOVG = os.path.join(ROOT, 'third_party', 'plutovg')
PV_FLAGS = ['-I', os.path.join(PLUTOVG, 'include'), '-DPLUTOVG_BUILD', '-DPLUTOVG_BUILD_STATIC',
            '-DSTBI_NO_THREAD_LOCALS']        # stb_image's error string: no implicit TLS in the DLL


def cflags(b):
    """flags for gdiplus's own sources"""
    return PV_FLAGS


def objs(b, odir):
    """plutovg's sources for this architecture"""
    src = os.path.join(PLUTOVG, 'source')
    out = []
    for f in sorted(os.listdir(src)):
        if f.endswith('.c'):
            obj = os.path.join(odir, 'plutovg_' + f[:-2] + '.obj')
            b.cc(os.path.join(src, f), obj, PV_FLAGS + ['-w'])
            out.append(obj)
    return out
