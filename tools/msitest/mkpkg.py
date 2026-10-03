#!/usr/bin/env python3
"""mkpkg.py — the Windows Installer packages the msitest self-test installs

    mkpkg.py OUTDIR MSITEST.EXE

tools/build_userland.py runs it and puts what it writes in C:\\Tests\\Msi:

  base.msi    "Nova MSI Test" 1.0.0: a.txt ("one") in C:\\Programs\\NovaMsiTest
              and HKLM\\SOFTWARE\\NovaMsiTest Version=1.0.0
  base.mst    a transform of base.msi adding the registry value
              Edition=Transformed (validated against base.msi's ProductCode)
  other.mst   the same change made for another product: must be refused
  patch.msp   base.msi to 1.0.1: a.txt becomes "two", Version=1.0.1
  rollback.msi "Nova Rollback Test": overwrites old.txt, adds new.txt and
              tool.exe (a copy of msitest) and HKLM\\SOFTWARE\\NovaRbTest, schedules
              a rollback custom action (tool.exe mark ...), then fails with
              an error custom action; everything must come back as it was
  service.msi "Nova Service Test": svc.exe (msitest again, run as
              "svc.exe service") installed as the automatic service
              NovaMsiTestSvc and started on install

Each package is written by mkmsi.py; nothing here needs msitools.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkmsi

BASE = '{6E1D0C3A-5A1B-4C2D-8E3F-00000000B001}'
BASE_UP = '{6E1D0C3A-5A1B-4C2D-8E3F-00000000B0F1}'
RB = '{6E1D0C3A-5A1B-4C2D-8E3F-00000000F001}'
RB_UP = '{6E1D0C3A-5A1B-4C2D-8E3F-00000000F0F1}'
SVC = '{6E1D0C3A-5A1B-4C2D-8E3F-000000005001}'
SVC_UP = '{6E1D0C3A-5A1B-4C2D-8E3F-0000000050F1}'
OTHER = '{6E1D0C3A-5A1B-4C2D-8E3F-00000000E001}'
PATCH = '{6E1D0C3A-5A1B-4C2D-8E3F-00000000A001}'

HKLM = 2


def base(version='1.0.0', text=b'one\n'):
    db = mkmsi.package('Nova MSI Test', version, BASE, BASE_UP, 'NovaMsiTest', [('a.txt', 'a.txt', text)])
    db.add('Registry', ('RegVersion', HKLM, 'SOFTWARE\\NovaMsiTest', 'Version', version, 'C_a.txt'))
    return db


def with_edition(db):
    db = db.copy()
    db.add('Registry', ('RegEdition', HKLM, 'SOFTWARE\\NovaMsiTest', 'Edition', 'Transformed', 'C_a.txt'))
    return db


def rollback(tool):
    db = mkmsi.package('Nova Rollback Test', '1.0.0', RB, RB_UP, 'NovaRbTest',
                       [('old.txt', 'old.txt', b'after\n'), ('new.txt', 'new.txt', b'new\n'), ('tool', 'tool.exe', tool)])
    db.add('Registry', ('RbValue', HKLM, 'SOFTWARE\\NovaRbTest', 'Installed', '1', 'C_new.txt'))
    # 18 = a program the package installs, 0x500 = rollback (runs only if
    # the installation fails); 19 = an error message that fails it
    db.add('CustomAction', ('MarkRollback', 18 | 0x500, 'tool', 'mark C:\\Tests\\MsiOut\\rollback-ran.txt'),
           ('PlannedFailure', 19, None, 'This package fails on purpose (msitest rollback)'))
    db.add('InstallExecuteSequence', ('MarkRollback', None, 4100), ('PlannedFailure', None, 6000))
    return db


def service(exe):
    db = mkmsi.package('Nova Service Test', '1.0.0', SVC, SVC_UP, 'NovaSvcTest', [('svcexe', 'svc.exe', exe)])
    db.add('ServiceInstall', ('si1', 'NovaMsiTestSvc', 'Nova MSI Test Service', 0x10, 2, 1, None, None, None, None,
                              'service', 'C_svcexe', 'Writes C:\\Tests\\MsiOut\\svc.txt each time it starts'))
    # 0x1 start on install, 0x20 stop and 0x80 delete on uninstall
    db.add('ServiceControl', ('sc1', 'NovaMsiTestSvc', 0x1 | 0x20 | 0x80, None, 1, 'C_svcexe'))
    return db


def main():
    out, exe = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    tool = open(exe, 'rb').read()
    b = base()
    b.write(os.path.join(out, 'base.msi'))
    mkmsi.write_transform(b, with_edition(b), os.path.join(out, 'base.mst'))
    other = mkmsi.package('Nova Other', '1.0.0', OTHER, OTHER, 'NovaOther', [('a.txt', 'a.txt', b'x\n')])
    mkmsi.write_transform(other, with_edition(other), os.path.join(out, 'other.mst'))
    mkmsi.patch(b, base('1.0.1', b'two\n'), PATCH, {'a.txt': b'two\n'}, os.path.join(out, 'patch.msp'))
    rollback(tool).write(os.path.join(out, 'rollback.msi'))
    service(tool).write(os.path.join(out, 'service.msi'))


if __name__ == '__main__':
    main()
