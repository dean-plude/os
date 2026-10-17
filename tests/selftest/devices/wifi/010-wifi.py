# wifi: NovaOS's WDI command layer (step 28.1, kernel/nt/wdi.c).  QEMU has no
# Wi-Fi card and Intel's own driver stops at its hardware probe on a
# stand-in (the ax211 boot), so QEMU's edu device (-device edu, PCI
# 1234:11e8) gets wditest.sys (tools/drivertest/wditest.c, in C:\Tests\Wifi),
# a WDI miniport as Intel's Netwtw14.sys is one, whose radio hears three
# access points.  With test signing on, "pnputil /add-driver wditest.inf
# /install" installs it: it registers with ndis.sys as a WDI miniport, the
# upper edge allocates and opens its adapter, asks its capabilities
# (OID_WDI_GET_ADAPTER_CAPABILITIES: MAC address, firmware, radio, bands)
# and creates its station port (OID_WDI_TASK_CREATE_PORT, finished by the
# create-port-complete indication).  "netsh wlan show interfaces" lists the
# adapter; "netsh wlan show networks mode=bssid" scans (OID_WDI_TASK_SCAN:
# the driver pends it, indicates BSS entries with each access point's
# beacon, completes it and indicates the scan's end) and lists the networks
# by SSID with their security from the beacons' RSN elements (WPA2-Personal,
# WPA3-Personal with SAE, open), signal, band and channel.  Disabling the
# device deletes the port and closes the adapter, and the interface is gone.
DOC = ('a WDI Wi-Fi miniport (wditest.sys) on QEMU\'s edu device, installed with `pnputil /add-driver /install`: '
       'NovaOS\'s WDI command layer reads its capabilities and creates its station port, `netsh wlan show interfaces` '
       'lists it and `netsh wlan show networks mode=bssid` scans (OID_WDI_TASK_SCAN and its BSS entry indications) '
       'and lists three networks (WPA2-Personal, WPA3-Personal, open) with signal, band and channel; disabling the '
       'device deletes the port and closes the adapter')
EDU_ID = '"PCI\\VEN_1234&DEV_11E8"'
TESTS = [
    Test('wifi test signing on', r'reg add HKLM\SYSTEM\CurrentControlSet\Control\NovaOS /v TestSigning /t REG_DWORD /d 1 /f',
         [r'completed successfully'], settle=2),
    Test('wifi restart', 'shutdown /r /t 0', [r'\[NT\] Test signing on'], reboot=True),
    Test('wifi no interface', 'netsh wlan show interfaces', [r'There is no wireless interface on the system\.'],
         builtin=True),
    Test('wifi install', r'pnputil /add-driver C:\Tests\Wifi\wditest.inf /install',
         [r'Driver package installed on device: PCI\\VEN_1234&DEV_11E8',
          r'\[NDIS\] WdiTest: WDI miniport registered \(NDIS 6\.50, WDI version 0x10108\)',
          r'\[DbgPrint\] wditest: adapter allocated',
          r'\[DbgPrint\] wditest: adapter open: radio on, 3 access points in range',
          r'\[NDIS\] WdiTest: WDI: open adapter completed \(0x00000000\)',
          r'\[DbgPrint\] wditest: adapter capabilities answered',
          r'\[WDI\] WdiTest: adapter capabilities: MAC 02:4e:6f:76:61:00, firmware wditest 1\.0, radio on/on, bands 2\.4 5',
          r'\[DbgPrint\] wditest: create port task \d+: modes 0x1, NDIS port 0',
          r'\[DbgPrint\] wditest: station port 1 created',
          r'\[WDI\] WdiTest: station port 1 created \(MAC 02:4e:6f:76:61:00\)',
          r'\[PNP\] PCI\\VEN_1234&DEV_11E8\S*: started \(driver WdiTest\)']),
    Test('wifi interfaces', 'netsh wlan show interfaces',
         [r'There is 1 interface on the system:', r'Name +: Wi-Fi\n',
          r'Description +: NovaOS WDI test Wi-Fi adapter', r'Physical address +: 02:4e:6f:76:61:00',
          r'State +: disconnected', r'Radio status +: Hardware On\n +Software On'], builtin=True),
    Test('wifi scan', 'netsh wlan show networks mode=bssid',
         [r'\[DbgPrint\] wditest: scan task \d+ on port 1: wildcard BSSID, any SSID, type 3, dwell 20/110 ms, at most 4000 ms',
          r'\[DbgPrint\] wditest: scan complete: 3 BSS entries indicated',
          r'\[WDI\] WdiTest: scan on port 1: 0x00000000, 3 access point\(s\)',
          r'Interface name : Wi-Fi', r'There are 3 networks currently visible\.',
          r'SSID 1 : NovaNet\n +Network type +: Infrastructure\n +Authentication +: WPA2-Personal\n +Encryption +: CCMP\n'
          r' +BSSID 1 +: 02:4e:6f:76:61:01\n +Signal +: 90% *\n +Radio type +: 802\.11n\n +Band +: 2\.4 GHz\n'
          r' +Channel +: 6\n',
          r'SSID 2 : NovaNet-5G\n +Network type +: Infrastructure\n +Authentication +: WPA3-Personal\n'
          r' +Encryption +: CCMP\n +BSSID 1 +: 02:4e:6f:76:61:02\n +Signal +: 72% *\n +Radio type +: 802\.11ax\n'
          r' +Band +: 5 GHz\n +Channel +: 36\n',
          r'SSID 3 : NovaGuest\n +Network type +: Infrastructure\n +Authentication +: Open\n +Encryption +: None\n'
          r' +BSSID 1 +: 02:4e:6f:76:61:03\n +Signal +: 35% *\n +Radio type +: 802\.11n\n +Band +: 2\.4 GHz\n'
          r' +Channel +: 11\b'], builtin=True),
    Test('wifi networks', 'netsh wlan show networks',
         [r'There are 3 networks currently visible\.', r'SSID 2 : NovaNet-5G\n +Network type +: Infrastructure\n'
          r' +Authentication +: WPA3-Personal\n +Encryption +: CCMP\n'], builtin=True),
    Test('wifi disable', 'pnputil /disable-device /deviceid ' + EDU_ID,
         [r'Device disabled successfully', r'\[DbgPrint\] wditest: station port 1 deleted',
          r'\[WDI\] WdiTest: station port 1 deleted \(0x00000000\)', r'\[DbgPrint\] wditest: adapter closed(?! \()',
          r'\[NDIS\] WdiTest: WDI: adapter closed \(0x00000000\)', r'\[DbgPrint\] wditest: adapter freed']),
    Test('wifi gone', 'netsh wlan show interfaces', [r'There is no wireless interface on the system\.'],
         builtin=True),
]
