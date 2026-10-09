# the network suite's IPv6 boot: an IPv6-only network that is tools/v6peer.py
# (SLAAC and RDNSS, DNS for nova6.test, HTTP on port 80)
TESTS = [
    Test('IPv6 address', 'ipconfig', [r'IPv6 Address[ .]*: fd00:6e6f:7661:0:[0-9a-f:]+',
                                      r'DNS Servers[ .]*: fd00:6e6f:7661::2'], builtin=True),
    Test('ping -6', 'ping -6 nova6.test', [r'Reply from fd00:6e6f:7661::1', r'Received = 4, Lost = 0'], builtin=True),
    Test('curl -6', 'curl -6 http://nova6.test/', [r'200 OK', r'Hello over IPv6'], builtin=True),
    Test('winsock IPv6', 'netcat nova6.test', [r'nova6\.test -> fd00:6e6f:7661::1 \(IPv6\)',
                                               r'connected \[fd00:6e6f:7661:[0-9a-f:]+\]:\d+ -> \[fd00:6e6f:7661::1\]:80',
                                               r'Hello over IPv6']),
]
