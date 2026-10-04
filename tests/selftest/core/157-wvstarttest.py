# wvstarttest: what the WebView2 runtime's browser process
# (msedgewebview2.exe) and its loader need to start: COM's apartment in
# the TEB, TerminateProcess on itself without DLL detach or FLS
# callbacks, more than 128 FLS slots, GetAddrInfoExW, the Rtl IP address
# parsers, LdrLockLoaderLock, CryptFindOIDInfo, the performance counter
# provider API and kernel32's GetDllDirectoryW and package queries.
DOC = '`wvstarttest` (COM\'s apartment in the TEB, TerminateProcess on itself, FLS slots and the functions the WebView2 runtime\'s browser process calls)'
TESTS = [
    Test('wvstarttest', 'wvstarttest', [r'wvstarttest: \d+ passed, 0 failed'], timeout=120),
]
