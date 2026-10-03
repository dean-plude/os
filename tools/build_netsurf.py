#!/usr/bin/env python3
"""Cross-build the NetSurf web browser (framebuffer frontend) for NovaOS.

    tools/build_netsurf.py OUT_DIR USERLAND_OUT_DIR

Compiles NetSurf and its libraries from third_party/netsurf, plus Mbed TLS
(third_party/mbedtls) and the NovaOS glue in userland/netsurf (libnsfb
surface, HTTP(S) fetcher), with clang for x86_64-pc-windows-msvc against the
NovaOS SDK headers (userland/include), and links OUT_DIR/netsurf.exe against
the system DLLs built in USERLAND_OUT_DIR.  Objects are cached by a hash of
their command line, source and the newest header, so rebuilds only compile
what changed.  build_userland.py calls build() and installs the result in
C:\\Programs\\NetSurf.
"""
import hashlib, json, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, 'third_party', 'netsurf')
GLUE = os.path.join(ROOT, 'userland', 'netsurf')
SDK = os.path.join(ROOT, 'userland', 'include')

BASE = ['clang', '--target=x86_64-pc-windows-msvc', '-O2', '-ffreestanding', '-nostdlibinc',
        '-fno-stack-protector', '-mno-stack-arg-probe', '-fms-extensions', '-fasync-exceptions',
        '-std=gnu99', '-w', '-Wno-error', '-Werror=implicit-function-declaration',
        '-Werror=int-conversion', '-Werror=incompatible-pointer-types',
        '-D_NOVAOS', '-DNOVA_POSIX', '-U_WIN32', '-U_WIN64',
        '-fgnuc-version=4.2.1', '-D_ALIGNED=__attribute__((aligned))', '-DSTMTEXPR=1',
        '-isystem', os.path.join(SDK, 'posix'), '-isystem', SDK]

LIBS = ('libwapcaplet', 'libparserutils', 'libcss', 'libhubbub', 'libdom', 'libnsbmp', 'libnsgif',
        'libnsutils', 'libutf8proc', 'libnspsl', 'libnsfb', 'libsvgtiny')

def pub_inc():
    # every library's public headers (never another library's private src/)
    out = []
    for n in LIBS:
        out += ['-I', os.path.join(TP, n, 'include')]
    out += ['-I', os.path.join(TP, 'libutf8proc', 'include', 'libutf8proc')]
    out += ['-I', os.path.join(TP, 'libexpat', 'lib')]
    return out

def own_inc(name):
    # a library's own private headers first, then everyone's public ones
    base = os.path.join(TP, name)
    return ['-I', os.path.join(base, 'src'), '-I', base] + pub_inc()

# Where it is installed on drive C:.  NetSurf's framebuffer frontend thinks
# in POSIX paths (":" separates search paths, "/" joins, file:///... URLs);
# NovaOS's kernel32 reads "/x/y" as "C:\x\y".
INSTALL_DIR = '\\Programs\\NetSurf'
RESPATH = '/Programs/NetSurf/res'
NS_DEFS = ['-Dnsframebuffer', '-Dsmall', '-DSTMTEXPR=1', '-DWITH_BMP', '-DWITH_GIF', '-DWITH_PNG',
           '-DWITH_JPEG',                   # userland/netsurf/jpeg_stb.c (stb_image)
           '-DWITH_NS_SVG',                 # SVG images: libsvgtiny on libdom's XML parser (expat)
           '-DWITH_NSPSL', '-DWITH_UTF8PROC', '-DWITH_NOVA_HTTP', '-DUTF8PROC_STATIC',
           '-DDUK_OPT_HAVE_CUSTOM_H',        # JavaScript: Duktape (content/handlers/javascript/duktape)
           '-DNETSURF_HOMEPAGE="about:welcome"', '-DNETSURF_LOG_LEVEL=WARNING',
           '-DNETSURF_UA_FORMAT_STRING="Mozilla/5.0 (NovaOS%.0s) NetSurf/%d.%d"',
           f'-DNETSURF_FB_RESPATH="{RESPATH}"', '-DNETSURF_FB_FONTPATH=""',
           '-DFB_USE_NOVA_FONT',            # userland/netsurf/font_nova.c (TrueType)
           '-DNETSURF_BUILTIN_LOG_FILTER="(level:WARNING)"',
           '-DNETSURF_BUILTIN_VERBOSE_FILTER="(level:VERBOSE)"']

PV_FLAGS = ['-I', os.path.join(ROOT, 'third_party', 'plutovg', 'include'), '-DPLUTOVG_BUILD_STATIC', '-DPLUTOVG_BUILD',
            '-DSTBI_NO_THREAD_LOCALS']

