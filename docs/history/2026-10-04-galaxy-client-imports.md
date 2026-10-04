## The rest of GOG GALAXY's load-time imports

GOG GALAXY's client (`GalaxyClient.exe`, 64-bit Qt 6 WebEngine) did not
start: Qt WebEngine (Chromium) imports Bluetooth's `bthprops.cpl`,
`d3d12.dll` and `winusb.dll`, which NovaOS did not have, and about forty
functions its DLLs lacked.  All of them are there now, each answering as
Windows does on a PC without the hardware it reaches, and the client loads
(89 modules) and runs until Qt's Windows platform plugin asks for a
Windows Runtime class NovaOS does not have yet
([compatibility.md](../compatibility.md)).

- **`bthprops.cpl`** (x64 and x86; a DLL with a Control Panel item's
  name, which a DLL's `dll.json` now gives as `"file"`): no Bluetooth
  radio, so the radio and device searches end at once with
  `ERROR_NO_MORE_ITEMS`; the Service Discovery Protocol record parsers
  (`BluetoothSdpGetAttributeValue`, `BluetoothSdpGetContainerElementData`,
  `BluetoothSdpGetElementData`, `BluetoothSdpEnumAttributes`) work in full.
- **`d3d12.dll`**: NovaOS's own front, as for `d3d9` and `d3d11`: no
  Direct3D 12 device (`DXGI_ERROR_UNSUPPORTED`) and no debug layer, with
  `D3D12CreateDevice` and `D3D12GetDebugInterface` at Windows' ordinals 101
  and 102, which Qt imports them by.
- **`winusb.dll`**: the WinUSB calls, with no WinUSB device to open.
- **Functions**: `SetEnvironmentStringsW`; `TreeResetNamedSecurityInfoW`
  (a folder's tree made to inherit a new DACL); `NetShareEnum`;
  `ConvertInterfaceNameToLuid`, `ConvertInterfaceLuidToGuid` and back,
  `GetInterfaceInfo`, `IpReleaseAddress`/`IpRenewAddress`,
  `CancelIPChangeNotify`; `DnsQueryEx` (with a completion routine too);
  `WSAAccept` with its condition function; `SetArcDirection`,
  `GetArcDirection` (arcs, pies and chords drawn clockwise) and `CancelDC`;
  `SetupDiOpenDeviceInfo`, `SetupDiOpenDeviceInterface`; the HID report
  parsers and `HidD_GetSerialNumberString`; `CreateAppContainerProfile`
  and `DeleteAppContainerProfile` (the profile's registry mapping and its
  `%LOCALAPPDATA%\Packages` folder); `CryptVerifyCertificateSignatureEx`
  (a certificate or CRL's signature checked against a certificate, chain
  or public key), `CertCompareCertificateName`, `CertControlStore`;
  WinHTTP's proxy resolver (`WinHttpCreateProxyResolver`,
  `WinHttpGetProxyForUrlEx`, which ends, as `WinHttpGetProxyForUrl` does,
  with `ERROR_WINHTTP_AUTODETECTION_FAILED`); urlmon's
  `CoInternetCreateSecurityManager` (Windows' default zones: files are
  the Local Machine zone, the web the Internet zone); `_ultow_s`,
  `_ui64tow_s`, `_i64tow_s`, `_itow_s`.
- **`tools/pe_imports.py`** maps API sets as the kernel's loader does
  (`api-ms-win-core-synch-*` to `kernelbase`, `api-ms-win-power-*` to
  `powrprof`, `kernel32` and `kernelbase` standing in for each other), so
  it no longer lists `WaitOnAddress` or `CallNtPowerInformation` as
  missing.
- **Tests**: `qtwebtest` (core suite) checks every one of them, 64- and
  32-bit.
