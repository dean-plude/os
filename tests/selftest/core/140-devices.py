# devices: the Terminal's list of PCI functions and the driver that took
# each one (docs/hardware.md uses it on a real machine).  The core boot's
# q35 machine has an AHCI controller, a standard VGA card, an HD Audio
# card and a qemu-xhci controller, which must show their drivers; the
# host bridge is a bridge
DOC = '`devices` (every PCI function and the driver that claimed it: AHCI, the VGA card, HD Audio, xHCI; bridges marked)'
TESTS = [
    Test('devices', 'devices', [r'00:00\.0  8086:29c0  Intel\s+Host bridge\s+\(bridge\)',
                                r'00:1f\.2  8086:2922  Intel\s+SATA \(AHCI\)\s+AHCI',
                                r'1234:1111  QEMU\s+VGA display\s+Bochs VBE',
                                r'8086:2668  Intel\s+HD Audio\s+HD Audio',
                                r'00:05\.0  1b36:000d  QEMU\s+USB \(xHCI\)\s+xHCI',
                                r'devices: \d+ PCI functions, [1-9]\d* with a driver, [1-9]\d* bridges?, \d+ without a driver'],
         builtin=True),
]
