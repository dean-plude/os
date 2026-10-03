# nlstest calendars / override: the locales' calendars (GetCalendarInfo,
# EnumCalendarInfo, DATE_USE_ALT_CALENDAR, EnumDateFormats), GetDurationFormat
# and the user's overrides (SetLocaleInfo), 64- and 32-bit
# (tools/selftest.py's core suite)
DOC = '`nlstest calendars` (Japanese eras, Buddhist, Taiwan, Tangun, Hebrew, Hijri, Um Al Qura and Persian dates; `GetDurationFormat`) and `nlstest override` (`SetLocaleInfo`), 64- and 32-bit'
TESTS = [
    Test('nlstest calendars', 'nlstest calendars', [r'nlstest calendars: \d+ passed, 0 failed']),
    Test('nlstest calendars x86', r'C:\Programs\x86\nlstest.exe calendars', [r'nlstest calendars: \d+ passed, 0 failed']),
    Test('nlstest override', 'nlstest override', [r'nlstest override: \d+ passed, 0 failed']),
    Test('nlstest override x86', r'C:\Programs\x86\nlstest.exe override', [r'nlstest override: \d+ passed, 0 failed']),
]
