## Wi-Fi: the WDI command layer scans

Intel's AX211 driver opened its adapter on NovaOS but could do nothing
after that: on Windows a WDI miniport is driven by Microsoft's
`wdiwifi.sys` with WDI messages, and NovaOS had no such layer.  Now it has
the first part of one (roadmap step 28.1, `kernel/nt/wdi.c`):

- **Commands and tasks** (`kernel/nt/wdi.h`, `kernel/nt/ndis.c`): a WDI
  message is an `OID_WDI_*` method request, a `WDI_MESSAGE_HEADER` and
  TLVs, sent to the driver's `MiniportOidRequest` and waited for also
  when it pends; a task waits as well for its completion indication,
  matched by transaction ID; `NdisMIndicateStatusEx` passes WDI
  indications to the layer.
- **Bring-up**: after `MiniportWdiOpenAdapter`, the adapter's
  capabilities (MAC address, firmware, radio state, bands) and its
  station port (`OID_WDI_TASK_CREATE_PORT`); the port is deleted before
  the adapter closes.
- **Scanning**: `OID_WDI_TASK_SCAN` on the port; the BSS entries the
  driver indicates are read from each access point's beacon: SSID, WPA2
  or WPA3 (the RSN element's key management), cipher, 802.11n/ac/ax,
  signal, band and channel.
- **`netsh wlan show interfaces`** and **`netsh wlan show networks
  [mode=bssid]`** in the Terminal list the adapters and what a scan found,
  as Windows prints them.

QEMU has no Wi-Fi card, so the devices suite's new `wifi` boot installs
`wditest.sys`, NovaOS's own WDI miniport, on QEMU's `edu` card; its radio
hears three access points.  Connecting, the data path, `wlanapi.dll` and
Settings' Wi-Fi page come next; Intel's driver on the real card is the
T14's check.
