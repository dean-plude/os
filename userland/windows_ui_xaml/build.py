# Windows.UI.Xaml's build: Microsoft's XAML framework (WinUI's dxaml tree,
# third_party/microsoft-ui-xaml, MIT) compiled for NovaOS, roadmap step 32.3
# (docs/design/windows-ui-xaml.md).  tools/build_userland.py loads this
# module for a C++ program whose NAME.json says "xaml": true and calls
# program_flags(b) and program_objs(b, odir).
#
# The sources are compiled as Microsoft compiles them (clang in MSVC mode,
# C++17, their MSBuild projects' sources, include directories and
# definitions, read by vcxproj.py), over Microsoft's STL and the C runtime
# and Windows headers userland/msvcp140/build.py uses (MinGW-w64's), plus:
#   inc/            what the Windows SDK has and MinGW-w64 lacks or spells
#                   differently: NovaOS's WRL (wrl/), weakreference.h,
#                   xamlom.h, TraceLogging (compiled out), SAL annotations,
#                   the Windows Runtime's HRESULTs
#   third_party/wil Microsoft's Windows Implementation Library (MIT)
#   the Windows Runtime ABI headers tools/winrt/abigen.py generates (the
#   ones MIDL makes from the XAML IDL), and the ETW header etwgen.py makes
#   from XAML's manifest (as mc.exe would)
#   a directory of one-line headers giving every include spelling in the
#   sources the file it names on a case-insensitive disk (Windows.h for
#   windows.h, wil\Result.h for wil/result.h)
#
# So far the XAML core's lowest layers build: the component libraries in
# LIBRARIES, as one static library (xaml_components.lib) that the
# self-test xamlbasetest links, up to the type metadata and
# CDependencyObject's property system.  inc/ also stands in for the closed
# Windows App SDK headers those include (FrameworkUdk's helpers, the lifted
# IXP interop headers); the rest of the core links as it builds.
import os, re, threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MUX = os.path.join(ROOT, 'third_party', 'microsoft-ui-xaml')
XCP = os.path.join(MUX, 'dxaml', 'xcp')
WIL = os.path.join(ROOT, 'third_party', 'wil', 'include')
INC = os.path.join(HERE, 'inc')

