# msiqtest: Windows Installer queries as bootstrappers such as WiX Burn (the
# Visual C++ Redistributable) make them, from a 64- and a 32-bit process: a
# 32-bit package's SystemFolder files go to SysWOW64 and a 64-bit one's to
# System32; product information by installation context; the source list
# (MsiSourceListAddSourceEx and friends); which patches fit a product
# (MsiDetermineApplicablePatches, MsiDeterminePatchSequence); the patches
# applied to one (MsiEnumPatchesEx, MsiGetPatchInfoEx); removal.
DOC = ('`msiqtest` (Windows Installer: 32-bit packages into SysWOW64, product information by context, '
       'source lists, patch applicability; 64- and 32-bit)')

TESTS = [
    Test('msi queries x64', 'msiqtest', [r'msiqtest: \d+ passed, 0 failed']),
    Test('msi queries x86', r'C:\Programs\x86\msiqtest.exe', [r'msiqtest: \d+ passed, 0 failed']),
]
