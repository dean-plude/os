## One-shot thread-pool callbacks release their resources

`TrySubmitThreadpoolCallback` previously created a persistent work object
and an idle event for every submission and never released either. Repeated
submissions could exhaust the process's 4096 handle slots. It also called
the two-argument simple callback as a three-argument work callback, which
does not match the x86 callee-cleaned calling convention.

One-shot submissions now use a small callback context that the worker
releases before calling the application, without allocating an idle event.
The worker uses the two-argument callback signature. Thread creation failure
returns `FALSE`, preserves the error, and releases the context.

`tpsimpletest` submits 5120 callbacks in batches and checks callback context
and handle counts on x64 and x86. This fixes a runtime resource leak useful
to desktop applications; Firefox's latest nightly crash reported error 4
(too many open files), but that application's recovery still needs a corpus
run and is not claimed here.
