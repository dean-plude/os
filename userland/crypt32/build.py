# crypt32.dll's build hooks (tools/build_userland.py calls them with itself
# as b).  Its certificates and chains (certs.c) are built on Mbed TLS
# (third_party/mbedtls), configured as for NetSurf
# (userland/netsurf/mbedtls_user_config.h), and NetSurf's glue for it
# (entropy, roots), as secur32's Schannel is.
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MBEDTLS = os.path.join(ROOT, 'third_party', 'mbedtls')
TLS_GLUE = os.path.join(ROOT, 'userland', 'netsurf')
MB_FLAGS = ['-I', os.path.join(MBEDTLS, 'include'), '-I', os.path.join(MBEDTLS, 'library'), '-I', TLS_GLUE,
            '-DMBEDTLS_CONFIG_FILE="mbedtls_user_config.h"',
            # as NetSurf builds it: Mbed TLS's POSIX/GCC paths, not MSVC's
            '-std=gnu99', '-w', '-D_NOVAOS', '-DNOVA_POSIX', '-U_WIN32', '-U_WIN64', '-fgnuc-version=4.2.1']


def cflags(b):
    """flags for crypt32's own sources"""
    return MB_FLAGS


def objs(b, odir):
    """Mbed TLS's library and the TLS glue for this architecture, compiled
    in parallel; an object newer than its source and the configuration is
    reused"""
    lib = os.path.join(MBEDTLS, 'library')
    srcs = [os.path.join(lib, f) for f in sorted(os.listdir(lib)) if f.endswith('.c') and f != 'net_sockets.c']
    srcs.append(os.path.join(TLS_GLUE, 'tls_glue.c'))
    headers = [os.path.join(TLS_GLUE, h) for h in os.listdir(TLS_GLUE) if h.endswith('.h')]
    return b.compile_many(srcs, odir, 'mbedtls_', b.cflags() + MB_FLAGS, headers)
