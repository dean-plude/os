# afdtest: sockets polled through a \Device\Afd helper handle over
# 127.0.0.1, as wepoll does it (Poco's PollSet in GOG Galaxy's client
# service, libevent): IOCTL_AFD_POLL pending and finishing on a completion
# port, time-outs, NtCancelIoFileEx, the peer and the program closing;
# the keyed events wepoll locks with; a UDP socket bound to 127.0.0.1
# (and ::1) waking its own poll, as Poco's PollSet::wakeUp does
DOC = '`afdtest` (IOCTL_AFD_POLL on a \\Device\\Afd helper bound to a completion port: accept, receive, send, time-outs, cancel, disconnect, local close; a loopback-bound UDP socket waking its own poll; keyed events), 64- and 32-bit'
TESTS = [
    Test('afdtest', 'afdtest', [r'afdtest: \d+ passed, 0 failed']),
    Test('afdtest x86', r'C:\Programs\x86\afdtest.exe', [r'afdtest: \d+ passed, 0 failed']),
]
