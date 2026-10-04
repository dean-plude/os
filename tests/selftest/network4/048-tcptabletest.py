# tcptabletest: iphlpapi's TCP connection tables over 127.0.0.1 and ::1:
# GetExtendedTcpTable's owner-PID, owner-module and basic classes
# (listeners, connections, all), sorting and the size query, GetTcpTable
# and GetTcp6Table; GOG Galaxy's client service finds which process is
# calling it this way
DOC = '`tcptabletest` (GetExtendedTcpTable / GetTcpTable / GetTcp6Table: a listener and a connection with their states and owning process), 64- and 32-bit'
TESTS = [
    Test('tcptabletest', 'tcptabletest', [r'tcptabletest: \d+ passed, 0 failed']),
    Test('tcptabletest x86', r'C:\Programs\x86\tcptabletest.exe', [r'tcptabletest: \d+ passed, 0 failed']),
]
