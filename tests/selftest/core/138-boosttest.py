# boosttest: a thread woken by an event, a semaphore, a condition variable
# or a thread message gets NT's priority boost and runs within 2 ms while a
# thread of the same base priority spins on every processor; the boost
# decays back to the base priority, 64- and 32-bit
DOC = '`boosttest` (a woken thread is boosted above its base priority and runs within 2 ms while threads of the same priority spin on every processor; the boost decays back to base, 64- and 32-bit)'
TESTS = [
    Test('boosttest x64', 'boosttest', [r'boosttest: woken threads run within \d+\.\d+ ms under load', r'boosttest: PASS']),
    Test('boosttest x86', r'C:\Programs\x86\boosttest.exe', [r'boosttest: woken threads run within \d+\.\d+ ms under load', r'boosttest: PASS']),
]
