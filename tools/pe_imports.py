#!/usr/bin/env python3
"""Check Windows programs against the NovaOS DLLs.

    tools/pe_imports.py [--dlls DIR] PROGRAM.exe ...

Lists every function each program imports that NovaOS cannot provide:
DLLs it does not have, and functions missing from the DLLs it does have
(api-ms-win-* API set names are mapped the way the kernel's loader maps
them).  Delay-loaded imports are checked too, marked "(delay)", imports by
ordinal (#N) against the ordinals the DLL exports, and DLLs
next to the program count as present (an app ships its own).  DIR
defaults to the userland build directory.  With --exports,
prints a DLL's export names instead.
"""
import os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DLLS = os.path.join(ROOT, 'build', 'kernel_build', 'userland')


class PE:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        nt = struct.unpack_from('<I', d, 0x3C)[0]
        if d[nt:nt + 4] != b'PE\0\0':
            raise ValueError(f'{path}: not a PE file')
        self.machine, nsec = struct.unpack_from('<HH', d, nt + 4)
        optsz = struct.unpack_from('<H', d, nt + 20)[0]
        opt = nt + 24
        magic = struct.unpack_from('<H', d, opt)[0]
        dd = opt + (112 if magic == 0x20B else 96)
        self.pe64 = magic == 0x20B
        self.dirs = [struct.unpack_from('<II', d, dd + 8 * i) for i in range(16)]
        self.secs = []
        for i in range(nsec):
            s = opt + optsz + 40 * i
            vsz, va, rsz, ptr = struct.unpack_from('<IIII', d, s + 8)
            self.secs.append((va, max(vsz, rsz), ptr, rsz))

    def off(self, rva):
        for va, sz, ptr, rsz in self.secs:
            if va <= rva < va + sz:
                return ptr + rva - va
        return rva

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b'\0', o)].decode('latin-1')

    def imports(self, delay=False):
        """{dll: [function or #ordinal]}: the import table, or with @delay
        the delay-load table (IMAGE_DELAYLOAD_DESCRIPTOR)"""
        out = {}
        rva = self.dirs[13 if delay else 1][0]
        if not rva:
            return out
        o = self.off(rva)
        while True:
            if delay:
                _, name, _, iat, ilt = struct.unpack_from('<IIIII', self.d, o)
            else:
                ilt, _, _, name, iat = struct.unpack_from('<IIIII', self.d, o)
            if not name and not iat:
                break
            dll = self.cstr(name)
            fns = out.setdefault(dll, [])
            t = self.off(ilt or iat)
            width = 8 if self.pe64 else 4
            while True:
                v = struct.unpack_from('<Q' if self.pe64 else '<I', self.d, t)[0]
                if not v:
                    break
                if v >> (63 if self.pe64 else 31):
                    fns.append(f'#{v & 0xFFFF}')
                else:
                    fns.append(self.cstr((v & 0x7FFFFFFF) + 2))
                t += width
            o += 32 if delay else 20
        return out

    def exports(self):
        rva, size = self.dirs[0]
        if not rva:
            return set()
        o = self.off(rva)
        base, nfuncs, nnames, funcs, names = struct.unpack_from('<IIIII', self.d, o + 16)
        out = {self.cstr(struct.unpack_from('<I', self.d, self.off(names) + 4 * i)[0]) for i in range(nnames)}
        return out | {f'#{base + i}' for i in range(nfuncs)       # ordinals, as imports name them
                      if struct.unpack_from('<I', self.d, self.off(funcs) + 4 * i)[0]}


# API sets, mapped the way the kernel's loader maps them (map_api_set in
# kernel/um/um.c): first matching prefix wins
API_SETS = [
    ('api-ms-win-crt-', 'ucrtbase.dll'),
    ('api-ms-win-core-synch-', 'kernel32.dll'),
    ('api-ms-win-core-com-', 'ole32.dll'),
    ('api-ms-win-core-winrt-', 'ole32.dll'),
    ('combase.dll', 'ole32.dll'),
    ('api-ms-win-core-', 'kernel32.dll'),
    ('api-ms-win-security-', 'advapi32.dll'),
    ('api-ms-win-eventing-', 'advapi32.dll'),
    ('api-ms-win-power-', 'powrprof.dll'),
    ('api-ms-win-shell-', 'shell32.dll'),
    ('api-ms-win-shcore-', 'shlwapi.dll'),
    ('shcore.dll', 'shlwapi.dll'),
    ('ext-ms-win-', 'kernel32.dll'),
    ('kernelbase.dll', 'kernel32.dll'),
    ('api-ms-win-', 'kernel32.dll'),
    ('msvcrt40.dll', 'msvcrt.dll'),
]


def apiset(name):
    """The DLL an API set name resolves to (as kernel/um/um.c maps them)"""
    n = name.lower()
    for prefix, dll in API_SETS:
        if n.startswith(prefix):
            return dll
    return n


def main():
    args = sys.argv[1:]
    dlldir = DEFAULT_DLLS
    if args[:1] == ['--dlls']:
        dlldir, args = args[1], args[2:]
    if args[:1] == ['--exports']:
        for n in sorted(n for n in PE(args[1]).exports() if not n.startswith('#')):
            print(n)
        return 0
    have = {}
    for f in os.listdir(dlldir):
        if f.lower().endswith(('.dll', '.drv')):
            have[f.lower()] = PE(os.path.join(dlldir, f)).exports()
    missing_total = 0
    for prog in args:
        pe = PE(prog)
        print(f'== {prog}' + ('' if pe.machine == 0x8664 else '  (not x64: cannot run)'))
        here = dict(have)                         # the app's own DLLs, beside it
        appdir = os.path.dirname(os.path.abspath(prog))
        for f in os.listdir(appdir):
            if f.lower().endswith('.dll') and f.lower() not in here:
                try:
                    here[f.lower()] = PE(os.path.join(appdir, f)).exports()
                except (ValueError, struct.error):
                    pass
        for delay in (False, True):
            tag = ' (delay)' if delay else ''
            for dll, fns in pe.imports(delay).items():
                real = apiset(dll)
                if real not in here:
                    print(f'  missing DLL {dll}{tag}: {len(fns)} functions')
                    missing_total += len(fns)
                    continue
                gone = [f for f in fns if f not in here[real]]
                for f in gone:
                    print(f'  {dll}!{f}{tag}')
                missing_total += len(gone)
    print(f'{missing_total} missing')
    return 1 if missing_total else 0


if __name__ == '__main__':
    sys.exit(main())
