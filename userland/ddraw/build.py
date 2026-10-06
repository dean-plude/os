# ddraw.dll's build hooks (tools/build_userland.py calls them with itself
# as b).  DirectDraw is cnc-ddraw (third_party/cnc-ddraw, MIT): every
# surface in system memory, the primary drawn into the program's window by
# a render thread with Direct3D 9 (DXVK, when installed), OpenGL or GDI.
# It is 32-bit code (window procedures stored with SetWindowLong, x86
# exception contexts), so ddraw.dll is a SysWOW64 DLL (dll.json x86_only).
# cnc-ddraw is written against the Windows SDK's or MinGW-w64's headers;
# NovaOS compiles it with clang in MSVC mode over MinGW-w64's headers, as
# it does Microsoft's STL (userland/msvcp140/build.py), plus the few in
# userland/ddraw/inc.  Its settings file, ddraw.ini beside the DLL, is the
# one cnc-ddraw writes on its first start, with NovaOS's settings (see
# ini_text).
import os, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CNC = os.path.join(ROOT, 'third_party', 'cnc-ddraw')
SHIM = os.path.join(HERE, 'inc')


def sources():
    out = []
    for d, _, files in sorted(os.walk(os.path.join(CNC, 'src'))):
        out += [os.path.join(d, f) for f in sorted(files) if f.endswith('.c')]
    return out


def cnc_flags(b):
    resdir = subprocess.run(['clang', '-print-resource-dir'], capture_output=True, text=True).stdout.strip()
    return ['--target=' + b.TARGETS[b.ARCH], '-msse2', '-D_X86_=1', '-DNOVAOS=1', '-O2', '-fms-extensions', '-fms-compatibility',
            '-fms-compatibility-version=19.43', '-fgnuc-version=4.2.1', '-fno-stack-protector',
            '-mno-stack-arg-probe', '-nostdinc', '-w', '-D__USE_MINGW_ANSI_STDIO=0',
            '-isystem', os.path.join(resdir, 'include'), '-I', SHIM, '-I', os.path.join(CNC, 'inc'),
            '-isystem', b.HOOKS['msvcp140'].mingw_include(), '-include', os.path.join(SHIM, '_nova_ddraw.h')]


def objs(b, odir):
    """cnc-ddraw's sources (userland/ddraw/*.c, NovaOS's own, build as
    every DLL's do)"""
    inc = os.path.join(CNC, 'inc')
    headers = [os.path.join(inc, h) for h in os.listdir(inc) if h.endswith('.h')] + \
              [os.path.join(SHIM, h) for h in os.listdir(SHIM)] + [os.path.abspath(__file__)]
    return b.compile_many(sources(), odir, 'cnc_', cnc_flags(b), headers)


# NovaOS's settings, in place of cnc-ddraw's own in its ddraw.ini.
# hook=0 everywhere: cnc-ddraw would otherwise rewrite the import tables
# of the program and its DLLs to put its own functions between them and
# Windows (to move the mouse and windows of the games it scales); NovaOS's
# DirectDraw leaves programs as they are.  savesettings=0: nothing is
# written back into the system folder.
SETTINGS = [(r'^hook=\d+$', 'hook=0'), (r'^savesettings=\d+$', 'savesettings=0')]


def ini_text():
    """the text cnc-ddraw's cfg_create_ini writes (the C string literal
    it hands to fputs), with SETTINGS applied"""
    src = open(os.path.join(CNC, 'src', 'config.c'), encoding='utf-8').read()
    body = src[src.index('static void cfg_create_ini()'):]
    body = body[body.index('fputs(') + 6:body.index(', fh);')]
    text = ''.join(bytes(s, 'utf-8').decode('unicode_escape') for s in re.findall(r'"((?:[^"\\]|\\.)*)"', body))
    for pat, new in SETTINGS:
        text = re.sub(pat, new, text, flags=re.M)
    return text.replace('\n', '\r\n')


def link(b, odir, objs, deps, base):
    dll = os.path.join(odir, 'ddraw.dll')
    b.run(['lld-link', '/dll', '/nodefaultlib', f'/base:{base:#x}', '/entry:DllMain', '/safeseh:no', '/machine:x86',
           f'/out:{dll}', f'/implib:{os.path.join(odir, "ddraw.lib")}', f'/map:{os.path.join(odir, "ddraw.map")}',
           '/def:' + os.path.join(CNC, 'exports.def')] + objs + [os.path.join(odir, d + '.lib') for d in deps])
    ini = os.path.join(odir, 'ddraw.ini')
    text = ini_text()
    if not os.path.exists(ini) or open(ini, newline='').read() != text:
        open(ini, 'w', newline='').write(text)
    b.built.append(('\\Windows\\SysWOW64\\ddraw.dll', dll))
    b.built.append(('\\Windows\\SysWOW64\\ddraw.ini', ini))
    b.placed.append((base, base + b.image_size(dll), 'ddraw'))
