## Sound: a speaker another one took over from goes quiet

The devices suite's `usbaudio ohci` and `usbaudio unplug` checks failed on
loaded CI runners: the OHCI speaker's 550 Hz tone read 519 Hz (5.6% flat,
over the 5% limit), while the same tests passed locally.  The cause was in
the mixer, not in isochronous scheduling or the measurement.  When a new
output is attached, the mixer writes only to the new one, but the old
output still streams its ring (a USB speaker keeps its isochronous
transfers going, the HD Audio card its DMA).  Nothing wrote that ring any
more, so the old speaker played its last 341 ms over and over until it
was unplugged or became the playing output again.  When the switch came
while the ring still held the end of a tone, as it can when the mixer
thread runs late on a loaded host, the loop joined onto the tone through
gaps short enough to be bridged, and the recording held one long, flat
"tone" (reproduced locally at 527 Hz by plugging the next speaker in
during the tone).  When the ring held only a little of the tone, the
speaker repeated 100 ms bursts of it every 341 ms, which the check did
not look at.

- `kernel/drivers/audio.c`: at the switch the mixer clears the old
  output's ring beyond what was mixed for it, and from then on keeps
  80 ms of silence ahead of each attached output that is not playing, so
  what was mixed before the switch still plays and then it is quiet.
  (Clearing only from the next tick on was not enough: on a CI runner
  the mixer had fallen behind at the switch and 30 ms of the old lap
  still played.)  This also applies to the HD Audio card when a USB
  headset takes over.
- `tools/selftest.py`: `tones(..., only=True)` also fails when anything
  else sounds in the recording.  The `usbaudio xhci`, `usbaudio uhci` and
  `usbaudio unplug` checks use it on their speakers' WAVs.  With four busy
  host threads, the old kernel failed it (40 repeated bursts on the xHCI
  speaker) and the fixed one passed every run, also with QEMU and three
  busy threads pinned to one host CPU.