import importlib.util
def _load(name):
    spec = importlib.util.spec_from_file_location('nova_xaml_' + name, os.path.join(HERE, name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
vcxproj = _load('vcxproj')
etwgen = _load('etwgen')

# MSBuild's properties as the projects use them
PROPS = {'XcpPath': XCP, 'XamlSourcePath': os.path.join(MUX, 'dxaml'), 'RepoRoot': MUX}

# The component libraries built so far (dxaml/xcp/components/...: the
# lowest layer, the type metadata and CDependencyObject)
LIBRARIES = '''base/lib/Microsoft.UI.Xaml.Base.vcxproj
xstring/lib/Microsoft.UI.Xaml.XString.vcxproj
criticalsection/lib/Microsoft.UI.Xaml.CriticalSection.vcxproj
dependencyLocator/lib/Microsoft.UI.Xaml.DependencyLocator.vcxproj
runtimeEnabledFeatures/lib/Microsoft.UI.Xaml.RuntimeEnabledFeatures.vcxproj
com/lib/Microsoft.UI.Xaml.Com.vcxproj
threading/lib/Microsoft.UI.Xaml.Threading.vcxproj
staticpal/lib/Microsoft.UI.Xaml.StaticPal.vcxproj
qualifiers/lib/Microsoft.UI.Xaml.Qualifiers.vcxproj
metadata/lib/Microsoft.UI.Xaml.Metadata.vcxproj
metadata/DynamicMetadataStorage/Microsoft.UI.Xaml.DynamicMetadataStorage.vcxproj
DependencyObject/lib/Microsoft.UI.Xaml.DependencyObject.vcxproj
DependencyObject/minlib/Microsoft.UI.Xaml.DependencyObjectMin.vcxproj
CValue/lib/Microsoft.UI.Xaml.CValue.vcxproj
flyweight/lib/Microsoft.UI.Xaml.Flyweight.vcxproj
primitiveDependencyObjects/lib/Microsoft.UI.Xaml.PrimitiveDependencyObjects.vcxproj
math/lib/Microsoft.UI.Xaml.Math.vcxproj
theming/lib/Microsoft.UI.Xaml.Theming.vcxproj
Collection/Lib/Microsoft.UI.Xaml.Collection.vcxproj'''.split()

# Sources compiled in place of Microsoft's, where clang has to be told what
# MSVC does on its own (src/, each says what)
REPLACED = {'CValue/CValueStores.cpp': os.path.join(HERE, 'src', 'CValueStores.cpp')}
# Sources compiled with a project's that it does not list: two of the
# platform layer's the components call (pal/common: character classes, the
# 2D matrix), and NovaOS's definitions of what MSVC defines from a
# declaration (src/definitions.cpp)
ADDED = {'math/lib/Microsoft.UI.Xaml.Math.vcxproj': [os.path.join(XCP, 'pal', 'common', 'ctypes.cpp'),
                                                     os.path.join(XCP, 'pal', 'common', 'matrix.cpp')],
         'DependencyObject/lib/Microsoft.UI.Xaml.DependencyObject.vcxproj': [os.path.join(HERE, 'src', 'definitions.cpp')]}

# The include directories every component sees (components/base.props,
# in its order) and the definitions components/common.props and
# base.props add
COMPONENT_INCLUDES = '''components components/pch components/base/inc components/brushes/inc
components/criticalsection/inc components/focus/inc components/FocusRect/inc components/com/inc components/common/inc
components/deferral/inc components/metadata/inc components/xstring/inc components/legacy/inc
components/primitiveDependencyObjects/inc components/dependencyLocator/inc components/lifetime/inc
components/DependencyObject/inc components/associative/inc components/flyweight/inc components/simple/inc
components/runtimeEnabledFeatures/inc components/accesskeys/inc components/allocation/inc components/moco/inc
components/mrt/inc components/animation/inc components/math/inc components/ContentRoot/inc components/gestures/inc
components/collection/inc components/scaling/inc components/Switcher/inc components/imaging/inc
components/resources/inc inc core/compositor core/hw core/inc core/text/RichTextServices/inc
core/imaging/ImagingProvider pal/inc win/inc'''.split()
DEFINES = '''UNICODE _UNICODE WIL_SUPPRESS_PRIVATE_API_USE MIDL_NS_PREFIX ENABLE_WINRT_EXPERIMENTAL_TYPES=1
__WRL_STRICT__ __WRL_FORCE_INSPECTABLE_CLASS_MACRO__ _HAS_EXCEPTIONS=0 WIL_SUPPRESS_EXCEPTIONS=1
_WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000F STRSAFE_NO_DEPRECATE _ENFORCE_MATCHING_ALLOCATORS=0
_HAS_FUNCTION_ALLOCATOR_SUPPORT=1'''.split()
# What the libraries call in the XAML core above them, for programs that
# test the components on their own (xamlbasetest): Microsoft's isolated unit
# tests' stubs and metadata mocks (built as Microsoft builds them, with
# __XAML_UNITTESTS__; MSVC lets the mocks reach a base class's protected
# members through another object), less the stubs of the element tree,
# and NovaOS's stand-ins for the few element members the libraries call
# (src/core_stubs.cpp, compiled with the collections' include
# directories).  As a library, a program takes only the stubs it needs; it
# shrinks as the core builds.
TEST_STUBS = [('unittests/stubs/Microsoft.UI.Xaml.Stubs.vcxproj',
               ('UIElement_Stub.cpp', 'DCompVisual_Stub.cpp', 'Geometry_Stub.cpp', 'VisualStateManager_Stub.cpp'), []),
              ('metadata/mocks/Microsoft.UI.Xaml.Tests.Isolated.Mocks.Framework.Metadata.vcxproj', (),
               ['-fno-access-control', '-I', os.path.join(XCP, 'components', 'metadata', 'mocks')])]
PCH = os.path.join(XCP, 'components', 'pch', 'precomp.h')
# the DLLs XAML's sources call beyond a program's usual ones
LIBS = ['dbghelp', 'shlwapi']
ETW_MANIFEST = os.path.join(XCP, 'plat', 'win', 'desktop', 'Microsoft-Windows-XAML-ETW.man')

_SETUP = threading.Lock()
_done = {}


def _msvcp(b):
    return b.HOOKS['msvcp140']


def generated(b):
    """the directory of generated headers (ETW events, include spellings),
    made once per build"""
    abi = [x for x in b.winrt_abi_flags({'winrt_abi': True}) if x != '-I']  # generated first: fold() maps to them
    with _SETUP:
        d = os.path.join(b.out, 'include', 'xaml')
        if 'gen' not in _done:
            os.makedirs(d, exist_ok=True)
            etwgen.main(ETW_MANIFEST, os.path.join(d, 'Microsoft-Windows-XAML-ETWEvents.h'))
            # the ABI headers' base (IInspectable...), alone of NovaOS's own headers
            text = '#include "%s"\n' % os.path.join(os.path.dirname(HERE), 'include', 'winrt_abi.h')
            p = os.path.join(d, 'winrt_abi.h')
            if not os.path.exists(p) or open(p).read() != text:
                open(p, 'w').write(text)
            fold(os.path.join(d, 'fold'), component_includes() + project_includes() +
                 [INC, WIL] + abi + [d, _msvcp(b).mingw_include(),
                  os.path.join(ROOT, 'third_party', 'msstl', 'inc'), _msvcp(b).SHIM])
            _done['gen'] = d
        return d


def component_includes():
    """COMPONENT_INCLUDES as spelled on disk (the projects name
    collection\\inc for Collection/Inc)"""
    return [p for p in (vcxproj.find_case(os.path.join(XCP, d)) for d in COMPONENT_INCLUDES) if p]


def project_includes():
    """the include directories LIBRARIES' projects add"""
    return [i for proj in LIBRARIES for i in vcxproj.read(os.path.join(XCP, 'components', proj), PROPS)['includes']]


def fold(out, dirs):
    """write OUT/<spelling> for every include spelling in the XAML and WIL
    sources that names a file in @dirs only when case is ignored: a header
    that includes the file by its real name (so its own relative includes
    still resolve beside it)"""
    real, lower = set(), {}
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), d).replace(os.sep, '/')
                real.add(rel)
                lower.setdefault(rel.lower(), (d, rel))
    pat = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
    want = set()
    for top in (XCP, WIL):
        for root, _, files in os.walk(top):
            for f in files:
                if f.endswith(('.h', '.cpp', '.inl', '.hpp')):
                    for m in pat.finditer(open(os.path.join(root, f), errors='replace').read()):
                        s = m.group(1).replace('\\', '/')
                        if s not in real and not s.startswith('..') and not os.path.exists(os.path.join(root, s)):
                            want.add(s)
    os.makedirs(out, exist_ok=True)
    for s in sorted(want):
        target = lower.get(s.lower())
        if target:
            p = os.path.join(out, s)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            # NovaOS's own headers by name, so their #include_next goes on
            # to the header they extend
            text = ('#include <%s>\n' % target[1]) if target[0] == INC else \
                '#include "%s"\n' % os.path.abspath(os.path.join(*target))
            if not os.path.exists(p) or open(p).read() != text:
                open(p, 'w').write(text)


