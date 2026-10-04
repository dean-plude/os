## The graphics job's hang: a guest clock that ran slow

The graphics job hung now and then: `d3dtest` x86 (about 2 runs in 200), then,
on the pull request that added the thread dump, `d3dtest` x64 and x86 both,
and once `gltest`.  The dump showed the main thread sleeping in Venus's ring
wait (`vn_relax`, from `vulkan_virtio.dll`) with the kernel idle: the guest
was waiting for a host that never answered.

The cause is in the boot, not in Venus.  The kernel calibrates the TSC and the
APIC timer over 10 ms of the HPET.  In the hung runs (and only in them: 2 of
the 13 runs whose logs were kept) that window came out at 40.0 and 40.2 million
TSC ticks and 1.03 million APIC ticks, against 23 to 26 million and 0.627
million in every other run: 16 ms had passed while the HPET counted 10.  Every
clock the guest had then ran 1.64 times slow, QueryPerformanceCounter among
them.  Mesa's Venus wakes the host's idle ring thread at most once per
millisecond of that clock (`VN_RING_IDLE_TIMEOUT_NS`), and the host's ring
thread goes idle after one millisecond of its own: with the slow clock a
wake-up that is due can be held back just as the host's thread goes to sleep,
and then neither side ever speaks again.

Fixes:

- `kernel/arch/x86_64/apic.c`: the HPET calibration takes five windows and
  keeps the one in which the TSC counted least (a stretched window only ever
  counts more, as in Linux's TSC calibration).  It logs the windows when they
  disagree by more than 10 %.
- `third_party/mesa-venus/novaos.patch`: `vn_ring_wait_seqno` and
  `vn_ring_wait_space` wake the ring again every 32 polls (a few tens of
  milliseconds) while it reports idle, so a lost wake-up costs that long, not
  forever.  Venus's own once-per-millisecond limit stays.
- `tools/ci/host_watch.py` and the graphics job: the host side is recorded
  (see building.md), so a hang that is not this one says which side stalled.
  The 900 s limit stays: a test that does not finish is still a failure.

If the graphics job hangs again, read `host-watch.log` and `host-hang-N.txt` in
the `graphics-out` artifact, and the `[APIC] Timer:` line in `serial.log`: a
TSC figure far from the others (about 24 million per 10 ms on these runners)
means the calibration was off again.
