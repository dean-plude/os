# samplertest: a sampling profiler's view of a thread waiting in a system
# call (Chromium's GPU process runs one from its start).  GetThreadContext
# must give every nonvolatile register the thread left user mode with,
# Rbp above all, so RtlVirtualUnwind can unwind a function that uses Rbp
# as its frame pointer (as the Vulkan loader's do).
DOC = '`samplertest` (a sampling profiler: GetThreadContext of a waiting thread, unwound with RtlVirtualUnwind)'

TESTS = [
    Test('thread context in a system call', 'samplertest', [r'samplertest: \d+ passed, 0 failed']),
]
