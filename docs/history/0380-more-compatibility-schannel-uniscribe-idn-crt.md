## More compatibility: Schannel, Uniscribe, IDN, CRT gaps

Driven by ffmpeg's imports (`tools/pe_imports.py`).  Before writing each
DLL we looked for an MIT, BSD or zlib licensed one to reuse; none existed
for these, so the new ones are written here, and Schannel reuses the Mbed
TLS already in the tree.

- **Schannel** (`userland/secur32/schannel.c`): `InitializeSecurityContext`,
  `EncryptMessage`, `DecryptMessage`, `QueryContextAttributes` (stream
  sizes, connection info, ALPN), `ApplyControlToken` (shutdown) and the
  `InitSecurityInterface` tables, on Mbed TLS with the Mozilla roots
  (`C:\Windows\System32\ca-bundle.der`).  It takes `SCHANNEL_CRED` and
  `SCH_CREDENTIALS`, SNI, ALPN, manual validation and
  `SCH_CRED_NO_SERVERNAME_CHECK`, and reports untrusted roots, expired
  certificates and name mismatches as Windows does.  Client side only.
- **New DLLs**: `usp10` (Uniscribe for left-to-right scripts:
  `ScriptItemize`, `ScriptShape`, `ScriptPlace`, `ScriptTextOut`,
  `ScriptBreak`, `ScriptString*`…), `normaliz` (`IdnToAscii`/`IdnToUnicode`,
  RFC 3492 Punycode), `ncrypt` (the provider opens; there are no stored
  keys), `avicap32` (no capture devices), `d2d1` (the matrix helpers;
  factories report `E_NOTIMPL`).
- **More of existing DLLs**: the CRT's `mbstowcs_s`/`wcstombs_s`, the
  `_nolock` functions, `freopen_s`, `tmpnam_s`, `_utime64`, `_wspawnvp`
  and the single-byte `_mbs*` set; `ws2_32` `getservbyname`/`getservbyport`,
  `gethostbyaddr` and `WSAPoll`; `winmm` `waveIn*` (no recording devices);
  `dnsapi` `DnsQuery_UTF8`; `iphlpapi` `GetIpForwardTable2` and friends;
  `advapi32` `RegLoadMUIStringW`; `shlwapi` `SHCreateStreamOnFileEx` and
  `StrRetTo*`; `ole32` `CreateBindCtx`, `ReadClassStm`/`WriteClassStm`,
  `OleSaveToStream`/`OleLoadFromStream`; `gdi32` DIB colour tables.
- **Loader**: a program's own TLS callbacks now run (process and thread
  attach and detach), not only those of DLLs; GLib checks for this.
- **Exceptions**: `RtlRaiseException` reports its caller's frame, so a
  handler that continues execution (the "set thread name" exception
  `0x406D1388`) resumes after the call instead of raising again forever.
- **`tools/novarun.py --net`** gives the guest a network card on QEMU's
  user network (the host is `10.0.2.2`).
- Tested with an FFmpeg nightly (BtbN's static x64 build) in QEMU, with
  the GDI and DirectWrite functions of the Firefox work in place: an x264
  encode, decoding it back, and streaming a WAV over HTTPS from the host
  with TLS 1.3 and with TLS 1.2; with `tls_verify` on (ffmpeg's default) a
  self-signed server is refused as an untrusted root.