def components():
    lists = json.load(open(os.path.join(TP, 'sources.json')))
    comps = []
    drop = {
        'libnsfb': set(),
        'netsurf': {'content/handlers/image/nssprite.c', 'desktop/font_haru.c'},
    }
    for name, srcs in lists.items():
        base = os.path.join(TP, name)
        files = []
        for s in srcs:
            if s.endswith('.a') or s in drop.get(name, set()):
                continue
            if s.startswith('build/'):
                s = 'generated/' + os.path.basename(s)
            files.append(os.path.join(base, s))
        if name == 'netsurf':
            flags = ['-I', base, '-I', os.path.join(base, 'include'), '-I', os.path.join(base, 'frontends'),
                     '-I', os.path.join(base, 'content', 'handlers'), '-I', os.path.join(base, 'generated'),
                     '-I', GLUE] + pub_inc() + ['-I', os.path.join(TP, 'libpng'), '-I', os.path.join(TP, 'zlib')] + \
                    PV_FLAGS[:3] + NS_DEFS
        elif name == 'libdom':
            flags = own_inc(name) + ['-I', os.path.join(base, 'bindings')]
        elif name == 'libutf8proc':
            flags = own_inc(name) + ['-DUTF8PROC_STATIC']
        elif name == 'libexpat':
            flags = ['-I', base + '/lib', '-DHAVE_EXPAT_CONFIG_H', '-DXML_STATIC']
        elif name == 'libparserutils':
            flags = own_inc(name) + ['-DWITHOUT_ICONV_FILTER']
        else:
            flags = own_inc(name)
        comps.append((name, files, flags))
    # zlib, libpng
    z = os.path.join(TP, 'zlib')
    zsrc = ['adler32', 'compress', 'crc32', 'deflate', 'infback', 'inffast', 'inflate', 'inftrees',
            'trees', 'uncompr', 'zutil', 'gzlib', 'gzread', 'gzwrite', 'gzclose']
    comps.append(('zlib', [os.path.join(z, s + '.c') for s in zsrc], ['-I', z, '-DZ_HAVE_UNISTD_H']))
    p = os.path.join(TP, 'libpng')
    psrc = ['png', 'pngerror', 'pngget', 'pngmem', 'pngpread', 'pngread', 'pngrio', 'pngrtran',
            'pngrutil', 'pngset', 'pngtrans', 'pngwio', 'pngwrite', 'pngwtran', 'pngwutil']
    comps.append(('libpng', [os.path.join(p, s + '.c') for s in psrc],
                  ['-I', p, '-I', z, '-DPNG_ARM_NEON_OPT=0', '-DPNG_INTEL_SSE_OPT=0']))
    # plutovg (MIT): the framebuffer's path plotter (SVG shapes) in
    # frontends/framebuffer/framebuffer.c
    pv = os.path.join(ROOT, 'third_party', 'plutovg')
    comps.append(('plutovg', sorted(os.path.join(pv, 'source', f) for f in os.listdir(os.path.join(pv, 'source'))
                                    if f.endswith('.c')), PV_FLAGS + ['-D_WIN32']))   # (its font file loader's Win32 branch)
    # Mbed TLS (for https), configured by userland/netsurf/mbedtls_user_config.h
    mb = os.path.join(ROOT, 'third_party', 'mbedtls')
    mb_flags = ['-I', os.path.join(mb, 'include'), '-I', os.path.join(mb, 'library'), '-I', GLUE,
                '-DMBEDTLS_CONFIG_FILE="mbedtls_user_config.h"']
    comps.append(('mbedtls', sorted(os.path.join(mb, 'library', f) for f in os.listdir(os.path.join(mb, 'library'))
                                    if f.endswith('.c') and f != 'net_sockets.c'), mb_flags))
    # NovaOS glue: the libnsfb surface builds against libnsfb's private
    # headers, the rest against NetSurf's
    ns_flags = [c for c in comps if c[0] == 'netsurf'][0][2]
    for f in sorted(os.listdir(GLUE)):
        if not f.endswith('.c'):
            continue
        if f.startswith('nsfb_'):
            comps.append(('glue', [os.path.join(GLUE, f)], own_inc('libnsfb')))
        else:
            comps.append(('glue', [os.path.join(GLUE, f)],
                          ns_flags + mb_flags + ['-I', os.path.join(ROOT, 'third_party', 'stb')]))
    return comps

def header_epoch():
    """Newest header under the trees we compile against: part of every
    object's cache key, so editing a header rebuilds everything."""
    newest = 0
    for top in (TP, SDK, GLUE, os.path.join(ROOT, 'third_party', 'mbedtls'), os.path.join(ROOT, 'third_party', 'plutovg')):
        for d, _, files in os.walk(top):
            for f in files:
                if f.endswith('.h'):
                    newest = max(newest, os.stat(os.path.join(d, f)).st_mtime_ns)
    return str(newest)

EPOCH = None

