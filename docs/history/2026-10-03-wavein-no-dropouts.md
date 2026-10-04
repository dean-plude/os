## waveIn: no dropouts when a program falls behind

Audacity's recording in the app corpus came back with a "Dropouts" label
track (13 dropouts in ten seconds under QEMU without KVM, two under KVM)
and a recording shorter than it should be.  Audacity records through
PortAudio's MME host: eight `waveIn` buffers of about 14 ms, taken in turn
by one thread.  When that thread finds every buffer done, PortAudio counts
an input overflow, throws all but the newest away and tells Audacity,
which marks a dropout.  On NovaOS a thread, not the sound card, hands the
buffers back, and while Audacity redraws on a slow machine both that
thread and PortAudio's were held up for 100 to 270 ms.  The waveIn thread
then filled every queued buffer at once from the 1 s the kernel stream
holds, dropped the rest because no buffer was left, and PortAudio dropped
seven more.

`waveIn` now hands the last buffer queued back only once the program has
queued another (or after half a second), when the program ever queued more
than one, so a program that falls behind never finds every buffer done;
and with no buffer queued it keeps the newest half second of recording
instead of dropping it all, so the frames wait for the program's next
buffer.  A program cycling a single buffer gets it back as soon as it is
full, as before.

New self-test `soundtest mme`: records five seconds as PortAudio does with
the thread held up 250 ms every second.  Before, six overflows and 42
buffers lost; now none, and `tools/wavcheck.py --gaps HZ` (used by the
test) finds no jump anywhere in the recorded tone.

In the app corpus, Audacity's ten seconds now come back whole: the
screenshot after Stop differs from the reference in 0.2% of pixels (10%
before, with the dropout track), and its `dir` step that looks for the saved
project is marked a Terminal command, so it passes instead of waiting for a
program that never starts.
