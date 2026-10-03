# sleeptest timer: Sleep, wait timeouts and waitable timers (timer queue
# timers too) end within 1 ms under load (the HPET-calibrated one-shot or
# TSC-deadline APIC timer, and timer wake-ups preempting the running thread)
DOC = '`sleeptest timer` (`Sleep(1)`, 1 ms wait timeouts and waitable timers (periodic ones, their completion routines and timer queue timers too) end within a millisecond with every CPU busy)'
TESTS = [
    Test('sleeptest timer', 'sleeptest timer', [r'sleeptest: resolution \d+\.\d+ ms under load',
                                         r'sleeptest: waitable timers \d+\.\d+ ms under load', r'sleeptest: PASS'],
         boot_expect=[r'\[HPET\] At 0x[0-9a-f]+: \d+ Hz', r'calibrated against the HPET',
                      r'\[APIC\] (One-shot|TSC-deadline) timer started']),
]