def flags(b):
    """clang's flags for XAML's sources"""
    gen = generated(b)
    # as Microsoft builds XAML: C++17, no C++ exceptions (errors are HRESULTs)
    base = [f for f in _msvcp(b).cxx_flags(b) if f not in ('-D_CRTBLD', '-D_VCRT_ALLOW_INTERNALS',
                                                          '-D_HAS_OLD_IOSTREAMS_MEMBERS=1', '-fcxx-exceptions', '-fexceptions')]
    base = ['-std=c++17' if f.startswith('-std=') else f for f in base] + ['-fno-exceptions']
    inc = [os.path.join(gen, 'fold'), INC] + [x for x in b.winrt_abi_flags({'winrt_abi': True}) if x != '-I'] + \
          [gen, WIL] + component_includes() + [os.path.join(MUX, 'external', 'inc')]
    # MSVC accepts, with a warning, what clang makes errors by default
    # (narrowing an HRESULT case label or an enumerator)
    return [f for d in inc for f in ('-I', d)] + base + ['-D' + d for d in DEFINES] + \
        ['-fdelayed-template-parsing', '-Wno-everything']


_LIB = threading.Lock()


def library(b, odir):
    """xaml_components.lib: LIBRARIES' sources, built once per architecture"""
    lib = os.path.join(odir, 'xaml_components.lib')
    with _LIB:
        f = flags(b)
        headers = [os.path.join(r, h) for r, _, fs in os.walk(INC) for h in fs if h.endswith('.h')] + \
                  [os.path.abspath(__file__)] + list(REPLACED.values()) + [x for v in ADDED.values() for x in v]
        objs = []
        for proj in LIBRARIES:
            p = vcxproj.read(os.path.join(XCP, 'components', proj), PROPS)
            extra = [x for i in p['includes'] for x in ('-I', i)] + ['-D' + d for d in p['defines']] + ['-include', PCH]
            prefix = 'xaml_' + proj.split('/')[0] + '_'
            srcs = [REPLACED.get(os.path.relpath(s, os.path.join(XCP, 'components')).replace(os.sep, '/'), s)
                    for s in p['sources']] + ADDED.get(proj, [])
            objs += b.compile_many(srcs, odir, prefix, f + extra, headers, compiler='clang++')
        if not os.path.exists(lib) or any(os.path.getmtime(o) > os.path.getmtime(lib) for o in objs):
            b.run(['lld-link', '/lib', f'/out:{lib}'] + objs)
    return lib


