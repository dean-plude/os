# aligntest: the general-protection faults Windows looks into before
# reporting them (Roblox's Hyperion relies on these): alignment-fault
# fixup rewrites a misaligned MOVDQA to MOVDQU and retries,
# ThreadHideFromDebugger validates its length, and ntdll's
# extended-context functions describe the processor state.
DOC = ('`aligntest` (a misaligned SSE access is fixed up to the unaligned move when alignment-fault '
       'fixup is on, as anti-cheat code expects; ThreadHideFromDebugger and the extended-context functions)')
TESTS = [
    Test('aligntest', 'aligntest', [r'aligntest: \d+ passed, 0 failed']),
]
