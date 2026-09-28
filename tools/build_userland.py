#!/usr/bin/env python3
"""Build NovaOS's Windows userland and embed it into the kernel.

    tools/build_userland.py OUT_DIR GENERATED_C KERNEL_SYSCALL_H

Compiles, with clang --target=x86_64-pc-windows-msvc and lld-link:
  ntdll.dll, kernel32.dll, msvcrt.dll, ...  -> C:\\Windows\\System32
  crt/crt0.c (static startup code) + programs/*.c -> C:\\Programs\\*.exe
    (with programs/NAME.rc compiled by llvm-rc: the program's icon)
  samples/* -> C:\\Pictures
  the NetSurf browser (tools/build_netsurf.py) -> C:\\Programs\\NetSurf
and writes GENERATED_C: every file pulled in with .incbin plus a table the
kernel uses to install them on drive C: at boot.  Set NOVA_NO_NETSURF=1 to
leave the browser out (it is most of the build time and image size).
"""
import os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_netsurf

HERE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'userland')
out, gen_c, syscall_h = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
inc_gen = os.path.join(out, 'include')
os.makedirs(inc_gen, exist_ok=True)

CFLAGS = ['--target=x86_64-pc-windows-msvc', '-O2', '-ffreestanding', '-nostdlibinc',
          '-fno-stack-protector', '-mno-stack-arg-probe', '-fms-extensions', '-fasync-exceptions',
          '-Wall', '-Wno-unused-function', '-Werror=implicit-function-declaration',
          '-isystem', os.path.join(HERE, 'include', 'posix'), '-I', os.path.join(HERE, 'include'), '-I', inc_gen]

# DLL load addresses (distinct, so no relocation is needed)
DLLS = [
    ('ntdll',    [],                     0x7FFA00000000),
    ('kernel32', ['ntdll'],              0x7FFA10000000),
    ('msvcrt',   ['kernel32', 'ntdll'],  0x7FFA20000000),
    ('ws2_32',   ['kernel32', 'ntdll'],  0x7FFA30000000),
    ('gdi32',    ['kernel32', 'ntdll'],  0x7FFA50000000),
    ('user32',   ['gdi32', 'kernel32', 'ntdll'], 0x7FFA60000000),
    ('testdll',  ['kernel32', 'ntdll'],  0x7FFA40000000),
]

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        sys.exit(1)

def cc(src, obj, extra=()):
    run(['clang'] + CFLAGS + list(extra) + ['-c', src, '-o', obj])

# 1. syscall numbers from the kernel
nums = {}
for m in re.finditer(r'#define\s+SYSCALL_(Nt\w+)\s+(0x[0-9A-Fa-f]+)', open(syscall_h).read()):
    nums[m.group(1)] = m.group(2)
with open(os.path.join(inc_gen, 'syscall_numbers.h'), 'w') as f:
    f.write('/* generated from kernel/ke/syscall.h */\n#pragma once\n')
    for k, v in sorted(nums.items()):
        f.write(f'#define SYS_{k} {v}\n')

# 1b. startup code + implicit-TLS support (needed by DLLs and programs)
crt0 = os.path.join(out, 'crt0.obj')
cc(os.path.join(HERE, 'crt', 'crt0.c'), crt0)
tlssup = os.path.join(out, 'tlssup.obj')
cc(os.path.join(HERE, 'lib', 'tlssup.c'), tlssup)

# 2. system DLLs
built = []
for name, deps, base in DLLS:
    srcdir = os.path.join(HERE, name)
    objs = []
    for src in sorted(os.listdir(srcdir)):
        if src.endswith('.c'):
            obj = os.path.join(out, f'{name}_{src[:-2]}.obj')
            cc(os.path.join(srcdir, src), obj)
            objs.append(obj)
    if name in ('testdll', 'ws2_32'):
        objs.append(tlssup)
    dll = os.path.join(out, f'{name}.dll')
    entry = ['/entry:DllMain'] if name == 'testdll' else ['/noentry']
    run(['lld-link', '/dll', '/nodefaultlib', f'/base:{base:#x}'] + entry +
        [f'/out:{dll}', f'/implib:{os.path.join(out, name + ".lib")}'] + objs +
        [os.path.join(out, d + '.lib') for d in deps])
    built.append((f'\\Windows\\System32\\{name}.dll', dll))

# 3. startup code and programs
progdir = os.path.join(HERE, 'programs')
for src in sorted(os.listdir(progdir)):
    if not src.endswith('.c'):
        continue
    name = src[:-2]
    obj = os.path.join(out, f'prog_{name}.obj')
    cc(os.path.join(progdir, src), obj)
    exe = os.path.join(out, f'{name}.exe')
    res = []                                  # NAME.rc: resources (e.g. the icon)
    rc = os.path.join(progdir, name + '.rc')
    if os.path.exists(rc):
        res = [os.path.join(out, f'prog_{name}.res')]
        run([build_netsurf.llvm_rc(), '/FO', res[0], rc])
    run(['lld-link', '/subsystem:console', '/entry:mainCRTStartup', '/nodefaultlib',
         f'/out:{exe}', crt0, tlssup, obj] + res + [os.path.join(out, 'msvcrt.lib'),
         os.path.join(out, 'kernel32.lib'), os.path.join(out, 'ntdll.lib'),
         os.path.join(out, 'ws2_32.lib'), os.path.join(out, 'user32.lib'),
         os.path.join(out, 'gdi32.lib'), os.path.join(out, 'testdll.lib')])
    built.append((f'\\Programs\\{name}.exe', exe))

# 3a. sample files for the user's folders (tools/make_icons.py draws the icons)
samples = os.path.join(HERE, 'samples')
for n in sorted(os.listdir(samples)):
    built.append((f'\\Pictures\\{n}', os.path.join(samples, n)))

# 3b. the NetSurf web browser
if os.environ.get('NOVA_NO_NETSURF') != '1':
    built += build_netsurf.build(os.path.join(out, 'netsurf'), out)

# 4. embed (.incbin: the browser alone is megabytes, too much for C arrays)
with open(gen_c + '.tmp', 'w') as f:
    f.write('/* generated by tools/build_userland.py — the Windows userland on drive C: */\n')
    f.write('#include "um/userland_files.h"\n\n')
    for i, (path, file) in enumerate(built):
        src = os.path.abspath(file).replace('\\', '\\\\').replace('"', '\\"')
        f.write(f'extern const unsigned char nova_userland_{i}[];\n')
        f.write(f'__asm__(".section .rodata.userland,\\"a\\"\\n.balign 16\\n'
                f'.globl nova_userland_{i}\\nnova_userland_{i}:\\n.incbin \\"{src}\\"\\n.previous");\n')
    f.write('\nconst UserlandFile g_userland_files[] = {\n')
    for i, (path, file) in enumerate(built):
        cpath = path.replace('\\', '\\\\')
        f.write(f'    {{ "{cpath}", nova_userland_{i}, {os.path.getsize(file)} }},\n')
    f.write('};\n')
    f.write(f'const int g_userland_file_count = {len(built)};\n')
os.replace(gen_c + '.tmp', gen_c)
print(f'userland: {len(built)} files')
