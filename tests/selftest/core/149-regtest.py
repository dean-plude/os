# regtest: registry keys opened, closed and deleted at once by threads of
# one process and of four waves of four child processes (which inherit a
# key handle and end with keys open and change notifications pending),
# handles duplicated, keys deleted while others hold them, one key's values
# set, read and deleted by everyone, and a thread waiting synchronously
# for changes meanwhile; the shared key must stay whole.
DOC = '`regtest` (isolated 32- and 64-bit machine registry views, shared keys, and registry keys and values changed concurrently)'

TESTS = [
    Test('registry keys x64', 'regtest', [r'regtest: \d+ passed, 0 failed'], timeout=300),
    Test('registry keys x86', r'C:\Programs\x86\regtest.exe 150', [r'regtest: \d+ passed, 0 failed'], timeout=300),
]
