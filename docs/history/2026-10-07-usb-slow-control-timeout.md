## USB devices that are slow to answer

The xHCI driver gave a control transfer or a command a fixed number of
polls of the event ring (twenty million), a few tenths of a second on a
fast processor, and a device whose first request took longer was dropped
with "control request 06 timed out": it never attached.  On a loaded CI
machine that is what happened to the test surround headset after the
restart in one run of the device self-tests, and with it the five tests
after it.  Both now wait at least the polls and at least five seconds, in
time, as the EHCI driver always did and the USB specification allows a
control transfer.  A device self-test plugs a speaker that answers its
first request three seconds late and plays a tone on it.

The test device (`tools/usbredirpeer.py`) is much cheaper to run beside
QEMU: it reads its socket in large chunks, converts the speaker's samples
in batches (about five times more packets a second than before) and sends the
microphone's packets one write each (a burst in one write overflows QEMU's
buffer and the guest then hears nothing), with one loop for each start
of the stream so a stop and a start close together no longer leave two
running.  With `--log-times` it logs the connection's events with times
and, every five seconds, what it took and sent, so a stall in a CI run
shows in the artifact.
