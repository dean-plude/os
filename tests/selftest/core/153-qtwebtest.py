# qtwebtest: the calls Qt WebEngine (Chromium, in GOG GALAXY's client) imports
# that NovaOS lacked, each answering as Windows does without the hardware it
# reaches: no Bluetooth radio (and the SDP record parsers in full), no
# Direct3D 12 device, no WinUSB or HID device; SetEnvironmentStringsW,
# TreeResetNamedSecurityInfoW, NetShareEnum, the interface's LUID and GUID,
# DnsQueryEx, WSAAccept's condition function, SetArcDirection, AppContainer
# profiles, CryptVerifyCertificateSignatureEx, WinHTTP's proxy resolver,
# urlmon's zones, _ultow_s; 64- and 32-bit.
DOC = ('`qtwebtest` (the calls Qt WebEngine imports: Bluetooth and SDP records, Direct3D 12, WinUSB, '
       'AppContainer profiles, proxy resolver, security zones and more; 64- and 32-bit)')

TESTS = [
    Test('qt webengine imports x64', 'qtwebtest', [r'qtwebtest: \d+ passed, 0 failed']),
    Test('qt webengine imports x86', r'C:\Programs\x86\qtwebtest.exe', [r'qtwebtest: \d+ passed, 0 failed']),
]
