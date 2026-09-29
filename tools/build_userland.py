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
    ('user32',   ['gdi32', 'msvcrt', 'kernel32', 'ntdll'], 0x7FFA60000000),
    ('testdll',  ['kernel32', 'ntdll'],  0x7FFA40000000),
    ('vcruntime140', ['kernel32', 'ntdll'], 0x7FFA70000000),
    ('advapi32', ['kernel32', 'ntdll'],  0x7FFA80000000),
    ('bcrypt',   ['kernel32', 'ntdll'],  0x7FFA90000000),
    ('bcryptprimitives', ['kernel32', 'ntdll'], 0x7FFAA0000000),
    ('userenv',  ['kernel32', 'ntdll'],  0x7FFAB0000000),
    ('shlwapi',  ['msvcrt', 'kernel32', 'ntdll'], 0x7FFAD0000000),
    ('shell32',  ['user32', 'kernel32', 'ntdll'], 0x7FFAC0000000),
    ('psapi',    ['kernel32'],           0x7FFB40000000),
    ('version',  ['kernel32', 'ntdll'],  0x7FFB20000000),
    ('winmm',    ['kernel32', 'ntdll'],  0x7FFB30000000),
    ('comctl32', ['user32', 'gdi32', 'msvcrt', 'kernel32', 'ntdll'], 0x7FFB00000000),
    ('comdlg32', ['kernel32', 'ntdll'],  0x7FFB10000000),
    ('ole32',    ['advapi32', 'kernel32', 'ntdll'], 0x7FFAE0000000),
    ('oleaut32', ['ole32', 'msvcrt', 'kernel32', 'ntdll'], 0x7FFAF0000000),
]
UCRT_BASE = 0x7FFA28000000
# DLLs built from more than their own directory
DLL_SOURCES = {
    'advapi32': ['advapi32', 'common'],
    'bcrypt':   ['bcrypt', 'common'],
}

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
MUSL = os.path.join(os.path.dirname(HERE), 'third_party', 'musl')

