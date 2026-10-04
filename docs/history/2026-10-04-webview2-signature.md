## WebView2: Edge Update accepts Microsoft's signature on the runtime

Microsoft Edge Update unpacked the WebView2 runtime's package and then
refused it: "failed to verify Microsoft signature for file", `0xa0430233`.
It checks the package with `WinVerifyTrust` (which NovaOS already did,
#184) and then asks crypt32 whether the signer's chain ends at one of
Microsoft's roots, with `CertVerifyCertificateChainPolicy` and the
Microsoft root policy (`CERT_CHAIN_POLICY_MICROSOFT_ROOT`), first plain and
then with `MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG`.
NovaOS's crypt32 did not know that policy and failed the call, so no
Microsoft signature could ever pass.  Nothing in Edge Update is skipped or
changed: the check now gets Windows' answer.

- **The Microsoft root policy**: the last certificate of the first chain
  must carry one of Microsoft's root keys (Microsoft Root Authority,
  Microsoft Root Certificate Authority, and its 2010 successor); with the
  application root flag, Microsoft Root Certificate Authority 2011 (which
  signs Microsoft's code signing CAs 2011 and 2024, and so the WebView2
  package) counts too.  Any other root fails with `CERT_E_UNTRUSTEDROOT`
  at the root's element.  As on Windows, the policy judges only the root's
  key; the chain's own errors are the base and Authenticode policies'.
- **Test**: `authtest` checks the policy on the test root (refused with
  either flag), on Microsoft's code signing CA 2024 (a copy in
  `tools/authenticode/mspca2024.cer`, chained to the 2011 root: refused
  without the application root flag, accepted with it) and on the 2010
  root (accepted).  Run alone, the WebView2 corpus test now logs the
  package cached (`[PackageCache::Put][Cache succeeded]`); the test does
  not require it, because in a full corpus run drive C: can be full by the
  time it runs.
- **Where the install stops now**: with the package accepted, the install
  fails with `0x80070003` because Edge Update's own background update
  pass, running at the same time, does not see the install worker and
  uninstalls Edge Update under it
  ([compatibility.md](../compatibility.md#webview2)).
