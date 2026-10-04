# gatetest: Windows' x64 segment layout and self-inspection (Roblox's
# Hyperion relies on these): selectors 0x33/0x2B/0x53, a far jump to 0x23
# runs 32-bit code in a 64-bit program and its faults carry SegCs 0x23,
# Get/SetThreadContext work on the calling thread, and __fastfail ends the
# program with STATUS_STACK_BUFFER_OVERRUN.
DOC = ('`gatetest` (Windows\' segment selectors, 32-bit code in a 64-bit program through a far jump '
       'to 0x23, GetThreadContext on the calling thread and `__fastfail`, as anti-cheat code expects)')
TESTS = [
    Test('gatetest', 'gatetest', [r'gatetest: \d+ passed, 0 failed']),
]
