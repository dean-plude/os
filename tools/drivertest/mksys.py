#!/usr/bin/env python3
"""mksys.py — builds a Windows kernel-mode test driver: wdmtest.sys,
edutest.sys (step 30.3's Plug and Play driver for QEMU's edu device),
wddmdod.sys (Phase 25.3's WDDM display-only driver for QEMU's standard VGA)
or wditest.sys (step 28.1's WDI Wi-Fi miniport for QEMU's edu device)

    mksys.py OUTDIR [NAME]

tools/build_userland.py runs it and puts what it writes in C:\\Tests\\Wdm,
C:\\Tests\\Edu, C:\\Tests\\Wddm and C:\\Tests\\Wifi.  The driver (tools/drivertest/NAME.c) is built as a Windows driver is:
clang for x86_64-pc-windows-msvc, linked by lld-link /driver against an
import library for ntoskrnl.exe and hal.dll.  The import libraries are made
here from the functions NovaOS's kernel exports to drivers
(kernel/nt/exports.h), so the WDK is not needed.

Then it is signed as a driver developer signs one for test signing, by a
test certificate ("NovaOS Test Driver Signer" under "NovaOS Driver Test
Root", made here from fixed seeds and trusted by nobody), and the signing
policy's other cases are made from it:

    wdmtest.sys         an embedded test signature: loads only with test signing on
    wdmunsigned.sys     no signature: never loads
    wdmtampered.sys     wdmtest.sys with a code byte changed after signing: never loads
    wdmcat\\            a package signed by its catalog: wdmcat.inf, wdmcat.sys
                        (unsigned itself), wdmcat.cat (test-signed, listing both)
    wdmbad\\            the same package with wdmbad.sys changed after the catalog
                        was made: pnputil refuses to stage it

edutest.sys's, wddmdod.sys's and wditest.sys's packages (NAME.inf, from tools/drivertest)
are signed as wdmcat's is: a test-signed catalog, NAME.cat, lists the INF
and the .sys.
"""
import os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'authenticode'))
from pki import Cert, Key                                       # noqa: E402
from sign import sign                                           # noqa: E402
from catalog import catalog                                     # noqa: E402

LONG = ((2020, 1, 1, 0, 0, 0), (2049, 12, 31, 23, 59, 59))

INF = r'''; {name}.inf - a test-signed kernel driver package (tools/drivertest/mksys.py)
[Version]
Signature   = "$WINDOWS NT$"
Class       = System
ClassGuid   = {{4d36e97d-e325-11ce-bfc1-08002be10318}}
Provider    = %ProviderName%
DriverVer   = 10/12/2026,1.0.0.0
CatalogFile = {name}.cat
PnpLockdown = 1

[DestinationDirs]
DefaultDestDir = 13

[SourceDisksNames]
1 = %DiskName%,,,""

[SourceDisksFiles]
{name}.sys = 1,,

[Manufacturer]
%ProviderName% = NovaTest,NTamd64

[NovaTest.NTamd64]
%DeviceDesc% = Wdm_Install, ROOT\{upper}

[Wdm_Install.NT]
CopyFiles = Wdm_Files

[Wdm_Files]
{name}.sys

[Wdm_Install.NT.Services]
AddService = {upper}, 0x00000002, Wdm_Service

[Wdm_Service]
ServiceType   = 1               ; SERVICE_KERNEL_DRIVER
StartType     = 3               ; SERVICE_DEMAND_START
ErrorControl  = 1
ServiceBinary = %13%\{name}.sys

[Strings]
ProviderName = "NovaOS Test"
DiskName     = "NovaOS driver test disk"
DeviceDesc   = "NovaOS catalog-signed test driver"
'''


def restamp(pe, delta):
    """@pe with its TimeDateStamp moved: a different image (another hash)"""
    b = bytearray(pe)
    e = struct.unpack_from('<I', b, 60)[0]
    struct.pack_into('<I', b, e + 8, (struct.unpack_from('<I', b, e + 8)[0] + delta) & 0xFFFFFFFF)
    return bytes(b)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def test_signer():
    """The test certificate NovaOS's test drivers are signed with, and its root"""
    root_key, signer_key = Key('novaos-driver-test-root'), Key('novaos-driver-test-signer')
    root = Cert('NovaOS Driver Test Root', root_key, 1, LONG, ca=True)
    signer = Cert('NovaOS Test Driver Signer', signer_key, 2, LONG, root, root_key, usage='codesign')
    return signer, root


