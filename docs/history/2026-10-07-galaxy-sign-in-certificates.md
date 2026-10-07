## GOG GALAXY's sign-in page over HTTPS

With the network on, GOG GALAXY's sign-in window stayed an empty panel:
its Chromium (Qt WebEngine) refused `login.gog.com`'s certificate ("No
matching issuer found"), its Qt network requests failed, and it said there
was no Internet.  Four things in NovaOS were short of Windows:

- **Certificate stores in stores.**  Chromium's `TrustStoreWin` looks up
  issuers in a collection store that holds its roots and intermediates
  collections, which hold the system stores.  crypt32 looked one level
  into a collection; it now looks through collections of collections
  (each store once), for `CertEnumCertificatesInStore`,
  `CertFindCertificateInStore` and the chain builder.
- **Extended key usages.**  `CertGetEnhancedKeyUsage` always failed, and
  Chromium trusts a root from the Windows stores for servers only when
  this call says it may.  It now answers as Windows does: the
  certificate's usage OIDs, or none listed and `CRYPT_E_NOT_FOUND` (every
  usage) when it has no such extension.
- **Schannel as Qt uses it.**  `InitializeSecurityContext` now returns the
  mutual-authentication and manual-validation flags when they were asked
  for (Qt drops a connection whose flags differ), and
  `QueryContextAttributes` answers `SECPKG_ATTR_CIPHER_INFO` (the suite,
  `TLS_...`) and `SECPKG_ATTR_REMOTE_CERT_CONTEXT` (the server's
  certificate, with the chain it sent in the context's store).
- **Online.**  wininet's `InternetGetConnectedState` reported offline
  always; it now reports a LAN connection while the network has an
  address (`NtNovaSockCtl` 15, the network's state).

32-bit programs also missed the roots added with `certutil -addstore
root`: their `C:\Windows\System32\CertStore` was redirected to SysWOW64.
File system redirection now leaves the folders Windows shares between the
two alone (catroot, catroot2, driverstore, drivers\etc, logfiles, spool)
and CertStore.

GALAXY now loads and draws GOG's sign-in page, which shows "Oops!
Something went wrong" with a Log in button: the client's own requests
through Qt's Schannel backend still end with "Connection closed".

- Tested: `schanneltest` (core suite `schanneltest stores`, 64- and
  32-bit; network suite against `tools/h2server.js`): a certificate found
  through a collection of collections, a root's key usages, the Schannel
  flags and attributes, the server's certificate and its chain, a request
  and its answer, and online/offline.
