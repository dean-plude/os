## Read the current thread without retaining another CPU's KPCR

The scheduler's current-thread lookup read `GS:0` (the CPU's KPCR), then
read its `CurrentThread` field. Preemption and migration between those
instructions could make the second read return the previous CPU's next
thread. In particular, its idle thread has no `UmThread`.

`KiGetCurrentThread` now reads `GS:KPCR_CURRENT_THREAD` in one instruction,
and the scheduler uses that getter. A migration before or after this
instruction preserves the caller's identity.

The failed Roblox installer corpus run faulted writing address `0xe0` in
`wait_objects`, which registers the caller in the waiter list. This race
fits the null user-thread pointer in that path. The earlier missing DXVK
and WebView2 messages are not evidence that those dependencies caused the
kernel panic; a new corpus run is still needed to confirm recovery.

`waitmigrationtest` repeatedly yields and performs timed wait-any and
wait-all calls from multiple workers, in both x64 and x86 core suites.
The nightly workflow's failure gate remains unchanged.
