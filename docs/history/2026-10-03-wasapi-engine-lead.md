## WASAPI: the engine keeps a 100 ms lead

The core suite's `soundtest wasapi` recording check failed on some CI runs
and passed on others: the 660 Hz tone came out with silent gaps (the
zero-crossing pitch read 580 to 620 Hz).  A shared-mode client fills its
own buffer, 30 ms at the least, and `mmdevapi` passed every released frame
straight to the kernel mixer stream and reported the stream's queue as the
padding, so the client never had more than its buffer's worth queued.  On a
loaded host (GitHub's runners) the client's 10 ms wakeups came late by more
than that and the mixer ran dry.

`mmdevapi` now keeps an engine lead, as Windows' audio engine has a buffer
of its own: the kernel stream holds the client buffer plus 100 ms, and the
padding is what the stream holds beyond that lead.  The client therefore
writes 100 ms ahead of the mixer and a late wakeup does not starve it.
`GetStreamLatency` reports the lead with the mixer's 80 ms.  Under a CPU
load that split the tone before, the recording now holds one unbroken
1010 ms tone.
