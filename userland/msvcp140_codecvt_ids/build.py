# msvcp140_codecvt_ids.dll's build hooks: the C++ library's satellite DLL
# with the locale ids of codecvt<char16_t/char32_t...>, built like
# msvcp140.dll (see its build.py).


def objs(b, odir):
    m = b.HOOKS['msvcp140']
    return m.compile_stl(b, odir, ['ulocale', 'dllmain_satellite'], 'msvcpcv_',
                         ['_BUILDING_SATELLITE_CODECVT_IDS']) + m.start_obj(b, odir, 'msvcpcv_')


def link(b, odir, objs, deps, base):
    b.HOOKS['msvcp140'].link_dll(b, odir, 'msvcp140_codecvt_ids', objs, base, ['msvcp140'])
