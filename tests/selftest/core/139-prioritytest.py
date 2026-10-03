# prioritytest: SetThreadPriority/SetPriorityClass map onto NT's base
# priorities and round-trip through the query functions, REALTIME without
# the privilege is HIGH, and with boosts off a THREAD_PRIORITY_HIGHEST
# thread woken by an event preempts busy NORMAL threads; run from the
# Terminal it is the foreground process, its threads get 60 ms time slices
# against a background process's 20 ms, and the foreground boost, 64- and 32-bit
DOC = '`prioritytest` (every priority class and thread level gives NT\'s base priority and reads back; REALTIME without the privilege is HIGH; with boosts off a HIGHEST thread woken by an event runs within 2 ms while NORMAL threads spin on every processor; the Terminal\'s console program is the foreground process, gets the foreground boost and three times a background process\'s time slice, 64- and 32-bit)'
TESTS = [
    Test('prioritytest x64', 'prioritytest', [r'prioritytest: 35 class/level combinations match NT',
                                              r'prioritytest: the foreground process\'s threads run', r'prioritytest: PASS']),
    Test('prioritytest x86', r'C:\Programs\x86\prioritytest.exe', [r'prioritytest: 35 class/level combinations match NT',
                                                                     r'prioritytest: the foreground process\'s threads run', r'prioritytest: PASS']),
]
