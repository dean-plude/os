# after "crash kernel" (900) and a reset: the start that follows turns the
# report the fault wrote into \NOVA\PANIC.TXT into a file in
# C:\NovaOS\Crashes, with the backtrace
DOC = 'the kernel crash report: after `crash kernel` and a reset, `crashes last` shows the fault\'s backtrace'
SAVED = r'\[CRASH\] The kernel crashed during the last start: report saved as C:\\NovaOS\\Crashes\\kernel-\d{8}-\d{6}\.txt'
TESTS = [
    Test('kernel crash report', 'crashes last', [r'NovaOS kernel crash report', r'KeCrashTestFault\+0x[0-9a-f]+'],
         builtin=True, quotes_panic=True, boot_expect=[SAVED]),
]
