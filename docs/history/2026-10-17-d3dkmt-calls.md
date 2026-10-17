## D3DKMT calls reach the kernel

gdi32's `D3DKMT*` functions, the door Windows programs and graphics drivers
use into the WDDM graphics kernel, all answered `STATUS_NOT_SUPPORTED`.

Now they reach it, as the first part of step 3 of the Intel GPU design
([design/intel-gpu.md](design/intel-gpu.md)): each passes its `D3DKMT_*`
structure, as Windows lays it out on x64, to one NovaOS system call,
`NtNovaDxgkCtl` (`kernel/um/um_d3dkmt.c`).  The kernel lists a Basic Render
adapter, the software render-only adapter every Windows machine has, with
its LUID, type, WDDM version and shared memory (half the RAM, as Windows
gives integrated GPUs).  Programs create devices on it and allocations in
that memory, which `D3DKMTQueryVideoMemoryInfo` counts and `D3DKMTLock2`
maps into the process; fences and monitored fences (whose value the
process reads from a read-only page, as on Windows) are signalled from the
CPU and waited on in place, by another thread, or through an event, all of
them or any one.  A process's objects go with it.  `kmttest` checks this in
the core suite.

Next in step 3 are render miniports (`DxgkInitialize` and the test driver
`wddmtest.sys`), GPU virtual addresses, submission and the scheduler, fences
the GPU signals, preemption and TDR, sharing by handle, and 32-bit
programs.
