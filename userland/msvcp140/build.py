# msvcp140.dll's build hooks (tools/build_userland.py calls them with itself
# as b).  The C++ standard library MSVC-built programs import: Microsoft's
# own STL sources (third_party/msstl, Apache-2.0 WITH LLVM-exception),
# compiled with clang in MSVC mode.  They are written against the UCRT's
# and vcruntime's headers; NovaOS gives them MinGW-w64's C and Windows
# headers (public domain) plus the small set in userland/msvcp140/inc
# (vcruntime.h, eh.h, ppltasks.h...).  The satellite DLLs
# (userland/msvcp140_1, _2, _atomic_wait, _codecvt_ids) use the helpers here.
import os, re, shutil, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STL = os.path.join(ROOT, 'third_party', 'msstl')
SHIM = os.path.join(HERE, 'inc')

# stl/CMakeLists.txt's DLL_SOURCES, SOURCES, INITIALIZER_SOURCES and
# EHA_SOURCES (msvcp140.dll), plus nothrow.cpp (std::nothrow, which
# Microsoft links in from its static library)
SOURCES = '''dllmain instances StlCompareStringA StlCompareStringW StlLCMapStringA StlLCMapStringW _tolower _toupper
atomic cerr cin clog cond cout cthread excptptr filesys fiopen future iomanip ios iosptrs iostream locale locale0
multprec mutex nothrow pplerror ppltasks raisehan stdhndlr stdthrow syserror taskscheduler thread0
uncaught_exception uncaught_exceptions ushcerr ushcin ushclog ushcout ushiostr wcerr wcin wclog wcout winapinls
winapisupp wiostrea wlocale xalloc xcosh xdateord xdint xdnorm xdscale xdtento xdtest xdunscal xexp xfcosh xfdint
xfdnorm xfdscale xfdtento xfdtest xfdunsca xferaise xfexp xfprec xfsinh xfvalues xgetwctype xlcosh xldint xldscale
xldtento xldtest xldunsca xlexp xlgamma xlocale xlock xlpoly xlprec xlsinh xlvalues xmbtowc xmtx xnotify xonce
xpoly xprec xrngabort xrngdev xsinh xstod xstof xstoflt xstol xstold xstoll xstopfx xstoul xstoull xstoxflt
xstrcoll xstrxfrm xthrow xtime xtowlower xtowupper xvalues xwcscoll xwcsxfrm xwctomb xwstod xwstof xwstoflt
xwstold xwstopfx xwstoxfl'''.split()

# IMPLIB_SOURCES: what Microsoft's import library msvcprt.lib carries
# besides the imports, linked into each module that uses the STL (here
# the satellite DLLs and NovaOS's own C++ programs) as msvcprt_static.lib.
# (Not vector_algorithms, which NovaOS's modules turn off with
# _USE_STD_VECTOR_ALGORITHMS=0, nor stacktrace, which needs DbgEng.)
IMPLIB = '''asan_noop charconv filesystem format locale0_implib nothrow print sharedmutex syserror_import_lib
xcharconv_ryu_tables xcharconv_tables_double xcharconv_tables_float xonce2'''.split()

# Where clang accepts what MSVC does not need to: (file, old, new)
PATCHES = [
    # an explicit specialization of a class template's static member
    ('wlocale.cpp', '__PURE_APPDOMAIN_GLOBAL locale::id time_put<wchar_t>::id{};',
     'template <> __PURE_APPDOMAIN_GLOBAL locale::id time_put<wchar_t>::id{};'),
]

# "#pragma init_seg(compiler)" and "(lib)": clang files the initializers of
# a source's exported objects (cerr) apart from its static ones, and the
# linker then runs the static _Init_cerr (which uses cerr) first.  Under a
# section name of its own (one that sorts where .CRT$XCC and .CRT$XCL do)
# each source's initializers stay together, in the source's order.
INIT_SEGS = {'#pragma init_seg(compiler)': '#pragma init_seg(".CRT$XCC0")',
             '#pragma init_seg(lib)': '#pragma init_seg(".CRT$XCL0")'}


