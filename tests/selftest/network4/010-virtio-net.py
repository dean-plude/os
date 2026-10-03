# the network suite's IPv4 boot: a virtio-net adapter on QEMU's user-mode network
def _dhcp(nova):
    """wait for the DHCP lease (DHCP follows the link-up by a moment, longer
    under CI load) so `ipconfig` does not print "(waiting for DHCP)"""
    if '[NET] DHCP: address' not in open(nova.serial_path, 'rb').read().decode('latin-1'):
        nova.sr.wait('[NET] DHCP: address', 90)


TESTS = [
    Test('virtio-net', 'ipconfig', [r'Virtio network adapter', r'IPv4 Address[ .]*: 10\.0\.2\.15'], builtin=True, before=_dhcp,
         boot_expect=[r'\[VIRTIO\] Network adapter at [0-9a-f:.]+, MAC [0-9a-f:]+, queues \d+/\d+, link up']),
    Test('ping', 'ping 10.0.2.2', [r'Received = 4, Lost = 0'], builtin=True),
]
