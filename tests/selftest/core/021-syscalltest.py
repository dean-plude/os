# syscalltest: the system-call table as code that calls the kernel without
# ntdll sees it (Roblox's Hyperion ranks ntdll's Zw exports by address to
# find each service's number, then issues raw `syscall`s).  Checks the
# ranked numbers against Windows 10 1903 and that the kernel answers them.
DOC = ('`syscalltest` (the system-call table reached without ntdll: ntdll\'s Zw exports ranked by address give '
       'each service its Windows 10 1903 number, and the kernel answers that number, as anti-cheat code expects)')
TESTS = [
    Test('syscalltest', 'syscalltest', [r'syscalltest: \d+ passed, 0 failed']),
]