def mingw_include():
    """MinGW-w64's headers: $NOVA_MINGW_INCLUDE, or the folder of _mingw.h
    on the MinGW-w64 GCC's search path (Debian/Ubuntu, Homebrew)"""
    cands = [os.environ.get('NOVA_MINGW_INCLUDE', '')]
    for gcc in ('x86_64-w64-mingw32-gcc', 'i686-w64-mingw32-gcc'):
        if shutil.which(gcc):
            r = subprocess.run([gcc, '-xc', '-E', '-v', os.devnull], capture_output=True, text=True)
            cands += [l.strip() for l in r.stderr.splitlines() if l.startswith(' /')]
    cands += ['/usr/share/mingw-w64/include', '/usr/x86_64-w64-mingw32/include']
    d = next((d for d in cands if d and os.path.isfile(os.path.join(d, '_mingw.h'))), None)
    if not d:
        raise SystemExit("MinGW-w64's headers not found (install mingw-w64, or set NOVA_MINGW_INCLUDE)")
    return d


def cxx_flags(b, defines=()):
    """clang in MSVC mode, as Microsoft builds the STL (/EHsc, /Zc:threadSafeInit-,
    _CRTBLD...), over the shim headers and MinGW-w64's"""
    resdir = subprocess.run(['clang', '-print-resource-dir'], capture_output=True, text=True).stdout.strip()
    return ['--target=' + b.TARGETS[b.ARCH]] + (['-msse2', '-D_X86_=1'] if b.ARCH == 'x86' else ['-mcx16']) + \
        ['-O2', '-std=c++23', '-fms-compatibility', '-fms-extensions', '-fms-compatibility-version=19.43',
         '-fgnuc-version=4.2.1', '-fcxx-exceptions', '-fexceptions', '-fno-threadsafe-statics',
         '-fno-stack-protector', '-mno-stack-arg-probe', '-nostdinc', '-nostdinc++', '-w',
         '-D_USE_STD_VECTOR_ALGORITHMS=0', '-D_CRTBLD', '-D_VCRT_ALLOW_INTERNALS', '-D_HAS_OLD_IOSTREAMS_MEMBERS=1',
         '-D_DLL', '-D_UCRT'] + \
        [f'-D{d}' for d in defines] + \
        ['-iquote', os.path.join(STL, 'src'), '-I', os.path.join(STL, 'inc'), '-I', SHIM,
         '-isystem', os.path.join(resdir, 'include'), '-isystem', mingw_include(),
         '-include', os.path.join(SHIM, '_nova_msvcp.h')]


def source_text(n):
    """third_party/msstl/src/N.cpp as NovaOS compiles it"""
    text = open(os.path.join(STL, 'src', n + '.cpp')).read()
    for f, old, new in PATCHES:
        if f == n + '.cpp':
            if old not in text:
                raise SystemExit(f'third_party/msstl/src/{n}.cpp: patch no longer applies: {old}')
            text = text.replace(old, new)
    for old, new in INIT_SEGS.items():
        text = text.replace(old, new)
    return text


def compile_stl(b, odir, names, prefix, defines=(), extra=()):
    """third_party/msstl/src/NAME.cpp for each name, into ODIR/PREFIXNAME.obj
    (with the compiler flags @extra),
    compiled from ODIR/PREFIXsrc (the sources as source_text gives them,
    side by side, since some include others: ushcerr.cpp has wcerr.cpp)"""
    sdir = os.path.join(odir, prefix + 'src')
    os.makedirs(sdir, exist_ok=True)
    srcs, todo, done = [], list(names), set()
    while todo:                                   # each source, and the .cpp files they include
        n = todo.pop(0)
        if n in done:
            continue
        done.add(n)
        text, src = source_text(n), os.path.join(sdir, n + '.cpp')
        if not os.path.exists(src) or open(src).read() != text:
            open(src, 'w').write(text)
        if n in names:
            srcs.append(src)
        todo += re.findall(r'^\s*#\s*include\s+"(\w+)\.cpp"', text, re.M)
    headers = [os.path.join(SHIM, h) for h in os.listdir(SHIM)] + [os.path.abspath(__file__)]
    return b.compile_many(srcs, odir, prefix, cxx_flags(b, defines) + list(extra), headers, compiler='clang++')


