# edgeupdtest: the Windows APIs Microsoft Edge Update (the WebView2
# runtime's installer) calls: Task Scheduler 2.0 (register a task from its
# XML, read it back, disable and enable it, run its action, list and
# delete it), the Data Protection API (CryptProtectData round trips with
# the user's and the machine's key; the wrong entropy or a changed blob
# fails), shlwapi's UrlCombineW (RFC 3986's examples), UrlEscapeA and
# UrlUnescapeA, the package-name parsers, WTSEnumerateSessionsW,
# MakeAbsoluteSD, the MDM and Azure AD enrolment checks and ole32's
# CoRegisterPSClsid and CoGetCallContext, 64- and 32-bit; and cmd's `type`
# showing a UTF-16 file (Edge Update's log is one) as text.
DOC = '`edgeupdtest` (Task Scheduler 2.0, DPAPI, UrlCombine and the other APIs Microsoft Edge Update needs, 64- and 32-bit)'
TESTS = [
    Test('edgeupdtest x64', 'edgeupdtest', [r'edgeupdtest: \d+ passed, 0 failed'], timeout=120),
    Test('edgeupdtest x86', r'C:\Programs\x86\edgeupdtest.exe', [r'edgeupdtest: \d+ passed, 0 failed'], timeout=120),
    Test('type of a UTF-16 file', r'cmd.exe /c "edgeupdtest utf16 C:\Temp\utf16.log & type C:\Temp\utf16.log"',
         [r'line 0 caf', r'line 999 caf.* ok', r'end of the UTF-16 file'], timeout=120),
]
