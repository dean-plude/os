# wvsetuptest: what the WebView2 runtime's own setup (Chromium's setup.exe)
# needs to unpack its archive: a 300 MB file mapped whole, read-only,
# through a duplicate of its handle (sections were limited to 256 MB, and
# the failure came back as ERROR_INVALID_FUNCTION; setup.exe maps the 728
# MB MSEDGE.7z), an empty file's mapping refused with ERROR_FILE_INVALID;
# wer.dll's report API (made, filled in, submitted to nowhere as
# WerDisabled, closed); kernel32's FlsGetValue2.  The 32-bit run leaves
# out the large file.
DOC = '`wvsetuptest` (a 300 MB file mapped whole, wer.dll\'s report API and FlsGetValue2, as the WebView2 runtime\'s setup needs; 64- and 32-bit)'
TESTS = [
    Test('wvsetuptest x64', 'wvsetuptest', [r'wvsetuptest: \d+ passed, 0 failed'], timeout=180),
    Test('wvsetuptest x86', r'C:\Programs\x86\wvsetuptest.exe small', [r'wvsetuptest: \d+ passed, 0 failed'], timeout=120),
]
