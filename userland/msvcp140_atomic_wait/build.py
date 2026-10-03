# msvcp140_atomic_wait.dll's build hooks: the C++ library's satellite DLL
# for atomic waits, the parallel algorithms, <syncstream> and the time-zone
# database (over System32\icu.dll), built like msvcp140.dll (see its
# build.py).  Its exports are listed in third_party/msstl/src/msvcp_atomic_wait.src.
import os


def objs(b, odir):
    m = b.HOOKS['msvcp140']
    return m.compile_stl(b, odir, ['atomic_wait', 'parallel_algorithms', 'syncstream', 'tzdb', 'dllmain_satellite'],
                         'msvcpaw_', ['_BUILDING_SATELLITE_ATOMIC_WAIT']) + m.start_obj(b, odir, 'msvcpaw_')


def link(b, odir, objs, deps, base):
    m = b.HOOKS['msvcp140']
    src = open(os.path.join(m.STL, 'src', 'msvcp_atomic_wait.src')).read()
    path = os.path.join(odir, 'msvcp140_atomic_wait.def')
    open(path, 'w').write(src.replace('LIBRARYNAME', 'MSVCP140_ATOMIC_WAIT'))
    m.link_dll(b, odir, 'msvcp140_atomic_wait', objs, base, ['msvcp140', 'advapi32'], ['/def:' + path])
