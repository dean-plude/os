#!/usr/bin/env python3
"""Check Windows programs against the NovaOS DLLs.

    tools/pe_imports.py [--dlls DIR] PROGRAM.exe ...

Lists every function each program imports that NovaOS cannot provide:
DLLs it does not have, and functions missing from the DLLs it does have
(api-ms-win-* API set names are mapped the way the kernel's loader maps
them).  DIR defaults to the userland build directory.  With --exports,
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

    def imports(self):
        out = {}
        rva = self.dirs[1][0]
        if not rva:
            return out
        o = self.off(rva)
        while True:
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
            o += 20
        return out

    def exports(self):
        rva, size = self.dirs[0]
        if not rva:
            return set()
        o = self.off(rva)
        nnames = struct.unpack_from('<I', self.d, o + 24)[0]
        names = struct.unpack_from('<I', self.d, o + 32)[0]
        return {self.cstr(struct.unpack_from('<I', self.d, self.off(names) + 4 * i)[0]) for i in range(nnames)}


def apiset(name):
    """The DLL an API set name resolves to (as kernel/um/um.c maps them)"""
    n = name.lower()
    if n.startswith('api-ms-win-crt-'):
        return 'ucrtbase.dll'
    if n.startswith('api-ms-win-') or n.startswith('ext-ms-win-'):
        return 'kernel32.dll'
    return n


def main():
    args = sys.argv[1:]
    dlldir = DEFAULT_DLLS
    if args[:1] == ['--dlls']:
        dlldir, args = args[1], args[2:]
    if args[:1] == ['--exports']:
        for n in sorted(PE(args[1]).exports()):
            print(n)
        return 0
    have = {}
    for f in os.listdir(dlldir):
        if f.lower().endswith('.dll'):
            have[f.lower()] = PE(os.path.join(dlldir, f)).exports()
    missing_total = 0
    for prog in args:
        pe = PE(prog)
        print(f'== {prog}' + ('' if pe.machine == 0x8664 else '  (not x64: cannot run)'))
        for dll, fns in pe.imports().items():
            real = apiset(dll)
            if real not in have:
                print(f'  missing DLL {dll}: {len(fns)} functions')
                missing_total += len(fns)
                continue
            gone = [f for f in fns if not f.startswith('#') and f not in have[real]]
            for f in gone:
                print(f'  {dll}!{f}')
            missing_total += len(gone)
    print(f'{missing_total} missing')
    return 1 if missing_total else 0


if __name__ == '__main__':
    sys.exit(main())