def musl_math_objs():
    """musl's libm (third_party/musl/src/math), compiled once for the C
    runtime DLLs; returns (objects, exported names)"""
    objs, names = [], set()
    srcdir = os.path.join(MUSL, 'src', 'math')
    flags = ['--target=x86_64-pc-windows-msvc', '-O2', '-ffreestanding', '-nostdlibinc', '-fno-builtin',
             '-D_GNU_SOURCE', '-w', '-I', os.path.join(MUSL, 'include'), '-I', os.path.join(MUSL, 'src', 'internal'),
             '-I', os.path.join(HERE, 'include')]
    mdir = os.path.join(out, 'musl')
    os.makedirs(mdir, exist_ok=True)
    for src in sorted(os.listdir(srcdir)):
        if src.endswith('.c'):
            obj = os.path.join(mdir, src[:-2] + '.obj')
            run(['clang'] + flags + ['-c', os.path.join(srcdir, src), '-o', obj])
            objs.append(obj)
    # every public function (musl's internal helpers start with "__")
    r = subprocess.run([llvm_tool('llvm-nm'), '--defined-only', '--extern-only'] + objs,
                       capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'TW' and not parts[2].startswith('__'):
            names.add(parts[2])
    return objs, sorted(names)

def llvm_tool(t):
    import shutil
    for v in [''] + [f'-{n}' for n in range(30, 13, -1)]:
        if shutil.which(t + v):
            return t + v
    raise SystemExit(f'{t} not found (install llvm)')

def dll_objs(name, srcdirs):
    objs = []
    for d in srcdirs:
        srcdir = os.path.join(HERE, d)
        for src in sorted(os.listdir(srcdir)):
            if src.endswith('.c'):
                obj = os.path.join(out, f'{name}_{d}_{src[:-2]}.obj')
                cc(os.path.join(srcdir, src), obj)
                objs.append(obj)
    return objs

def flavor_obj(legacy):
    """msvcrt.dll and ucrtbase.dll share the C runtime's objects; this one
    differs: legacy msvcrt behaviour (e.g. printf rounding) or the UCRT's"""
    src = os.path.join(out, 'crt_flavor.c')
    obj = os.path.join(out, f'crt_flavor{legacy}.obj')
    open(src, 'w').write('const int __nova_crt_legacy = %d;\n' % legacy)
    cc(src, obj)
    return obj

# Windows' export ordinals for DLLs programs import from by number
# (oleaut32's BSTR and VARIANT calls, Winsock 1, comctl32's subclassing...)
ORDINALS = {
    'oleaut32': {'SysAllocString': 2, 'SysReAllocString': 3, 'SysAllocStringLen': 4, 'SysReAllocStringLen': 5,
                 'SysFreeString': 6, 'SysStringLen': 7, 'VariantInit': 8, 'VariantClear': 9, 'VariantCopy': 10,
                 'VariantCopyInd': 11, 'VariantChangeType': 12, 'VariantTimeToDosDateTime': 13,
                 'DosDateTimeToVariantTime': 14, 'SafeArrayCreate': 15, 'SafeArrayDestroy': 16, 'SafeArrayGetDim': 17,
                 'SafeArrayGetElemsize': 18, 'SafeArrayGetUBound': 19, 'SafeArrayGetLBound': 20, 'SafeArrayLock': 21,
                 'SafeArrayUnlock': 22, 'SafeArrayAccessData': 23, 'SafeArrayUnaccessData': 24,
                 'SafeArrayGetElement': 25, 'SafeArrayPutElement': 26, 'SafeArrayCopy': 27, 'DispGetParam': 28,
                 'DispGetIDsOfNames': 29, 'DispInvoke': 30, 'CreateDispTypeInfo': 31, 'CreateStdDispatch': 32,
                 'RegisterActiveObject': 33, 'RevokeActiveObject': 34, 'GetActiveObject': 35,
                 'SafeArrayAllocDescriptor': 36, 'SafeArrayAllocData': 37, 'SafeArrayDestroyDescriptor': 38,
                 'SafeArrayDestroyData': 39, 'SafeArrayRedim': 40, 'SafeArrayAllocDescriptorEx': 41,
                 'SafeArrayCreateEx': 42, 'SafeArrayCreateVectorEx': 43, 'SafeArraySetRecordInfo': 44,
                 'SafeArrayGetRecordInfo': 45, 'VarParseNumFromStr': 46, 'VarNumFromParseNum': 47,
                 'SafeArraySetIID': 57, 'SafeArrayGetIID': 67, 'SafeArrayGetVartype': 77,
                 'VarI4FromStr': 64, 'VarR8FromStr': 84, 'VarDateFromStr': 94, 'VarBstrFromI4': 110,
                 'VarBstrFromR8': 112, 'VarBstrFromDate': 114, 'VarBoolFromStr': 125,
                 'DispCallFunc': 146, 'VariantChangeTypeEx': 147, 'SafeArrayPtrOfIndex': 148,
                 'SysStringByteLen': 149, 'SysAllocStringByteLen': 150, 'LoadTypeLib': 161, 'LoadRegTypeLib': 162,
                 'RegisterTypeLib': 163, 'QueryPathOfRegTypeLib': 164, 'LoadTypeLibEx': 183,
                 'SystemTimeToVariantTime': 184, 'VariantTimeToSystemTime': 185, 'UnRegisterTypeLib': 186,
                 'GetErrorInfo': 200, 'SetErrorInfo': 201, 'CreateErrorInfo': 202,
                 'SafeArrayCreateVector': 411},
    'ws2_32': {'accept': 1, 'bind': 2, 'closesocket': 3, 'connect': 4, 'getpeername': 5, 'getsockname': 6,
               'getsockopt': 7, 'htonl': 8, 'htons': 9, 'ioctlsocket': 10, 'inet_addr': 11, 'inet_ntoa': 12,
               'listen': 13, 'ntohl': 14, 'ntohs': 15, 'recv': 16, 'recvfrom': 17, 'select': 18, 'send': 19,
               'sendto': 20, 'setsockopt': 21, 'shutdown': 22, 'socket': 23, 'gethostbyaddr': 51,
               'gethostbyname': 52, 'getprotobyname': 53, 'getprotobynumber': 54, 'getservbyname': 55,
               'getservbyport': 56, 'gethostname': 57, 'WSAAsyncSelect': 101, 'WSAAsyncGetHostByAddr': 102,
               'WSAAsyncGetHostByName': 103, 'WSACancelAsyncRequest': 108, 'WSASetBlockingHook': 109,
               'WSAUnhookBlockingHook': 110, 'WSAGetLastError': 111, 'WSASetLastError': 112,
               'WSACancelBlockingCall': 113, 'WSAIsBlocking': 114, 'WSAStartup': 115, 'WSACleanup': 116,
               '__WSAFDIsSet': 151},
    'comctl32': {'MenuHelp': 2, 'ShowHideMenuCtl': 3, 'GetEffectiveClientRect': 4, 'DrawStatusTextA': 5,
                 'CreateStatusWindowA': 6, 'CreateToolbar': 7, 'CreateMappedBitmap': 8, 'MakeDragList': 13,
                 'LBItemFromPt': 14, 'DrawInsert': 15, 'CreateUpDownControl': 16, 'InitCommonControls': 17,
                 'Str_SetPtrW': 236, 'DSA_Create': 320, 'DSA_Destroy': 321, 'DSA_GetItem': 322,
                 'DSA_GetItemPtr': 323, 'DSA_InsertItem': 324, 'DSA_SetItem': 325, 'DSA_DeleteItem': 326,
                 'DSA_DeleteAllItems': 327, 'DPA_Create': 328, 'DPA_Destroy': 329, 'DPA_Grow': 330,
                 'DPA_Clone': 331, 'DPA_GetPtr': 332, 'DPA_GetPtrIndex': 333, 'DPA_InsertPtr': 334,
                 'DPA_SetPtr': 335, 'DPA_DeletePtr': 336, 'DPA_DeleteAllPtrs': 337, 'DPA_Sort': 338,
                 'DPA_Search': 339, 'DPA_CreateEx': 340, 'LoadIconMetric': 380, 'LoadIconWithScaleDown': 381,
                 'DPA_DestroyCallback': 385, 'DSA_DestroyCallback': 386, 'SetWindowSubclass': 410,
                 'GetWindowSubclass': 411, 'RemoveWindowSubclass': 412, 'DefSubclassProc': 413},
    'shell32': {'SHChangeNotifyRegister': 2, 'SHChangeNotifyDeregister': 4, 'ILFindLastID': 16,
                'ILRemoveLastID': 17, 'ILClone': 18, 'ILCloneFirst': 19, 'ILIsEqual': 21, 'ILCombine': 25,
                'ILGetSize': 152, 'ILGetNext': 153, 'ILFree': 155, 'ILCreateFromPathW': 190,
                'SHCreateDirectory': 165, 'IsUserAnAdmin': 680, 'SHGetImageList': 727},
}

def ordinal_exports(name, objs):
    """/export:NAME,@N for each ordinal-table name the DLL defines"""
    table = ORDINALS.get(name)
    if not table:
        return []
    r = subprocess.run([llvm_tool('llvm-nm'), '--defined-only', '--extern-only'] + objs, capture_output=True, text=True)
    defined = {l.split()[-1] for l in r.stdout.splitlines() if l.strip()}
    return [f'/export:{n},@{o}' for n, o in sorted(table.items(), key=lambda x: x[1]) if n in defined]

def link_dll(name, objs, deps, base, extra=()):
    extra = list(extra) + ordinal_exports(name, objs)
    dll = os.path.join(out, f'{name}.dll')
    entry = ['/entry:DllMain'] if name in ('testdll', 'comctl32') else ['/noentry']
    run(['lld-link', '/dll', '/nodefaultlib', f'/base:{base:#x}'] + entry +
        [f'/out:{dll}', f'/implib:{os.path.join(out, name + ".lib")}'] + objs + list(extra) +
        [os.path.join(out, d + '.lib') for d in deps])
    built.append((f'\\Windows\\System32\\{name}.dll', dll))

built = []
math_objs, math_names = musl_math_objs()
for name, deps, base in DLLS:
    if name == 'ucrtbase':
        continue                                  # linked from msvcrt's objects below
    srcdirs = DLL_SOURCES.get(name, [name])
    objs = dll_objs(name, srcdirs)
    extra = []
    if name in ('testdll', 'ws2_32', 'ole32', 'oleaut32'):
        objs.append(tlssup)
    if name == 'msvcrt':
        objs += math_objs
        rsp = os.path.join(out, 'crt_exports.rsp')
        open(rsp, 'w').write('\n'.join([f'/export:{n}' for n in math_names] +
                                     ['/export:__C_specific_handler=ntdll.__C_specific_handler']))
        extra = ['@' + rsp]
        crt_objs, crt_extra = objs, extra
        objs = objs + [flavor_obj(1)]
        # the old msvcrt.dll also carried the C++ runtime (7-Zip and other
        # programs built against it import exceptions and RTTI from it)
        rsp2 = os.path.join(out, 'msvcrt_cxx.rsp')
        cxx = ['_CxxThrowException', '__CxxFrameHandler', '__CxxFrameHandler2', '__CxxFrameHandler3',
               '??1type_info@@UEAA@XZ', '??_7type_info@@6B@', '_purecall', '__RTDynamicCast', '__RTtypeid',
               '__RTCastToVoid', 'set_unexpected', 'unexpected', '__uncaught_exception', '_set_se_translator',
               '_is_exception_typeof', '__DestructExceptionObject', '__AdjustPointer', '_local_unwind']
        open(rsp2, 'w').write('\n'.join([f'/export:{n}=vcruntime140.{n}' for n in cxx] +
                                        ['/export:?terminate@@YAXXZ=terminate']))
        extra = extra + ['@' + rsp2]
    link_dll(name, objs, deps, base, extra)
    if name == 'msvcrt':
        # the Universal C Runtime: the same C runtime under its Windows 10 name
        # (programs reach it through the api-ms-win-crt-* API sets)
        link_dll('ucrtbase', crt_objs + [flavor_obj(0)], deps, UCRT_BASE, crt_extra)

# 3. startup code and programs
progdir = os.path.join(HERE, 'programs')
for src in sorted(os.listdir(progdir)):
    if not src.endswith(('.c', '.cpp')):
        continue
    name = src.rsplit('.', 1)[0]
    obj = os.path.join(out, f'prog_{name}.obj')
    if src.endswith('.cpp'):                  # C++ (exceptions, RTTI): vcruntime140
        run(['clang++'] + CFLAGS + ['-fcxx-exceptions', '-fexceptions', '-std=c++17',
             '-c', os.path.join(progdir, src), '-o', obj])
    else:
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
         os.path.join(out, 'gdi32.lib'), os.path.join(out, 'testdll.lib'),
         os.path.join(out, 'vcruntime140.lib'), os.path.join(out, 'advapi32.lib'),
         os.path.join(out, 'ole32.lib'), os.path.join(out, 'oleaut32.lib')])
    built.append((f'\\Programs\\{name}.exe', exe))

# 3a0. fonts gdi32 draws text with (C:\Windows\Fonts)
TP = os.path.join(os.path.dirname(HERE), 'third_party')
for src, dst in [('inter/Inter-Regular.ttf', 'inter.ttf'), ('inter/Inter-Bold.ttf', 'interbd.ttf'),
                 ('dejavu/DejaVuSansMono.ttf', 'dejavumono.ttf'), ('dejavu/DejaVuSansMono-Bold.ttf', 'dejavumonobd.ttf')]:
    built.append((f'\\Windows\\Fonts\\{dst}', os.path.join(TP, src)))

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
