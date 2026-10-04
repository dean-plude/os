## Authenticode: WinVerifyTrust checks program signatures

Steam's service checks Steam's files with `WinVerifyTrust` and refused them
as unsigned, because NovaOS's `wintrust` did not check signatures.  It now
does, the way Windows does, offline: the signature in a program's
certificate table, its digest, its signer's certificate chain to a trusted
root, and a timestamp that keeps an expired signer valid.  Nothing is
skipped and nothing answers yes without checking.  The cryptography is
Mbed TLS's (Apache-2.0), already in NovaOS for TLS.

- **`WinVerifyTrust`** with `WINTRUST_ACTION_GENERIC_VERIFY_V2` (and the
  driver and published-software actions) over a file or a memory blob: the
  PE's Authenticode digest (SHA-1, SHA-256, SHA-384, SHA-512 or MD5, the
  checksum, the certificate-table entry and the table left out) against
  the signed `SpcIndirectDataContent`, the signer's signature over its
  authenticated attributes, the chain under the Authenticode policy with
  the code-signing usage, PKCS #9 countersignatures and RFC 3161 tokens
  (their own chain with the time-stamping usage), nested signatures
  (`WINTRUST_SIGNATURE_SETTINGS`), and the provider state
  (`WTD_STATEACTION_VERIFY`, `WTHelperProvDataFromStateData` and the
  signer and certificate helpers).  A revocation check fails with
  `CERT_E_REVOCATION_FAILURE`: NovaOS cannot reach revocation lists, and
  "unknown" is never "not revoked".
- **crypt32**: signed PKCS #7 messages (`CryptMsgOpenToDecode`,
  `CryptMsgUpdate`, `CryptMsgGetParam`, `CryptMsgControl`, countersignature
  checks), `CryptQueryObject` (embedded signatures, PKCS #7 and
  certificates, from a file or memory), message stores,
  `CertGetSubjectCertificateFromStore`, `CertVerifyTimeValidity`,
  `CertGetNameString` and `CertNameToStr`, chains checked at a given time,
  and the Authenticode policies in `CertVerifyCertificateChainPolicy`.
  Roots added to the machine's `ROOT` store are kept in
  `C:\Windows\System32\CertStore` and survive restarts; deleted ones stay
  deleted.
- **Roots**: Microsoft's code-signing and time-stamping roots join the
  Mozilla list, one file each in `userland/crypt32/roots`.
- **Catalogs**: `CryptCATAdminAcquireContext`,
  `CryptCATAdminCalcHashFromFileHandle` and the rest; NovaOS has no
  catalog files, so no catalog holds a hash.
- **Tests**: `tools/authenticode` signs test programs under a test root
  in plain Python (`mktests.py`, made the same way on every build), and
  the `authtest` self-test checks the files it must accept and the ones it
  must refuse (a changed byte, a damaged signature, an untrusted root, an
  expired signer without a timestamp, the wrong certificate usage).
