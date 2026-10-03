#!/usr/bin/env python3
"""Build ICU as NovaOS's icu.dll, the way Windows 10 ships it.

    tools/build_icu.py [--work DIR]

.NET (5 and later) does its globalization through ICU when
C:\\Windows\\System32\\icu.dll loads: one DLL holding both of ICU's
libraries (common and i18n) that exports ICU's C API under plain,
unversioned names (`ucol_open`, not `ucol_open_77`).  This script builds
exactly that from the official ICU4C release, with the MinGW-w64 cross
compilers (`apt install g++-mingw-w64-x86-64 g++-mingw-w64-i686`), and
writes into third_party/icu:

    x64/icu.dll       -> C:\\Windows\\System32\\icu.dll
    x86/icu.dll       -> C:\\Windows\\SysWOW64\\icu.dll
    icudt77l.dat      -> C:\\Windows\\Globalization\\ICU\\icudt77l.dat

Like Windows, the data is a separate file both DLLs read
(tools/icu/nova_icu.c points ICU at it).  It is ICU's own data less what
neither .NET nor the rest of the C API that programs call needs often:
legacy code-page converters, the word-break dictionaries, transliteration,
measurement units and character names.  (Rule-based number formats stay:
the Japanese calendar's dates need them for the first year of an era.)

The outputs are committed, so the normal build needs no MinGW; run this
again only to move to another ICU release (change ICU_VERSION and the hash).
"""
import argparse, glob, hashlib, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'third_party', 'icu')
GLUE = os.path.join(ROOT, 'tools', 'icu', 'nova_icu.c')

ICU_VERSION = '77.1'
ICU_URL = ('https://github.com/unicode-org/icu/releases/download/release-77-1/'
           'icu4c-77_1-src.tgz')
ICU_SHA256 = '588e431f77327c39031ffbb8843c0e3bc122c211374485fa87dc5f3faff24061'
DATA = 'icudt77l'
HOSTS = {'x64': 'x86_64-w64-mingw32', 'x86': 'i686-w64-mingw32'}

# data items left out (names inside the .dat, or patterns on them)
DROP = [r'.*\.cnv$',              # code-page converters (ucnv_open of legacy charsets)
        r'^brkitr/.*\.dict$',     # Chinese/Japanese/Thai/Lao/Khmer/Burmese word dictionaries
                                  # (word breaks there fall back to per-character)
        r'^translit/',            # transliterators
        r'^unit/',                # measurement-unit names
        r'^unames\.icu$',         # character names (u_charName)
        r'.*\.spp$']              # StringPrep profiles (IDNA2003; .NET uses UTS #46)


def run(cmd, cwd=None, log=None):
    print('+', ' '.join(cmd) if isinstance(cmd, list) else cmd, flush=True)
    out = open(log, 'w') if log else None
    r = subprocess.run(cmd, cwd=cwd, stdout=out, stderr=subprocess.STDOUT if out else None,
                       shell=isinstance(cmd, str))
    if r.returncode:
        sys.exit(f'failed ({r.returncode})' + (f', see {log}' if log else ''))


def fetch(work):
    tgz = os.path.join(work, os.path.basename(ICU_URL))
    if not os.path.exists(tgz):
        run(['curl', '-sSLf', '--retry', '4', '-o', tgz + '.part', ICU_URL])
        os.replace(tgz + '.part', tgz)
    if hashlib.sha256(open(tgz, 'rb').read()).hexdigest() != ICU_SHA256:
        sys.exit(f'{tgz}: unexpected SHA-256')
    src = os.path.join(work, 'icu')
    if not os.path.isdir(src):
        run(['tar', 'xzf', tgz, '-C', work])
    return os.path.join(src, 'source')


def build_host(src, work):
    """ICU's own tools (icupkg, ...), which a cross build needs"""
    d = os.path.join(work, 'host')
    if not os.path.exists(os.path.join(d, 'bin', 'icupkg')):
        os.makedirs(d, exist_ok=True)
        run([os.path.join(src, 'runConfigureICU'), 'Linux', '--disable-tests', '--disable-samples',
             '--disable-extras'], cwd=d, log=os.path.join(d, 'configure.log'))
        run(['make', f'-j{os.cpu_count()}'], cwd=d, log=os.path.join(d, 'make.log'))
    return d