def signed_package(out, name):
    """@name.sys with its INF beside it and a test-signed catalog of both (the
    .sys signed by the catalog, as wdmcat's is)"""
    signer, root = test_signer()
    pe = open(os.path.join(out, name + '.sys'), 'rb').read()
    inf = open(os.path.join(HERE, name + '.inf'), 'rb').read()
    write(os.path.join(out, name + '.inf'), inf)
    write(os.path.join(out, name + '.cat'), catalog([(name + '.inf', inf), (name + '.sys', pe)], signer, [root]))


def signed_variants(out):
    signer, root = test_signer()
    path = os.path.join(out, 'wdmtest.sys')
    pe = open(path, 'rb').read()
    good = sign(pe, signer, [root], 'sha256')
    write(path, good)
    write(os.path.join(out, 'wdmunsigned.sys'), restamp(pe, 1))
    t = bytearray(good)
    e = struct.unpack_from('<I', t, 60)[0]
    sec = e + 24 + struct.unpack_from('<H', t, e + 20)[0]         # (the first section's raw data)
    text = struct.unpack_from('<I', t, sec + 20)[0]
    t[text + 16] ^= 0x01
    write(os.path.join(out, 'wdmtampered.sys'), bytes(t))
    for name, changed in (('wdmcat', False), ('wdmbad', True)):
        inf = INF.format(name=name, upper=name.upper()).replace('\n', '\r\n').encode()
        sys_ = restamp(pe, 2)
        cat = catalog([(name + '.inf', inf), (name + '.sys', sys_)], signer, [root])
        write(os.path.join(out, name, name + '.inf'), inf)
        write(os.path.join(out, name, name + '.sys'), restamp(sys_, 1) if changed else sys_)
        write(os.path.join(out, name, name + '.cat'), cat)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        sys.exit(1)


def import_libs(work):
    """Import libraries for ntoskrnl.exe and hal.dll in @work, made from the
    export list (kernel/nt/exports.h); returns their paths"""
    mods = {}
    src = open(os.path.join(ROOT, 'kernel', 'nt', 'exports.h')).read()
    for m in re.finditer(r'^\s*NT_EXPORT(_DATA)?\(\s*([\w.]+)\s*,\s*(\w+)\s*\)', src, re.M):
        # data exports are imported by address: the .def marks them DATA
        mods.setdefault(m.group(2), []).append((m.group(3), bool(m.group(1))))
    libs = []
    for mod, names in sorted(mods.items()):
        stem = os.path.splitext(mod)[0]
        d = os.path.join(work, stem + '.def')
        with open(d, 'w') as f:
            f.write(f'LIBRARY {mod}\nEXPORTS\n' + ''.join(f'    {n}{" DATA" if data else ""}\n' for n, data in names))
        lib = os.path.join(work, stem + '.lib')
        run(['llvm-lib', f'/def:{d}', '/machine:x64', f'/out:{lib}'])
        libs.append(lib)
    return libs


def main():
    out = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else 'wdmtest'
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, 'obj')
    os.makedirs(work, exist_ok=True)
    libs = import_libs(work)
    obj = os.path.join(work, name + '.obj')
    # -fasync-exceptions (MSVC's /EHa for the kernel): without it the
    # optimizer assumes a memory access never faults and deletes a
    # __try/__except around one, dropping the .pdata/.xdata and the
    # __C_specific_handler import the kernel's SEH (kernel/nt/seh.c) walks.
    run(['clang', '--target=x86_64-pc-windows-msvc', '-O2', '-ffreestanding', '-fno-stack-protector',
         '-mno-stack-arg-probe', '-fno-builtin', '-fasync-exceptions', '-Wall', '-c',
         os.path.join(HERE, name + '.c'), '-o', obj])
    run(['lld-link', '/driver', '/subsystem:native', '/entry:DriverEntry', '/nodefaultlib', '/machine:x64',
         '/base:0x140000000', obj] + libs + [f'/out:{os.path.join(out, name + ".sys")}'])
    for n in os.listdir(work):
        os.remove(os.path.join(work, n))
    os.rmdir(work)
    if name == 'wdmtest':
        signed_variants(out)
    else:
        signed_package(out, name)


if __name__ == '__main__':
    main()
