## soundtest dscapture: the recording buffer was half its size

With CI's test VMs on KVM, the core suite's `soundtest dscapture` crashed
after recording ("access violation at ntdll.dll+0x1e9f", that is
`RtlFreeHeap`, with a non-canonical address), while every TCG run passed.
It was not a DirectSound start-up race: `dsound`'s capture and mixer
threads start only after their buffer or device is fully set up.  The
fault was in the test program.  `dscapture` gathered its samples into
`calloc(total + half, 1)`, a size in bytes, then copied `total` 16-bit
samples into it, writing about 88 KB past the block.  What the overrun hit
depended on where the heap placed that block: on the TCG runs, memory
nobody used again; on the KVM run, the capture buffer's own `CBuffer`, so
releasing it freed pointers made of recorded samples.

Under TCG the crash reproduces by freeing a 200 KB block just before
`CreateCaptureBuffer`, so the recording buffer reuses that lower block and
`CBuffer` lies inside the overrun.  The buffer is now
`calloc(total, sizeof(short))`, and the same placement no longer crashes.
