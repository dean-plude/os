## The Direct3D hang: a stuck GPU wait can be stopped, and the next one says who did not answer

The graphics job hung once more (run 37209922948, cancelled at its one-hour
limit): `d3dtest` on Venus printed `D3D9 863 frames in 6000 ms`, then every
thread of the program waited and neither Ctrl+C nor a kill ended it.  The
diagnostics added earlier showed what this is not: no kernel lock was held
(both CPUs idle, no `[WATCHDOG]` line), and what it is: one thread sat in
the GPU wait of `NtNovaGpuCtl` (timeline wait), and the others waited on
it.  The host never answered a fenced request; the render server had one
process and one thread, so the program's Venus worker was gone.

- **The wait can be stopped.**  `NtNovaGpuCtl` op 10 waits in 50 ms
  slices and returns when the program is being stopped (Ctrl+C, a kill),
  as the other kernel waits do.  Before, a host that never answered held
  the program and the self-test machine until the job's time ran out, with
  every later test lost; now the test fails at its own limit and the job
  goes on.
- **The guest says what it sent.**  Ctrl+Alt+F12 also logs the virtio-gpu
  control queue (`[VGPU]` lines): requests the card took and answered, and
  each fenced request still out with the timeline value it would set.
- **The host says who is alive.**  `tools/ci/host_watch.py` logs every
  change of the render server's processes, lists the server and its
  workers and the host kernel's messages about crashed or killed processes
  in `host-hang-N.txt`, and asks QEMU for the virtio-gpu queue state
  (`info virtio-queue-status`): did QEMU take the request, did it answer?

Next hang: a `[VGPU]` request out with QEMU's used index behind its
last-available index means virglrenderer took it and never finished it (look
for a missing worker and a `segfault` line); a request out that QEMU never
took means the notification was lost; no request out means the guest never
sent the one the program waits for.
