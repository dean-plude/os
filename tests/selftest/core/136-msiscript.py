# Windows Installer script custom actions: JScript and VBScript (types 5,
# 6, 21, 22, 37, 38, 53, 54) on msiscript.dll, in tools/msitest/mkpkg.py's
# script.msi and scriptfail.msi (C:\Tests\Msi)
DOC = ('Windows Installer JScript and VBScript custom actions setting and reading properties and writing files, '
       'and a failing script rolling its install back (`msitest script`)')
TESTS = [
    Test('msi script actions', 'msitest script', [r'msitest script: \d+ passed, 0 failed']),
]
