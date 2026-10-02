# the network suite's IPv4 boot: a virtio-net adapter on QEMU's user-mode network
TESTS = [
    Test('virtio-net', 'ipconfig', [r'Virtio network adapter', r'IPv4 Address[ .]*: 10\.0\.2\.15'], builtin=True,
         boot_expect=[r'\[VIRTIO\] Network adapter at [0-9a-f:.]+, MAC [0-9a-f:]+, queues \d+/\d+, link up']),
    Test('ping', 'ping 10.0.2.2', [r'Received = 4, Lost = 0'], builtin=True),
]
