# qtthemetest: what Qt's Windows platform plugin (qwindows.dll, in GOG
# GALAXY's client) asks of the Windows Runtime: UISettings (the light and
# dark app modes, the accent palette, ColorValuesChanged when the theme's
# keys change, AdvancedEffectsEnabled) and UIViewSettings (no tablet mode);
# and CNG's BCryptEnumContextFunctions (Schannel's cipher suites, which
# Galaxy's CrashReporter.exe lists); 64- and 32-bit.
DOC = ('`qtthemetest` (Windows.UI.ViewManagement UISettings and UIViewSettings as Qt reads them, '
       'ColorValuesChanged, Schannel\'s cipher suites; 64- and 32-bit)')

TESTS = [
    Test('qt theme x64', 'qtthemetest', [r'qtthemetest: \d+ passed, 0 failed']),
    Test('qt theme x86', r'C:\Programs\x86\qtthemetest.exe', [r'qtthemetest: \d+ passed, 0 failed']),
]
