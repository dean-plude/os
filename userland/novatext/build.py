# novatext.dll's build hooks (tools/build_userland.py calls them with itself
# as b).  The text core Uniscribe, DirectWrite and Direct2D share: HarfBuzz
# (third_party/harfbuzz, C++ against libc++'s headers, no C++ runtime)
# shapes text, FreeType (third_party/freetype) loads and rasterizes fonts,
# and the DLL exports both libraries' C APIs (hb_*, FT_*).
import os, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TP = os.path.join(ROOT, 'third_party')
FT_DIR = os.path.join(TP, 'freetype')
FT_SRCS = ['base/ftsystem.c', 'base/ftinit.c', 'base/ftdebug.c', 'base/ftbase.c', 'base/ftbbox.c', 'base/ftbitmap.c',
           'base/ftglyph.c', 'base/ftsynth.c', 'base/ftmm.c', 'base/fttype1.c', 'base/ftstroke.c',
           'base/ftgasp.c', 'base/ftcid.c', 'base/ftfstype.c', 'base/ftpatent.c', 'base/ftwinfnt.c',
           'autofit/autofit.c', 'truetype/truetype.c', 'type1/type1.c', 'cff/cff.c', 'cid/type1cid.c',
           'psaux/psaux.c', 'psnames/psnames.c', 'pshinter/pshinter.c', 'sfnt/sfnt.c', 'smooth/smooth.c',
           'raster/raster.c', 'winfonts/winfnt.c']
FT_FLAGS = ['-w', '-I', os.path.join(FT_DIR, 'include'), '-I', HERE, '-DFT2_BUILD_LIBRARY',
            '-DFT_CONFIG_MODULES_H=<nova_ftmodule.h>', '-DFT_CONFIG_OPTIONS_H=<nova_ftoption.h>']
HB_FLAGS = ['-std=c++17', '-fno-exceptions', '-fno-rtti', '-w', '-D_LIBCPP_NO_VCRUNTIME',
            '-D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES', '-DHB_NO_MT', '-DHB_NO_MMAP', '-DHAVE_FREETYPE',
            '-I', os.path.join(FT_DIR, 'include')]


def find_libcxx():
    """libc++'s headers: $NOVA_LIBCXX, next to the clang in use (Homebrew's
    LLVM), or Debian/Ubuntu's libc++-dev"""
    cands = [os.environ.get('NOVA_LIBCXX', '')]
    if shutil.which('clang'):
        cands.append(os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(shutil.which('clang')))),
                                  'include', 'c++', 'v1'))
    cands += ['/usr/lib/llvm-%d/include/c++/v1' % v for v in range(30, 13, -1)] + ['/usr/include/c++/v1']
    return next((d for d in cands if d and os.path.isfile(os.path.join(d, '__config'))), None)


def cxx_flags(b):
    """C++ against libc++'s headers: our <functional>, libc++, then the C headers"""
    libcxx = find_libcxx()
    if not libcxx:
        raise SystemExit("libc++'s headers not found (install libc++-dev)")
    inc = os.path.join(b.HERE, 'include')
    return ['--target=' + b.TARGETS[b.ARCH]] + (['-msse2'] if b.ARCH == 'x86' else []) + \
        ['-O2', '-ffreestanding', '-nostdlibinc', '-fno-stack-protector', '-mno-stack-arg-probe', '-fms-extensions',
         '-isystem', os.path.join(inc, 'cxx'), '-isystem', libcxx, '-isystem', os.path.join(inc, 'posix'),
         '-isystem', inc, '-I', str(b.inc_gen)]


def objs(b, odir):
    """FreeType and HarfBuzz for this architecture"""
    headers = [os.path.join(HERE, h) for h in os.listdir(HERE) if h.endswith('.h')]
    ft = b.compile_many([os.path.join(FT_DIR, 'src', f) for f in FT_SRCS], odir, 'ft_', b.cflags() + FT_FLAGS, headers)
    hb = b.compile_many([os.path.join(TP, 'harfbuzz', 'src', 'harfbuzz.cc')], odir, 'hb_', cxx_flags(b) + HB_FLAGS,
                        headers, compiler='clang++')
    return ft + hb


def link(b, odir, objs, deps, base):
    """exports every hb_* and FT_* function (cdecl, so no x86 .def)"""
    names = sorted(n for n in b.defined_names(objs) if n.startswith(('hb_', 'FT_')))
    path = os.path.join(odir, 'novatext.def')
    open(path, 'w').write('LIBRARY novatext.dll\nEXPORTS\n' + ''.join(f'  {n}\n' for n in names))
    dll = os.path.join(odir, 'novatext.dll')
    b.run(['lld-link', '/dll', '/nodefaultlib', f'/base:{base:#x}', '/noentry', f'/out:{dll}',
           f'/implib:{os.path.join(odir, "novatext.lib")}', f'/map:{os.path.join(odir, "novatext.map")}',
           '/def:' + path] + (['/safeseh:no', '/machine:x86'] if b.ARCH == 'x86' else []) + objs +
          [os.path.join(odir, d + '.lib') for d in deps])
    b.built.append((f'\\Windows\\{"System32" if b.ARCH == "x64" else "SysWOW64"}\\novatext.dll', dll))
    b.placed.append((base, base + b.image_size(dll), 'novatext'))
