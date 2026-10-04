# setuptest: what game and application installers need (Inno Setup's,
# GOG's, NSIS's).  ShellExecuteEx's "runas" starts a program elevated and a
# manifest asking for administrator does too (setupadmin.exe), while a plain
# CreateProcess stays limited; msftedit.dll's and riched20.dll's Rich Edit
# classes take a licence's RTF and answer the Rich Edit messages; oleaut32's
# variant arithmetic (VarAdd ... VarCmp, Delphi's variant operators) works;
# "setuptest bigfile" writes, reads and deletes a 300 MB file on C: (files
# stopped at 256 MB, under GOG Galaxy's 342 MB offline installer).
DOC = '`setuptest` (run as administrator, Rich Edit with RTF, variant arithmetic, a 300 MB file on C:, 64- and 32-bit)'

TESTS = [
    Test('setup x64', 'setuptest', [r'setuptest: \d+ passed, 0 failed']),
    Test('setup x86', r'C:\Programs\x86\setuptest.exe', [r'setuptest: \d+ passed, 0 failed']),
    Test('setup big file', 'setuptest bigfile', [r'setuptest bigfile: \d+ passed, 0 failed'], timeout=300),
]
