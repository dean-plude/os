## The `powertest` self-test no longer races the keyboard's return after the lid wake

- **Before:** `powertest` failed now and then in CI (65 s, "the zone reads
  70 C" and "the zone reads 45 C") and held up pull requests that touched no
  power code; a rerun or a merge of main made it pass.
- **Cause:** a bug in the self-test's helper, not in the power code.  After
  `close_lid` wakes the machine it waits for NovaOS to find the USB keyboard
  again, and it looked for the `[USB] port N: keyboard` line *after* the
  `[SLEEP] Woke up` line.  The xHCI port is re-enumerated while NovaOS
  resumes, so the kernel logs the keyboard either side of `Woke up`
  (under KVM it came first).  When it came first the helper waited its full
  60 s, `powertest` had already asked for the 70 C reading and gone on to
  wait only 30 s for it, and by the time the helper wrote the temperature
  both waits had run out.  The thermal zone and the sleep and wake path
  were fine: the log of every failed run shows the lid closing, sleeping,
  waking and opening correctly.
- **Now:** `close_lid` is done once the log since the lid closed has
  `Woke up` and the keyboard's return after its removal, in either order.
  The checks `powertest` makes are unchanged.