def static_lib(b, odir):
    """msvcprt_static.lib (IMPLIB above), built once per architecture"""
    lib = os.path.join(odir, 'msvcprt_static.lib')
    objs = compile_stl(b, odir, IMPLIB, 'msvcprt_')
    if not os.path.exists(lib) or any(os.path.getmtime(o) > os.path.getmtime(lib) for o in objs):
        b.run(['lld-link', '/lib', f'/out:{lib}'] + objs)
    return lib


def link_dll(b, odir, name, objs, base, libs, extra=()):
    """a DLL of the C++ library: its own start-up (start.cpp), the C
    runtime, vcruntime140, @libs"""
    dll = os.path.join(odir, name + '.dll')
    machine = ['/safeseh:no', '/machine:x86'] if b.ARCH == 'x86' else []
    b.run(['lld-link', '/dll', '/nodefaultlib', f'/base:{base:#x}', '/entry:_DllMainCRTStartup', f'/out:{dll}',
           f'/implib:{os.path.join(odir, name + ".lib")}', f'/map:{os.path.join(odir, name + ".map")}'] +
          machine + list(extra) + objs + ([static_lib(b, odir)] if name != 'msvcp140' else []) +
          [os.path.join(odir, l + '.lib') for l in ['ucrtbase', 'vcruntime140', 'kernel32', 'ntdll'] + list(libs)])
    b.built.append((f'\\Windows\\{"System32" if b.ARCH == "x64" else "SysWOW64"}\\{name}.dll', dll))
    b.placed.append((base, base + b.image_size(dll), name))


def start_obj(b, odir, prefix):
    """start.cpp (the DLL entry point, atexit) and new.cpp (operator new and delete)"""
    headers = [os.path.join(SHIM, h) for h in os.listdir(SHIM)]
    return b.compile_many([os.path.join(HERE, 'start.cpp'), os.path.join(HERE, 'new.cpp')], odir, prefix,
                          cxx_flags(b), headers, compiler='clang++')


def program_flags(b):
    """for a NovaOS C++ program on the STL ("msstl" in its NAME.json): the
    flags a program built by Visual Studio against msvcp140.dll has"""
    return [f for f in cxx_flags(b) if f not in ('-D_CRTBLD', '-D_VCRT_ALLOW_INTERNALS',
                                                   '-D_HAS_OLD_IOSTREAMS_MEMBERS=1')]


def program_objs(b, odir):
    """what such a program links besides msvcp140.lib: operator new and
    delete, and msvcprt_static.lib"""
    headers = [os.path.join(SHIM, h) for h in os.listdir(SHIM)]
    return b.compile_many([os.path.join(HERE, 'new.cpp')], odir, 'stlprog_', program_flags(b), headers,
                          compiler='clang++') + [static_lib(b, odir)]


def objs(b, odir):
    return compile_stl(b, odir, SOURCES, 'msvcp_', ['CRTDLL2']) + start_obj(b, odir, 'msvcp_')


def link(b, odir, objs, deps, base):
    # std::locale::facet's default-constructor closure was public before
    # Visual Studio 17.13; keep the name older programs import
    facet = ('??_Ffacet@locale@std@@QEAAXXZ=??_Ffacet@locale@std@@IEAAXXZ' if b.ARCH == 'x64' else
             '??_Ffacet@locale@std@@QAEXXZ=??_Ffacet@locale@std@@IAEXXZ')
    link_dll(b, odir, 'msvcp140', objs, base, ['advapi32'], ['/export:' + facet])
