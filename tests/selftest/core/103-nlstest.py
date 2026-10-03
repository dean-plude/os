# nlstest: GetDateFormat, GetTimeFormat, GetNumberFormat and
# GetCurrencyFormat in de-DE, ja-JP and en-US against Windows' output, 64-
# and 32-bit; then the user locale set with intl.exe and read by new
# processes (tools/selftest.py's core suite)
DOC = '`nlstest` (date, time, number and currency formats in German and Japanese, 64- and 32-bit; `nlstest user`)'
TESTS = [
    Test('nlstest', 'nlstest', [r'nlstest: \d+ passed, 0 failed']),
    Test('nlstest x86', r'C:\Programs\x86\nlstest.exe', [r'nlstest: \d+ passed, 0 failed']),
    Test('user locale', 'nlstest user', [r'user locale: de-DE, Freitag, 2\. Oktober 2026', r'nlstest user: \d+ passed, 0 failed']),
]
