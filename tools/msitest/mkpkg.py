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
  script.msi  "Nova Script Test": script custom actions, JScript and
              VBScript, from the Binary table (5, 6), as text in the
              CustomAction table (37, 38), in a property (53, 54) and in
              installed files (21, 22, deferred); scripts/ holds them.  They
              set properties the Registry table then writes to
              HKLM\\SOFTWARE\\NovaScriptTest, and write C:\\Tests\\MsiOut\\script-*.txt
  scriptfail.msi "Nova Script Failure": a JScript action that throws but
              may fail (0x40, ignored), then a VBScript one that raises an
              error: the installation fails and is rolled back
  wow32.msi   "Nova WoW32 Test", a 32-bit package (Template "Intel;1033"):
              app.txt in C:\\Programs\\NovaWow32 and novawow32.txt in its
              SystemFolder, which is SysWOW64 on 64-bit Windows
  wow64.msi   "Nova WoW64 Test", the same as a 64-bit package: its
              novawow64.txt goes to System32 (msiqtest installs both)

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


SCRIPT = '{6E1D0C3A-5A1B-4C2D-8E3F-00000000C001}'
SCRIPT_UP = '{6E1D0C3A-5A1B-4C2D-8E3F-00000000C0F1}'
SCRIPT_FAIL = '{6E1D0C3A-5A1B-4C2D-8E3F-00000000C002}'
SCRIPTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scripts')
# the CustomAction table with a long Target (inline scripts)
CA_LONG = [('Action', 's72*'), ('Type', 'i2'), ('Source', 'S72'), ('Target', 'S0')]


def script_text(name):
    return open(os.path.join(SCRIPTS, name), 'rb').read()


def script():
    db = mkmsi.package('Nova Script Test', '1.0.0', SCRIPT, SCRIPT_UP, 'NovaScriptTest',
                       [('checkjs', 'check.js', script_text('check.js')), ('checkvbs', 'check.vbs', script_text('check.vbs'))],
                       extra_props=[
                           ('JS_CODE', 'function Main() { Session.Property("JS_FROM_PROPERTY") = "yes " + Session.Property("ProductVersion"); }'),
                           ('VBS_CODE', 'Sub Main() : Session.Property("VBS_FROM_PROPERTY") = "yes " & Session.Property("ProductVersion") : End Sub'),
                       ])
    db.table('CustomAction', CA_LONG)
    db.add('Binary', ('SetupJs', script_text('setup.js')), ('SetupVbs', script_text('setup.vbs')))
    for name, value in [('JsProp', '[JS_PROP]'), ('VbsProp', '[VBS_PROP]'), ('JsInline', '[JS_INLINE]'),
                        ('VbsInline', '[VBS_INLINE]'), ('JsFromProperty', '[JS_FROM_PROPERTY]'),
                        ('VbsFromProperty', '[VBS_FROM_PROPERTY]')]:
        db.add('Registry', ('Reg' + name, HKLM, 'SOFTWARE\\NovaScriptTest', name, value, 'C_checkjs'))
    # 5/6 a function in the Binary table, 37/38 the script text itself,
    # 53/54 a function in a property's value, 21/22 a function in an
    # installed file (0x400: deferred, after InstallFiles; 51 sets their
    # CustomActionData)
    db.add('CustomAction',
           ('JsBinary', 5, 'SetupJs', 'SetUp'),
           ('VbsBinary', 6, 'SetupVbs', 'SetUp'),
           ('JsInline', 37, None, 'Session.Property("JS_INLINE") = Session.Property("JS_PROP") + "!";'),
           ('VbsInline', 38, None, 'Session.Property("VBS_INLINE") = Session.Property("VBS_PROP") & "!"'),
           ('JsProperty', 53, 'JS_CODE', 'Main'),
           ('VbsProperty', 54, 'VBS_CODE', 'Main'),
           ('SetJsFile', 51, 'JsFile', 'C:\\Tests\\MsiOut\\|[JS_INLINE]'),
           ('SetVbsFile', 51, 'VbsFile', 'C:\\Tests\\MsiOut\\|[VBS_INLINE]'),
           ('JsFile', 21 | 0x400, 'checkjs', 'Check'),
           ('VbsFile', 22 | 0x400, 'checkvbs', 'Check'))
    for seq, a in enumerate(['JsBinary', 'VbsBinary', 'JsInline', 'VbsInline', 'JsProperty', 'VbsProperty',
                             'SetJsFile', 'SetVbsFile'], 1100):
        db.add('InstallExecuteSequence', (a, 'NOT Installed', seq))
    db.add('InstallExecuteSequence', ('JsFile', 'NOT Installed', 4100), ('VbsFile', 'NOT Installed', 4110))
    return db


def script_fail():
    db = mkmsi.package('Nova Script Failure', '1.0.0', SCRIPT_FAIL, SCRIPT_FAIL, 'NovaScriptFail', [('a.txt', 'a.txt', b'x\n')])
    db.table('CustomAction', CA_LONG)
    db.add('CustomAction',
           ('JsMayFail', 37 | 0x40, None, 'throw new Error("this failure is ignored (0x40)");'),
           ('VbsFails', 38, None, 'Err.Raise 5, "scriptfail.msi", "This script fails on purpose"'))
    db.add('InstallExecuteSequence', ('JsMayFail', None, 4100), ('VbsFails', None, 4200))
    return db


WOW32 = '{6E1D0C3A-5A1B-4C2D-8E3F-000000003201}'
WOW32_UP = '{6E1D0C3A-5A1B-4C2D-8E3F-0000000032F1}'
WOW64 = '{6E1D0C3A-5A1B-4C2D-8E3F-000000006401}'
WOW64_UP = '{6E1D0C3A-5A1B-4C2D-8E3F-0000000064F1}'


def wow(bits):
    """A package with one file in its program folder and one in SystemFolder"""
    code, up = (WOW32, WOW32_UP) if bits == 32 else (WOW64, WOW64_UP)
    sysfile = 'novawow%d.txt' % bits
    db = mkmsi.package('Nova WoW%d Test' % bits, '1.0.0', code, up, 'NovaWow%d' % bits,
                       [('app.txt', 'app.txt', b'app\n'), ('sys', sysfile, b'%d-bit\n' % bits)])
    db.summary[mkmsi.PID_TEMPLATE] = 'Intel;1033' if bits == 32 else 'x64;1033'
    db.add('Directory', ('SystemFolder', 'TARGETDIR', '.'))
    for row in db.tables['Component'].rows:
        if bits == 32:
            row[3] = 0                     # (no msidbComponentAttributes64bit)
        if row[0] == 'C_sys':
            row[2] = 'SystemFolder'
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
    script().write(os.path.join(out, 'script.msi'))
    script_fail().write(os.path.join(out, 'scriptfail.msi'))
    wow(32).write(os.path.join(out, 'wow32.msi'))
    wow(64).write(os.path.join(out, 'wow64.msi'))


if __name__ == '__main__':
    main()
