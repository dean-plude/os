# cabinet.dll's build hooks (tools/build_userland.py calls them with itself
# as b).  The cabinet reader and its MSZIP and LZX decompressors are the
# Windows Installer's (userland/msi/cab.c and lzx.c), compiled in here too.
import os

HERE = os.path.dirname(os.path.abspath(__file__))
MSI = os.path.join(os.path.dirname(HERE), 'msi')


def objs(b, odir):
    srcs = [os.path.join(MSI, 'cab.c'), os.path.join(MSI, 'lzx.c')]
    return b.compile_many(srcs, odir, 'cabinet_msi_', b.cflags(), [os.path.join(MSI, 'msi_int.h')])
