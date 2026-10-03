## Firefox's delay-loaded DLLs and DirectWrite fallback

Two Phase 16 steps for Firefox (tested with Floorp 12.19): every DLL
`xul.dll` delay-loads now exists (16.2), and DirectWrite's font fallback
has a test (16.1).  No MIT, BSD or zlib implementation of these DLLs
exists (Wine's are LGPL, and the only HLSL compilers are LGPL too), so
they are NovaOS's own.

- **`d3d11.dll`** (`userland/d3d11`): NovaOS's own front for Direct3D 11.
  With DXVK installed from the App Store, which now installs DXVK's
  `d3d11` as `d3d11_dxvk.dll`, every entry point hands the call to DXVK
  (DXVK's `d3d10core` reaches it through `D3D11CoreCreateDevice`).
  Without DXVK, device creation fails with `DXGI_ERROR_UNSUPPORTED`, as
  on a PC with no Direct3D 11 driver, so programs use their software
  path.
- **`urlmon.dll`**: `CreateUri` (an `IUri` with every string and number
  property, IPv4/IPv6/DNS host types and default ports) and
  `CoInternetParseUrl` (scheme, domain, document, anchor, canonicalize,
  and path and URL conversion through shlwapi).
- **`winspool.drv`**: the print spooler's client, built and installed
  under its `.drv` name (`userland/winspool/build.py`), with Windows'
  ordinals for the default-printer calls (Firefox imports
  `GetDefaultPrinterW` as ordinal 203).  There are no printers yet: the
  lists are empty and opening a printer fails.
- **`credui.dll`**: the credential prompts report that the user
  cancelled, since NovaOS has no credential dialog yet.
- **`dhcpcsvc.dll`**: `DhcpRequestParams` finds no extra DHCP options, so
  a WPAD lookup moves on.
- **`d3dcompiler_47.dll`**: blobs (`D3DCreateBlob`, `D3DStripShader`),
  and a `D3DCompile` that fails with a message in its error blob.
- **`tools/pe_imports.py`** now checks delay-loaded imports, marked
  "(delay)", counts DLLs shipped beside a program, and reads `.drv`
  files.  It reports `0 missing` for Floorp's `xul.dll`.
- **Tests**: the new `delaytest` self-test exercises every one of these
  DLLs (31 checks, 64- and 32-bit).  The new `tools/dwtest`, in the
  graphics suite, lays out "Hello", an Arabic word and a Devanagari word
  in one line from a Latin-only font.  It checks that each script falls
  back to a font that has it, that the Arabic is joined and runs right to
  left, that the Devanagari conjunct forms, and that every run draws, and
  it shows the line in a window for the screenshot.
- `profapi.dll`, also on the 16.2 list, is not added.  Only
  `Microsoft.Internal.FrameworkUdk.dll` imports it, and that DLL also
  needs `Bcp47Langs`, `CoreMessaging` and `dcomp`.  It is Windows App SDK
  code Floorp runs without.
