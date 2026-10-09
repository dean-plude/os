# schanneltest stores: crypt32 finds a certificate through a collection that
# holds a collection (Chromium's TrustStoreWin looks up issuers that way, so
# Qt WebEngine's sign-in pages could not build a chain), each store's
# certificates once; a root without extended key usages is good for every
# usage; wininet reports this boot (no network adapter) offline
DOC = ('`schanneltest stores` (a certificate found through a collection of collections, a root\'s extended key '
       'usages, and `InternetGetConnectedState` offline without a network; 64- and 32-bit)')
TESTS = [
    Test('schanneltest stores x64', 'schanneltest stores', [r'network: offline', r'schanneltest: \d+ passed, 0 failed']),
    Test('schanneltest stores x86', r'C:\Programs\x86\schanneltest.exe stores', [r'network: offline', r'schanneltest: \d+ passed, 0 failed']),
]
