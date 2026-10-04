# mmcsstest: a thread registered with the Multimedia Class Scheduler
# (avrt.dll) runs at a real-time priority without the privilege (18 for
# "Pro Audio", 16 for "Games" and LOW, back to its own on revert), runs
# within 2 ms while a busy TIME_CRITICAL thread (what wake-up boosts can
# lift a foreground program's window threads to) holds every processor,
# and a busy one on every processor still leaves a NORMAL thread room
# (80% budget), 64- and 32-bit
DOC = '`mmcsstest` (MMCSS through avrt.dll: "Pro Audio" runs at 18 and "Games" at 16 without the privilege and reverts to the thread\'s own priority; a registered thread woken by an event runs within 2 ms while TIME_CRITICAL threads spin on every processor; registered threads spinning on every processor still leave a NORMAL thread room, 64- and 32-bit)'
TESTS = [
    Test('mmcsstest x64', 'mmcsstest', [r'mmcsstest: MMCSS priorities match', r'mmcsstest: PASS']),
    Test('mmcsstest x86', r'C:\Programs\x86\mmcsstest.exe', [r'mmcsstest: MMCSS priorities match', r'mmcsstest: PASS']),
]