def compile_one(args):
    src, obj, flags = args
    cmd = BASE + flags + ['-c', src, '-o', obj]
    h = hashlib.sha1((' '.join(cmd) + EPOCH).encode())
    h.update(open(src, 'rb').read())
    stamp = obj + '.hash'
    if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == h.hexdigest():
        return None
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        return f'{src}:\n{r.stderr}'
    open(stamp, 'w').write(h.hexdigest())
    return None

def root_bundle(path):
    """The kernel's built-in Mozilla roots (kernel/net/tls_roots.c, made by
    scripts/gen-tls-roots.py) as DER certificates back to back, for the
    fetcher's Mbed TLS."""
    src = open(os.path.join(ROOT, 'kernel', 'net', 'tls_roots.c')).read()
    blob = bytearray()
    for m in re.finditer(r'static const UINT8 ROOT\d+\[\d+\] = \{([^}]*)\}', src):
        blob += bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1)))
    open(path, 'wb').write(blob)

def resources(out):
    """(path on drive C:, local file) for everything NetSurf reads at run time."""
    res = os.path.join(TP, 'netsurf', 'frontends', 'framebuffer', 'res')
    files = []
    for d, _, names in sorted(os.walk(res)):
        for n in sorted(names):
            local = os.path.join(d, n)
            rel = os.path.relpath(local, res).replace('/', '\\')
            files.append((INSTALL_DIR + '\\res\\' + rel, local))
    bundle = os.path.join(out, 'ca-bundle.der')
    root_bundle(bundle)
    files.append((INSTALL_DIR + '\\res\\ca-bundle.der', bundle))
    # TrueType fonts for font_nova.c, in the Windows fonts folder
    for d, names in (('inter', ('Inter-Regular.ttf', 'Inter-Bold.ttf')),
                     ('dejavu', ('DejaVuSansMono.ttf', 'DejaVuSansMono-Bold.ttf'))):
        for n in names:
            files.append(('\\Windows\\Fonts\\' + n, os.path.join(ROOT, 'third_party', d, n)))
    return files

def llvm_rc():
    """The resource compiler: llvm-rc, or a versioned llvm-rc-NN"""
    import shutil
    for v in [''] + [f'-{n}' for n in range(30, 13, -1)]:
        if shutil.which('llvm-rc' + v):
            return 'llvm-rc' + v
    raise SystemExit('llvm-rc not found (install llvm: sudo apt install llvm)')

def build(out, ul):
    """Build netsurf.exe into @out, linking the system DLLs' import libraries
    in @ul; returns what to install on drive C: as (path, local file)."""
    global EPOCH
    EPOCH = header_epoch()
    objdir = os.path.join(out, 'nsobj')
    os.makedirs(objdir, exist_ok=True)
    jobs, objs = [], []
    for name, files, flags in components():
        for f in files:
            rel = os.path.relpath(f, ROOT).replace('/', '_').replace('.', '_')
            obj = os.path.join(objdir, rel + '.obj')
            jobs.append((f, obj, flags))
            objs.append(obj)
    errors = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for e in ex.map(compile_one, jobs):
            if e:
                errors.append(e)
    if errors:
        limit = int(os.environ.get('NS_ERR_LIMIT', '8'))
        for e in errors[:limit]:
            sys.stderr.write(e + '\n')
        sys.stderr.write(f'netsurf: {len(errors)} file(s) failed to compile\n')
        raise SystemExit(1)
    exe = os.path.join(out, 'netsurf.exe')
    libs = [os.path.join(ul, n + '.lib') for n in ('msvcrt', 'kernel32', 'ntdll', 'ws2_32', 'user32', 'gdi32')]
    # the program icon (NetSurf's own, from its Windows frontend)
    res = os.path.join(out, 'netsurf.res')
    r = subprocess.run([llvm_rc(), '/FO', res, '/I', os.path.join(TP, 'netsurf', 'frontends', 'windows', 'res'),
                        os.path.join(GLUE, 'netsurf.rc')], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(1)
    rsp = os.path.join(out, 'netsurf.rsp')
    open(rsp, 'w').write('\n'.join(objs))
    # a console program like the rest of C:\Programs: started from the
    # Terminal, its log and errors appear there
    cmd = ['lld-link', '/subsystem:console', '/entry:mainCRTStartup', '/nodefaultlib', '/stack:8388608',
           f'/out:{exe}', os.path.join(ul, 'crt0.obj'), os.path.join(ul, 'tlssup.obj'),
           '@' + rsp, res] + libs
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(1)
    print(f'netsurf: {exe} ({os.path.getsize(exe) // 1024} KB, {len(objs)} objects)')
    return [(INSTALL_DIR + '\\netsurf.exe', exe)] + resources(out)

if __name__ == '__main__':
    for path, local in build(sys.argv[1], sys.argv[2]):
        print(f'  {path} <- {os.path.relpath(local)}')