_STUBS = threading.Lock()


def test_stubs(b, odir):
    """xaml_teststubs.lib (TEST_STUBS), built once per architecture"""
    lib = os.path.join(odir, 'xaml_teststubs.lib')
    with _STUBS:
        f = flags(b) + ['-D__XAML_UNITTESTS__']
        headers = [os.path.join(r, h) for r, _, fs in os.walk(INC) for h in fs if h.endswith('.h')] + \
                  [os.path.abspath(__file__)]
        objs = []
        for proj, skip, more in TEST_STUBS + [('Collection/Lib/Microsoft.UI.Xaml.Collection.vcxproj', None, [])]:
            p = vcxproj.read(os.path.join(XCP, 'components', proj), PROPS)
            extra = more + [x for i in p['includes'] for x in ('-I', i)] + ['-D' + d for d in p['defines']] + \
                ['-include', PCH]
            srcs = [os.path.join(HERE, 'src', 'core_stubs.cpp')] if skip is None else \
                [s for s in p['sources'] if os.path.basename(s) not in skip]
            objs += b.compile_many(srcs, odir, 'xamlstub_', f + extra, headers, compiler='clang++')
        if not os.path.exists(lib) or any(os.path.getmtime(o) > os.path.getmtime(lib) for o in objs):
            b.run(['lld-link', '/lib', f'/out:{lib}'] + objs)
    return lib


def program_flags(b):
    """for a C++ program built against XAML's sources ("xaml" in its NAME.json)"""
    return flags(b) + ['-include', PCH]


def program_objs(b, odir):
    """what such a program links besides the C++ library: xaml_components.lib
    and, after it, xaml_teststubs.lib"""
    return [library(b, odir), test_stubs(b, odir)]