def build_libs(src, work, host, arch):
    """static common + i18n libraries, unversioned names, data from a file"""
    d = os.path.join(work, arch)
    if not os.path.exists(os.path.join(d, 'lib', 'libsicuin.a')):
        os.makedirs(os.path.join(d, 'lib'), exist_ok=True)
        env = 'CFLAGS="-O2" CXXFLAGS="-O2 -std=c++17"'
        run(f'{env} {src}/configure --host={HOSTS[arch]} --with-cross-build={host} '
            '--enable-static --disable-shared --disable-renaming --with-data-packaging=archive '
            '--disable-tools --disable-extras --disable-tests --disable-samples --disable-icuio '
            '--disable-layoutex', cwd=d, log=os.path.join(d, 'configure.log'))
        for sub in ['stubdata', 'common', 'i18n']:
            run(['make', f'-j{os.cpu_count()}'], cwd=os.path.join(d, sub),
                log=os.path.join(d, f'make-{sub}.log'))
    return d


def exports(src, libs, arch):
    """ICU's public C API (declared U_CAPI/U_DEPRECATED ... U_EXPORT2 in
    the unicode/ headers) that the libraries define"""
    declared = set()
    for h in glob.glob(os.path.join(src, 'common', 'unicode', '*.h')) + \
             glob.glob(os.path.join(src, 'i18n', 'unicode', '*.h')):
        text = open(h, encoding='utf-8').read()
        declared |= set(re.findall(r'\bU_(?:CAPI|DEPRECATED)\b[^;{]*?\bU_EXPORT2\s+(\w+)\s*\(', text))
    defined = set()
    for lib in libs:
        out = subprocess.run([HOSTS[arch] + '-nm', '-g', '--defined-only', lib],
                             capture_output=True, text=True, check=True).stdout
        for line in out.split('\n'):
            p = line.split()
            if len(p) == 3:
                defined.add(p[2][1:] if arch == 'x86' else p[2])   # i686 C names carry a '_'
    return sorted(declared & defined)


def link(src, d, arch):
    libs = [os.path.join(d, 'lib', 'libsicuin.a'), os.path.join(d, 'lib', 'libsicuuc.a'),
            os.path.join(d, 'stubdata', 'libsicudt.a')]
    names = exports(src, libs, arch)
    deffile = os.path.join(d, 'icu.def')
    with open(deffile, 'w') as f:
        f.write('LIBRARY icu.dll\nEXPORTS\n' + ''.join(f'    {n}\n' for n in names))
    dll = os.path.join(d, 'icu.dll')
    run([HOSTS[arch] + '-g++', '-shared', '-O2', '-s', '-o', dll, deffile,
         '-DU_DISABLE_RENAMING=1', '-DU_STATIC_IMPLEMENTATION',
         '-I', os.path.join(src, 'common'), '-x', 'c', GLUE, '-x', 'none'] + libs +
        ['-static-libgcc', '-static-libstdc++', '-static', '-Wl,--dynamicbase,--nxcompat',
         '-Wl,--high-entropy-va' if arch == 'x64' else '-Wl,--large-address-aware'])
    print(f'{arch}: icu.dll exports {len(names)} functions')
    return dll


def trim_data(src, host):
    full = os.path.join(src, 'data', 'in', DATA + '.dat')
    icupkg = os.path.join(host, 'bin', 'icupkg')
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(host, 'lib'))
    items = subprocess.run([icupkg, '-l', full], capture_output=True, text=True, check=True,
                           env=env).stdout.split()
    drop = [i for i in items if any(re.search(p, i) for p in DROP)]
    lst = os.path.join(host, 'drop.lst')
    open(lst, 'w').write('\n'.join(drop) + '\n')
    out = os.path.join(OUT, DATA + '.dat')
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run([icupkg, '--ignore-deps', '-r', lst, full, out], env=env)
    if r.returncode:
        sys.exit('icupkg failed')
    print(f'{DATA}.dat: {len(items) - len(drop)} of {len(items)} items, '
          f'{os.path.getsize(out) // 1024} KB (from {os.path.getsize(full) // 1024} KB)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', default=os.path.join(ROOT, 'build', 'icu'))
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    src = fetch(a.work)
    host = build_host(src, a.work)
    for arch in HOSTS:
        d = build_libs(src, a.work, host, arch)
        os.makedirs(os.path.join(OUT, arch), exist_ok=True)
        shutil.copy(link(src, d, arch), os.path.join(OUT, arch, 'icu.dll'))
    trim_data(src, host)
    shutil.copy(os.path.join(os.path.dirname(src), 'LICENSE'), os.path.join(OUT, 'LICENSE'))


if __name__ == '__main__':
    main()
