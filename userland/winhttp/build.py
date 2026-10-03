# winhttp.dll's build hooks (tools/build_userland.py calls them with itself
# as b).  HTTP/2 comes from nghttp2 (third_party/nghttp2, MIT), compiled
# into the DLL.
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NGHTTP2 = os.path.join(ROOT, 'third_party', 'nghttp2', 'lib')
NG_FLAGS = ['-I', os.path.join(NGHTTP2, 'includes'), '-DNGHTTP2_STATICLIB']


def cflags(b):
    """flags for winhttp's own sources"""
    return NG_FLAGS


def objs(b, odir):
    """nghttp2's library for this architecture, compiled in parallel; an
    object newer than its source and the library's headers is reused"""
    srcs = [os.path.join(NGHTTP2, f) for f in sorted(os.listdir(NGHTTP2)) if f.endswith('.c')]
    headers = [os.path.join(NGHTTP2, h) for h in os.listdir(NGHTTP2) if h.endswith('.h')]
    flags = b.cflags() + NG_FLAGS + ['-I', NGHTTP2, '-w', '-DWIN32', '-DHAVE_WINDOWS_H', '-DHAVE_GETTICKCOUNT64']
    return b.compile_many(srcs, odir, 'nghttp2_', flags, headers)
