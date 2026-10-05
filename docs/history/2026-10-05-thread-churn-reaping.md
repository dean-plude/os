## Short-lived threads reuse slots before the background poll

The core CI failures shared by PRs 243–247 were in tpsimpletest, including on
main: x64/x86 reported one extra handle, and an x86 run exhausted its 96 thread
slots. A burst of callbacks can create threads faster than UmPoll reaps their
exited kernel objects. um_create_thread now invokes the same reaper under the
process lock before selecting a slot. Only exited threads that the scheduler
confirms are dead and off CPU are eligible. Live threads and exited threads
still on CPU keep their slots; object references held by handles remain valid.

The test's cold baseline also preceded the loader critical section's first
contention. That creates one persistent process-wide event, rather than a leaked
callback handle. The test now forces loader-lock contention with a joined warm-up
thread before recording its baseline. It still checks every one of 160 measured
batches (5120 callbacks) for correct context, x86 calling convention and no
handle-count growth. Timeouts remain failures; no sleep-based tolerance or extra
handle allowance is added. Host source-based regressions cover slot reuse and
reaper safety plus the lazy contention event. Native x64/x86 SMP validation is
required in Actions. The existing scheduler migration fix is preserved.
