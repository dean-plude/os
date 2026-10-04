# authtest: Authenticode.  WinVerifyTrust checks the signed test files
# tools/authenticode/mktests.py makes (a test certificate authority the
# test trusts for the run): SHA-256, SHA-1, 32-bit and nested signatures
# and expired signers saved by a PKCS #9 or RFC 3161 timestamp verify; a
# changed byte, a damaged signature, an untrusted root, an expired signer,
# the wrong key usage, no signature and a text file fail with Windows'
# errors; the provider state, CryptQueryObject, signer names, offline
# revocation and the catalog hash.
DOC = '`authtest` (Authenticode: WinVerifyTrust, signer chains and timestamps, CryptQueryObject; 64- and 32-bit)'

TESTS = [
    Test('authenticode x64', 'authtest', [r'authtest: \d+ passed, 0 failed']),
    Test('authenticode x86', r'C:\Programs\x86\authtest.exe', [r'authtest: \d+ passed, 0 failed']),
]
