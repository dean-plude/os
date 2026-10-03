# msvcp140_1.dll's build hooks: the C++ library's first satellite DLL
# (std::pmr's memory resources), built like msvcp140.dll (see its build.py).
import os


def objs(b, odir):
    m = b.HOOKS['msvcp140']
    return m.compile_stl(b, odir, ['memory_resource', 'dllmain_satellite'], 'msvcp1_', ['_BUILDING_SATELLITE_1']) + \
        m.start_obj(b, odir, 'msvcp1_')


def link(b, odir, objs, deps, base):
    b.HOOKS['msvcp140'].link_dll(b, odir, 'msvcp140_1', objs, base, ['msvcp140'])
