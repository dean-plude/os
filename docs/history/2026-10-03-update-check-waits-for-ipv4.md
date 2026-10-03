## The update check waits for an IPv4 address

Right after a restart the update check waited for "an address" and went on
as soon as the machine had a global IPv6 address.  SLAAC finishes before
the DHCP lease, so a channel at an IPv4 address (the self-test's
`http://10.0.2.2:18090/`) was tried with no IPv4 address yet and failed with
"Downloading from 10.0.2.2 failed: Could not connect." (the devices suite's
"up to date" test, 0.8 s after the restart).

When the channel's host is an IPv4 literal, the check now waits for the IPv4
address (the DHCP lease or a static one) and no longer counts an IPv6
address; names and IPv6 literals still go on with either.  The wait is the
same bounded 30 seconds as before, so a machine with no IPv4 network still
reports the failure.
