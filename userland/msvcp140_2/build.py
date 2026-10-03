# msvcp140_2.dll's build hooks: the C++ library's second satellite DLL, the
# C++17 special math functions (std::cyl_bessel_j, std::expint,
# std::riemann_zeta...: the __std_smf_* exports <cmath> calls), built like
# msvcp140.dll (see its build.py) from the STL's special_math.cpp, which
# wraps Boost.Math (third_party/boost-math, Boost Software License),
# standalone as the STL's own build uses it.
import os


def objs(b, odir):
    m = b.HOOKS['msvcp140']
    boost = os.path.join(m.ROOT, 'third_party', 'boost-math', 'include')
    return m.compile_stl(b, odir, ['special_math', 'dllmain_satellite'], 'msvcp2_',
                         ['_BUILDING_SATELLITE_2', 'BOOST_MATH_STANDALONE=1'], ['-I', boost]) + \
        m.start_obj(b, odir, 'msvcp2_')


def link(b, odir, objs, deps, base):
    b.HOOKS['msvcp140'].link_dll(b, odir, 'msvcp140_2', objs, base, ['msvcp140'])
